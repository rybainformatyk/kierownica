"""
Force Feedback: odbior efektow z gry przez vJoy i zamiana ich na moment
obrotowy wysylany do plytki.

Gra wysyla do urzadzenia vJoy pakiety HID PID (Physical Interface Device).
vJoy przekazuje je nam przez callback FfbRegisterGenCB. Rozkladamy je
funkcjami pomocniczymi Ffb_h_* z vJoyInterface.dll, utrzymujemy tablice
aktywnych efektow i co `update_hz` razy na sekunde sumujemy ich wklad.

BEZPIECZENSTWO
  * kazda wartosc z DLL jest sprawdzana zakresem (+-10000); wartosci poza
    zakresem traktujemy jako blad parsowania i ignorujemy - uszkodzony
    pakiet nie moze wystrzelic silnikiem,
  * callback nigdy nie rzuca wyjatku do kodu C,
  * firmware ma wlasny watchdog: brak komend = zwolnienie silnika.
"""

from __future__ import annotations

import ctypes
import math
import threading
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

# ----------------------------------------------------------- stale HID PID

# typy pakietow (FFBPType z public.h)
PT_EFFREP = 0x01        # parametry efektu (typ, czas trwania, kierunek)
PT_ENVREP = 0x02        # obwiednia
PT_CONDREP = 0x03       # efekt warunkowy (spring / damper / inertia / friction)
PT_PRIDREP = 0x04       # efekt okresowy
PT_CONSTREP = 0x05      # sila stala
PT_RAMPREP = 0x06       # narastanie
PT_CSTMREP = 0x07
PT_SMPLREP = 0x08
PT_EFOPREP = 0x0A       # start / stop efektu
PT_BLKFRREP = 0x0B      # zwolnienie bloku efektu
PT_CTRLREP = 0x0C       # sterowanie urzadzeniem
PT_GAINREP = 0x0D       # globalne wzmocnienie urzadzenia
PT_SETCREP = 0x0E
PT_NEWEFREP = 0x01      # feature: nowy efekt
PT_BLKLDREP = 0x02
PT_POOLREP = 0x03

# typy efektow (FFBEType)
ET_NONE = 0
ET_CONST = 1
ET_RAMP = 2
ET_SQR = 3
ET_SINE = 4
ET_TRNGL = 5
ET_STUP = 6
ET_STDN = 7
ET_SPRNG = 8
ET_DMPR = 9
ET_INRT = 10
ET_FRCTN = 11
ET_CSTM = 12

EFFECT_NAMES = {
    ET_NONE: "-", ET_CONST: "Stała siła", ET_RAMP: "Narastanie",
    ET_SQR: "Prostokąt", ET_SINE: "Sinus", ET_TRNGL: "Trójkąt",
    ET_STUP: "Piła w górę", ET_STDN: "Piła w dół", ET_SPRNG: "Sprężyna",
    ET_DMPR: "Tłumienie", ET_INRT: "Bezwładność", ET_FRCTN: "Tarcie",
    ET_CSTM: "Własny",
}

# operacje na efekcie (FFBOP)
EFF_START = 1
EFF_SOLO = 2
EFF_STOP = 3

# sterowanie urzadzeniem (FFB_CTRL)
CTRL_ENACT = 1
CTRL_DISACT = 2
CTRL_STOPALL = 3
CTRL_DEVRST = 4
CTRL_DEVPAUSE = 5
CTRL_DEVCONT = 6

#: Pelna skala wartosci HID PID.
PID_SCALE = 10000.0
#: Wartosci poza tym zakresem traktujemy jako blad parsowania.
PID_LIMIT = 10001

DIRECTION_MODES = ("auto", "ignore", "cos", "sin")
DIRECTION_LABELS = {
    "auto": "Auto (zalecane)",
    "ignore": "Ignoruj kierunek (znak z magnitudy)",
    "cos": "Polarny - cosinus",
    "sin": "Polarny - sinus",
}


# -------------------------------------------------------- struktury ctypes
# Uklad pol odpowiada public.h z SDK vJoy. Wyrownanie naturalne (bez _pack_),
# tak jak kompiluje MSVC.

class FFB_DATA(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_ulong),
        ("cmd", ctypes.c_ulong),
        ("data", ctypes.POINTER(ctypes.c_ubyte)),
    ]


class FFB_EFF_REPORT(ctypes.Structure):
    _fields_ = [
        ("EffectBlockIndex", ctypes.c_ubyte),
        ("EffectType", ctypes.c_int),
        ("Duration", ctypes.c_ushort),
        ("TrigerRpt", ctypes.c_ushort),
        ("SamplePrd", ctypes.c_ushort),
        ("Gain", ctypes.c_ubyte),
        ("TrigerBtn", ctypes.c_ubyte),
        ("Polar", ctypes.c_int),
        ("Direction", ctypes.c_ubyte),
        ("DirY", ctypes.c_ubyte),
    ]


class FFB_EFF_OP(ctypes.Structure):
    _fields_ = [
        ("EffectBlockIndex", ctypes.c_ubyte),
        ("EffectOp", ctypes.c_int),
        ("LoopCount", ctypes.c_ubyte),
    ]


class FFB_EFF_CONSTANT(ctypes.Structure):
    _fields_ = [
        ("EffectBlockIndex", ctypes.c_ubyte),
        ("Magnitude", ctypes.c_long),
    ]


class FFB_EFF_RAMP(ctypes.Structure):
    _fields_ = [
        ("EffectBlockIndex", ctypes.c_ubyte),
        ("Start", ctypes.c_long),
        ("End", ctypes.c_long),
    ]


class FFB_EFF_PERIOD(ctypes.Structure):
    _fields_ = [
        ("EffectBlockIndex", ctypes.c_ubyte),
        ("Magnitude", ctypes.c_ulong),
        ("Offset", ctypes.c_long),
        ("Phase", ctypes.c_ulong),
        ("Period", ctypes.c_ulong),
    ]


class FFB_EFF_COND(ctypes.Structure):
    _fields_ = [
        ("EffectBlockIndex", ctypes.c_ubyte),
        ("isY", ctypes.c_int),
        ("CenterPointOffset", ctypes.c_long),
        ("PosCoeff", ctypes.c_long),
        ("NegCoeff", ctypes.c_long),
        ("PosSatur", ctypes.c_ulong),
        ("NegSatur", ctypes.c_ulong),
        ("DeadBand", ctypes.c_long),
    ]


class FFB_EFF_ENVLP(ctypes.Structure):
    _fields_ = [
        ("EffectBlockIndex", ctypes.c_ubyte),
        ("AttackLevel", ctypes.c_ulong),
        ("FadeLevel", ctypes.c_ulong),
        ("AttackTime", ctypes.c_ulong),
        ("FadeTime", ctypes.c_ulong),
    ]


FFB_GEN_CB = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)

ERROR_SUCCESS = 0


# ---------------------------------------------------------------- model efektu

@dataclass
class Effect:
    index: int
    type: int = ET_NONE
    gain: float = 1.0
    duration_ms: int = 0            # 0 = nieskonczony
    direction: int = 0              # 0..255
    polar: bool = True
    # stala sila / narastanie
    magnitude: float = 0.0          # -1..1
    ramp_start: float = 0.0
    ramp_end: float = 0.0
    # okresowy
    period_ms: int = 100
    phase_deg: float = 0.0
    offset: float = 0.0
    amplitude: float = 0.0
    # warunkowy
    center: float = 0.0
    pos_coeff: float = 0.0
    neg_coeff: float = 0.0
    pos_sat: float = 1.0
    neg_sat: float = 1.0
    deadband: float = 0.0
    # obwiednia
    attack_level: float = 0.0
    attack_ms: int = 0
    fade_level: float = 0.0
    fade_ms: int = 0
    # stan
    active: bool = False
    start_time: float = 0.0
    loop_count: int = 1

    def reset_state(self) -> None:
        self.active = False
        self.start_time = 0.0


def _norm(value: int) -> Optional[float]:
    """HID PID -10000..10000 -> -1..1. None gdy wartosc jest bezsensowna."""
    if value is None:
        return None
    if value > PID_LIMIT or value < -PID_LIMIT:
        return None
    return max(-1.0, min(1.0, value / PID_SCALE))


def _norm_u(value: int) -> Optional[float]:
    if value is None or value < 0 or value > PID_LIMIT:
        return None
    return max(0.0, min(1.0, value / PID_SCALE))


# ------------------------------------------------------------------- manager

class FfbManager:
    """Tlumaczy efekty FFB z gry na pojedynczy moment obrotowy -1..1."""

    MAX_EFFECTS = 32

    def __init__(self, on_log=None) -> None:
        self._on_log = on_log
        self._lock = threading.Lock()
        self._effects: Dict[int, Effect] = {}
        self._device_gain = 1.0
        self._actuators_on = True
        self._paused = False
        self._cb: Optional[FFB_GEN_CB] = None
        self._h: Dict[str, Optional[object]] = {}
        self._api = None
        self._device_id = 0
        self._registered = False

        self.packets = 0
        self.parse_errors = 0
        self.last_packet_time = 0.0
        self.last_torque = 0.0
        self.direction_mode = "auto"

    # ------------------------------------------------------------ rejestracja

    def register(self, api, device_id: int) -> bool:
        """Podlacza callback vJoy. Zwraca False gdy DLL nie ma API FFB."""
        if self._registered:
            return True
        if api is None or not getattr(api, "has_ffb_api", False):
            self._log("warn", "Ta wersja vJoyInterface.dll nie udostępnia API FFB")
            return False
        self._api = api
        self._device_id = int(device_id)
        self._prepare_helpers(api)
        self._cb = FFB_GEN_CB(self._callback)
        try:
            api.dll.FfbRegisterGenCB(
                ctypes.cast(self._cb, ctypes.c_void_p), None)
        except Exception as exc:
            self._log("error", f"Nie udało się podłączyć FFB: {exc}")
            self._cb = None
            return False
        self._registered = True
        self._log("ok", "Odbiór Force Feedback z gier aktywny")
        return True

    def unregister(self) -> None:
        """vJoy nie ma funkcji odrejestrowania - przestajemy reagowac."""
        with self._lock:
            self._effects.clear()
        self._registered = False
        self._cb = None

    @property
    def registered(self) -> bool:
        return self._registered

    def _prepare_helpers(self, api) -> None:
        d = api.dll
        spec = [
            ("DeviceID", "Ffb_h_DeviceID", ctypes.POINTER(ctypes.c_int)),
            ("Type", "Ffb_h_Type", ctypes.POINTER(ctypes.c_int)),
            ("EBI", "Ffb_h_EBI", ctypes.POINTER(ctypes.c_int)),
            ("EffReport", "Ffb_h_Eff_Report", ctypes.POINTER(FFB_EFF_REPORT)),
            ("EffOp", "Ffb_h_EffOp", ctypes.POINTER(FFB_EFF_OP)),
            ("Constant", "Ffb_h_Eff_Constant", ctypes.POINTER(FFB_EFF_CONSTANT)),
            ("Ramp", "Ffb_h_Eff_Ramp", ctypes.POINTER(FFB_EFF_RAMP)),
            ("Period", "Ffb_h_Eff_Period", ctypes.POINTER(FFB_EFF_PERIOD)),
            ("Cond", "Ffb_h_Eff_Cond", ctypes.POINTER(FFB_EFF_COND)),
            ("Envlp", "Ffb_h_Eff_Envlp", ctypes.POINTER(FFB_EFF_ENVLP)),
            ("DevCtrl", "Ffb_h_DevCtrl", ctypes.POINTER(ctypes.c_int)),
            ("DevGain", "Ffb_h_DevGain", ctypes.POINTER(ctypes.c_ubyte)),
        ]
        for key, fname, out_type in spec:
            fn = getattr(d, fname, None)
            if fn is None:
                self._h[key] = None
                continue
            fn.restype = ctypes.c_ulong
            fn.argtypes = [ctypes.POINTER(FFB_DATA), out_type]
            self._h[key] = fn

    # ---------------------------------------------------------------- callback

    def _callback(self, data_ptr, _user) -> None:
        # Uruchamiane przez sterownik vJoy. Nie moze nic rzucic ani blokowac.
        try:
            if not data_ptr:
                return
            packet = ctypes.cast(data_ptr, ctypes.POINTER(FFB_DATA))
            self.packets += 1
            self.last_packet_time = time.perf_counter()

            dev = self._call_int("DeviceID", packet)
            if dev is not None and dev != self._device_id:
                return

            ptype = self._call_int("Type", packet)
            if ptype is None:
                return
            self._handle_packet(ptype, packet)
        except Exception:
            self.parse_errors += 1

    def _call_int(self, key: str, packet) -> Optional[int]:
        fn = self._h.get(key)
        if fn is None:
            return None
        out = ctypes.c_int(0)
        try:
            if fn(packet, ctypes.byref(out)) != ERROR_SUCCESS:
                return None
        except Exception:
            self.parse_errors += 1
            return None
        return out.value

    def _call_struct(self, key: str, packet, struct_type):
        fn = self._h.get(key)
        if fn is None:
            return None
        out = struct_type()
        try:
            if fn(packet, ctypes.byref(out)) != ERROR_SUCCESS:
                return None
        except Exception:
            self.parse_errors += 1
            return None
        return out

    def _effect(self, index: int) -> Optional[Effect]:
        if not (0 <= index <= self.MAX_EFFECTS):
            return None
        eff = self._effects.get(index)
        if eff is None:
            if len(self._effects) >= self.MAX_EFFECTS:
                return None
            eff = self._effects[index] = Effect(index=index)
        return eff

    def _handle_packet(self, ptype: int, packet) -> None:
        if ptype == PT_EFFREP:
            rep = self._call_struct("EffReport", packet, FFB_EFF_REPORT)
            if rep is None:
                return
            with self._lock:
                eff = self._effect(rep.EffectBlockIndex)
                if eff is None:
                    return
                if ET_NONE <= rep.EffectType <= ET_CSTM:
                    eff.type = rep.EffectType
                eff.duration_ms = int(rep.Duration)
                eff.gain = max(0.0, min(1.0, rep.Gain / 255.0))
                eff.direction = int(rep.Direction) & 0xFF
                eff.polar = bool(rep.Polar)
            return

        if ptype == PT_CONSTREP:
            rep = self._call_struct("Constant", packet, FFB_EFF_CONSTANT)
            if rep is None:
                return
            mag = _norm(rep.Magnitude)
            if mag is None:
                self.parse_errors += 1
                return
            with self._lock:
                eff = self._effect(rep.EffectBlockIndex)
                if eff is not None:
                    eff.magnitude = mag
                    if eff.type == ET_NONE:
                        eff.type = ET_CONST
            return

        if ptype == PT_RAMPREP:
            rep = self._call_struct("Ramp", packet, FFB_EFF_RAMP)
            if rep is None:
                return
            s, e = _norm(rep.Start), _norm(rep.End)
            if s is None or e is None:
                self.parse_errors += 1
                return
            with self._lock:
                eff = self._effect(rep.EffectBlockIndex)
                if eff is not None:
                    eff.ramp_start, eff.ramp_end = s, e
                    if eff.type == ET_NONE:
                        eff.type = ET_RAMP
            return

        if ptype == PT_PRIDREP:
            rep = self._call_struct("Period", packet, FFB_EFF_PERIOD)
            if rep is None:
                return
            amp = _norm_u(rep.Magnitude)
            off = _norm(rep.Offset)
            if amp is None or off is None:
                self.parse_errors += 1
                return
            with self._lock:
                eff = self._effect(rep.EffectBlockIndex)
                if eff is not None:
                    eff.amplitude = amp
                    eff.offset = off
                    eff.phase_deg = (int(rep.Phase) % 36000) / 100.0
                    eff.period_ms = max(1, min(32767, int(rep.Period)))
            return

        if ptype == PT_CONDREP:
            rep = self._call_struct("Cond", packet, FFB_EFF_COND)
            if rep is None:
                return
            if rep.isY:                      # os Y nas nie dotyczy
                return
            center = _norm(rep.CenterPointOffset)
            pos_c = _norm(rep.PosCoeff)
            neg_c = _norm(rep.NegCoeff)
            pos_s = _norm_u(rep.PosSatur)
            neg_s = _norm_u(rep.NegSatur)
            dead = _norm_u(rep.DeadBand)
            if None in (center, pos_c, neg_c, pos_s, neg_s, dead):
                self.parse_errors += 1
                return
            with self._lock:
                eff = self._effect(rep.EffectBlockIndex)
                if eff is not None:
                    eff.center = center
                    eff.pos_coeff = pos_c
                    eff.neg_coeff = neg_c
                    eff.pos_sat = pos_s
                    eff.neg_sat = neg_s
                    eff.deadband = dead
            return

        if ptype == PT_ENVREP:
            rep = self._call_struct("Envlp", packet, FFB_EFF_ENVLP)
            if rep is None:
                return
            al, fl = _norm_u(rep.AttackLevel), _norm_u(rep.FadeLevel)
            if al is None or fl is None:
                self.parse_errors += 1
                return
            with self._lock:
                eff = self._effect(rep.EffectBlockIndex)
                if eff is not None:
                    eff.attack_level = al
                    eff.fade_level = fl
                    eff.attack_ms = min(60000, int(rep.AttackTime))
                    eff.fade_ms = min(60000, int(rep.FadeTime))
            return

        if ptype == PT_EFOPREP:
            rep = self._call_struct("EffOp", packet, FFB_EFF_OP)
            if rep is None:
                return
            with self._lock:
                eff = self._effect(rep.EffectBlockIndex)
                if eff is None:
                    return
                if rep.EffectOp in (EFF_START, EFF_SOLO):
                    if rep.EffectOp == EFF_SOLO:
                        for other in self._effects.values():
                            if other is not eff:
                                other.reset_state()
                    eff.active = True
                    eff.start_time = time.perf_counter()
                    eff.loop_count = max(1, int(rep.LoopCount) or 1)
                elif rep.EffectOp == EFF_STOP:
                    eff.reset_state()
            return

        if ptype == PT_BLKFRREP:
            index = self._call_int("EBI", packet)
            with self._lock:
                if index is None:
                    self._effects.clear()
                else:
                    self._effects.pop(index, None)
            return

        if ptype == PT_CTRLREP:
            ctrl = self._call_int("DevCtrl", packet)
            if ctrl is None:
                return
            with self._lock:
                if ctrl == CTRL_ENACT:
                    self._actuators_on = True
                elif ctrl == CTRL_DISACT:
                    self._actuators_on = False
                elif ctrl == CTRL_STOPALL:
                    for eff in self._effects.values():
                        eff.reset_state()
                elif ctrl == CTRL_DEVRST:
                    self._effects.clear()
                    self._device_gain = 1.0
                    self._actuators_on = True
                    self._paused = False
                elif ctrl == CTRL_DEVPAUSE:
                    self._paused = True
                elif ctrl == CTRL_DEVCONT:
                    self._paused = False
            return

        if ptype == PT_GAINREP:
            fn = self._h.get("DevGain")
            if fn is None:
                return
            out = ctypes.c_ubyte(255)
            try:
                if fn(packet, ctypes.byref(out)) != ERROR_SUCCESS:
                    return
            except Exception:
                self.parse_errors += 1
                return
            with self._lock:
                self._device_gain = max(0.0, min(1.0, out.value / 255.0))
            return

    # ------------------------------------------------------------- obliczenia

    def reset(self) -> None:
        with self._lock:
            self._effects.clear()
            self._device_gain = 1.0
            self._actuators_on = True
            self._paused = False
        self.last_torque = 0.0

    def active_effects(self) -> List[Tuple[int, str, float]]:
        """Podglad do interfejsu: (indeks, nazwa, aktualny wklad)."""
        with self._lock:
            return [(e.index, EFFECT_NAMES.get(e.type, "?"), e.magnitude)
                    for e in self._effects.values() if e.active]

    def device_gain(self) -> float:
        return self._device_gain

    def compute(self, position: float, velocity_norm: float,
                gains: Optional[Dict[str, float]] = None,
                now: Optional[float] = None) -> float:
        """Sumuje efekty gry. position -1..1, velocity_norm -1..1 -> moment -1..1.

        `gains` pozwala osobno skalowac kategorie efektow - klucze:
        "constant", "periodic", "spring", "damper". Brak klucza = 1.0.
        """
        now = time.perf_counter() if now is None else now
        gains = gains or {}
        total = 0.0
        with self._lock:
            if self._paused or not self._actuators_on:
                self.last_torque = 0.0
                return 0.0
            for eff in self._effects.values():
                if not eff.active:
                    continue
                elapsed_ms = (now - eff.start_time) * 1000.0
                if eff.duration_ms and elapsed_ms > eff.duration_ms * eff.loop_count:
                    eff.reset_state()
                    continue
                total += self._effect_force(eff, position, velocity_norm,
                                            elapsed_ms, gains)
            total *= self._device_gain
        total = max(-1.0, min(1.0, total))
        self.last_torque = total
        return total

    #: Przypisanie typu efektu do kategorii wzmocnienia.
    _CATEGORY = {
        ET_CONST: "constant", ET_RAMP: "constant",
        ET_SINE: "periodic", ET_SQR: "periodic", ET_TRNGL: "periodic",
        ET_STUP: "periodic", ET_STDN: "periodic",
        ET_SPRNG: "spring",
        ET_DMPR: "damper", ET_FRCTN: "damper", ET_INRT: "damper",
    }

    def _effect_force(self, eff: Effect, position: float, velocity: float,
                      elapsed_ms: float, gains: Dict[str, float]) -> float:
        t = eff.type
        force = 0.0

        if t == ET_CONST:
            force = eff.magnitude * self._direction(eff)
        elif t == ET_RAMP:
            if eff.duration_ms > 0:
                k = max(0.0, min(1.0, elapsed_ms / eff.duration_ms))
            else:
                k = 0.0
            force = (eff.ramp_start + (eff.ramp_end - eff.ramp_start) * k) \
                * self._direction(eff)
        elif t in (ET_SINE, ET_SQR, ET_TRNGL, ET_STUP, ET_STDN):
            force = self._periodic(eff, elapsed_ms) * self._direction(eff)
        elif t == ET_SPRNG:
            force = self._conditional(eff, position)
        elif t == ET_DMPR:
            force = self._conditional(eff, velocity)
        elif t == ET_FRCTN:
            sign = 0.0 if abs(velocity) < 1e-3 else (1.0 if velocity > 0 else -1.0)
            force = self._conditional(eff, sign)
        elif t == ET_INRT:
            # bezwladnosc wymaga przyspieszenia - pomijamy swiadomie,
            # zamiast zglaszac blad
            force = 0.0

        category_gain = gains.get(self._CATEGORY.get(t, "constant"), 1.0)
        return force * eff.gain * category_gain * self._envelope(eff, elapsed_ms)

    def _direction(self, eff: Effect) -> float:
        mode = self.direction_mode
        if mode == "ignore":
            return 1.0
        angle = eff.direction * 2.0 * math.pi / 255.0
        if mode == "cos":
            return math.cos(angle)
        if mode == "sin":
            return math.sin(angle)
        # auto: kierunek bierzemy z cosinusa, ale gdy wypada bliski zera
        # (gra uzywa znaku samej magnitudy) zostawiamy +1
        c = math.cos(angle)
        if abs(c) < 0.05:
            s = math.sin(angle)
            return 1.0 if abs(s) < 0.05 else (1.0 if s > 0 else -1.0)
        return 1.0 if c > 0 else -1.0

    @staticmethod
    def _periodic(eff: Effect, elapsed_ms: float) -> float:
        period = max(1, eff.period_ms)
        phase = (elapsed_ms / period + eff.phase_deg / 360.0) % 1.0
        t = eff.type
        if t == ET_SINE:
            w = math.sin(2.0 * math.pi * phase)
        elif t == ET_SQR:
            w = 1.0 if phase < 0.5 else -1.0
        elif t == ET_TRNGL:
            w = 4.0 * abs(phase - 0.5) - 1.0
        elif t == ET_STUP:
            w = 2.0 * phase - 1.0
        else:  # ET_STDN
            w = 1.0 - 2.0 * phase
        return max(-1.0, min(1.0, eff.offset + eff.amplitude * w))

    @staticmethod
    def _conditional(eff: Effect, metric: float) -> float:
        """Wspolny wzor dla sprezyny / tlumienia / tarcia (HID PID)."""
        delta = metric - eff.center
        if abs(delta) <= eff.deadband:
            return 0.0
        delta = delta - eff.deadband if delta > 0 else delta + eff.deadband
        if delta > 0:
            value = -delta * eff.pos_coeff
            limit = eff.pos_sat
        else:
            value = -delta * eff.neg_coeff
            limit = eff.neg_sat
        return max(-limit, min(limit, value))

    @staticmethod
    def _envelope(eff: Effect, elapsed_ms: float) -> float:
        if eff.attack_ms <= 0 and eff.fade_ms <= 0:
            return 1.0
        level = 1.0
        if eff.attack_ms > 0 and elapsed_ms < eff.attack_ms:
            k = elapsed_ms / eff.attack_ms
            level = eff.attack_level + (1.0 - eff.attack_level) * k
        if eff.duration_ms > 0 and eff.fade_ms > 0:
            fade_start = eff.duration_ms - eff.fade_ms
            if elapsed_ms > fade_start:
                k = min(1.0, (elapsed_ms - fade_start) / eff.fade_ms)
                level = min(level, 1.0 + (eff.fade_level - 1.0) * k)
        return max(0.0, min(1.0, level))

    def _log(self, level: str, msg: str) -> None:
        if self._on_log:
            try:
                self._on_log(level, msg)
            except Exception:
                pass
