"""
Przetwarzanie surowych odczytow z plytki na znormalizowane osie.

Wszystko co dotyczy kalibracji dzieje sie tutaj - plytka wysyla wylacznie
surowe liczby, wiec kazda zmiana ustawien dziala natychmiast, bez
programowania Arduino.
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from .config import (AppConfig, G29_BUTTON_NAMES, G29_DPAD, PAD_BUTTON_NAMES,
                     PAD_SOURCE_KEYS, PEDAL_KEYS, PedalConfig, SteerConfig)
from .outputs.xinput import XUSB, PadState
from .protocol import NUM_BUTTONS, StateFrame

#: Kierunki krzyzaka w kolejnosci konfiguracji: gora, prawo, dol, lewo.
HAT_DIRECTIONS = ("góra", "prawo", "dół", "lewo")


def _clamp01(v: float) -> float:
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def _clamp_sym(v: float) -> float:
    return -1.0 if v < -1.0 else (1.0 if v > 1.0 else v)


class PedalProcessor:
    """Surowy ADC -> 0..1 z kalibracja, martwa strefa, krzywa i filtrem."""

    def __init__(self) -> None:
        self._smoothed: float = 0.0
        self._primed = False

    def reset(self) -> None:
        self._smoothed = 0.0
        self._primed = False

    def process(self, raw: int, cfg: PedalConfig) -> float:
        lo, hi = cfg.raw_min, cfg.raw_max
        if hi == lo:
            value = 0.0
        else:
            value = (raw - lo) / (hi - lo)
        value = _clamp01(value)

        if cfg.invert:
            value = 1.0 - value

        # Martwa strefa: rozciagamy przedzial [dz_low, 1-dz_high] na pelne 0..1
        dz_lo = cfg.deadzone_low
        dz_hi = cfg.deadzone_high
        span = 1.0 - dz_lo - dz_hi
        if span <= 1e-6:
            value = 0.0 if value < 0.5 else 1.0
        else:
            value = _clamp01((value - dz_lo) / span)

        if cfg.gamma != 1.0:
            value = value ** cfg.gamma

        a = cfg.smoothing
        if a > 0.0:
            if not self._primed:
                self._smoothed = value
                self._primed = True
            else:
                self._smoothed += (value - self._smoothed) * (1.0 - a)
            value = self._smoothed
        else:
            self._smoothed = value
            self._primed = True

        return _clamp01(value)


class SteerProcessor:
    """Impulsy enkodera -> stopnie i os -1..1, z predkoscia katowa."""

    def __init__(self) -> None:
        self._smoothed_deg: float = 0.0
        self._primed = False
        self._last_deg: float = 0.0
        self._last_t: float = 0.0
        self._velocity: float = 0.0      # stopnie / s (wygladzone)

    def reset(self) -> None:
        self._smoothed_deg = 0.0
        self._primed = False
        self._last_deg = 0.0
        self._last_t = 0.0
        self._velocity = 0.0

    @property
    def velocity_deg_s(self) -> float:
        return self._velocity

    def process(self, counts: int, cfg: SteerConfig,
                now: Optional[float] = None) -> Tuple[float, float]:
        """Zwraca (stopnie, os -1..1)."""
        now = time.perf_counter() if now is None else now

        rel = counts - cfg.center_counts
        deg = rel * 360.0 / max(4, cfg.counts_per_rev)
        if cfg.invert:
            deg = -deg

        a = cfg.smoothing
        if a > 0.0:
            if not self._primed:
                self._smoothed_deg = deg
                self._primed = True
            else:
                self._smoothed_deg += (deg - self._smoothed_deg) * (1.0 - a)
            deg = self._smoothed_deg
        else:
            self._smoothed_deg = deg
            self._primed = True

        # predkosc katowa - potrzebna do tlumienia (damper) w FFB
        if self._last_t:
            dt = now - self._last_t
            if dt > 1e-4:
                inst = (deg - self._last_deg) / dt
                # filtr EMA, zeby nie szarpalo od szumu kwantyzacji
                self._velocity += (inst - self._velocity) * 0.25
                self._last_deg = deg
                self._last_t = now
        else:
            self._last_deg = deg
            self._last_t = now

        half = max(1.0, cfg.range_deg / 2.0)
        axis = _clamp_sym(deg / half)

        if cfg.linearity != 1.0:
            sign = -1.0 if axis < 0 else 1.0
            axis = sign * (abs(axis) ** cfg.linearity)

        return deg, axis


@dataclass
class ProcessedState:
    """Gotowy do wyslania do vJoy wynik przetworzenia jednej ramki."""
    steer_deg: float = 0.0
    steer_axis: float = 0.0             # -1..1
    steer_velocity: float = 0.0         # stopnie / s
    pedals: Dict[str, float] = None     # type: ignore[assignment]
    raw_axes: Tuple[int, ...] = ()
    raw_counts: int = 0
    buttons_raw: int = 0                # maska 32 bitow prosto z plytki
    buttons_effective: int = 0          # po uwzglednieniu inwersji (styk NC)
    out_buttons: List[int] = None       # type: ignore[assignment]
    hat: int = -1                       # -1 = srodek, 0..3 = gora/prawo/dol/lewo
    flags: int = 0
    seq: int = 0

    def __post_init__(self) -> None:
        if self.pedals is None:
            self.pedals = {k: 0.0 for k in PEDAL_KEYS}
        if self.out_buttons is None:
            self.out_buttons = []


class StateProcessor:
    """Spaja procesory osi i mapowanie przyciskow w jedna operacje."""

    def __init__(self) -> None:
        self.steer = SteerProcessor()
        self.pedals = {k: PedalProcessor() for k in PEDAL_KEYS}

    def reset(self) -> None:
        self.steer.reset()
        for p in self.pedals.values():
            p.reset()

    def process(self, frame: StateFrame, cfg: AppConfig,
                now: Optional[float] = None) -> ProcessedState:
        deg, axis = self.steer.process(frame.steer_counts, cfg.steer, now)

        pedals: Dict[str, float] = {}
        for key in PEDAL_KEYS:
            pcfg = cfg.pedals[key]
            if pcfg.source < 0 or pcfg.source >= len(frame.axes):
                pedals[key] = 0.0
                self.pedals[key].reset()
                continue
            pedals[key] = self.pedals[key].process(frame.axes[pcfg.source], pcfg)

        # Inwersja (styk normalnie zwarty) dziala na masce, jeszcze przed
        # krzyzakiem i mapowaniem - dzieki temu odwrocony przycisk zachowuje
        # sie tak samo niezaleznie od tego, do czego zostal przypisany.
        inv_mask = 0
        for i, flag in enumerate(cfg.buttons.invert):
            if flag and i < NUM_BUTTONS:
                inv_mask |= 1 << i
        effective = (frame.buttons ^ inv_mask) & 0xFFFFFFFF

        hat_src = cfg.buttons.hat
        hat = -1
        for direction, btn in enumerate(hat_src):
            if 0 <= btn < NUM_BUTTONS and (effective >> btn) & 1:
                hat = direction
                break

        hat_used = {b for b in hat_src if 0 <= b < NUM_BUTTONS}

        out_buttons: List[int] = []
        mapping = cfg.buttons.mapping
        for i in range(NUM_BUTTONS):
            if i in hat_used:
                continue
            if not ((effective >> i) & 1):
                continue
            target = mapping[i] if i < len(mapping) else 0
            if target > 0:
                out_buttons.append(target)

        return ProcessedState(
            steer_deg=deg,
            steer_axis=axis,
            steer_velocity=self.steer.velocity_deg_s,
            pedals=pedals,
            raw_axes=frame.axes,
            raw_counts=frame.steer_counts,
            buttons_raw=frame.buttons,
            buttons_effective=effective,
            out_buttons=out_buttons,
            hat=hat,
            flags=frame.flags,
            seq=frame.seq,
        )


# ----------------------------------------------------- tryb pada (XInput)

#: Kierunki krzyzaka w kolejnosci uzywanej przez ProcessedState.hat.
_HAT_TO_DPAD = ("DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT")


def _apply_deadzone(value: float, dz: float) -> float:
    """Martwa strefa wokol zera, z rozciagnieciem reszty na pelny zakres."""
    if dz <= 0.0:
        return value
    a = abs(value)
    if a <= dz:
        return 0.0
    span = 1.0 - dz
    if span <= 1e-6:
        return 1.0 if value > 0 else -1.0
    scaled = (a - dz) / span
    return scaled if value > 0 else -scaled


def _apply_antideadzone(value: float, anti: float) -> float:
    """Kompensacja martwej strefy GRY - odwrotnosc _apply_deadzone.

    Gra ucina poczatek wychylenia osi, wiec pierwsze stopnie obrotu
    kierownicy nie robia w niej nic. Przesuwamy caly niezerowy zakres nad ten
    prog: wejscie 0 zostaje zerem (auto nie moze skrecac samo), a najmniejszy
    ruch od razu daje `anti`. Wejscie 1 nadal daje 1, wiec maksymalny skret
    sie nie zmienia.
    """
    if anti <= 0.0:
        return value
    a = abs(value)
    if a <= 1e-9:
        return 0.0
    scaled = anti + (1.0 - anti) * a
    scaled = 1.0 if scaled > 1.0 else scaled
    return scaled if value > 0 else -scaled


def pad_sources(state: ProcessedState, cfg: AppConfig) -> Dict[str, float]:
    """Wartosci wszystkich zrodel pada, kazda 0..1.

    Kierownica jest dwukierunkowa, wiec rozpada sie na dwie polowki -
    dzieki temu kazde zrodlo da sie wpiac w dowolny kierunek drazka albo
    na spust, bez zadnych wyjatkow w dalszym kodzie.
    """
    pad = cfg.pad
    half = max(1.0, pad.steer_range_deg / 2.0)
    steer = _apply_deadzone(_clamp_sym(state.steer_deg / half), pad.steer_deadzone)
    # Kolejnosc ma znaczenie: najpierw krzywa (ksztaltuje srodek zakresu),
    # dopiero potem kompensacja progu gry - inaczej krzywa wciagnelaby
    # kompensacje z powrotem w martwa strefe.
    if pad.steer_linearity != 1.0:
        sign = -1.0 if steer < 0 else 1.0
        steer = sign * (abs(steer) ** pad.steer_linearity)
    steer = _apply_antideadzone(steer, pad.steer_antideadzone)
    if pad.steer_invert:
        steer = -steer

    out: Dict[str, float] = {
        "steer_left": max(0.0, -steer),
        "steer_right": max(0.0, steer),
    }
    for key in PEDAL_KEYS:
        out[key] = (0.0 if cfg.pedals[key].source < 0
                    else _clamp01(state.pedals[key]))
    return out


def build_pad_state(state: ProcessedState, cfg: AppConfig) -> PadState:
    """Zamienia przetworzony stan kierownicy na raport pada Xbox 360.

    Kazda polowka osi ma wlasne zrodlo, wiec os drazka to po prostu
    roznica obu polowek: LX = (w prawo) - (w lewo). Drazki trafiaja na
    -32768..32767, spusty na 0..255 (to ograniczenie XInputa - stad
    mniejsza rozdzielczosc pedalow niz w trybie kierownicy).
    """
    pad = cfg.pad
    src = pad_sources(state, cfg)

    def half(direction: str) -> float:
        key = pad.axes.get(direction, "")
        return src.get(key, 0.0) if key in PAD_SOURCE_KEYS else 0.0

    def stick(axis: str) -> int:
        v = _clamp_sym(half(f"{axis}+") - half(f"{axis}-"))
        return int(round(v * (32767 if v >= 0 else 32768)))

    def trigger(name: str) -> int:
        v = _apply_antideadzone(_clamp01(half(name)), pad.trigger_antideadzone)
        return int(round(_clamp01(v) * 255))

    # --- przyciski ---
    buttons = 0
    assigned_dpad = set()
    for name in PAD_BUTTON_NAMES:
        idx = pad.buttons.get(name, -1)
        if not (0 <= idx < NUM_BUTTONS):
            continue
        if name.startswith("DPAD_"):
            assigned_dpad.add(name)
        if (state.buttons_effective >> idx) & 1:
            buttons |= XUSB.get(name, 0)

    # Krzyzak z zakladki Przyciski dziala takze w trybie pada, ale tylko dla
    # kierunkow, ktorym nie przypisano tu wlasnego przycisku.
    if 0 <= state.hat < len(_HAT_TO_DPAD):
        name = _HAT_TO_DPAD[state.hat]
        if name not in assigned_dpad:
            buttons |= XUSB[name]

    return PadState(
        buttons=buttons,
        lt=trigger("LT"),
        rt=trigger("RT"),
        lx=stick("LX"),
        ly=stick("LY"),
        rx=stick("RX"),
        ry=stick("RY"),
    )


# ------------------------------------------------- tryb G29 (Native USB)

#: Krzyzak G29 liczy co 45 stopni od gory zgodnie z zegarem; 8 = srodek.
#: Klucz to (prawo - lewo, gora - dol).
_G29_HAT = {(0, 1): 0, (1, 1): 1, (1, 0): 2, (1, -1): 3,
            (0, -1): 4, (-1, -1): 5, (-1, 0): 6, (-1, 1): 7}
G29_HAT_CENTER = 8
#: Wartosc krzyzaka -> wcisniete kierunki (do podgladu w interfejsie).
_G29_HAT_DIRS = {v: k for k, v in _G29_HAT.items()}


def _g29_pedal(value: float, inverted: bool) -> int:
    v = int(round(_clamp01(value) * 255))
    return 255 - v if inverted else v


def build_g29_report(state: ProcessedState, cfg: AppConfig) -> bytes:
    """Stan kierownicy -> 12-bajtowy raport G29 (uklad w g29_usb.h).

    Skret bierze os z zakladki Kierownica (zakres, krzywa i odwrocenie sa
    juz policzone w steer_axis), pedaly - wyniki kalibracji, a przyciski -
    wlasne przypisania G29 (zakladka G29): kontrolka -> przycisk plytki.
    """
    assign = cfg.g29.buttons
    eff = state.buttons_effective

    def pressed(name: str) -> Optional[bool]:
        """True/False dla przypisanej kontrolki, None gdy gniazdo puste."""
        idx = assign.get(name, -1)
        if not (0 <= idx < NUM_BUTTONS):
            return None
        return bool((eff >> idx) & 1)

    # przyciski 1..25 leza w bitach 4..28 pierwszych czterech bajtow
    mask = 0
    for i, name in enumerate(G29_BUTTON_NAMES):
        if pressed(name):
            mask |= 1 << (4 + i)

    # Krzyzak: wlasne przypisania G29, a kierunki bez przypisania biora
    # krzyzak z zakladki Przyciski - tak samo jak w trybie pada.
    dirs = []
    for d, name in enumerate(G29_DPAD):
        on = pressed(name)
        dirs.append(state.hat == d if on is None else on)
    up, right, down, left = dirs
    key = (int(right) - int(left), int(up) - int(down))
    mask |= _G29_HAT.get(key, G29_HAT_CENTER)

    axis = state.steer_axis
    axis = 0.0 if axis != axis else (-1.0 if axis < -1.0 else (1.0 if axis > 1.0 else axis))
    wheel = int(round((axis + 1.0) * 32767.5))
    wheel = 0 if wheel < 0 else (65535 if wheel > 65535 else wheel)

    inv = cfg.g29.pedals_inverted

    def pedal(key: str) -> int:
        if cfg.pedals[key].source < 0:
            return _g29_pedal(0.0, inv)        # wylaczony kanal = puszczony
        return _g29_pedal(state.pedals.get(key, 0.0), inv)

    return bytes((
        mask & 0xFF, (mask >> 8) & 0xFF, (mask >> 16) & 0xFF, (mask >> 24) & 0xFF,
        wheel & 0xFF, wheel >> 8,
        pedal("throttle"),      # Z  - gaz
        pedal("brake"),         # Rz - hamulec
        pedal("clutch"),        # Y  - sprzeglo
        0x80, 0x80,             # drazek skrzyni w spoczynku
        0x00,
    ))


def g29_active(report: bytes) -> Dict[str, bool]:
    """Odwrotnosc build_g29_report dla przyciskow: co jest wcisniete w raporcie.

    Interfejs pokazuje dzieki temu dokladnie to, co dostaje gra, a nie to,
    co mysli, ze wyslal.
    """
    out = {name: False for name in G29_BUTTON_NAMES + G29_DPAD}
    if len(report) < 4:
        return out
    mask = int.from_bytes(report[:4], "little")
    for i, name in enumerate(G29_BUTTON_NAMES):
        out[name] = bool(mask & (1 << (4 + i)))
    dx, dy = _G29_HAT_DIRS.get(mask & 0x0F, (0, 0))
    out["DPAD_UP"] = dy > 0
    out["DPAD_DOWN"] = dy < 0
    out["DPAD_RIGHT"] = dx > 0
    out["DPAD_LEFT"] = dx < 0
    return out
