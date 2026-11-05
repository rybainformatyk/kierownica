"""Konfiguracja: zapis, odczyt i odpornosc na zepsuty plik."""

import json
from pathlib import Path

import pytest

from wheelbridge import config as config_mod
from wheelbridge.config import AppConfig, OUTPUT_MODES


def test_roundtrip(tmp_path):
    cfg = AppConfig()
    cfg.output.mode = "g29_vjoy"
    cfg.pedals["brake"].deadzone_low = 0.3
    cfg.ffb.direction_mode = "sin"
    cfg.ui.geometry, cfg.ui.tab = "1000x700+10+20", 3
    path = tmp_path / "config.json"
    cfg.save(path)
    back = AppConfig.load(path)
    assert back.to_dict() == cfg.to_dict()
    assert config_mod.last_load_problem == ""


def test_missing_file_gives_defaults_silently(tmp_path):
    cfg = AppConfig.load(tmp_path / "config.json")
    assert cfg.output.mode == "g29"
    assert config_mod.last_load_problem == ""


def _write(tmp_path, data):
    path = tmp_path / "config.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


def _saved(tmp_path):
    cfg = AppConfig()
    cfg.steer.range_deg = 540
    cfg.output.mode = "pad"
    cfg.g29.buttons["CROSS"] = 16
    return cfg.to_dict()


@pytest.mark.parametrize("name, mutate", [
    ("sekcja null", lambda d: d.update(steer=None)),
    ("tekst zamiast liczby", lambda d: d["steer"].update(range_deg="abc")),
    ("pedaly to liczba", lambda d: d.update(pedals=5)),
    ("lista zamiast slownika", lambda d: d["pad"].update(buttons=[])),
    ("mapowanie to liczba", lambda d: d["buttons"].update(mapping=7)),
    ("nieskonczonosc", lambda d: d["ffb"].update(update_hz=1e400)),
    ("port to liczba", lambda d: d["link"].update(port=5)),
    ("srodek to tekst", lambda d: d["steer"].update(center_counts="x")),
    ("ui to tekst", lambda d: d.update(ui="x")),
])
def test_one_bad_field_does_not_wipe_everything(tmp_path, name, mutate):
    data = _saved(tmp_path)
    mutate(data)
    cfg = AppConfig.load(_write(tmp_path, data))
    # reszta ustawien uzytkownika zostaje
    assert cfg.output.mode == "pad", name
    assert cfg.g29.buttons["CROSS"] == 16, name
    assert isinstance(cfg.steer.range_deg, int)
    assert isinstance(cfg.link.port, str) and isinstance(cfg.steer.center_counts, int)
    assert not (tmp_path / "config.json.bad").exists()


@pytest.mark.parametrize("content", [
    b'{"steer": {"range_deg": 540',           # uciety zapis
    b"\xff\xfe\x00smieci",                     # nie UTF-8
    b"[1, 2, 3]",                              # nie obiekt
    b"",                                       # pusty
])
def test_unreadable_file_is_kept_aside(tmp_path, content):
    path = tmp_path / "config.json"
    path.write_bytes(content)
    cfg = AppConfig.load(path)
    assert cfg.output.mode in OUTPUT_MODES
    # zamkniecie panelu zapisze domyslne - oryginal musi przetrwac obok
    assert (tmp_path / "config.json.bad").read_bytes() == content
    assert "config.json.bad" in config_mod.last_load_problem


def test_save_retries_when_file_is_busy(tmp_path, monkeypatch):
    path = tmp_path / "config.json"
    AppConfig().save(path)
    real_replace = Path.replace
    calls = []

    def flaky(self, target):
        calls.append(1)
        if len(calls) < 3:
            raise PermissionError(13, "plik zajety")
        return real_replace(self, target)

    monkeypatch.setattr(config_mod.time, "sleep", lambda s: None)
    monkeypatch.setattr(Path, "replace", flaky)
    cfg = AppConfig()
    cfg.steer.range_deg = 720
    cfg.save(path)
    assert len(calls) == 3
    assert AppConfig.load(path).steer.range_deg == 720


def test_save_gives_up_and_cleans_tmp(tmp_path, monkeypatch):
    path = tmp_path / "config.json"

    def always_busy(self, target):
        raise PermissionError(13, "plik zajety")

    monkeypatch.setattr(config_mod.time, "sleep", lambda s: None)
    monkeypatch.setattr(Path, "replace", always_busy)
    with pytest.raises(PermissionError):
        AppConfig().save(path)
    assert not (tmp_path / "config.json.tmp").exists()


def test_modes():
    o = AppConfig().output
    table = {   # tryb: (vJoy, pad, G29, vJoy tylko przyciski)
        "g29": (False, False, True, False),
        "g29_vjoy": (True, False, True, True),
        "wheel": (True, False, False, False),
        "pad": (False, True, False, False),
        "both": (True, True, False, False),
        "none": (False, False, False, False),
    }
    for mode, expect in table.items():
        o.mode = mode
        assert (o.wheel_on, o.pad_on, o.g29_on, o.vjoy_buttons_only) == expect
    o.mode = "g29"
    assert o.vjoy_wanted is False and o.g29_wanted is True
    o.mode = "wheel"
    assert o.vjoy_wanted is True and o.g29_wanted is False
    o.mode = "none"
    assert o.vjoy_wanted is None and o.g29_wanted is False
    o.hide_unused = False
    o.mode = "pad"
    assert o.vjoy_wanted is None and o.g29_wanted is True


def test_limits_match_sliders():
    """Suwaki w panelu maja te same granice co normalize()."""
    cfg = AppConfig()
    p = cfg.pedals["throttle"]
    p.deadzone_low, p.deadzone_high, p.gamma = 0.9, 0.9, 9.0
    cfg.ffb.min_duty, cfg.ffb.max_duty = 200, 100
    cfg.ffb.direction_mode = "???"
    cfg.normalize()
    assert (p.deadzone_low, p.deadzone_high, p.gamma) == (0.45, 0.45, 3.0)
    assert cfg.ffb.max_duty == 201
    assert cfg.ffb.direction_mode == "auto"
