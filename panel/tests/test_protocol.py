"""Protokol KWP v1 - musi zgadzac sie z firmware/kierownica/kierownica.ino."""

import struct

from conftest import hello_payload

from wheelbridge import protocol


def test_crc8_like_firmware():
    # CRC-8/ATM: wartosc kontrolna "123456789" to 0xF4
    assert protocol.crc8(b"123456789") == 0xF4


def test_frames_survive_garbage_between_them():
    p = protocol.FrameParser()
    f1 = protocol.cmd_g29(bytes(range(12)))
    f2 = protocol.cmd_g29_usb(False)
    out = p.feed(b"\x00\xAA\x13\xAA" + f1 + b"\xAA\x13" + f2)
    assert [t for t, _ in out] == [protocol.CMD_G29, protocol.CMD_G29_USB]
    assert dict(out)[protocol.CMD_G29_USB] == b"\x00"
    assert p.crc_errors == 0


def test_corrupted_frame_is_dropped():
    p = protocol.FrameParser()
    bad = bytearray(protocol.cmd_center())
    bad[-1] ^= 0xFF
    assert p.feed(bytes(bad)) == []
    assert p.crc_errors == 1
    assert p.feed(protocol.cmd_center()) == [(protocol.CMD_CENTER, b"")]


def test_state_flags():
    payload = struct.pack(protocol.STATE_FMT, 7, 1000, -123, 1, 2, 3, 4, 5, 6,
                          0b101, 0, 0x80 | 0x20 | 0x40)
    st = protocol.decode_state(payload)
    assert st.g29_attached and st.g29_usb and st.g29_fed
    assert st.steer_counts == -123 and st.axes == (1, 2, 3, 4, 5, 6)


def test_hello_caps_and_version():
    h = protocol.decode_hello(hello_payload(minor=2, caps=0x0F))
    assert h.has_g29 and h.can_hide_g29 and h.firmware == "2.2"
    assert not h.outdated
    old = protocol.decode_hello(hello_payload(minor=1, caps=0x07))
    assert old.has_g29 and not old.can_hide_g29 and old.outdated
    assert protocol.FW_LATEST_TEXT == "{}.{}".format(*protocol.FW_LATEST)
