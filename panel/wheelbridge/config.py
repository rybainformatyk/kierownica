"""
Konfiguracja aplikacji - dataclasses + zapis/odczyt JSON.

Kalibracja i mapowanie zyja TUTAJ, nie w firmware. Dzieki temu zmiana
zakresu obrotu, martwej strefy czy mapowania przyciskow nie wymaga
ponownego programowania plytki.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, asdict, fields
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import protocol

APP_NAME = "KierownicaPanel"

#: Nazwy osi wyjsciowych vJoy w kolejnosci uzywanej przez aplikacje.
PEDAL_KEYS = ("throttle", "brake", "clutch", "aux1", "aux2", "aux3")
PEDAL_LABELS = {
    "throttle": "Gaz",
    "brake": "Hamulec",
    "clutch": "Sprzeglo",
    "aux1": "AUX 1",
    "aux2": "AUX 2",
    "aux3": "AUX 3",
}

#: Domyslne przypisanie osi wyjsciowych wg ukladu Logitech G29
#: (G29: X = kierownica, Y = gaz, Z = hamulec, RZ = sprzeglo).
DEFAULT_VJOY_AXIS = {
    "steer": "X",
    "throttle": "Y",
    "brake": "Z",
    "clutch": "RZ",
    "aux1": "RX",
    "aux2": "RY",
    "aux3": "SL0",
}

VJOY_AXIS_CHOICES = ("X", "Y", "Z", "RX", "RY", "RZ", "SL0", "SL1", "WHL", "-")


def config_path() -> Path:
    """Obok skryptu jesli da sie pisac, inaczej w %APPDATA%."""
    local = Path(__file__).resolve().parent.parent / "config.json"
    try:
        local.parent.mkdir(parents=True, exist_ok=True)
        probe = local.parent / ".write_test"
        probe.write_text("x", encoding="utf-8")
        probe.unlink()
        return local
    except OSError:
        base = os.environ.get("APPDATA") or str(Path.home())
        p = Path(base) / APP_NAME
        p.mkdir(parents=True, exist_ok=True)
        return p / "config.json"


# --------------------------------------------------------------- dataclasses

@dataclass
class PedalConfig:
    """Jeden kanal analogowy: surowy ADC 0..4095 -> znormalizowane 0..1."""
    source: int = 0                 # indeks kanalu ADC (0 = A0 ... 5 = A5), -1 = wylaczony
    raw_min: int = 0
    raw_max: int = 4095
    invert: bool = False
    deadzone_low: float = 0.02      # 0..0.45 - ucina szum na poczatku skoku
    deadzone_high: float = 0.02     # 0..0.45 - gwarantuje osiagniecie 100 %
    gamma: float = 1.0              # < 1 agresywniej na poczatku, > 1 lagodniej
    smoothing: float = 0.0          # 0..0.95 filtr EMA
    vjoy_axis: str = "Y"


@dataclass
class SteerConfig:
    counts_per_rev: int = 2400      # impulsy enkodera x4 na obrot kierownicy
    range_deg: int = 900            # pelny zakres obrotu (lock-to-lock)
    invert: bool = False
    smoothing: float = 0.0
    linearity: float = 1.0          # 1.0 = liniowo
    vjoy_axis: str = "X"
    center_counts: int = 0          # offset srodka ustawiany w aplikacji
    soft_endstop: bool = True       # miekki ogranicznik w firmware (wymaga FFB)
    endstop_gain: int = 8


@dataclass
class FfbConfig:
    enabled: bool = False
    overall_gain: float = 0.70      # 0..1 - globalne wzmocnienie
    min_force: float = 0.00         # 0..0.5 - pokonanie tarcia mostka/silnika
    max_force: float = 1.00         # 0..1 - ograniczenie pradu
    invert: bool = False
    deadband: float = 0.01          # 0..0.2 - ponizej tego nie ruszamy silnika
    slew_per_ms: int = 4000         # max zmiana momentu na ms (ochrona mechaniki)
    timeout_ms: int = 200           # watchdog - brak komendy = zwolnienie silnika
    min_duty: int = 0               # 0..255 PWM
    max_duty: int = 255             # 0..255 PWM
    update_hz: int = 400
    # Efekty generowane lokalnie (dzialaja takze w grach bez FFB)
    spring_gain: float = 0.0        # samocentrowanie
    damper_gain: float = 0.0        # tlumienie predkosci obrotu
    friction_gain: float = 0.0
    # Skalowanie efektow przychodzacych z gry
    game_constant_gain: float = 1.0
    game_spring_gain: float = 1.0
    game_damper_gain: float = 1.0
    game_periodic_gain: float = 1.0


@dataclass
class ButtonsConfig:
    #: 32 pozycje: indeks = przycisk plytki, wartosc = numer przycisku vJoy
    #: (1-based) lub 0 = nieuzywany.
    mapping: List[int] = field(default_factory=lambda: list(range(1, 33)))
    #: Przyciski plytki (0-based) uzyte jako krzyzak: gora, prawo, dol, lewo.
    #: -1 = nieuzywany. Przypisany przycisk NIE jest juz wysylany jako przycisk.
    hat: List[int] = field(default_factory=lambda: [-1, -1, -1, -1])
    #: Odwrocenie logiki przycisku (styk normalnie zwarty / NC). True oznacza,
    #: ze zwarty styk to "puszczony", a rozwarty to "wcisniety".
    invert: List[bool] = field(
        default_factory=lambda: [False] * protocol.NUM_BUTTONS)


#: Kontrolki wirtualnego pada, ktorym mozna przypisac przyciski plytki.
PAD_BUTTON_NAMES = ("A", "B", "X", "Y", "LB", "RB", "LS", "RS",
                    "BACK", "START", "GUIDE",
                    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT")

#: Domyslne przypisanie - pierwsze przyciski plytki na najczesciej uzywane
#: kontrolki pada. GUIDE celowo zostaje pusty (otwiera nakladke Xboxa).
DEFAULT_PAD_BUTTONS = {
    "A": 0, "B": 1, "X": 2, "Y": 3,
    "LB": 4, "RB": 5, "LS": 8, "RS": 9,
    "BACK": 6, "START": 7, "GUIDE": -1,
    "DPAD_UP": 10, "DPAD_DOWN": 11, "DPAD_LEFT": 12, "DPAD_RIGHT": 13,
}

#: Kierunki wirtualnego pada. Kazda polowka osi jest osobnym gniazdem -
#: dokladnie jak w x360ce - dzieki czemu "lewy drazek w lewo" i "lewy drazek
#: w gore" moga miec dwa zupelnie rozne zrodla.
#: Dla osi Y "+" to gora (tak liczy XInput), wiec kolejnosc jest celowa.
PAD_DIRECTIONS = ("LX-", "LX+", "LY+", "LY-",
                  "RX-", "RX+", "RY+", "RY-",
                  "LT", "RT")

#: Pelne nazwy - do menu, dziennika i komunikatow.
PAD_DIRECTION_LABELS = {
    "LX-": "Lewy drazek w lewo",
    "LX+": "Lewy drazek w prawo",
    "LY+": "Lewy drazek w gore",
    "LY-": "Lewy drazek w dol",
    "RX-": "Prawy drazek w lewo",
    "RX+": "Prawy drazek w prawo",
    "RY+": "Prawy drazek w gore",
    "RY-": "Prawy drazek w dol",
    "LT": "Lewy spust (LT)",
    "RT": "Prawy spust (RT)",
}

#: Krotkie nazwy - do wierszy w interfejsie, gdzie grupa juz mowi, o ktory
#: drazek chodzi.
PAD_DIRECTION_SHORT = {
    "LX-": "w lewo",
    "LX+": "w prawo",
    "LY+": "w gore",
    "LY-": "w dol",
    "RX-": "w lewo",
    "RX+": "w prawo",
    "RY+": "w gore",
    "RY-": "w dol",
    "LT": "LT - lewy spust",
    "RT": "RT - prawy spust",
}

#: Zrodla analogowe, ktore mozna wpiac w kierunek pada. Kazde daje 0..1,
#: wiec kazde pasuje do kazdego kierunku. Kierownica jest dwukierunkowa,
#: dlatego wystepuje jako dwie polowki.
PAD_SOURCE_KEYS = ("steer_left", "steer_right", "throttle", "brake",
                   "clutch", "aux1", "aux2", "aux3")
PAD_SOURCE_LABELS = {
    "steer_left": "Kierownica w lewo",
    "steer_right": "Kierownica w prawo",
    "throttle": "Gaz",
    "brake": "Hamulec",
    "clutch": "Sprzeglo",
    "aux1": "AUX 1",
    "aux2": "AUX 2",
    "aux3": "AUX 3",
}

#: Ktory kanal pedalu stoi za danym zrodlem (kierownica nie ma kanalu ADC).
PAD_SOURCE_PEDAL = {
    "throttle": "throttle", "brake": "brake", "clutch": "clutch",
    "aux1": "aux1", "aux2": "aux2", "aux3": "aux3",
}

#: Domyslnie: skret na lewym drazku, gaz na prawym spuscie, hamulec na lewym.
DEFAULT_PAD_AXES = {
    "LX-": "steer_left",
    "LX+": "steer_right",
    "RT": "throttle",
    "LT": "brake",
}

#: Stary uklad konfiguracji (jedno zrodlo -> cala os). Zostawione wylacznie
#: do migracji plikow zapisanych przed podzialem osi na kierunki.
_LEGACY_PAD_TARGETS = ("-", "LX", "LY", "RX", "RY", "LT", "RT")


def pad_axes_from_legacy(raw_pad: Dict[str, Any]) -> Dict[str, str]:
    """Przepisuje stare pola `<zrodlo>_target` na gniazda kierunkow."""
    axes: Dict[str, str] = {}
    for key in ("steer",) + PEDAL_KEYS:
        target = raw_pad.get(f"{key}_target")
        if target not in _LEGACY_PAD_TARGETS or target == "-":
            continue
        if key == "steer":
            if target in ("LT", "RT"):
                # skret na spuscie byl mapowany symetrycznie na 0..1; nowy
                # model nie ma takiej polowki, wiec zostaje sam prawy skret
                axes[target] = "steer_right"
            else:
                axes[f"{target}-"] = "steer_left"
                axes[f"{target}+"] = "steer_right"
        else:
            # pedal daje 0..1, czyli dodatnia polowke osi
            axes[target if target in ("LT", "RT") else f"{target}+"] = key
    return axes


#: Gotowe nastawy trybu pada pod konkretne gry. Klucz to nazwa profilu,
#: wartosc to pola PadConfig, ktore profil ustawia. Mapowanie przyciskow i
#: kierunkow NIE jest czescia profilu - profil dotyczy wylacznie tego, jak
#: skret i pedaly sa skalowane dla gry.
PAD_PROFILES: Dict[str, Dict[str, Any]] = {
    "nfs": {
        "steer_range_deg": 360,
        "steer_deadzone": 0.0,
        "steer_antideadzone": 0.0,
        "steer_linearity": 1.0,
        "trigger_antideadzone": 0.0,
    },
    # Forza (Horizon i Motorsport) ma wlasna martwa strefe drazka, wiec
    # pierwsze kilkanascie procent obrotu kierownicy nie robi w grze nic.
    # Kompensacja po naszej stronie sprawia, ze najmniejszy ruch od razu
    # przebija ten prog. Najlepszy wynik daje dodatkowo wyzerowanie
    # "martwej strefy wewnetrznej" w ustawieniach zaawansowanych gry.
    "forza": {
        "steer_range_deg": 450,
        "steer_deadzone": 0.0,
        "steer_antideadzone": 0.15,
        "steer_linearity": 1.0,
        "trigger_antideadzone": 0.0,
    },
}

#: Kolejnosc i nazwy w interfejsie. "" to profil wlasny - nic nie ustawia.
PAD_PROFILE_ORDER = ("", "nfs", "forza")
PAD_PROFILE_LABELS = {
    "": "Wlasny",
    "nfs": "NFS i gry zrecznosciowe",
    "forza": "Forza Horizon / Motorsport",
}


@dataclass
class PadConfig:
    """Tryb pada: jak kierownica i pedaly maja wygladac dla gry XInput."""
    #: Ile stopni obrotu kierownicy odpowiada pelnemu wychyleniu drazka.
    #: Pad ma zakres drazka duzo mniejszy niz kierownica, wiec dla gier
    #: zrecznosciowych (NFS) sensowne jest 270-450, a nie pelne 900.
    steer_range_deg: int = 360
    steer_deadzone: float = 0.0
    #: Kompensacja martwej strefy GRY ("anty-martwa strefa"). Gra sama ucina
    #: poczatek wychylenia drazka, wiec pierwsze stopnie obrotu kierownicy nic
    #: nie robia. Tutaj najmniejszy ruch wychodzi od razu nad prog gry: 0 na
    #: wejsciu zostaje zerem, cokolwiek wiecej startuje od tej wartosci.
    steer_antideadzone: float = 0.0
    #: To samo dla spustow LT/RT - gry maja osobne martwe strefy gazu
    #: i hamulca.
    trigger_antideadzone: float = 0.0
    #: Krzywa skretu na padzie: < 1 czulszy srodek, > 1 lagodniejszy srodek.
    steer_linearity: float = 1.0
    #: Profil gry, z ktorego pochodza powyzsze wartosci ("" = wlasne).
    profile: str = ""
    steer_invert: bool = False
    #: kierunek pada (patrz PAD_DIRECTIONS) -> klucz zrodla, "" = nieuzywany
    axes: Dict[str, str] = field(
        default_factory=lambda: dict(DEFAULT_PAD_AXES))
    #: nazwa kontrolki pada -> indeks przycisku plytki (0..31), -1 = brak
    buttons: Dict[str, int] = field(
        default_factory=lambda: dict(DEFAULT_PAD_BUTTONS))

    def apply_profile(self, key: str) -> bool:
        """Wstawia nastawy profilu gry. False, gdy taki profil nie istnieje."""
        values = PAD_PROFILES.get(key)
        if not values:
            self.profile = ""
            return False
        for name, value in values.items():
            setattr(self, name, value)
        self.profile = key
        return True

    def matches_profile(self, key: str) -> bool:
        """Czy aktualne wartosci sa dokladnie takie jak w profilu."""
        values = PAD_PROFILES.get(key)
        if not values:
            return False
        return all(abs(float(getattr(self, name)) - float(value)) < 1e-6
                   for name, value in values.items())

    def detect_profile(self) -> str:
        """Nazwa profilu pasujacego do aktualnych wartosci albo ""."""
        for key in PAD_PROFILE_ORDER:
            if key and self.matches_profile(key):
                return key
        return ""

    def source_of(self, direction: str) -> str:
        """Zrodlo wpiete w ten kierunek albo "" gdy puste."""
        return self.axes.get(direction, "")

    def directions_of(self, source: str) -> List[str]:
        """Kierunki, ktorymi steruje dane zrodlo (moze byc kilka)."""
        return [d for d in PAD_DIRECTIONS if self.axes.get(d) == source]


@dataclass
class LinkConfig:
    port: str = ""                  # "" = autodetekcja
    baudrate: int = protocol.DEFAULT_BAUD
    auto_connect: bool = True
    report_hz: int = 333


#: Tryby wyjscia. "g29" to kierownica Logitech G29 wystawiana przez sama
#: plytke na porcie Native USB - gry widza prawdziwe G29, wiec dziala takze
#: tam, gdzie gra przyjmuje tylko kierownice ze swojej listy (Forza).
#: "wheel" to kierownica DirectInput na vJoy (Force Feedback z gry), "pad"
#: to wirtualny Xbox 360 na ViGEm (gry czytajace tylko XInput), "both"
#: wystawia vJoy i pada naraz.
OUTPUT_MODES = ("g29", "wheel", "pad", "both", "none")
OUTPUT_MODE_LABELS = {
    "g29": "Kierownica G29 (Native USB) - kazda gra",
    "wheel": "Kierownica (vJoy) - symulatory, Force Feedback",
    "pad": "Pad Xbox 360 (ViGEm) - gry XInput, np. NFS",
    "both": "Oba naraz - kierownica i pad",
    "none": "Tylko podglad - nic nie wysylaj",
}


#: Przyciski G29 w kolejnosci z raportu: pozycja + 1 = numer przycisku,
#: ktory widzi gra (1 krzyzyk ... 25 PS).
G29_BUTTON_NAMES = (
    "CROSS", "SQUARE", "CIRCLE", "TRIANGLE",
    "PADDLE_R", "PADDLE_L", "R2", "L2",
    "SHARE", "OPTIONS", "R3", "L3",
    "GEAR_1", "GEAR_2", "GEAR_3", "GEAR_4", "GEAR_5", "GEAR_6", "GEAR_R",
    "PLUS", "MINUS", "DIAL_CW", "DIAL_CCW", "ENTER", "PS",
)
#: Krzyzak G29 to os hat - kolejnosc jak ProcessedState.hat.
G29_DPAD = ("DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT")
#: Wszystkie kontrolki G29, ktorym przypisuje sie przycisk plytki.
G29_CONTROLS = G29_BUTTON_NAMES + G29_DPAD

G29_CONTROL_LABELS = {
    "CROSS": "Krzyzyk", "SQUARE": "Kwadrat", "CIRCLE": "Kolko",
    "TRIANGLE": "Trojkat",
    "PADDLE_R": "Prawa lopatka", "PADDLE_L": "Lewa lopatka",
    "R2": "R2", "L2": "L2", "R3": "R3", "L3": "L3",
    "SHARE": "Share", "OPTIONS": "Options", "PS": "PS",
    "GEAR_1": "Bieg 1", "GEAR_2": "Bieg 2", "GEAR_3": "Bieg 3",
    "GEAR_4": "Bieg 4", "GEAR_5": "Bieg 5", "GEAR_6": "Bieg 6",
    "GEAR_R": "Wsteczny",
    "PLUS": "Plus (+)", "MINUS": "Minus (-)",
    "DIAL_CW": "Pokretlo w prawo", "DIAL_CCW": "Pokretlo w lewo",
    "ENTER": "Enter (pokretlo)",
    "DPAD_UP": "Krzyzak w gore", "DPAD_RIGHT": "Krzyzak w prawo",
    "DPAD_DOWN": "Krzyzak w dol", "DPAD_LEFT": "Krzyzak w lewo",
}

#: Odpowiedniki kontrolek pada na G29 - po tym przenosimy przypisania
#: zrobione w zakladce Pad, zeby nie trzeba bylo ich ustawiac drugi raz.
PAD_TO_G29 = {
    "A": "CROSS", "B": "CIRCLE", "X": "SQUARE", "Y": "TRIANGLE",
    "LB": "PADDLE_L", "RB": "PADDLE_R",
    "LS": "L3", "RS": "R3",
    "BACK": "SHARE", "START": "OPTIONS", "GUIDE": "PS",
    "DPAD_UP": "DPAD_UP", "DPAD_DOWN": "DPAD_DOWN",
    "DPAD_LEFT": "DPAD_LEFT", "DPAD_RIGHT": "DPAD_RIGHT",
}


def g29_buttons_from_pad(pad_buttons: Dict[str, int]) -> Dict[str, int]:
    """Przypisania G29 wyliczone z przypisan pada; reszta zostaje pusta."""
    out = {name: -1 for name in G29_CONTROLS}
    for pad_name, g29_name in PAD_TO_G29.items():
        try:
            idx = int(pad_buttons.get(pad_name, -1))
        except (TypeError, ValueError):
            idx = -1
        out[g29_name] = idx if 0 <= idx < protocol.NUM_BUTTONS else -1
    return out


@dataclass
class G29Config:
    """Tryb G29: raport skladany w aplikacji, wypychany przez plytke."""
    #: Prawdziwe G29 raportuje pedaly odwrotnie: 255 = puszczony, 0 = wcisniety
    #: do konca. Profile gier dla G29 tego oczekuja, wiec domyslnie tak samo.
    pedals_inverted: bool = True
    #: kontrolka G29 (G29_CONTROLS) -> przycisk plytki 0..31, -1 = brak.
    #: Domyslnie to samo, co domyslne przypisania pada.
    buttons: Dict[str, int] = field(
        default_factory=lambda: g29_buttons_from_pad(DEFAULT_PAD_BUTTONS))


@dataclass
class OutputConfig:
    mode: str = "g29"               # patrz OUTPUT_MODES
    vjoy_device: int = 1            # 1..16
    auto_acquire: bool = True

    @property
    def wheel_on(self) -> bool:
        return self.mode in ("wheel", "both")

    @property
    def pad_on(self) -> bool:
        return self.mode in ("pad", "both")

    @property
    def g29_on(self) -> bool:
        return self.mode == "g29"


@dataclass
class AppConfig:
    link: LinkConfig = field(default_factory=LinkConfig)
    output: OutputConfig = field(default_factory=OutputConfig)
    steer: SteerConfig = field(default_factory=SteerConfig)
    pedals: Dict[str, PedalConfig] = field(default_factory=dict)
    ffb: FfbConfig = field(default_factory=FfbConfig)
    buttons: ButtonsConfig = field(default_factory=ButtonsConfig)
    pad: PadConfig = field(default_factory=PadConfig)
    g29: G29Config = field(default_factory=G29Config)

    def __post_init__(self) -> None:
        for i, key in enumerate(PEDAL_KEYS):
            if key not in self.pedals:
                self.pedals[key] = PedalConfig(
                    source=i, vjoy_axis=DEFAULT_VJOY_AXIS.get(key, "-")
                )
        self.normalize()

    # ----------------------------------------------------------- walidacja

    def normalize(self) -> None:
        """Doprowadza wszystkie wartosci do dozwolonych zakresow.

        Wywolywane po wczytaniu pliku - uszkodzony lub recznie edytowany
        config nie moze wywrocic aplikacji.
        """
        s = self.steer
        s.counts_per_rev = max(4, min(2_000_000, int(s.counts_per_rev)))
        s.range_deg = max(45, min(1800, int(s.range_deg)))
        s.smoothing = _clamp(s.smoothing, 0.0, 0.95)
        s.linearity = _clamp(s.linearity, 0.3, 3.0)
        s.endstop_gain = max(1, min(65535, int(s.endstop_gain)))
        if s.vjoy_axis not in VJOY_AXIS_CHOICES:
            s.vjoy_axis = "X"

        for key in PEDAL_KEYS:
            p = self.pedals.get(key)
            if p is None:
                p = self.pedals[key] = PedalConfig()
            p.source = int(p.source)
            if not (-1 <= p.source < protocol.NUM_AXES):
                p.source = -1
            p.raw_min = max(0, min(4095, int(p.raw_min)))
            p.raw_max = max(0, min(4095, int(p.raw_max)))
            if p.raw_min > p.raw_max:
                # odwrocony zakres zapisujemy jako normalny + flaga inwersji,
                # zeby dalej w kodzie min < max bylo zawsze prawda
                p.raw_min, p.raw_max = p.raw_max, p.raw_min
                p.invert = not p.invert
            if p.raw_max - p.raw_min < 32:             # bezsensowna kalibracja
                p.raw_min, p.raw_max = 0, 4095
            p.deadzone_low = _clamp(p.deadzone_low, 0.0, 0.45)
            p.deadzone_high = _clamp(p.deadzone_high, 0.0, 0.45)
            p.gamma = _clamp(p.gamma, 0.3, 3.0)
            p.smoothing = _clamp(p.smoothing, 0.0, 0.95)
            if p.vjoy_axis not in VJOY_AXIS_CHOICES:
                p.vjoy_axis = DEFAULT_VJOY_AXIS.get(key, "-")

        f = self.ffb
        f.overall_gain = _clamp(f.overall_gain, 0.0, 1.0)
        f.min_force = _clamp(f.min_force, 0.0, 0.5)
        f.max_force = _clamp(f.max_force, 0.05, 1.0)
        f.deadband = _clamp(f.deadband, 0.0, 0.2)
        f.slew_per_ms = max(256, min(65280, int(f.slew_per_ms)))
        f.timeout_ms = max(20, min(2000, int(f.timeout_ms)))
        f.min_duty = max(0, min(254, int(f.min_duty)))
        f.max_duty = max(f.min_duty + 1, min(255, int(f.max_duty)))
        f.update_hz = max(50, min(1000, int(f.update_hz)))
        for name in ("spring_gain", "damper_gain", "friction_gain"):
            setattr(f, name, _clamp(getattr(f, name), 0.0, 1.0))
        for name in ("game_constant_gain", "game_spring_gain",
                     "game_damper_gain", "game_periodic_gain"):
            setattr(f, name, _clamp(getattr(f, name), 0.0, 2.0))

        b = self.buttons
        mapping = [int(x) for x in (b.mapping or [])][:protocol.NUM_BUTTONS]
        mapping += list(range(len(mapping) + 1, protocol.NUM_BUTTONS + 1))
        b.mapping = [max(0, min(128, v)) for v in mapping[:protocol.NUM_BUTTONS]]
        hat = [int(x) for x in (b.hat or [])][:4]
        hat += [-1] * (4 - len(hat))
        b.hat = [v if -1 <= v < protocol.NUM_BUTTONS else -1 for v in hat]
        inv = [bool(x) for x in (b.invert or [])][:protocol.NUM_BUTTONS]
        inv += [False] * (protocol.NUM_BUTTONS - len(inv))
        b.invert = inv

        d = self.pad
        d.steer_range_deg = max(45, min(1800, int(d.steer_range_deg)))
        d.steer_deadzone = _clamp(d.steer_deadzone, 0.0, 0.45)
        d.steer_antideadzone = _clamp(d.steer_antideadzone, 0.0, 0.60)
        d.trigger_antideadzone = _clamp(d.trigger_antideadzone, 0.0, 0.60)
        d.steer_linearity = _clamp(d.steer_linearity, 0.3, 3.0)
        if not isinstance(d.profile, str) or d.profile not in PAD_PROFILES:
            d.profile = ""
        raw_axes = d.axes if isinstance(d.axes, dict) else {}
        d.axes = {name: (raw_axes.get(name) or "")
                  if raw_axes.get(name) in PAD_SOURCE_KEYS else ""
                  for name in PAD_DIRECTIONS}
        fixed: Dict[str, int] = {}
        for key in PAD_BUTTON_NAMES:
            try:
                v = int(d.buttons.get(key, -1))
            except (TypeError, ValueError):
                v = -1
            fixed[key] = v if 0 <= v < protocol.NUM_BUTTONS else -1
        d.buttons = fixed

        self.link.baudrate = max(9600, min(2_000_000, int(self.link.baudrate)))
        self.link.report_hz = max(50, min(500, int(self.link.report_hz)))
        self.output.vjoy_device = max(1, min(16, int(self.output.vjoy_device)))
        if self.output.mode not in OUTPUT_MODES:
            self.output.mode = "g29"
        self.g29.pedals_inverted = bool(self.g29.pedals_inverted)
        raw_g29 = self.g29.buttons if isinstance(self.g29.buttons, dict) else {}
        fixed_g29: Dict[str, int] = {}
        for key in G29_CONTROLS:
            try:
                v = int(raw_g29.get(key, -1))
            except (TypeError, ValueError):
                v = -1
            fixed_g29[key] = v if 0 <= v < protocol.NUM_BUTTONS else -1
        self.g29.buttons = fixed_g29

    # -------------------------------------------------------- serializacja

    def to_dict(self) -> Dict[str, Any]:
        return {"version": 1, **asdict(self)}

    def save(self, path: Optional[Path] = None) -> Path:
        path = path or config_path()
        tmp = path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(self.to_dict(), indent=2, ensure_ascii=False),
                       encoding="utf-8")
        tmp.replace(path)       # atomowy zapis - brak uszkodzonego pliku
        return path

    @classmethod
    def load(cls, path: Optional[Path] = None) -> "AppConfig":
        path = path or config_path()
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return cls()
        if not isinstance(raw, dict):
            return cls()
        try:
            cfg = _from_dict(cls, raw)
        except Exception:
            return cls()
        # Migracja z wersji sprzed trybu pada: pole output.backend
        out = raw.get("output")
        if isinstance(out, dict) and "mode" not in out and "backend" in out:
            cfg.output.mode = "none" if out.get("backend") == "none" else "wheel"
        # Migracja ze starego modelu osi pada (jedno zrodlo -> cala os).
        pad_raw = raw.get("pad")
        if isinstance(pad_raw, dict) and "axes" not in pad_raw:
            legacy = pad_axes_from_legacy(pad_raw)
            cfg.pad.axes = legacy if legacy else dict(DEFAULT_PAD_AXES)
        # Plik sprzed wlasnych przypisan G29: tryb G29 bral wtedy przyciski
        # z innej listy niz zakladka Pad i ignorowal to, co ustawil
        # uzytkownik. Przenosimy jego przypisania pada na odpowiedniki G29.
        g29_raw = raw.get("g29")
        if not (isinstance(g29_raw, dict) and "buttons" in g29_raw):
            cfg.g29.buttons = g29_buttons_from_pad(cfg.pad.buttons)
        cfg.normalize()
        return cfg


def _clamp(v: Any, lo: float, hi: float) -> float:
    try:
        f = float(v)
    except (TypeError, ValueError):
        return lo
    if f != f:          # NaN
        return lo
    return max(lo, min(hi, f))


# Mapa typow zagniezdzonych. Z `from __future__ import annotations` pole
# dataclassy ma typ jako string, wiec nie da sie go rozpoznac automatycznie.
#
# WAZNE: mapa jest per klasa, nie wspolna dla wszystkich. Nazwa "buttons"
# wystepuje w dwoch miejscach i znaczy dwie zupelnie inne rzeczy:
# AppConfig.buttons to ButtonsConfig, a PadConfig.buttons to zwykly
# slownik. Wspolna mapa zamieniala ten slownik na ButtonsConfig, przez co
# normalize() wywalalo sie i load() cicho zwracal cala konfiguracje
# domyslna - kazdy start kasowal ustawienia uzytkownika.
_NESTED: Dict[str, Dict[str, Any]] = {
    "AppConfig": {
        "link": LinkConfig,
        "output": OutputConfig,
        "steer": SteerConfig,
        "ffb": FfbConfig,
        "buttons": ButtonsConfig,
        "pad": PadConfig,
        "g29": G29Config,
    },
}


def _from_dict(cls: Any, data: Dict[str, Any]) -> Any:
    nested = _NESTED.get(cls.__name__, {})
    kwargs: Dict[str, Any] = {}
    for f in fields(cls):
        if f.name not in data:
            continue
        value = data[f.name]
        if f.name in nested and isinstance(value, dict):
            kwargs[f.name] = _from_dict(nested[f.name], value)
        elif f.name == "pedals" and isinstance(value, dict):
            kwargs[f.name] = {
                k: _from_dict(PedalConfig, v) if isinstance(v, dict) else PedalConfig()
                for k, v in value.items()
            }
        else:
            kwargs[f.name] = value
    return cls(**kwargs)
