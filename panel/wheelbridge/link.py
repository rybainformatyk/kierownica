"""
Warstwa transportowa: port szeregowy w osobnym watku.

Odpowiada za:
  * wyszukanie plytki na dowolnym porcie COM (wysyla IDENTIFY, czeka na HELLO),
  * automatyczne ponowne laczenie po odlaczeniu kabla,
  * statystyki jakosci lacza (ramki/s, bledy CRC, zgubione ramki, opoznienie),
  * nieblokujace wysylanie komend z innych watkow.
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Callable, List, Optional

import serial
import serial.tools.list_ports

from . import protocol
from .protocol import FrameParser, HelloInfo, StateFrame

# Stany lacza
DISCONNECTED = "disconnected"
SCANNING = "scanning"
CONNECTED = "connected"
ERROR = "error"

STATE_LABELS = {
    DISCONNECTED: "Rozlaczono",
    SCANNING: "Szukanie plytki...",
    CONNECTED: "Polaczono",
    ERROR: "Blad",
}

#: Czas oczekiwania na HELLO/STATE przy sprawdzaniu jednego portu.
PROBE_TIMEOUT = 1.2
#: Po tylu sekundach bez ramki uznajemy lacze za zerwane.
STALE_TIMEOUT = 1.5


@dataclass
class LinkStats:
    frames_per_sec: float = 0.0
    bytes_per_sec: float = 0.0
    crc_errors: int = 0
    dropped_frames: int = 0
    resyncs: int = 0
    latency_ms: float = 0.0
    total_frames: int = 0
    uptime: float = 0.0
    port: str = ""
    connected_at: float = 0.0

    def snapshot(self) -> "LinkStats":
        return LinkStats(**self.__dict__)


@dataclass
class PortInfo:
    device: str
    description: str
    hwid: str = ""

    @property
    def label(self) -> str:
        return f"{self.device} - {self.description}" if self.description else self.device


def list_ports() -> List[PortInfo]:
    out: List[PortInfo] = []
    for p in serial.tools.list_ports.comports():
        out.append(PortInfo(device=p.device,
                            description=(p.description or "").strip(),
                            hwid=(p.hwid or "")))
    out.sort(key=lambda x: _port_sort_key(x.device))
    return out


def _port_sort_key(device: str) -> tuple:
    digits = "".join(ch for ch in device if ch.isdigit())
    return (0, int(digits)) if digits else (1, 0)


def _looks_like_arduino(info: PortInfo) -> bool:
    """Heurystyka pierwszenstwa przy skanowaniu - nie wyklucza zadnego portu."""
    blob = f"{info.description} {info.hwid}".lower()
    if any(v in blob for v in ("2341", "1a86", "2a03", "046d")):
        return True
    return any(k in blob for k in ("arduino", "due", "ch340", "usb serial", "usb-serial"))


class SerialLink:
    """Nieblokujace lacze do plytki. Caly ruch obslugiwany w jednym watku."""

    def __init__(self,
                 on_state: Optional[Callable[[StateFrame], None]] = None,
                 on_hello: Optional[Callable[[HelloInfo], None]] = None,
                 on_status: Optional[Callable[[str, str], None]] = None,
                 on_log: Optional[Callable[[str, str], None]] = None,
                 on_g29_out: Optional[Callable[[bytes], None]] = None) -> None:
        self._on_state = on_state
        self._on_hello = on_hello
        self._on_status = on_status
        self._on_log = on_log
        self._on_g29_out = on_g29_out

        self._ser: Optional[serial.Serial] = None
        self._ser_lock = threading.Lock()
        self._parser = FrameParser()

        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._wake = threading.Event()

        self._requested_port: Optional[str] = None   # None = autodetekcja
        self._want_connect = False
        self._baudrate = protocol.DEFAULT_BAUD
        self._report_hz = 333

        self.state: str = DISCONNECTED
        self.hello: Optional[HelloInfo] = None
        self.stats = LinkStats()

        self._pending_tx: List[bytes] = []
        self._tx_lock = threading.Lock()

        # liczniki okna statystyk
        self._win_t = 0.0
        self._win_frames = 0
        self._win_bytes = 0
        self._last_seq: Optional[int] = None
        self._last_frame_t = 0.0
        self._ping_token = 0
        self._ping_sent_t = 0.0
        self._next_ping = 0.0

        self._on_connected_hook: Optional[Callable[[], None]] = None

    # ------------------------------------------------------------ cykl zycia

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="SerialLink", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()
        t = self._thread
        if t and t.is_alive():
            t.join(timeout=2.0)
        self._close()

    def set_on_connected(self, hook: Optional[Callable[[], None]]) -> None:
        """Wywolywane w watku lacza zaraz po otrzymaniu HELLO."""
        self._on_connected_hook = hook

    # ------------------------------------------------------------- sterowanie

    def connect(self, port: Optional[str] = None, baudrate: Optional[int] = None,
                report_hz: Optional[int] = None) -> None:
        self._requested_port = port or None
        if baudrate:
            self._baudrate = int(baudrate)
        if report_hz:
            self._report_hz = int(report_hz)
        self._want_connect = True
        self._wake.set()

    def disconnect(self) -> None:
        self._want_connect = False
        self._wake.set()

    @property
    def is_connected(self) -> bool:
        return self.state == CONNECTED

    def send(self, frame: bytes) -> None:
        """Mozna wolac z dowolnego watku. Przy braku polaczenia dane sa pomijane."""
        with self._ser_lock:
            ser = self._ser
            if ser is None:
                return
            try:
                ser.write(frame)
            except (serial.SerialException, OSError) as exc:
                self._queue_fault(f"Blad zapisu: {exc}")

    def _queue_fault(self, msg: str) -> None:
        with self._tx_lock:
            self._pending_tx.append(b"")      # znacznik: rozlacz
        self._log("error", msg)
        self._wake.set()

    # ---------------------------------------------------------------- petla

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                if not self._want_connect:
                    if self._ser is not None:
                        self._close()
                        self._set_state(DISCONNECTED)
                    self._wake.wait(0.2)
                    self._wake.clear()
                    continue

                if self._ser is None:
                    self._do_connect()
                    if self._ser is None:
                        # nie udalo sie - czekamy chwile i probujemy ponownie
                        self._wake.wait(1.5)
                        self._wake.clear()
                    continue

                self._pump()
            except Exception as exc:                      # ostatnia linia obrony
                self._log("error", f"Wyjatek w watku lacza: {exc!r}")
                self._close()
                self._set_state(ERROR, str(exc))
                time.sleep(1.0)

    # ------------------------------------------------------------ laczenie

    def _do_connect(self) -> None:
        if self._requested_port:
            candidates = [PortInfo(self._requested_port, "")]
        else:
            all_ports = list_ports()
            # najpierw to, co wyglada na Arduino
            candidates = ([p for p in all_ports if _looks_like_arduino(p)] +
                          [p for p in all_ports if not _looks_like_arduino(p)])
            if not candidates:
                self._set_state(SCANNING, "Brak portow COM")
                return

        self._set_state(SCANNING, f"Sprawdzanie {len(candidates)} portow")

        for info in candidates:
            if self._stop.is_set() or not self._want_connect:
                return
            self._log("info", f"Sprawdzam {info.device}...")
            ser = self._open_port(info.device)
            if ser is None:
                continue
            hello = self._probe(ser)
            if hello is None:
                try:
                    ser.close()
                except Exception:
                    pass
                continue

            # sukces
            with self._ser_lock:
                self._ser = ser
            self.hello = hello
            self._parser.reset()
            self._reset_stats(info.device)
            self._set_state(CONNECTED, info.device)
            self._log("ok", f"{info.device}: {hello.name} fw {hello.firmware}, "
                            f"{hello.num_buttons} przyciskow, {hello.num_axes} osi")
            if hello.proto != protocol.PROTO_VERSION:
                self._log("warn", f"Wersja protokolu plytki ({hello.proto}) rozni sie "
                                  f"od aplikacji ({protocol.PROTO_VERSION}) - "
                                  f"zaktualizuj firmware")
            if self._on_hello:
                self._safe(self._on_hello, hello)
            if self._on_connected_hook:
                self._safe_call(self._on_connected_hook)
            return

        self._set_state(SCANNING, "Nie znaleziono plytki")

    def _open_port(self, device: str) -> Optional[serial.Serial]:
        try:
            ser = serial.Serial()
            ser.port = device
            ser.baudrate = self._baudrate
            ser.timeout = 0.05
            ser.write_timeout = 0.3
            # DTR/RTS w stanie niskim - inaczej otwarcie portu resetuje Due
            ser.dtr = False
            ser.rts = False
            ser.open()
            try:
                ser.reset_input_buffer()
                ser.reset_output_buffer()
            except Exception:
                pass
            return ser
        except (serial.SerialException, OSError, ValueError) as exc:
            self._log("warn", f"{device}: {exc}")
            return None

    def _probe(self, ser: serial.Serial) -> Optional[HelloInfo]:
        """Pyta plytke kim jest. Akceptuje HELLO albo (po STATE) wymusza HELLO."""
        parser = FrameParser()
        deadline = time.monotonic() + PROBE_TIMEOUT
        asked_again = False
        try:
            ser.write(protocol.cmd_identify())
        except (serial.SerialException, OSError):
            return None

        while time.monotonic() < deadline:
            if self._stop.is_set():
                return None
            try:
                chunk = ser.read(512)
            except (serial.SerialException, OSError):
                return None
            if not chunk:
                continue
            for mtype, payload in parser.feed(chunk):
                if mtype == protocol.MSG_HELLO:
                    try:
                        return protocol.decode_hello(payload)
                    except ValueError:
                        return None
                if mtype == protocol.MSG_STATE and not asked_again:
                    # plytka gada naszym protokolem, ale HELLO wyslala przed
                    # otwarciem portu - prosimy jeszcze raz
                    asked_again = True
                    deadline = time.monotonic() + PROBE_TIMEOUT
                    try:
                        ser.write(protocol.cmd_identify())
                    except (serial.SerialException, OSError):
                        return None
        return None

    def _close(self) -> None:
        with self._ser_lock:
            ser, self._ser = self._ser, None
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass

    # -------------------------------------------------------------- odbior

    def _pump(self) -> None:
        ser = self._ser
        if ser is None:
            return

        # zlecony rozlacz po bledzie zapisu
        with self._tx_lock:
            fault = any(x == b"" for x in self._pending_tx)
            self._pending_tx.clear()
        if fault:
            self._close()
            self._set_state(SCANNING, "Utracono polaczenie")
            return

        try:
            waiting = ser.in_waiting
            chunk = ser.read(max(1, min(4096, waiting)))
        except (serial.SerialException, OSError) as exc:
            self._log("warn", f"Utracono port: {exc}")
            self._close()
            self._set_state(SCANNING, "Utracono polaczenie")
            return

        now = time.monotonic()

        if chunk:
            self._win_bytes += len(chunk)
            for mtype, payload in self._parser.feed(chunk):
                self._dispatch(mtype, payload, now)

        # ------- wykrycie zamilkniecia plytki -------
        if self._last_frame_t and (now - self._last_frame_t) > STALE_TIMEOUT:
            self._log("warn", "Plytka przestala odpowiadac")
            self._close()
            self._set_state(SCANNING, "Brak danych z plytki")
            return

        # ------- okno statystyk -------
        if now - self._win_t >= 0.5:
            dt = now - self._win_t
            self.stats.frames_per_sec = self._win_frames / dt
            self.stats.bytes_per_sec = self._win_bytes / dt
            self.stats.crc_errors = self._parser.crc_errors
            self.stats.resyncs = self._parser.resyncs
            if self.stats.connected_at:
                self.stats.uptime = now - self.stats.connected_at
            self._win_t = now
            self._win_frames = 0
            self._win_bytes = 0

        # ------- pomiar opoznienia -------
        if now >= self._next_ping:
            self._next_ping = now + 1.0
            self._ping_token = (self._ping_token + 1) & 0xFFFFFFFF
            self._ping_sent_t = time.perf_counter()
            self.send(protocol.cmd_ping(self._ping_token))

        if not chunk:
            time.sleep(0.001)

    def _dispatch(self, mtype: int, payload: bytes, now: float) -> None:
        if mtype == protocol.MSG_STATE:
            try:
                frame = protocol.decode_state(payload, host_time=now)
            except ValueError as exc:
                self._log("warn", f"Uszkodzona ramka STATE: {exc}")
                return
            self._win_frames += 1
            self.stats.total_frames += 1
            self._last_frame_t = now
            if self._last_seq is not None:
                gap = (frame.seq - self._last_seq - 1) & 0xFFFF
                if 0 < gap < 1000:
                    self.stats.dropped_frames += gap
            self._last_seq = frame.seq
            if self._on_state:
                self._safe(self._on_state, frame)

        elif mtype == protocol.MSG_PONG:
            try:
                token, _device_ms = protocol.decode_pong(payload)
            except ValueError:
                return
            if token == self._ping_token and self._ping_sent_t:
                self.stats.latency_ms = (time.perf_counter() - self._ping_sent_t) * 1000.0

        elif mtype == protocol.MSG_HELLO:
            try:
                hello = protocol.decode_hello(payload)
            except ValueError:
                return
            self.hello = hello
            if self._on_hello:
                self._safe(self._on_hello, hello)

        elif mtype == protocol.MSG_ACK:
            if len(payload) >= 2 and payload[1] == 0:
                name = protocol.MSG_NAMES.get(payload[0], f"0x{payload[0]:02X}")
                self._log("warn", f"Plytka odrzucila komende {name}")

        elif mtype == protocol.MSG_LOG:
            text = payload.decode("ascii", errors="replace").strip()
            if text:
                self._log("info", f"[plytka] {text}")

        elif mtype == protocol.MSG_G29_OUT:
            if self._on_g29_out:
                self._safe(self._on_g29_out, payload)

    # -------------------------------------------------------------- pomocnicze

    def _reset_stats(self, port: str) -> None:
        now = time.monotonic()
        self.stats = LinkStats(port=port, connected_at=now)
        self._win_t = now
        self._win_frames = 0
        self._win_bytes = 0
        self._last_seq = None
        self._last_frame_t = now
        self._next_ping = now + 0.5
        self._ping_sent_t = 0.0

    def _set_state(self, state: str, detail: str = "") -> None:
        if state == self.state and not detail:
            return
        self.state = state
        if state != CONNECTED:
            self.hello = None
        if self._on_status:
            self._safe(self._on_status, state, detail)

    def _log(self, level: str, msg: str) -> None:
        if self._on_log:
            self._safe(self._on_log, level, msg)

    def _safe(self, fn: Callable, *args) -> None:
        try:
            fn(*args)
        except Exception:
            pass

    def _safe_call(self, fn: Callable) -> None:
        try:
            fn()
        except Exception:
            pass
