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
from typing import Callable, Dict, List, Optional, Tuple

from . import link as link_mod
from . import protocol
from .config import AppConfig, PEDAL_KEYS
from .ffb import FfbManager
from .link import SerialLink
from .outputs import (NullBackend, OutputBackend, OutputError, VJoyBackend,
                      set_vjoy_driver, vjoy_driver_enabled)
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
    #: G29 podlaczona do komputera - panel odlacza ja w trybach bez G29
    g29_attached: bool = True
    #: Kiedy (time.monotonic) G29 zostala ponownie podlaczona - Windows
    #: potrzebuje chwili, zanim ja wyliczy (0 = podlaczona od poczatku)
    g29_attached_since: float = 0.0
    #: Dlaczego nie udalo sie wlaczyc / wylaczyc vJoy ("" = stan jak trzeba)
    vjoy_switch_error: str = ""
    #: Trwa wlaczanie / wylaczanie sterownika vJoy (okno UAC, vJoyConfig)
    vjoy_switching: bool = False


#: Co ile najpozniej powtarzamy niezmieniony raport G29. Plytka wraca do
#: pozycji spoczynkowej po 250 ms ciszy, wiec zapas jest pieciokrotny.
G29_KEEPALIVE_S = 0.05

#: Co ile najczesciej ponawiamy podlaczenie / odlaczenie G29, gdy plytka
#: zglasza inny stan niz chcemy (np. po resecie plytki).
G29_USB_RETRY_S = 0.5

#: Tyle po CMD_CENTER moga jeszcze przychodzic ramki ze starym srodkiem.
CENTER_SETTLE_S = 0.1


class WheelEngine:
    def __init__(self, cfg: AppConfig,
                 on_log: Optional[Callable[[str, str], None]] = None) -> None:
        self.cfg = cfg
        self._on_log = on_log

        self.processor = StateProcessor()
        self.ffb = FfbManager(on_log=self._log)
        self.ffb.direction_mode = cfg.ffb.direction_mode
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
        self._g29_usb_sent_t = 0.0
        self._g29_hide_warned = False
        #: Stan vJoy, o ktory juz raz poproszono bez skutku - zeby nie
        #: wyskakiwac z oknem UAC przy kazdym ponownym otwarciu wyjscia.
        self._vjoy_refused: Optional[bool] = None
        #: Okno panelu (HWND) - okno UAC wyskakuje nad nim.
        self.ui_hwnd = 0
        # Przelaczanie sterownika vJoy idzie w osobnym watku. _vjoy_pending
        # mowi, ze tryb sie zmienil i watek ma sprawdzic stan jeszcze raz -
        # dzieki temu zmiana trybu w trakcie pytania UAC nie przepada.
        self._vjoy_lock = threading.Lock()
        self._vjoy_pending = False
        self._vjoy_running = False

        # Wyjsc uzywa watek lacza (kazda ramka), a otwiera je i zamyka watek
        # interfejsu albo watek vJoy. _out_lock chroni podmiane backendow -
        # zamykany pad nie moze w tej samej chwili dostac raportu (ViGEm
        # pisalby do zwolnionej pamieci). _open_lock szereguje cale otwieranie.
        self._out_lock = threading.RLock()
        self._open_lock = threading.RLock()
        #: Dla jakiego trybu i numeru vJoy otwarto wyjscia (None = zamkniete)
        self._opened_for: Optional[Tuple[str, int]] = None
        #: Do kiedy moga przychodzic ramki sprzed ustawienia srodka
        self._center_settle_until = 0.0

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
        if not self.link.is_connected:
            self._log("warn", "Brak połączenia z płytką - środka nie ustawiono")
            return
        self.link.send(protocol.cmd_center())
        with self._cfg_lock:
            # plytka wyzeruje licznik, wiec offset aplikacji tez idzie do zera
            self.cfg.steer.center_counts = 0
        # Ramki w drodze maja jeszcze stary srodek - _handle_state je
        # przeczekuje (CENTER_SETTLE_S).
        self._center_settle_until = time.monotonic() + CENTER_SETTLE_S
        self._log("ok", "Środek kierownicy ustawiony")

    def reset_board(self) -> None:
        if not self.link.is_connected:
            self._log("warn", "Brak połączenia z płytką - resetu nie wysłano")
            return
        self.link.send(protocol.cmd_reset())
        self.ffb.reset()
        # reset ustawia tez srodek w plytce - jak center_wheel
        self._center_settle_until = time.monotonic() + CENTER_SETTLE_S
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
        # Na nowo otwieramy tylko po zmianie trybu albo numeru vJoy.
        # Porownanie z tym, co faktycznie dziala, ponawialo nieudane otwarcie
        # przy kazdym ruchu suwaka - w trybie "Oba naraz" bez vJoy pad
        # znikal wtedy i pojawial sie w grze od nowa.
        opened = self._opened_for
        if opened is not None and opened != (self.cfg.output.mode,
                                             self.cfg.output.vjoy_device):
            self.open_output()

    # -------------------------------------------------------------- wyjscie

    def open_output(self, ask_again: bool = False) -> bool:
        """Otwiera wyjscia zgodnie z trybem. Zwraca False, gdy ktores zawiodlo.

        `ask_again` ponawia prosbe o wlaczenie / wylaczenie vJoy, nawet gdy
        uzytkownik juz raz odmowil (jawne "Podlacz ponownie").
        """
        with self._open_lock:
            self.close_output()
            out = self.cfg.output
            self._sync_g29_usb(force=True)
            vjoy_later = self._sync_vjoy_driver(ask_again)
            ok = True
            if out.pad_on:
                ok = self._open_pad() and ok
            self.output_error = ""
            if out.wheel_on and not vjoy_later:
                ok = self._open_wheel() and ok
            # przy vjoy_later urzadzenie otworzy watek vJoy, gdy wlaczy sterownik
            if out.g29_on:
                # G29 wystawia sama plytka - po stronie PC nie ma czego otwierac
                self.last_g29 = None
                self._log("ok", "Tryb G29: gry widzą kierownicę Logitech G29 na "
                                "porcie Native USB płytki")
            self._opened_for = (out.mode, out.vjoy_device)
            self._refresh_output_status()
            return ok

    # --- wlaczanie / wylaczanie sterownika vJoy ---

    def _sync_vjoy_driver(self, ask_again: bool) -> bool:
        """Wlacza vJoy w trybach z vJoy, wylacza w pozostalych - w tle.

        Okno UAC i vJoyConfig trwaja kilka sekund, a w watku interfejsu
        zamrazaly okno (przy starcie panel w ogole sie nie pokazywal). Robi
        to wiec osobny watek. Zwraca True, gdy tryb potrzebuje vJoy, a
        sterownik jest dopiero przelaczany - urzadzenie otworzy wtedy watek
        vJoy, juz po wlaczeniu sterownika.
        """
        if ask_again:
            self._vjoy_refused = None
        want = self.cfg.output.vjoy_wanted
        with self._vjoy_lock:
            busy = self._vjoy_running
        if not busy:
            # Watek nie dziala - nowy uruchamiamy tylko wtedy, gdy stan
            # sterownika naprawde trzeba zmienic. Wazne: gdy watek dziala,
            # ten odczyt bylby nieaktualny (np. trwa wylaczanie).
            if want is None:
                return False
            current = vjoy_driver_enabled()
            if current is None or current == want:
                self._vjoy_refused = None
                self.status.vjoy_switch_error = ""
                return False
            if self._vjoy_refused == want:
                return False
        self._request_vjoy_switch()
        return bool(want)

    def _request_vjoy_switch(self) -> None:
        with self._vjoy_lock:
            self._vjoy_pending = True
            if self._vjoy_running:
                return              # dzialajacy watek sprawdzi stan jeszcze raz
            self._vjoy_running = True
            self.status.vjoy_switching = True
        threading.Thread(target=self._vjoy_worker, name="vJoySwitch",
                         daemon=True).start()

    def _vjoy_worker(self) -> None:
        while True:
            with self._vjoy_lock:
                if not self._vjoy_pending:
                    self._vjoy_running = False
                    self.status.vjoy_switching = False
                    return
                self._vjoy_pending = False
            try:
                self._vjoy_switch_once()
            except Exception as exc:            # watek nie moze zginac po cichu
                self._log("error", f"Przełączanie vJoy: {exc!r}")

    def _vjoy_switch_once(self) -> None:
        want = self.cfg.output.vjoy_wanted
        if want is None:
            return
        current = vjoy_driver_enabled()
        if current is None or current == want:
            self._vjoy_refused = None
            self.status.vjoy_switch_error = ""
        elif self._vjoy_refused != want:
            self._log("info", ("Włączam" if want else "Wyłączam") + " vJoy - "
                              "Windows może zapytać o zgodę administratora")
            ok, msg = set_vjoy_driver(want, hwnd=self.ui_hwnd)
            if ok:
                self._vjoy_refused = None
                self.status.vjoy_switch_error = ""
                self._log("ok", msg)
            else:
                self._vjoy_refused = want
                self.status.vjoy_switch_error = msg
                self._log("warn", f"vJoy: {msg}")
        self._open_deferred_wheel()

    def _open_deferred_wheel(self) -> None:
        """Otwiera vJoy, na ktore tryb czekal do wlaczenia sterownika."""
        with self._open_lock:
            out = self.cfg.output
            if (self._opened_for != (out.mode, out.vjoy_device)
                    or not out.wheel_on or isinstance(self.output, VJoyBackend)):
                return
            with self._vjoy_lock:
                if self._vjoy_pending:
                    return          # zaraz kolejne przelaczenie - otworzy po nim
            self._open_wheel()
            self._refresh_output_status()

    def _sync_g29_usb(self, force: bool = False,
                      frame: Optional[StateFrame] = None) -> None:
        """Podlacza G29 w trybach z G29, odlacza w pozostalych.

        Wolane przy zmianie trybu (`force`) i z kazda ramka STATE - wtedy
        tylko gdy plytka zglasza inny stan, np. po wlasnym resecie.
        """
        hello = self.link.hello
        if hello is None or not hello.has_g29:
            return
        want = self.cfg.output.g29_wanted
        if not hello.can_hide_g29:
            if not want and not self._g29_hide_warned:
                self._g29_hide_warned = True
                self._log("warn", f"Firmware {hello.firmware} nie umie ukryć G29 - "
                                  "gry nadal ją widzą. Wgraj firmware "
                                  f"{protocol.FW_LATEST_TEXT}.")
            return
        now = time.monotonic()
        if not force:
            if frame is None or frame.g29_attached == want:
                return
            if now - self._g29_usb_sent_t < G29_USB_RETRY_S:
                return
        self.link.send(protocol.cmd_g29_usb(want))
        self._g29_usb_sent_t = now

    # --- backendy ---
    #
    # Otwieranie (pad potrafi czekac do 3 s na Windowsa) idzie bez blokady -
    # pod _out_lock jest tylko podmiana gotowego obiektu, wiec watek lacza
    # nie stoi.

    def _open_pad(self) -> bool:
        backend = XInputBackend()
        try:
            backend.open()
        except Exception as exc:
            _close_quietly(backend)
            self.pad_error = _error_text(exc)
            self._log("error", f"Pad: {self.pad_error.splitlines()[0]}")
            return False
        with self._out_lock:
            self.pad_output = backend
        self.pad_error = ""
        self._log("ok", backend.status_text())
        return True

    def _open_wheel(self) -> bool:
        backend = VJoyBackend(self.cfg.output.vjoy_device)
        try:
            backend.open()
        except Exception as exc:
            _close_quietly(backend)
            self.output_error = _error_text(exc)
            self._log("error", f"vJoy: {self.output_error.splitlines()[0]}")
            return False

        with self._out_lock:
            self.output = backend
        self.output_error = ""
        self._log("ok", backend.status_text())
        if self.cfg.output.vjoy_buttons_only:
            # osie zostaja tam, gdzie zostawil je poprzedni tryb -
            # stawiamy je na srodku, zeby gra nie widziala wcisnietego gazu
            self._center_output()
        if self.cfg.ffb.enabled and backend.ffb_capable:
            self.ffb.register(backend.api, backend.device_id)
        elif self.cfg.ffb.enabled and not backend.ffb_capable:
            self._log("warn", "Urządzenie vJoy nie ma włączonego Force Feedback "
                              "- zaznacz 'Enable Effects' w vJoyConf")
        return True

    def close_output(self) -> None:
        with self._open_lock:
            null = NullBackend()
            null.open()
            with self._out_lock:
                old, self.output = self.output, null
                old_pad, self.pad_output = self.pad_output, None
                self.last_pad = None
            # Watek lacza dostaje backendy tylko pod _out_lock, wiec stare
            # mozna juz zamknac bez blokady.
            _close_quietly(old)
            if old_pad is not None:
                _close_quietly(old_pad)
            self._opened_for = None
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
        with self._out_lock:
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
        if self.cfg.output.vjoy_buttons_only:
            return out          # wszystkie osie na srodku

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
        # Tuz po "Ustaw srodek" przychodza jeszcze ramki ze starym srodkiem.
        # Skok pozycji dawal zryw predkosci (tlumienie FFB szarpalo silnikiem)
        # i falszywy koniec zakresu przy wykrywaniu impulsow na obrot.
        settling = frame.host_time < self._center_settle_until
        if settling:
            self.processor.steer.reset()
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
        if settling:
            self.raw_counts_min = self.raw_counts_max = frame.steer_counts
        else:
            if frame.steer_counts < self.raw_counts_min:
                self.raw_counts_min = frame.steer_counts
            if frame.steer_counts > self.raw_counts_max:
                self.raw_counts_max = frame.steer_counts

        axes: Dict[str, float] = {}
        if not cfg.output.vjoy_buttons_only:
            if cfg.steer.vjoy_axis != "-":
                axes[cfg.steer.vjoy_axis] = (state.steer_axis + 1.0) * 0.5
            for key in PEDAL_KEYS:
                p = cfg.pedals[key]
                if p.vjoy_axis != "-" and p.source >= 0:
                    axes[p.vjoy_axis] = state.pedals[key]

        with self._out_lock:
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
        hello = self.link.hello
        attached = (frame.g29_attached
                    if hello is not None and hello.can_hide_g29 else True)
        if attached != self.status.g29_attached:
            if attached:
                self.status.g29_attached_since = time.monotonic()
            self.status.g29_attached = attached
            self._log("info", "G29 podłączona do komputera" if attached else
                              "G29 odłączona od komputera - gry jej nie widzą")
        self._sync_g29_usb(frame=frame)
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
        self._g29_hide_warned = False
        if self.cfg.output.g29_on and not hello.has_g29:
            self._log("warn", f"Firmware {hello.firmware} nie ma trybu G29 - "
                              f"wgraj firmware {protocol.FW_LATEST_TEXT} "
                              "(firmware/kierownica)")
        elif hello.outdated:
            self._log("info", f"Płytka ma firmware {hello.firmware}, w "
                              f"firmware/kierownica jest {protocol.FW_LATEST_TEXT}")

    def _handle_status(self, state: str, detail: str) -> None:
        self.status.link_state = state
        self.status.link_detail = detail
        if state != link_mod.CONNECTED:
            self.processor.reset()
            self._center_output()
            self.status.g29_fw = False
            self.status.g29_usb = False
            self.status.g29_fed = False
            self.status.g29_attached = True

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


def _close_quietly(backend: OutputBackend) -> None:
    try:
        backend.close()
    except Exception:
        pass


def _error_text(exc: Exception) -> str:
    """Tresc bledu dla uzytkownika. OutputError ma gotowy opis, a cokolwiek
    innego (np. DLL bez oczekiwanej funkcji) nie moze wywrocic aplikacji."""
    if isinstance(exc, OutputError):
        return str(exc) or type(exc).__name__
    return f"{type(exc).__name__}: {exc}"
