"""Cala sciezka na prawdziwych watkach: SerialLink + silnik + udawana plytka.

Plytka odpowiada na IDENTIFY, wysyla STATE ~333 Hz, a na CMD_G29_USB
odlacza i podlacza G29 i pokazuje to we flagach - jak firmware 2.2.
"""

import struct
import threading
import time

import pytest
from conftest import hello_payload, wait_for

from wheelbridge import link as link_mod
from wheelbridge import protocol
from wheelbridge.config import AppConfig
from wheelbridge.engine import WheelEngine


class FakeBoard:
    """Zamiast serial.Serial - zachowuje sie jak firmware na drugim koncu."""

    def __init__(self):
        self.port = None
        self.baudrate = 0
        self.timeout = 0.05
        self.write_timeout = 0.3
        self.dtr = self.rts = False
        self._rx = bytearray()
        self._parser = protocol.FrameParser()
        self._lock = threading.Lock()
        self.cmds = []
        self.attached = True
        self.g29_reports = 0
        self.t0 = time.monotonic()
        self.states = 0

    def open(self):
        pass

    def close(self):
        pass

    def reset_input_buffer(self):
        pass

    def reset_output_buffer(self):
        pass

    def _emit(self, mtype, payload):
        with self._lock:
            self._rx += protocol.build_frame(mtype, payload)

    def write(self, data):
        for t, p in self._parser.feed(bytes(data)):
            self.cmds.append(t)
            if t == protocol.CMD_IDENTIFY:
                self._emit(protocol.MSG_HELLO, hello_payload())
            elif t == protocol.CMD_G29_USB:
                self.attached = bool(p[0])
                self._emit(protocol.MSG_ACK, bytes((t, 1)))
            elif t == protocol.CMD_PING:
                self._emit(protocol.MSG_PONG, p[:4] + struct.pack("<I", 0))
            elif t == protocol.CMD_G29:
                self.g29_reports += 1
        return len(data)

    def _pump(self):
        due = int((time.monotonic() - self.t0) * 333)
        while self.states < due:
            flags = 0x40 | ((0x80 | 0x20) if self.attached else 0)
            self._emit(protocol.MSG_STATE, struct.pack(
                protocol.STATE_FMT, self.states & 0xFFFF, self.states * 3,
                1000, 100, 200, 300, 400, 500, 600, 0b1, 0, flags))
            self.states += 1

    @property
    def in_waiting(self):
        self._pump()
        return len(self._rx)

    def read(self, n=1):
        self._pump()
        with self._lock:
            out = bytes(self._rx[:n])
            del self._rx[:n]
        if not out:
            time.sleep(0.002)
        return out


@pytest.fixture
def board(monkeypatch, fake_outputs):
    boards = []

    def factory():
        boards.append(FakeBoard())
        return boards[-1]

    monkeypatch.setattr(link_mod.serial, "Serial", factory)
    monkeypatch.setattr(link_mod, "list_ports",
                        lambda: [link_mod.PortInfo("COM9", "Arduino Due")])
    return boards


def test_full_path_and_mode_switching(board):
    cfg = AppConfig()
    cfg.output.mode = "wheel"           # tryb bez G29 - plytka ma ja odlaczyc
    logs = []
    eng = WheelEngine(cfg, on_log=lambda lvl, m: logs.append((lvl, m)))
    eng.start()
    try:
        assert wait_for(lambda: eng.link.is_connected)
        b = board[-1]
        assert wait_for(lambda: protocol.CMD_SET_RATE in b.cmds
                        and protocol.CMD_FFB_CONFIG in b.cmds)
        assert wait_for(lambda: not b.attached and not eng.status.g29_attached)
        assert b.g29_reports == 0

        eng.cfg.output.mode = "g29"
        eng.open_output()
        assert wait_for(lambda: b.attached and eng.status.g29_attached)
        assert wait_for(lambda: b.g29_reports > 10)

        for mode in ("pad", "g29", "both", "g29_vjoy", "g29"):
            eng.cfg.output.mode = mode
            eng.open_output()
        assert wait_for(lambda: b.attached)
        assert eng.link.stats.crc_errors == 0
    finally:
        eng.stop()
    assert not [m for lvl, m in logs if lvl == "error"]
