"""Silnik: przelaczanie vJoy w tle, blokada wyjsc, srodek, G29 na USB, FFB."""

import threading
import time

import pytest
from conftest import FakeWheel, make_frame, make_hello, wait_for

from wheelbridge import link as link_mod
from wheelbridge import protocol
from wheelbridge.config import AppConfig
from wheelbridge.engine import CENTER_SETTLE_S, WheelEngine


def _engine(mode="g29"):
    cfg = AppConfig()
    cfg.output.mode = mode
    logs = []
    eng = WheelEngine(cfg, on_log=lambda lvl, msg: logs.append((lvl, msg)))
    eng.sent = []
    eng.link.send = eng.sent.append
    eng.logs = logs
    return eng


def _connect(eng, minor=2, caps=0x0F):
    eng.link.state = link_mod.CONNECTED
    eng.link.hello = make_hello(minor, caps)


# -------------------------------------------------- sterownik vJoy w tle

class Driver:
    """Sterownik vJoy: stan i przelaczanie, ktore mozna wstrzymac (UAC)."""

    def __init__(self, enabled, answer=True):
        self.enabled, self.answer = enabled, answer
        self.calls = []
        self.threads = set()
        self.gate = threading.Event()
        self.gate.set()

    def is_enabled(self):
        return self.enabled

    def switch(self, enabled, timeout_s=20.0, hwnd=0):
        self.threads.add(threading.current_thread().name)
        self.calls.append(enabled)
        self.gate.wait(5)
        if not self.answer:
            return False, "brak zgody administratora (okno UAC zamknięte)"
        self.enabled = enabled
        return True, "ok"


@pytest.fixture
def driver(fake_outputs, monkeypatch):
    def install(enabled, answer=True):
        d = Driver(enabled, answer)
        monkeypatch.setattr(fake_outputs, "vjoy_driver_enabled", d.is_enabled)
        monkeypatch.setattr(fake_outputs, "set_vjoy_driver", d.switch)
        return d
    return install


def test_switch_runs_in_background(driver):
    d = driver(enabled=True)
    d.gate.clear()                          # okno UAC "wisi"
    eng = _engine("g29")
    t0 = time.monotonic()
    assert eng.open_output() is True
    assert time.monotonic() - t0 < 0.5      # okno panelu nie czeka
    assert wait_for(lambda: d.calls == [False])
    assert eng.status.vjoy_switching
    d.gate.set()
    assert wait_for(lambda: not eng.status.vjoy_switching)
    assert d.enabled is False and d.threads == {"vJoySwitch"}
    assert FakeWheel.opened == []


def test_wheel_opens_after_driver_is_enabled(driver):
    d = driver(enabled=False)
    d.gate.clear()
    eng = _engine("wheel")
    eng.open_output()
    assert FakeWheel.opened == [] and eng.output_error == ""
    d.gate.set()
    assert wait_for(lambda: isinstance(eng.output, FakeWheel))
    assert wait_for(lambda: not eng.status.vjoy_switching)
    assert FakeWheel.opened == [1] and d.calls == [True]


def test_mode_change_during_uac_is_not_lost(driver):
    d = driver(enabled=True)
    d.gate.clear()
    eng = _engine("g29")
    eng.open_output()
    assert wait_for(lambda: d.calls == [False])
    eng.cfg.output.mode = "wheel"           # zmiana trybu w trakcie pytania
    eng.open_output()
    d.gate.set()
    assert wait_for(lambda: not eng.status.vjoy_switching)
    assert d.calls == [False, True] and d.enabled
    assert isinstance(eng.output, FakeWheel) and FakeWheel.opened == [1]


def test_refusal_remembered_until_ask_again(driver):
    d = driver(enabled=True, answer=False)
    eng = _engine("g29")
    eng.open_output()
    assert wait_for(lambda: d.calls == [False] and not eng.status.vjoy_switching)
    assert "zgody" in eng.status.vjoy_switch_error
    eng.open_output()                       # zwykla zmiana trybu - bez pytania
    time.sleep(0.1)
    assert d.calls == [False]
    eng.open_output(ask_again=True)         # "Podlacz ponownie"
    assert wait_for(lambda: d.calls == [False, False])


def test_closing_cancels_deferred_open(driver):
    d = driver(enabled=False)
    d.gate.clear()
    eng = _engine("wheel")
    eng.open_output()
    eng.close_output()                      # np. zamkniecie panelu
    d.gate.set()
    assert wait_for(lambda: not eng.status.vjoy_switching)
    assert FakeWheel.opened == []


def test_hide_unused_off_leaves_vjoy_alone(driver):
    d = driver(enabled=True)
    eng = _engine("g29")
    eng.cfg.output.hide_unused = False
    eng.open_output()
    time.sleep(0.05)
    assert d.calls == []


# ------------------------------------------------------- blokada wyjsc

def test_pad_never_used_while_closing(fake_outputs):
    eng = _engine("both")
    eng.open_output()
    stop = threading.Event()

    def frames():
        i = 0
        while not stop.is_set():
            eng._handle_state(make_frame(counts=i % 50, seq=i))
            i += 1

    t = threading.Thread(target=frames)
    t.start()
    try:
        for _ in range(100):
            eng.open_output()
    finally:
        stop.set()
        t.join()
    errors = [m for lvl, m in eng.logs if lvl == "error"]
    assert not errors, errors[:3]


def test_apply_config_reopens_only_on_mode_change(fake_outputs):
    eng = _engine("wheel")
    eng.apply_config()
    assert FakeWheel.opened == []           # nie otwarte - nie otwiera samo
    eng.open_output()
    eng.apply_config()
    eng.apply_config()                      # np. ruch suwaka zakresu
    assert FakeWheel.opened == [1]
    eng.cfg.output.vjoy_device = 2
    eng.apply_config()
    assert FakeWheel.opened == [1, 2]


def test_failed_vjoy_does_not_recreate_pad(fake_outputs, monkeypatch):
    """Tryb "Oba naraz" bez vJoy: ruch suwaka nie moze tworzyc pada od nowa."""
    pads = []
    original = fake_outputs.XInputBackend

    class CountingPad(original):
        def open(self):
            pads.append(self)
            super().open()

    class BrokenWheel(FakeWheel):
        def open(self):
            raise fake_outputs.OutputError("brak vJoy")

    monkeypatch.setattr(fake_outputs, "XInputBackend", CountingPad)
    monkeypatch.setattr(fake_outputs, "VJoyBackend", BrokenWheel)
    eng = _engine("both")
    eng.open_output()
    for _ in range(5):
        eng.apply_config()
    assert len(pads) == 1
    assert "brak vJoy" in eng.output_error


# ---------------------------------------------------------------- srodek

def test_center_waits_out_frames_with_old_zero(fake_outputs):
    eng = _engine("g29")
    _connect(eng)
    eng.cfg.steer.counts_per_rev = 3600
    for _ in range(3):
        eng._handle_state(make_frame(counts=5000))
    eng.center_wheel()
    assert protocol.cmd_center() in eng.sent
    eng._handle_state(make_frame(counts=5000))      # jeszcze stary srodek
    eng._handle_state(make_frame(counts=0))
    assert eng.last_state.steer_velocity == 0.0     # bez zrywu tlumienia FFB
    time.sleep(CENTER_SETTLE_S + 0.02)
    for c in (0, 300, -300):
        eng._handle_state(make_frame(counts=c))
    assert (eng.raw_counts_min, eng.raw_counts_max) == (-300, 300)


def test_center_and_reset_need_link(fake_outputs):
    eng = _engine("g29")
    eng.center_wheel()
    eng.reset_board()
    assert eng.sent == []
    assert sum(lvl == "warn" for lvl, _ in eng.logs) == 2


# --------------------------------------------------------- G29 na USB

def test_g29_usb_follows_mode(fake_outputs):
    eng = _engine("wheel")
    _connect(eng)
    eng._handle_state(make_frame(flags=0x80 | 0x20))    # G29 podlaczona
    assert protocol.cmd_g29_usb(False) in eng.sent
    eng.sent.clear()
    eng.cfg.output.mode = "g29"
    eng.open_output()
    assert protocol.cmd_g29_usb(True) in eng.sent
    eng._handle_state(make_frame(flags=0x00))
    assert eng.status.g29_attached is False
    eng._handle_state(make_frame(flags=0x80))
    assert eng.status.g29_attached is True
    assert time.monotonic() - eng.status.g29_attached_since < 1.0


def test_old_firmware_gets_no_hide_command(fake_outputs):
    eng = _engine("wheel")
    _connect(eng, minor=1, caps=0x07)
    eng._handle_state(make_frame(flags=0x20))
    assert not any(f[2] == protocol.CMD_G29_USB for f in eng.sent)
    assert any("nie umie ukryć" in m for _, m in eng.logs)


# ------------------------------------------------------------------ FFB

def test_ffb_follows_new_vjoy_device():
    from wheelbridge.ffb import FfbManager

    class Api:
        has_ffb_api = True

        class dll:
            @staticmethod
            def FfbRegisterGenCB(cb, user):
                pass

    m = FfbManager()
    assert m.register(Api(), 1) and m.register(Api(), 2)
    assert m._device_id == 2


def test_direction_mode_from_config():
    cfg = AppConfig()
    cfg.ffb.direction_mode = "sin"
    assert WheelEngine(cfg).ffb.direction_mode == "sin"
