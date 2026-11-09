"""Wspolne dla testow panelu.

Uruchomienie (z katalogu panel):

    python -m pip install pytest
    python -m pytest tests

Testy nie potrzebuja plytki ani sterownikow: lacze, vJoy i pad ViGEm sa
podmienione, konfiguracja idzie do katalogu tymczasowego, a autostart do
osobnego klucza rejestru - prawdziwe ustawienia zostaja nietkniete.
"""

import struct
import sys
import time
from pathlib import Path

import pytest

PANEL = Path(__file__).resolve().parents[1]
if str(PANEL) not in sys.path:
    sys.path.insert(0, str(PANEL))

from wheelbridge import protocol  # noqa: E402
from wheelbridge.outputs.base import OutputBackend  # noqa: E402


def hello_payload(minor=2, caps=0x0F):
    """Ramka HELLO firmware 2.<minor>; caps 0x0F = FFB, G29, ukrywanie G29."""
    return (struct.pack(protocol.HELLO_FMT, 1, 2, minor, caps, 32, 6, 333, 0)
            + b"KIEROWNICA-DUE")


def make_hello(minor=2, caps=0x0F):
    return protocol.decode_hello(hello_payload(minor, caps))


def make_frame(counts=0, axes=(100,) * 6, buttons=0, flags=0x80 | 0x20,
               host_time=None, seq=0):
    return protocol.StateFrame(seq=seq, device_ms=0, steer_counts=counts,
                               axes=tuple(axes), buttons=buttons, hat=0,
                               flags=flags,
                               host_time=time.monotonic() if host_time is None
                               else host_time)


def wait_for(pred, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.01)
    return False


class FakeWheel(OutputBackend):
    """Udaje VJoyBackend (silnik sprawdza isinstance - test go podmienia)."""
    opened = []

    def __init__(self, device_id=1):
        self.device_id = device_id
        self._open = False
        self.closed = False

    def open(self):
        self._open = True
        FakeWheel.opened.append(self.device_id)

    def close(self):
        self._open = False
        self.closed = True

    @property
    def is_open(self):
        return self._open

    @property
    def ffb_capable(self):
        return False

    def update(self, axes, buttons, hat=-1):
        if self.closed:
            raise AssertionError("update po zamknieciu")

    def center(self, neutral=None):
        if self.closed:
            raise AssertionError("center po zamknieciu")

    def status_text(self):
        return "vJoy (atrapa)"


class FakePad(OutputBackend):
    """Pad ViGEm bez sterownika - wykrywa uzycie w trakcie zamykania."""

    def __init__(self):
        self._open = False
        self.closed = False
        self.slot = None

    def open(self):
        self._open = True

    def close(self):
        time.sleep(0.0005)          # okno na wyscig, jak prawdziwe wywolania DLL
        self._open = False
        self.closed = True

    @property
    def is_open(self):
        return self._open

    def send(self, state, force=False):
        if self.closed:
            raise AssertionError("send po zamknieciu")
        time.sleep(0.0002)
        if self.closed:
            raise AssertionError("send w trakcie zamykania")

    def center(self):
        self.send(None)

    def update(self, *a, **k):
        pass

    def status_text(self):
        return "pad (atrapa)"


@pytest.fixture
def fake_outputs(monkeypatch):
    """vJoy i pad ViGEm jako atrapy - testy nie tworza urzadzen w systemie."""
    from wheelbridge import engine as engine_mod
    FakeWheel.opened = []
    monkeypatch.setattr(engine_mod, "VJoyBackend", FakeWheel)
    monkeypatch.setattr(engine_mod, "XInputBackend", FakePad)
    monkeypatch.setattr(engine_mod, "vjoy_driver_enabled", lambda: None)
    return engine_mod
