"""Przetwarzanie: krzywe, raport G29, raport pada."""

import pytest
from conftest import make_frame

from wheelbridge.config import AppConfig, G29_CONTROLS, PEDAL_KEYS
from wheelbridge.processing import (StateProcessor, build_g29_report,
                                    build_pad_state, g29_active, pedal_curve,
                                    steer_curve)


def test_g29_neutral_report_matches_firmware():
    cfg = AppConfig()
    for key in PEDAL_KEYS:
        cfg.pedals[key].source = -1
    st = StateProcessor().process(make_frame(), cfg)
    # G29_NEUTRAL z firmware/kierownica/g29_usb.h
    assert build_g29_report(st, cfg) == bytes(
        (0x08, 0, 0, 0, 0x00, 0x80, 255, 255, 255, 0x80, 0x80, 0))


def test_g29_buttons_and_dpad_roundtrip():
    cfg = AppConfig()
    cfg.g29.buttons = {n: -1 for n in G29_CONTROLS}
    cfg.g29.buttons.update(CROSS=3, DPAD_UP=4, DPAD_RIGHT=5)
    st = StateProcessor().process(
        make_frame(buttons=(1 << 3) | (1 << 4) | (1 << 5)), cfg)
    act = g29_active(build_g29_report(st, cfg))
    assert act["CROSS"] and act["DPAD_UP"] and act["DPAD_RIGHT"]
    assert not act["DPAD_DOWN"] and not act["CIRCLE"]


def test_curves():
    p = AppConfig().pedals["throttle"]
    p.deadzone_low, p.deadzone_high, p.gamma = 0.1, 0.1, 1.0
    assert pedal_curve(0.05, p) == 0.0
    assert pedal_curve(0.95, p) == 1.0
    assert pedal_curve(0.5, p) == pytest.approx(0.5)
    assert steer_curve(-0.5, 2.0) == pytest.approx(-0.25)


def test_pad_default_steering_on_left_stick():
    cfg = AppConfig()
    st = StateProcessor().process(make_frame(counts=600), cfg)
    pad = build_pad_state(st, cfg)
    assert pad.lx > 0 and pad.ly == 0
