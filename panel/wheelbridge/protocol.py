"""
Protokol KWP v1 - warstwa ramek miedzy Arduino Due a aplikacja.

MUSI byc zgodny z firmware/kierownica/kierownica.ino.

Ramka:
    0xAA 0x55 | type (1B) | len (1B) | payload (len B) | CRC8 (1B)

CRC8 liczony jest po [type, len, payload] - polinom 0x07, init 0x00
(CRC-8/ATM), identycznie jak tablica CRC8_TABLE w firmware.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import List, Tuple

PROTO_VERSION = 1

SYNC1 = 0xAA
SYNC2 = 0x55
MAX_PAYLOAD = 64

# --- plytka -> PC -----------------------------------------------------------
MSG_HELLO = 0x01
MSG_STATE = 0x02
MSG_LOG = 0x03
MSG_PONG = 0x04
MSG_ACK = 0x05
MSG_G29_OUT = 0x06      # raport wyjsciowy, ktory gra wyslala do G29

# --- PC -> plytka -----------------------------------------------------------
CMD_PING = 0x10
CMD_IDENTIFY = 0x11
CMD_CENTER = 0x12
CMD_SET_ENDSTOP = 0x13
CMD_SET_RATE = 0x14
CMD_FFB = 0x15
CMD_FFB_CONFIG = 0x16
CMD_RESET = 0x17
CMD_G29 = 0x18          # gotowy 12-bajtowy raport G29 dla portu Native USB

MSG_NAMES = {
    MSG_HELLO: "HELLO",
    MSG_STATE: "STATE",
    MSG_LOG: "LOG",
    MSG_PONG: "PONG",
    MSG_ACK: "ACK",
    MSG_G29_OUT: "G29_OUT",
    CMD_PING: "PING",
    CMD_IDENTIFY: "IDENTIFY",
    CMD_CENTER: "CENTER",
    CMD_SET_ENDSTOP: "SET_ENDSTOP",
    CMD_SET_RATE: "SET_RATE",
    CMD_FFB: "FFB",
    CMD_FFB_CONFIG: "FFB_CONFIG",
    CMD_RESET: "RESET",
    CMD_G29: "G29",
}

CAP_FFB = 0x01
CAP_NATIVE_USB = 0x02
CAP_G29 = 0x04          # firmware 2.1+: Native USB udaje kierownice G29

# Flagi w ramce STATE
FLAG_FFB_ENABLED = 0x01
FLAG_FFB_ACTIVE = 0x02
FLAG_ENCODER_ERROR = 0x04
FLAG_ENDSTOP_HIT = 0x08
FLAG_FFB_WATCHDOG = 0x10
FLAG_G29_USB = 0x20     # Windows wyliczyl port Native USB jako G29
FLAG_G29_FED = 0x40     # raporty G29 z aplikacji docieraja do plytki

#: Dlugosc raportu wejsciowego G29 (bez numeru raportu) - patrz g29_usb.h.
G29_REPORT_SIZE = 12

NUM_AXES = 6
NUM_BUTTONS = 32

#: Domyslny baudrate. 250000 dzieli sie calkowicie przez zegary SAM3X
#: (84 MHz / 16 / 21) i ATmega16U2 (16 MHz / 8 / 8) - zero bledu zegara.
DEFAULT_BAUD = 250000

STATE_FMT = "<HIi6HIBB"
STATE_SIZE = struct.calcsize(STATE_FMT)  # 28
HELLO_FMT = "<BBBBBBHI"
HELLO_SIZE = struct.calcsize(HELLO_FMT)  # 12
PONG_FMT = "<II"


# --------------------------------------------------------------------- CRC8

def _build_crc8_table() -> bytes:
    table = bytearray(256)
    for i in range(256):
        c = i
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if (c & 0x80) else ((c << 1) & 0xFF)
        table[i] = c
    return bytes(table)


CRC8_TABLE = _build_crc8_table()


def crc8(data: bytes) -> int:
    c = 0x00
    for b in data:
        c = CRC8_TABLE[c ^ b]
    return c


# ------------------------------------------------------------------- ramki

def build_frame(msg_type: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} B > {MAX_PAYLOAD} B")
    head = bytes((msg_type, len(payload)))
    return bytes((SYNC1, SYNC2)) + head + payload + bytes((crc8(head + payload),))


class FrameParser:
    """Parser strumieniowy, odporny na utrate bajtow - sam sie resynchronizuje."""

    _WAIT_SYNC1, _WAIT_SYNC2, _TYPE, _LEN, _PAYLOAD, _CRC = range(6)

    def __init__(self) -> None:
        self._st = self._WAIT_SYNC1
        self._type = 0
        self._len = 0
        self._buf = bytearray()
        self.crc_errors = 0
        self.resyncs = 0
        self.frames_ok = 0

    def reset(self) -> None:
        self._st = self._WAIT_SYNC1
        self._buf.clear()

    def feed(self, data: bytes) -> List[Tuple[int, bytes]]:
        out: List[Tuple[int, bytes]] = []
        for b in data:
            st = self._st
            if st == self._WAIT_SYNC1:
                if b == SYNC1:
                    self._st = self._WAIT_SYNC2
            elif st == self._WAIT_SYNC2:
                if b == SYNC2:
                    self._st = self._TYPE
                elif b == SYNC1:
                    pass  # zostajemy - 0xAA 0xAA 0x55 tez jest poprawne
                else:
                    self._st = self._WAIT_SYNC1
                    self.resyncs += 1
            elif st == self._TYPE:
                self._type = b
                self._st = self._LEN
            elif st == self._LEN:
                if b > MAX_PAYLOAD:
                    self._st = self._WAIT_SYNC1
                    self.resyncs += 1
                    continue
                self._len = b
                self._buf.clear()
                self._st = self._CRC if b == 0 else self._PAYLOAD
            elif st == self._PAYLOAD:
                self._buf.append(b)
                if len(self._buf) >= self._len:
                    self._st = self._CRC
            else:  # _CRC
                head = bytes((self._type, self._len))
                if crc8(head + bytes(self._buf)) == b:
                    out.append((self._type, bytes(self._buf)))
                    self.frames_ok += 1
                else:
                    self.crc_errors += 1
                self._st = self._WAIT_SYNC1
        return out


# ---------------------------------------------------------------- dekodery

@dataclass(frozen=True)
class StateFrame:
    seq: int
    device_ms: int
    steer_counts: int
    axes: Tuple[int, ...]
    buttons: int
    hat: int
    flags: int
    host_time: float = 0.0

    @property
    def encoder_error(self) -> bool:
        return bool(self.flags & FLAG_ENCODER_ERROR)

    @property
    def endstop_hit(self) -> bool:
        return bool(self.flags & FLAG_ENDSTOP_HIT)

    @property
    def ffb_enabled(self) -> bool:
        return bool(self.flags & FLAG_FFB_ENABLED)

    @property
    def ffb_watchdog(self) -> bool:
        return bool(self.flags & FLAG_FFB_WATCHDOG)

    @property
    def g29_usb(self) -> bool:
        return bool(self.flags & FLAG_G29_USB)

    @property
    def g29_fed(self) -> bool:
        return bool(self.flags & FLAG_G29_FED)


@dataclass(frozen=True)
class HelloInfo:
    proto: int
    fw_major: int
    fw_minor: int
    caps: int
    num_buttons: int
    num_axes: int
    report_hz: int
    endstop_counts: int
    name: str

    @property
    def firmware(self) -> str:
        return f"{self.fw_major}.{self.fw_minor}"

    @property
    def has_ffb(self) -> bool:
        return bool(self.caps & CAP_FFB)

    @property
    def native_usb(self) -> bool:
        return bool(self.caps & CAP_NATIVE_USB)

    @property
    def has_g29(self) -> bool:
        return bool(self.caps & CAP_G29)


def decode_state(payload: bytes, host_time: float = 0.0) -> StateFrame:
    if len(payload) < STATE_SIZE:
        raise ValueError(f"STATE: {len(payload)} B, oczekiwano {STATE_SIZE} B")
    v = struct.unpack(STATE_FMT, payload[:STATE_SIZE])
    return StateFrame(
        seq=v[0],
        device_ms=v[1],
        steer_counts=v[2],
        axes=tuple(v[3:9]),
        buttons=v[9],
        hat=v[10],
        flags=v[11],
        host_time=host_time,
    )


def decode_hello(payload: bytes) -> HelloInfo:
    if len(payload) < HELLO_SIZE:
        raise ValueError(f"HELLO: {len(payload)} B, oczekiwano >= {HELLO_SIZE} B")
    v = struct.unpack(HELLO_FMT, payload[:HELLO_SIZE])
    name = payload[HELLO_SIZE:].decode("ascii", errors="replace")
    return HelloInfo(
        proto=v[0], fw_major=v[1], fw_minor=v[2], caps=v[3],
        num_buttons=v[4], num_axes=v[5], report_hz=v[6],
        endstop_counts=v[7], name=name,
    )


def decode_pong(payload: bytes) -> Tuple[int, int]:
    if len(payload) < 8:
        raise ValueError("PONG za krotki")
    token, device_ms = struct.unpack(PONG_FMT, payload[:8])
    return token, device_ms


# ---------------------------------------------------------------- koderzy

def cmd_ping(token: int) -> bytes:
    return build_frame(CMD_PING, struct.pack("<I", token & 0xFFFFFFFF))


def cmd_identify() -> bytes:
    return build_frame(CMD_IDENTIFY)


def cmd_center() -> bytes:
    return build_frame(CMD_CENTER)


def cmd_reset() -> bytes:
    return build_frame(CMD_RESET)


def cmd_set_endstop(counts: int, gain: int = 8) -> bytes:
    counts = max(0, min(2_000_000_000, int(counts)))
    gain = max(1, min(65535, int(gain)))
    return build_frame(CMD_SET_ENDSTOP, struct.pack("<iH", counts, gain))


def cmd_set_rate(hz: int) -> bytes:
    return build_frame(CMD_SET_RATE, struct.pack("<H", max(50, min(500, int(hz)))))


def cmd_g29(report: bytes) -> bytes:
    """Raport G29 do wypchniecia przez Native USB - dokladnie 12 bajtow."""
    if len(report) != G29_REPORT_SIZE:
        raise ValueError(f"raport G29 ma {len(report)} B, musi miec "
                         f"{G29_REPORT_SIZE} B")
    return build_frame(CMD_G29, bytes(report))


def cmd_ffb(torque: int) -> bytes:
    return build_frame(CMD_FFB, struct.pack("<h", max(-32767, min(32767, int(torque)))))


def cmd_ffb_config(enabled: bool, min_duty: int, max_duty: int, invert: bool,
                   deadband: int, timeout_ms: int, slew_per_ms: int) -> bytes:
    """slew_per_ms jest wysylany jako krotnosc 256 (1 bajt)."""
    return build_frame(CMD_FFB_CONFIG, struct.pack(
        "<BBBBHHB",
        1 if enabled else 0,
        max(0, min(255, int(min_duty))),
        max(0, min(255, int(max_duty))),
        1 if invert else 0,
        max(0, min(65535, int(deadband))),
        max(20, min(65535, int(timeout_ms))),
        max(0, min(255, int(slew_per_ms) // 256)),
    ))
