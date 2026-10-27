"""
Silnik aplikacji - spaja lacze szeregowe, przetwarzanie, wyjscie vJoy i FFB.

Przeplyw danych:

    plytka --(ramka STATE)--> SerialLink (watek lacza)
                                  |
                            StateProcessor  (kalibracja, krzywe, mapowanie)
                                  |
                            OutputBackend   (vJoy -> gra)

    gra --(HID PID)--> vJoy --(callback)--> FfbManager
                                                |
                                  watek FFB: moment -> CMD_FFB -> plytka

Przetwarzanie dzieje sie w watku lacza, zaraz po odebraniu ramki - to
najkrotsza mozliwa droga do vJoy. Interfejs tylko odczytuje gotowe migawki.
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional

from . import link as link_mod
from . import protocol
from .config import AppConfig, PEDAL_KEYS
from .ffb import FfbManager
from .link import SerialLink
from .outputs import NullBackend, OutputBackend, OutputError, VJoyBackend
from .outputs.xinput import PadState, XInputBackend
from .processing import (ProcessedState, StateProcessor, build_g29_report,
                         build_pad_state)
from .protocol import HelloInfo, StateFrame


@dataclass
class EngineStatus:
    link_state: str = link_mod.DISCONNECTED
    link_detail: str = ""
    output_name: str = "none"
    output_text: str = "Nie uruchomiono"
    output_ok: bool = False
    pad_ok: bool = False
    pad_text: str = "Pad nieaktywny"
    ffb_registered: bool = False
    ffb_torque: float = 0.0
    ffb_packets: int = 0
    firmware: str = ""
    device_name: str = ""
    board_has_ffb: bool = False
    #: Tryb G29: czy firmware to umie, czy Windows widzi Native USB i czy
    #: raporty z aplikacji docieraja do plytki.
    g29_fw: bool = False
    g29_usb: bool = False
    g29_fed: bool = False
    g29_out_packets: int = 0
    g29_game_range: int = 0


#: Co ile najpozniej powtarzamy niezmieniony raport G29. Plytka wraca do
#: pozycji spoczynkowej po 250 ms ciszy, wiec zapas jest pieciokrotny.
G29_KEEPALIVE_S = 0.05


class WheelEngine:
    def __init__(self, cfg: AppConfig,
                 on_log: Optional[Callable[[str, str], None]] = None) -> None:
        self.cfg = cfg
        self._on_log = on_log

        self.processor = StateProcessor()
        self.ffb = FfbManager(on_log=self._log)
        self.output: OutputBackend = NullBackend()
        self.output_error: str = ""
        #: Drugie, niezalezne wyjscie - wirtualny pad Xbox 360 (ViGEm).
        self.pad_output: Optional[XInputBackend] = None
        self.pad_error: str = ""
        self.last_pad: Optional[PadState] = None
        #: Ostatni raport G29 wyslany do plytki i ostatni raport od gry.
        self.last_g29: Optional[bytes] = None
        self.g29_last_out: bytes = b""
        self._g29_sent_t = 0.0

        self.link = SerialLink(
            on_state=self._handle_state,
            on_hello=self._handle_hello,
            on_status=self._handle_status,
            on_log=self._log,
            on_g29_out=self._handle_g29_out,
        )
        self.link.set_on_connected(self._push_board_config)

        self.status = EngineStatus()
        self.last_state: Optional[ProcessedState] = None
        self.last_frame: Optional[StateFrame] = None

        #: Podgladowe min/max surowych kanalow - uzywane przez kreator kalibracji
        self.raw_seen_min: List[int] = [4095] * protocol.NUM_AXES
        self.raw_seen_max: List[int] = [0] * protocol.NUM_AXES
        self.raw_counts_min = 0
        self.raw_counts_max = 0

        self._ffb_thread: Optional[threading.Thread] = None
        self._ffb_stop = threading.Event()
        self._ffb_last_sent = 0
        self._ffb_sent_count = 0
        self._cfg_lock = threading.Lock()

    # ------------------------------------------------------------ cykl zycia

    def start(self) -> None:
        self.link.start()
        self._start_ffb_thread()
        if self.cfg.output.auto_acquire:
            self.open_output()
        if self.cfg.link.auto_connect:
            self.connect()

    def stop(self) -> None:
        self._ffb_stop.set()
        t = self._ffb_thread
        if t and t.is_alive():
            t.join(timeout=1.0)
        # zerowy moment przed zamknieciem - silnik nie moze zostac pod napieciem
        try:
            self.link.send(protocol.cmd_ffb(0))
            self.link.send(protocol.cmd_ffb_config(
                False, 0, 255, False, 0, self.cfg.ffb.timeout_ms, 65280))
        except Exception:
            pass
        self.link.stop()
        self.close_output()

    # --------------------------------------------------------------- lacze

    def connect(self, port: Optional[str] = None) -> None:
        p = port if port is not None else (self.cfg.link.port or None)
        self.link.connect(port=p,
                          baudrate=self.cfg.link.baudrate,
                          report_hz=self.cfg.link.report_hz)

    def disconnect(self) -> None:
        self.link.disconnect()
        self.processor.reset()
        self._center_output()

    def center_wheel(self) -> None:
        """Ustawia aktualna pozycje jako srodek - i w plytce, i w aplikacji."""
        frame = self.last_frame
        self.link.send(protocol.cmd_center())
        with self._cfg_lock:
            self.cfg.steer.center_counts = 0
        if frame is not None:
            # plytka wyzeruje licznik, wiec offset aplikacji tez idzie do zera
            self.processor.steer.reset()
        self._log("ok", "Środek kierownicy ustawiony")

    def reset_board(self) -> None:
        self.link.send(protocol.cmd_reset())
        self.ffb.reset()
        self._log("info", "Wysłano reset do płytki")

    def _push_board_config(self) -> None:
        """Wywolywane z watku lacza po nawiazaniu polaczenia."""
        cfg = self.cfg
        self.link.send(protocol.cmd_set_rate(cfg.link.report_hz))

        counts = 0
        if cfg.steer.soft_endstop:
            # polowa zakresu w impulsach enkodera
            counts = int(cfg.steer.range_deg / 2.0 * cfg.steer.counts_per_rev / 360.0)
        self.link.send(protocol.cmd_set_endstop(counts, cfg.steer.endstop_gain))
        self._send_ffb_config()

    def _send_ffb_config(self) -> None:
        f = self.cfg.ffb
        self.link.send(protocol.cmd_ffb_config(
            enabled=f.enabled,
            min_duty=f.min_duty,
            max_duty=f.max_duty,
            invert=f.invert,
            deadband=int(f.deadband * 32767),
            timeout_ms=f.timeout_ms,
            slew_per_ms=f.slew_per_ms,
        ))

    def set_ffb_enabled(self, enabled: bool) -> None:
        """Wlacza / wylacza FFB: rejestracja callbacku vJoy + konfiguracja plytki."""
        self.cfg.ffb.enabled = bool(enabled)
        if enabled:
            backend = self.output
            if isinstance(backend, VJoyBackend) and backend.is_open:
                if backend.ffb_capable:
                    self.ffb.register(backend.api, backend.device_id)
                else:
                    self._log("warn", "Urządzenie vJoy nie ma Force Feedback - "
                                      "włącz 'Enable Effects' w vJoyConf")
            else:
                self._log("warn", "FFB z gier wymaga działającego vJoy; "
                                  "efekty lokalne (sprężyna, tłumienie) działają i tak")
        else:
            self.ffb.reset()
            self.link.send(protocol.cmd_ffb(0))
        self._send_ffb_config()
        self.status.ffb_registered = self.ffb.registered

    def apply_config(self) -> None:
        """Przesyla do plytki to, co plytka musi wiedziec, i odswieza wyjscie."""
        self.cfg.normalize()
        if self.link.is_connected:
            self._push_board_config()
        wheel_mismatch = (isinstance(self.output, VJoyBackend)
                          and self.output.device_id != self.cfg.output.vjoy_device)
        mode_mismatch = ((self.cfg.output.wheel_on
                          != isinstance(self.output, VJoyBackend))
                         or (self.cfg.output.pad_on
                             != (self.pad_output is not None)))
        if wheel_mismatch or mode_mismatch:
            self.open_output()

    # -------------------------------------------------------------- wyjscie

    def open_output(self) -> bool:
        """Otwiera wyjscia zgodnie z trybem. Zwraca False, gdy ktores zawiodlo."""
        self.close_output()
        ok = True
        if self.cfg.output.pad_on:
            ok = self._open_pad() and ok
        if self.cfg.output.wheel_on:
            ok = self._open_wheel() and ok
        else:
            self.output = NullBackend()
            self.output.open()
            self.output_error = ""
        if self.cfg.output.g29_on:
            # G29 wystawia sama plytka - po stronie PC nie ma czego otwierac
            self.last_g29 = None
            self._log("ok", "Tryb G29: gry widzą kierownicę Logitech G29 na "
                            "porcie Native USB płytki")
        self._refresh_output_status()
        return ok

    def _open_pad(self) -> bool:
        backend = XInputBackend()
        try:
            backend.open()
        except OutputError as exc:
            self.pad_error = str(exc)
            self.pad_output = None
            self._log("error", f"Pad: {str(exc).splitlines()[0]}")
            return False
        self.pad_output = backend
        self.pad_error = ""
        self._log("ok", backend.status_text())
        return True

    def _open_wheel(self) -> bool:
        backend = VJoyBackend(self.cfg.output.vjoy_device)
        try:
            backend.open()
        except OutputError as exc:
            self.output_error = str(exc)
            self.output = NullBackend()
            self.output.open()
            self._log("error", f"vJoy: {str(exc).splitlines()[0]}")
            return False

        self.output = backend
        self.output_error = ""
        self._log("ok", backend.status_text())
        if self.cfg.ffb.enabled and backend.ffb_capable:
            self.ffb.register(backend.api, backend.device_id)
        elif self.cfg.ffb.enabled and not backend.ffb_capable:
            self._log("warn", "Urządzenie vJoy nie ma włączonego Force Feedback "
                              "- zaznacz 'Enable Effects' w vJoyConf")
        return True

    def close_output(self) -> None:
        try:
            self.output.close()
        except Exception:
            pass
        self.output = NullBackend()
        if self.pad_output is not None:
            try:
                self.pad_output.close()
            except Exception:
                pass
            self.pad_output = None
        self.last_pad = None
        self._refresh_output_status()

    def _refresh_output_status(self) -> None:
        self.status.output_name = self.cfg.output.mode
        self.status.output_ok = isinstance(self.output, VJoyBackend) and self.output.is_open
        self.status.output_text = ("Kierownica G29 na porcie Native USB"
                                   if self.cfg.output.g29_on
                                   else self.output.status_text())
        self.status.pad_ok = self.pad_output is not None and self.pad_output.is_open
        self.status.pad_text = (self.pad_output.status_text()
                                if self.pad_output is not None
                                else "Pad nieaktywny")
        self.status.ffb_registered = self.ffb.registered

    def _center_output(self) -> None:
        try:
            if isinstance(self.output, VJoyBackend):
                self.output.center(self._neutral_axes())
            else:
                self.output.center()
        except Exception:
            pass
        if self.pad_output is not None:
            try:
                self.pad_output.center()
            except Exception:
                pass
        self.last_pad = None

    def _neutral_axes(self) -> Dict[str, float]:
        """Pozycja spoczynkowa: kierownica na srodku, pedaly puszczone."""
        out: Dict[str, float] = {}
        if self.cfg.steer.vjoy_axis != "-":
            out[self.cfg.steer.vjoy_axis] = 0.5
        for key in PEDAL_KEYS:
            p = self.cfg.pedals[key]
            if p.vjoy_axis != "-" and p.source >= 0:
                out[p.vjoy_axis] = 0.0
        return out

    # ------------------------------------------------------- obsluga ramek

    def _handle_state(self, frame: StateFrame) -> None:
        cfg = self.cfg
        state = self.processor.process(frame, cfg)
        self.last_frame = frame
        self.last_state = state

        # podglad surowych zakresow dla kreatora kalibracji
        for i, raw in enumerate(frame.axes):
            if i >= len(self.raw_seen_min):
                break
            if raw < self.raw_seen_min[i]:
                self.raw_seen_min[i] = raw
            if raw > self.raw_seen_max[i]:
                self.raw_seen_max[i] = raw
        if frame.steer_counts < self.raw_counts_min:
            self.raw_counts_min = frame.steer_counts
        if frame.steer_counts > self.raw_counts_max:
            self.raw_counts_max = frame.steer_counts

        axes: Dict[str, float] = {}
        if cfg.steer.vjoy_axis != "-":
            axes[cfg.steer.vjoy_axis] = (state.steer_axis + 1.0) * 0.5
        for key in PEDAL_KEYS:
            p = cfg.pedals[key]
            if p.vjoy_axis != "-" and p.source >= 0:
                axes[p.vjoy_axis] = state.pedals[key]

        try:
            self.output.update(axes, state.out_buttons, state.hat)
        except Exception as exc:
            self._log("error", f"Błąd wyjścia: {exc}")

        pad = self.pad_output
        if pad is not None:
            try:
                pad_state = build_pad_state(state, cfg)
                self.last_pad = pad_state
                pad.send(pad_state)
            except Exception as exc:
                self._log("error", f"Błąd pada: {exc}")

        self.status.g29_usb = frame.g29_usb
        self.status.g29_fed = frame.g29_fed
        if cfg.output.g29_on:
            self._send_g29(state)

    def _send_g29(self, state: ProcessedState) -> None:
        """Raport G29 do plytki - przy zmianie i co G29_KEEPALIVE_S."""
        hello = self.link.hello
        if hello is None or not hello.has_g29:
            # Stary firmware nie zna CMD_G29 i odpowiadalby odmowa na kazda
            # ramke - zalalby lacze i dziennik.
            return
        try:
            report = build_g29_report(state, self.cfg)
        except Exception as exc:
            self._log("error", f"Błąd raportu G29: {exc}")
            return
        now = time.monotonic()
        if report == self.last_g29 and now - self._g29_sent_t < G29_KEEPALIVE_S:
            return
        self.link.send(protocol.cmd_g29(report))
        self.last_g29 = report
        self._g29_sent_t = now

    def _handle_g29_out(self, payload: bytes) -> None:
        """Gra (przez sterownik) wyslala cos do G29 - FFB albo zakres obrotu."""
        self.status.g29_out_packets += 1
        self.g29_last_out = bytes(payload)
        if self.status.g29_out_packets == 1:
            self._log("info", "Gra wysyła komendy do G29: " + payload.hex(" "))
        # f8 81 lo hi - zakres obrotu (tak ustawia go sterownik Logitecha)
        if len(payload) >= 4 and payload[0] == 0xF8 and payload[1] == 0x81:
            deg = payload[2] | (payload[3] << 8)
            if deg != self.status.g29_game_range:
                self.status.g29_game_range = deg
                self._log("info", f"Gra ustawiła zakres obrotu G29: {deg} st.")

    def _handle_hello(self, hello: HelloInfo) -> None:
        self.status.firmware = hello.firmware
        self.status.device_name = hello.name
        self.status.board_has_ffb = hello.has_ffb
        self.status.g29_fw = hello.has_g29
        self.last_g29 = None
        if self.cfg.output.g29_on and not hello.has_g29:
            self._log("warn", f"Firmware {hello.firmware} nie ma trybu G29 - "
                              "wgraj firmware 2.1 (firmware/kierownica)")

    def _handle_status(self, state: str, detail: str) -> None:
        self.status.link_state = state
        self.status.link_detail = detail
        if state != link_mod.CONNECTED:
            self.processor.reset()
            self._center_output()
            self.status.g29_fw = False
            self.status.g29_usb = False
            self.status.g29_fed = False

    # ------------------------------------------------------------ watek FFB

    def _start_ffb_thread(self) -> None:
        if self._ffb_thread and self._ffb_thread.is_alive():
            return
        self._ffb_stop.clear()
        self._ffb_thread = threading.Thread(target=self._ffb_loop, name="FFB",
                                            daemon=True)
        self._ffb_thread.start()

    def _ffb_loop(self) -> None:
        last_cfg_push = 0.0
        while not self._ffb_stop.is_set():
            f = self.cfg.ffb
            period = 1.0 / max(50, f.update_hz)
            t0 = time.perf_counter()

            try:
                torque = self._compute_torque()
            except Exception as exc:
                torque = 0.0
                self._log("error", f"Błąd obliczania FFB: {exc}")

            self.status.ffb_torque = torque
            self.status.ffb_packets = self.ffb.packets
            self.status.ffb_registered = self.ffb.registered

            if self.link.is_connected and f.enabled:
                value = int(round(torque * 32767))
                value = max(-32767, min(32767, value))
                # wysylamy takze gdy sie nie zmienilo - inaczej watchdog
                # w firmware zwolnilby silnik
                self.link.send(protocol.cmd_ffb(value))
                self._ffb_last_sent = value
                self._ffb_sent_count += 1

                # odswiezenie konfiguracji FFB co 2 s (np. po zmianie suwakow)
                if t0 - last_cfg_push > 2.0:
                    last_cfg_push = t0
                    self._send_ffb_config()
            elif self._ffb_last_sent != 0 and self.link.is_connected:
                self.link.send(protocol.cmd_ffb(0))
                self._ffb_last_sent = 0

            dt = time.perf_counter() - t0
            # time.sleep, nie Event.wait: na Windowsie Event.wait czeka z
            # dokladnoscia zegara systemu (15,6 ms, gdy gra zaslania panel),
            # co dawalo ~64 obiegi/s zamiast ustawionych 400. time.sleep
            # (Python 3.11+) uzywa zegara wysokiej rozdzielczosci. Sygnal
            # zatrzymania sprawdzamy co obieg, czyli najpozniej po 20 ms.
            time.sleep(max(0.0005, period - dt))

    def _compute_torque(self) -> float:
        f = self.cfg.ffb
        if not f.enabled:
            return 0.0
        state = self.last_state
        if state is None:
            return 0.0

        pos = max(-1.0, min(1.0, state.steer_axis))
        # predkosc normalizujemy wzgledem 1000 st/s - tyle wyciaga reka
        vel = max(-1.0, min(1.0, state.steer_velocity / 1000.0))

        torque = 0.0

        if self.ffb.registered:
            torque += self.ffb.compute(pos, vel, gains={
                "constant": f.game_constant_gain,
                "periodic": f.game_periodic_gain,
                "spring": f.game_spring_gain,
                "damper": f.game_damper_gain,
            })

        # efekty lokalne - dzialaja takze gdy gra nie wysyla FFB
        if f.spring_gain > 0.0:
            torque += -pos * f.spring_gain
        if f.damper_gain > 0.0:
            torque += -vel * f.damper_gain
        if f.friction_gain > 0.0 and abs(vel) > 0.005:
            torque += (-1.0 if vel > 0 else 1.0) * f.friction_gain * min(1.0, abs(vel) * 10)

        torque *= f.overall_gain
        torque = max(-1.0, min(1.0, torque))

        if abs(torque) < f.deadband:
            return 0.0
        if f.min_force > 0.0 and torque != 0.0:
            sign = 1.0 if torque > 0 else -1.0
            torque = sign * (f.min_force + abs(torque) * (1.0 - f.min_force))
        torque = max(-f.max_force, min(f.max_force, torque))
        return torque

    # ----------------------------------------------------------- kalibracja

    def reset_raw_tracking(self) -> None:
        self.raw_seen_min = [4095] * protocol.NUM_AXES
        self.raw_seen_max = [0] * protocol.NUM_AXES
        frame = self.last_frame
        base = frame.steer_counts if frame else 0
        self.raw_counts_min = base
        self.raw_counts_max = base

    def calibrate_pedal_from_seen(self, key: str) -> bool:
        p = self.cfg.pedals[key]
        if p.source < 0 or p.source >= protocol.NUM_AXES:
            return False
        lo = self.raw_seen_min[p.source]
        hi = self.raw_seen_max[p.source]
        if hi - lo < 64:
            return False
        p.raw_min, p.raw_max = lo, hi
        self.cfg.normalize()
        self.processor.pedals[key].reset()
        return True

    def detect_counts_per_rev(self) -> Optional[int]:
        """Wyliczenie impulsow na obrot z pelnego przekreconego zakresu."""
        span = self.raw_counts_max - self.raw_counts_min
        if span < 100:
            return None
        # uzytkownik przekrecil lock-to-lock, czyli range_deg stopni
        cpr = int(round(span * 360.0 / max(1, self.cfg.steer.range_deg)))
        return cpr if cpr >= 4 else None

    # ------------------------------------------------------------- pomocnicze

    def link_stats(self) -> link_mod.LinkStats:
        return self.link.stats.snapshot()

    def _log(self, level: str, msg: str) -> None:
        if self._on_log:
            try:
                self._on_log(level, msg)
            except Exception:
                pass
