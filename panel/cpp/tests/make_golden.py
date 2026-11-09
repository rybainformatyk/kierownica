#!/usr/bin/env python3
"""
Wzorcowe wyniki wersji Python dla testu zgodnosci wersji C++.

    py -3 -B tests/make_golden.py golden.json
    bin/tests/core_tests.exe golden.json

Ramki protokolu, wczytywanie (takze uszkodzonych) config.json i cala droga
od ramki STATE do raportow G29 i pada musza dawac identyczne wyniki.
"""

import json
import random
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))

from wheelbridge import protocol as P                          # noqa: E402
from wheelbridge.config import PEDAL_KEYS, AppConfig           # noqa: E402
from wheelbridge.processing import (StateProcessor, build_g29_report,  # noqa: E402
                                    build_pad_state)
from wheelbridge.protocol import StateFrame                    # noqa: E402


def frames(rng):
    out = []

    def add(cmd, args, expect):
        out.append({"id": f"{cmd}-{len(out)}", "cmd": cmd, "args": args,
                    "bytes": [], "expect": list(expect)})

    edge = [-(2 ** 31), -70000, -32768, -1, 0, 1, 49, 50, 255, 256, 500, 501,
            32767, 32768, 65535, 65536, 2_000_000_000, 2 ** 31 - 1]
    for v in edge:
        add("rate", [v], P.cmd_set_rate(v))
        add("ffb", [v], P.cmd_ffb(v))
        add("endstop", [v, 8], P.cmd_set_endstop(v, 8))
        add("endstop", [1000, v], P.cmd_set_endstop(1000, v))
        add("ping", [v & 0xFFFFFFFF], P.cmd_ping(v))
    for b in (True, False):
        add("usb", [b], P.cmd_g29_usb(b))
    for _ in range(300):
        a = [rng.random() < 0.5, rng.randint(-10, 300), rng.randint(-10, 300),
             rng.random() < 0.5, rng.randint(-10, 70000), rng.randint(0, 70000),
             rng.randint(-1000, 70000)]
        add("ffbcfg", a, P.cmd_ffb_config(*a))
    return out


CONFIG_CASES = [
    "{}",
    '{"version": 1}',
    '{"steer": null, "ffb": [], "link": 5, "output": "x", "pad": false, "g29": 1, "ui": null}',
    '{"steer": {"counts_per_rev": 1, "range_deg": 99999, "smoothing": 5, "linearity": 0, "endstop_gain": 0}}',
    '{"steer": {"counts_per_rev": "1000", "range_deg": 720.9, "smoothing": "0.5", "vjoy_axis": "Q", "center_counts": "12"}}',
    '{"steer": {"counts_per_rev": null, "range_deg": [], "smoothing": {}, "linearity": "x", "endstop_gain": true}}',
    '{"steer": {"smoothing": NaN, "linearity": Infinity, "range_deg": -Infinity}}',
    '{"pedals": {"throttle": {"source": 9, "raw_min": 4000, "raw_max": 10}}}',
    '{"pedals": {"brake": 5, "clutch": null}}',
    '{"pedals": {"aux1": {"raw_min": 100, "raw_max": 110}, "aux2": {"source": -1, "vjoy_axis": "nope"}}}',
    '{"pedals": {"aux3": {"source": "x", "deadzone_low": 2, "deadzone_high": -1, "gamma": 99, "smoothing": 0.99}}}',
    '{"pedals": []}',
    '{"ffb": {"overall_gain": 2, "min_force": 1, "max_force": 0, "deadband": 1, "slew_per_ms": 1, "timeout_ms": 1, "min_duty": 300, "max_duty": 1, "update_hz": 1}}',
    '{"ffb": {"overall_gain": "abc", "slew_per_ms": "x", "spring_gain": 5, "game_constant_gain": 7, "direction_mode": "zle"}}',
    '{"buttons": {"mapping": [3, "x", 999, -5, 2.7], "hat": [1, 99, "z"], "invert": [1, 0, "a", ""]}}',
    '{"buttons": {"mapping": "x", "hat": null, "invert": {}}}',
    '{"buttons": {"mapping": [' + ",".join(["7"] * 40) + ']}}',
    '{"pad": {"steer_range_deg": 1, "steer_deadzone": 1, "steer_antideadzone": 2, "trigger_antideadzone": -1, "steer_linearity": 9, "profile": "zly"}}',
    '{"pad": {"profile": "forza"}}',
    '{"pad": {"axes": {"LX-": "throttle", "LX+": "bzdura", "RT": 5}, "buttons": {"A": 40, "B": "3", "C": 1}}}',
    '{"pad": {"axes": 5, "buttons": 7}}',
    '{"pad": {"steer_target": "RX", "throttle_target": "LY", "brake_target": "LT", "clutch_target": "-"}}',
    '{"pad": {"steer_target": "RT", "aux1_target": "RY"}}',
    '{"pad": {"steer_target": "zly"}}',
    '{"pad": {"buttons": {"A": 17, "GUIDE": 3, "LB": 31}}}',
    '{"g29": {"pedals_inverted": 0}}',
    '{"g29": {"buttons": {"CROSS": 5, "PS": 99, "GEAR_R": "2"}}}',
    '{"g29": {"buttons": null}}',
    '{"output": {"backend": "vjoy"}}',
    '{"output": {"backend": "none"}}',
    '{"output": {"backend": "none", "mode": "pad"}}',
    '{"output": {"mode": "zly", "vjoy_device": 99}}',
    '{"output": {"mode": 5, "vjoy_device": "3"}}',
    '{"link": {"port": 5, "baudrate": 1, "report_hz": 100000}}',
    '{"link": {"port": "COM7", "baudrate": "115200", "report_hz": 100}}',
    '{"ui": {"geometry": 5, "zoomed": "tak", "tab": 99}}',
    '{"ui": {"geometry": "1000x700+1+2", "zoomed": false, "tab": -3}}',
    '{"steer": {"range_deg": 123456789012345678901234567890}}',
    '{"steer": {"range_deg": 540}, "steer": {"range_deg": 720}}',
    '{"pad": {"steer_linearity": 1e400}}',
    "[1, 2]",
    "5",
    '"tekst"',
    "{ to nie json",
    '{"a": 1,}',
    "",
]


def rand_config(rng):
    raw = {
        "steer": {
            "counts_per_rev": rng.choice([400, 2400, 3600, 7777]),
            "range_deg": rng.choice([180, 270, 540, 900, 1080]),
            "invert": rng.random() < 0.5,
            "smoothing": rng.choice([0.0, 0.0, 0.3, 0.8]),
            "linearity": rng.choice([1.0, 0.5, 1.7, 2.4]),
            "center_counts": rng.randint(-500, 500),
        },
        "pedals": {},
        "buttons": {
            "mapping": [rng.randint(0, 40) for _ in range(32)],
            "hat": [rng.randint(-1, 31) for _ in range(4)],
            "invert": [rng.random() < 0.15 for _ in range(32)],
        },
        "pad": {
            "steer_range_deg": rng.choice([270, 360, 450, 900]),
            "steer_deadzone": rng.choice([0.0, 0.05, 0.3]),
            "steer_antideadzone": rng.choice([0.0, 0.15, 0.4]),
            "trigger_antideadzone": rng.choice([0.0, 0.2]),
            "steer_linearity": rng.choice([1.0, 0.6, 2.0]),
            "steer_invert": rng.random() < 0.3,
            "axes": {d: rng.choice(["", "steer_left", "steer_right", "throttle",
                                    "brake", "clutch", "aux1", "aux2", "aux3"])
                     for d in ("LX-", "LX+", "LY+", "LY-", "RX-", "RX+", "RY+",
                               "RY-", "LT", "RT")},
            "buttons": {n: rng.randint(-1, 31)
                        for n in ("A", "B", "X", "Y", "LB", "RB", "LS", "RS",
                                  "BACK", "START", "GUIDE", "DPAD_UP",
                                  "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT")},
        },
        "g29": {
            "pedals_inverted": rng.random() < 0.7,
            "buttons": {n: rng.randint(-1, 31)
                        for n in ("CROSS", "SQUARE", "CIRCLE", "TRIANGLE",
                                  "PADDLE_R", "PADDLE_L", "R2", "L2", "SHARE",
                                  "OPTIONS", "R3", "L3", "GEAR_1", "GEAR_2",
                                  "GEAR_3", "GEAR_4", "GEAR_5", "GEAR_6",
                                  "GEAR_R", "PLUS", "MINUS", "DIAL_CW",
                                  "DIAL_CCW", "ENTER", "PS", "DPAD_UP",
                                  "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT")},
        },
    }
    for i, key in enumerate(PEDAL_KEYS):
        lo = rng.randint(0, 1500)
        raw["pedals"][key] = {
            "source": rng.choice([-1, i, rng.randint(0, 5)]),
            "raw_min": lo, "raw_max": lo + rng.randint(40, 2500),
            "invert": rng.random() < 0.3,
            "deadzone_low": rng.choice([0.0, 0.02, 0.2, 0.45]),
            "deadzone_high": rng.choice([0.0, 0.02, 0.2, 0.45]),
            "gamma": rng.choice([1.0, 0.5, 1.5, 3.0]),
            "smoothing": rng.choice([0.0, 0.0, 0.5, 0.9]),
        }
    return json.dumps(raw)


def states(rng, tmp):
    out = []
    for c in range(60):
        text = rand_config(rng)
        path = tmp / f"s{c}.json"
        path.write_text(text, encoding="utf-8")
        cfg = AppConfig.load(path)
        proc = StateProcessor()
        fr, res = [], []
        t = 1.0
        counts = rng.randint(-3000, 3000)
        for _ in range(60):
            t += rng.choice([0.002, 0.003, 0.00005, 0.01])
            counts += rng.randint(-400, 400)
            axes = [rng.randint(0, 4095) for _ in range(6)]
            buttons = rng.getrandbits(32) if rng.random() < 0.5 else (1 << rng.randint(0, 31))
            f = StateFrame(seq=0, device_ms=0, steer_counts=counts,
                           axes=tuple(axes), buttons=buttons, hat=0, flags=0)
            s = proc.process(f, cfg, t)
            fr.append({"counts": counts, "buttons": buttons, "axes": axes, "t": t})
            pad = build_pad_state(s, cfg)
            res.append({
                "deg": s.steer_deg, "axis": s.steer_axis, "vel": s.steer_velocity,
                "eff": s.buttons_effective, "hat": s.hat,
                "pedals": [s.pedals[k] for k in PEDAL_KEYS],
                "out": s.out_buttons,
                "g29": list(build_g29_report(s, cfg)),
                "pad": list(pad.as_tuple()),
            })
        out.append({"id": f"cfg{c}", "config": text, "frames": fr, "results": res})
    return out


def main():
    rng = random.Random(20261109)
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        configs = []
        for i, text in enumerate(CONFIG_CASES):
            path = tmp / f"c{i}.json"
            path.write_text(text, encoding="utf-8")
            configs.append({"id": f"case{i}", "input": text,
                            "output": AppConfig.load(path).to_dict()})
        data = {"frames": frames(rng), "configs": configs,
                "states": states(rng, tmp)}
    Path(sys.argv[1]).write_text(json.dumps(data), encoding="utf-8")


if __name__ == "__main__":
    main()
