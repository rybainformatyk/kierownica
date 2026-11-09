"""Okno panelu (Tk) - ukryte, z konfiguracja w katalogu tymczasowym."""

import gc
import itertools
import json
import sys
import time
import tkinter as tk

import pytest
from conftest import make_frame, make_hello
from PIL import ImageChops

from wheelbridge import link as link_mod
from wheelbridge.config import AppConfig, G29_CONTROLS, OUTPUT_MODE_LABELS


def test_g29_drawing_controls_do_not_overlap():
    """Obietnica z wheelview.py: kontrolki nie nachodza na siebie i nie
    wychodza poza plotno."""
    from wheelbridge.ui import art
    from wheelbridge.ui.theme import Fonts
    from wheelbridge.ui.wheelview import CANVAS_H, CANVAS_W, WheelCanvas
    root = tk.Tk()
    root.withdraw()
    try:
        canvas = WheelCanvas(root, Fonts(root), on_button_click=lambda n: None,
                             on_button_clear=lambda n: None,
                             on_hover=lambda p: None)
        assert set(G29_CONTROLS) <= set(canvas._parts)
        masks = {}
        for name, part in canvas._parts.items():
            x1, y1, x2, y2 = art.shape_bbox(part.shape)
            assert x1 >= 0 and y1 >= 0 and x2 <= CANVAS_W and y2 <= CANVAS_H, name
            masks[name] = art.Paint(CANVAS_W, CANVAS_H, ss=2).mask(
                part.shape).point(lambda v: 255 if v >= 128 else 0)
        for a, b in itertools.combinations(masks, 2):
            assert ImageChops.multiply(masks[a], masks[b]).getbbox() is None, (a, b)
    finally:
        root.destroy()


@pytest.fixture
def app(tmp_path, monkeypatch, fake_outputs):
    from wheelbridge import config as config_mod
    from wheelbridge.ui import app as app_mod
    cfgfile = tmp_path / "config.json"
    cfg = AppConfig()
    cfg.link.auto_connect = False           # bez szukania plytki po portach
    cfg.save(cfgfile)
    monkeypatch.setattr(config_mod, "config_path", lambda: cfgfile)
    monkeypatch.setattr(app_mod, "config_path", lambda: cfgfile)
    switch = sys.getswitchinterval()
    panel = app_mod.PanelApp()
    panel.root.withdraw()
    panel.cfgfile = cfgfile
    panel.mod = app_mod
    try:
        yield panel
    finally:
        if not panel._closing:
            panel._on_close()
        gc.unfreeze()
        sys.setswitchinterval(switch)


def _connect(panel):
    eng = panel.engine
    eng.link.send = lambda frame: None
    eng.link.hello = make_hello()
    eng.link.state = link_mod.CONNECTED
    eng.link.stats.port = "COM9"
    eng._handle_hello(eng.link.hello)


def _dirty_text(panel):
    panel._dirty_check_t = 0.0
    panel._saved_flash_until = 0.0
    panel._refresh_saved_label()
    return panel.lbl_saved.cget("text")


def test_every_tab_refreshes_with_board(app):
    _connect(app)
    for i in range(len(app.mod.TAB_NAMES)):
        app._go_tab(i)
        for k in range(4):
            app.engine._handle_state(make_frame(counts=k * 400, buttons=1 << k,
                                                axes=(1000 + k * 500,) * 6))
            app._refresh()
            app.root.update()
    app._drain_log()
    log = app.txt_log.get("1.0", "end")
    assert "Błąd odświeżania" not in log and "Błąd w oknie" not in log
    assert app.lbl_vjoy.cget("text") == "G29"
    assert "połączono (COM9)" in app.root.title()


def test_callback_error_goes_to_log(app):
    app.root.after(1, lambda: 1 / 0)
    for _ in range(20):
        app.root.update()
        time.sleep(0.01)
    app._drain_log()
    assert "Błąd w oknie: ZeroDivisionError" in app.txt_log.get("1.0", "end")


def test_mode_choice_is_shared_by_all_tabs(app):
    app.var_mode_dash.set(OUTPUT_MODE_LABELS["g29_vjoy"])
    app._on_mode_change(app.var_mode_dash)
    shown = {app.var_mode_dash.get(), app.var_mode_pad.get(),
             app.var_mode_g29.get(), app.var_mode_diag.get()}
    assert shown == {OUTPUT_MODE_LABELS["g29_vjoy"]}
    assert app.cfg.output.mode == "g29_vjoy"


def test_dirty_indicator_and_saved_window_state(app):
    mod = app.mod
    assert _dirty_text(app) == mod.SAVE_HINT
    app._go_tab(3)                          # zakladka to nie ustawienie
    assert _dirty_text(app) == mod.SAVE_HINT
    app._pedal_widgets["brake"]["gamma"]._apply(1.5)
    assert _dirty_text(app) == mod.DIRTY_HINT
    app._save_config()
    saved = json.loads(app.cfgfile.read_text("utf-8"))
    assert saved["pedals"]["brake"]["gamma"] == pytest.approx(1.5)
    assert saved["ui"]["tab"] == 3
    assert _dirty_text(app) == mod.SAVE_HINT


def test_sliders_match_config_limits(app):
    w = app._pedal_widgets["throttle"]
    for name, field, top in (("dz_lo", "deadzone_low", 0.45),
                             ("gamma", "gamma", 3.0)):
        w[name]._apply(w[name]._value_at(10_000))     # suwak do konca
        assert getattr(app.cfg.pedals["throttle"], field) == pytest.approx(top)
        assert w[name].get() == pytest.approx(top)
    app.sl_minduty._apply(200.0)
    assert app.cfg.ffb.max_duty > app.cfg.ffb.min_duty
    assert app.sl_maxduty.get() == pytest.approx(app.cfg.ffb.max_duty)


def test_g29_states_in_header(app):
    _connect(app)
    eng = app.engine
    eng._handle_state(make_frame(flags=0x40))            # odlaczona
    app._refresh()
    assert app.lbl_vjoy.cget("text") == "G29: podłączanie"
    eng._handle_state(make_frame(flags=0x80 | 0x40))     # Windows jeszcze liczy
    app._refresh()
    assert app.lbl_vjoy.cget("text") == "G29: wykrywanie"
    eng.status.g29_attached_since -= 60                  # dawno - brak kabla
    app._refresh()
    assert app.lbl_vjoy.cget("text") == "G29: podłącz Native USB"
