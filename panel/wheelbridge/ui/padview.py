"""
Widok kontrolera Xbox w stylu x360ce.

Dwie czesci:

  * rysunek pada - pokazuje na zywo, co sie dzieje, i sluzy do klikania
    przyciskow (klikniecie wlacza tryb nauki),
  * lista przypisan obok - jeden wiersz na jedno gniazdo.

Kazdy kierunek drazka jest OSOBNYM gniazdem (lewy drazek w lewo, w prawo,
w gore, w dol), dokladnie jak w x360ce. Dzieki temu w jeden drazek mozna
wpiac dwa rozne zrodla: skret w poziomie, a gaz i hamulec w pionie.

Najechanie na wiersz podswietla odpowiednia czesc rysunku i odwrotnie -
dlatego nie ma tu zadnych linii odniesienia, ktore wczesniej zasmiecaly
caly obrazek.

Geometria (stale _STICKS, _DPAD, _FACE, _SHOULDER_X ...) jest sprawdzana
osobnym testem: nic nie moze wyjsc poza korpus ani nachodzic na sasiada.
Warstwa malarska moze sie zmieniac dowolnie, ale te liczby - nie.
"""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import (Callable, Dict, Iterable, List, Optional, Sequence,
                    Tuple)

from ..config import (PAD_BUTTON_NAMES, PAD_DIRECTIONS, PAD_DIRECTION_LABELS,
                      PAD_DIRECTION_SHORT)
from . import theme
from .theme import Fonts
from .widgets import round_points, set_round_rect

#: Kontrolki przyciskowe - tym przypisuje sie przyciski kierownicy.
CONTROLS = PAD_BUTTON_NAMES

#: Kierunki analogowe - tym przypisuje sie zrodla (kierownica albo pedal).
AXES = PAD_DIRECTIONS

FACE_COLORS = {
    "A": "#5ac25a",
    "B": "#e8534a",
    "X": "#3f9be8",
    "Y": "#f0c419",
}

#: Krotkie nazwy w wierszach - grupa mowi juz, o ktora kontrolke chodzi.
ROW_LABELS = {
    "LT": "LT (lewy)",
    "RT": "RT (prawy)",
    "LB": "LB",
    "RB": "RB",
    "LS": "L3",
    "RS": "R3",
    "A": "A",
    "B": "B",
    "X": "X",
    "Y": "Y",
    "BACK": "Back",
    "START": "Start",
    "GUIDE": "Guide",
    "DPAD_UP": "▲ gora",
    "DPAD_DOWN": "▼ dol",
    "DPAD_LEFT": "◀ lewo",
    "DPAD_RIGHT": "▶ prawo",
}
_ARROWS = {"LX-": "◀", "LX+": "▶", "LY+": "▲", "LY-": "▼",
           "RX-": "◀", "RX+": "▶", "RY+": "▲", "RY-": "▼"}
for _d in PAD_DIRECTIONS:
    if _d in _ARROWS:
        ROW_LABELS[_d] = f"{_ARROWS[_d]} {PAD_DIRECTION_SHORT[_d]}"

#: Pelne nazwy - do menu, podpowiedzi i dziennika.
TITLES = dict(PAD_DIRECTION_LABELS)
TITLES.update({
    "LB": "Lewy bumper (LB)", "RB": "Prawy bumper (RB)",
    "LS": "Klik lewego drazka (L3)", "RS": "Klik prawego drazka (R3)",
    "A": "Przycisk A", "B": "Przycisk B", "X": "Przycisk X", "Y": "Przycisk Y",
    "BACK": "Back", "START": "Start", "GUIDE": "Guide",
    "DPAD_UP": "Krzyzak w gore", "DPAD_DOWN": "Krzyzak w dol",
    "DPAD_LEFT": "Krzyzak w lewo", "DPAD_RIGHT": "Krzyzak w prawo",
})
BOX_TITLES = TITLES          # zgodnosc z poprzednia nazwa

#: Kolumny listy przypisan: (naglowek, rodzaj, gniazda). Dwie kolumny
#: zamiast trzech - inaczej calosc nie miesci sie obok rysunku w oknie
#: 1240 px, a trzecia kolumna wychodzi za krawedz (zakladka przewija sie
#: tylko w pionie).
GROUPS: Tuple[Tuple[Tuple[str, str, Tuple[str, ...]], ...], ...] = (
    (
        ("Lewy drazek", "axis", ("LX-", "LX+", "LY+", "LY-")),
        ("Prawy drazek", "axis", ("RX-", "RX+", "RY+", "RY-")),
        ("Spusty analogowe", "axis", ("LT", "RT")),
        ("Bumpery", "button", ("LB", "RB")),
    ),
    (
        ("Przyciski", "button", ("A", "B", "X", "Y")),
        ("Krzyzak", "button", ("DPAD_UP", "DPAD_DOWN", "DPAD_LEFT",
                               "DPAD_RIGHT")),
        ("Klik drazkow i srodek", "button", ("LS", "RS", "BACK", "START",
                                             "GUIDE")),
    ),
)

#: Co podswietlic na rysunku dla danego gniazda.
_PART_OF_SLOT = {d: ("STICK_L" if d[0] == "L" else "STICK_R")
                 for d in PAD_DIRECTIONS if d not in ("LT", "RT")}
_PART_OF_SLOT["LT"] = "LT"
_PART_OF_SLOT["RT"] = "RT"
for _n in CONTROLS:
    _PART_OF_SLOT[_n] = _n

#: Co podswietlic na liscie, gdy kursor jest nad czescia rysunku.
_SLOTS_OF_PART: Dict[str, Tuple[str, ...]] = {
    "STICK_L": ("LX-", "LX+", "LY+", "LY-"),
    "STICK_R": ("RX-", "RX+", "RY+", "RY-"),
}
for _n in CONTROLS:
    _SLOTS_OF_PART[_n] = (_n,)
_SLOTS_OF_PART["LT"] = ("LT",)
_SLOTS_OF_PART["RT"] = ("RT",)

EMPTY = "—"                 # pauza - gniazdo bez przypisania


# --------------------------------------------------------------- rysunek

CANVAS_W, CANVAS_H = 486, 388
CX, CY = 243.0, 174.0

#: Obrys korpusu jako punkty wzgledem srodka, zgodnie z ruchem wskazowek
#: zegara od lewego barku. Krzywa Catmulla-Roma przechodzi dokladnie przez
#: te punkty - w odroznieniu od `smooth=True` w Tk, ktore scina naroza i
#: potrafilo wypchnac przyciski poza korpus.
SILHOUETTE = (
    (-150, -96), (-100, -108), (-46, -100), (0, -92), (46, -100), (100, -108),
    (150, -96), (188, -70), (206, -22), (204, 34), (190, 86), (172, 136),
    (144, 180), (108, 200), (78, 186), (60, 148), (50, 104), (34, 78),
    (0, 72), (-34, 78), (-50, 104), (-60, 148), (-78, 186), (-108, 200),
    (-144, 180), (-172, 136), (-190, 86), (-204, 34), (-206, -22),
    (-188, -70),
)

_STICKS = {"L": (-110.0, -36.0), "R": (72.0, 58.0)}
_STICK_RING = 40.0
_STICK_KNOB = 20.0
_STICK_TRAVEL = 17.0

_DPAD = (-104.0, 70.0)
_DPAD_ARM, _DPAD_THICK = 32.0, 21.0

_FACE = (114.0, -34.0)
_FACE_OFFSET, _FACE_R = 40.0, 22.0

_GUIDE = (0.0, -54.0)
_GUIDE_R = 24.0
_CENTER_BTN_X, _CENTER_BTN_Y, _CENTER_BTN_R = 34.0, -8.0, 12.0

#: Barki i spusty: wspolny zakres w poziomie, zeby czytaly sie jako jeden
#: blok przylegajacy do gornej krawedzi korpusu.
_SHOULDER_X = (60.0, 156.0)
_BUMPER_Y = (-116.0, -90.0)
_TRIGGER_Y = (-156.0, -122.0)

#: Kolory korpusu - gradient z gory na dol daje wrazenie bryly.
_SHELL_TOP = theme.mix(theme.lighten(theme.PANEL_HI, 0.33),
                       theme.ACCENT, 0.07)
_SHELL_BOTTOM = theme.darken(theme.PANEL_HI, 0.12)
_SHELL_MID = theme.mix(_SHELL_TOP, _SHELL_BOTTOM, 0.5)
_WELL = theme.darken(theme.TRACK, 0.45)       # wglebienie pod drazkiem
_CAP = theme.lighten(theme.PANEL_HI, 0.22)    # plastik przyciskow


def spline(points: Sequence[Tuple[float, float]],
           steps: int = 12) -> List[Tuple[float, float]]:
    """Zamknieta krzywa Catmulla-Roma przechodzaca przez wszystkie punkty."""
    n = len(points)
    out: List[Tuple[float, float]] = []
    for i in range(n):
        p0, p1 = points[(i - 1) % n], points[i]
        p2, p3 = points[(i + 1) % n], points[(i + 2) % n]
        for step in range(steps):
            t = step / steps
            t2, t3 = t * t, t * t * t
            out.append((
                0.5 * (2 * p1[0] + (-p0[0] + p2[0]) * t
                       + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2
                       + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3),
                0.5 * (2 * p1[1] + (-p0[1] + p2[1]) * t
                       + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2
                       + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3),
            ))
    return out


def inside_body(x: float, y: float) -> bool:
    """Czy punkt (wzgledem srodka) lezy w obrysie korpusu."""
    poly = spline(SILHOUETTE)
    hit = False
    j = len(poly) - 1
    for i, (xi, yi) in enumerate(poly):
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            hit = not hit
        j = i
    return hit


def _spans(poly: Sequence[Tuple[float, float]], y: float) -> List[Tuple[float, float]]:
    """Odcinki poziome, ktore na wysokosci `y` leza wewnatrz wielokata.

    Korpus ma u dolu wciecie miedzy uchwytami, wiec nie wystarczy wziac
    skrajnych przeciec - trzeba je sparowac.
    """
    xs: List[float] = []
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            xs.append(x1 + (x2 - x1) * (y - y1) / (y2 - y1))
    xs.sort()
    return [(xs[i], xs[i + 1]) for i in range(0, len(xs) - 1, 2)]


class PadCanvas(tk.Canvas):
    """Rysowany kontroler: klikalne przyciski, podglad osi na zywo.

    Podklasa (rysunek kierownicy G29) podmienia _CONTROLS, _SIZE i _build().
    """

    _CONTROLS: Tuple[str, ...] = CONTROLS
    _SIZE: Tuple[int, int] = (CANVAS_W, CANVAS_H)

    def __init__(self, master: tk.Misc, fonts: Fonts,
                 on_button_click: Callable[[str], None],
                 on_button_clear: Callable[[str], None],
                 on_hover: Callable[[Optional[str]], None]) -> None:
        super().__init__(master, width=self._SIZE[0], height=self._SIZE[1],
                         bg=theme.PANEL, highlightthickness=0, bd=0)
        self._fonts = fonts
        self._on_button_click = on_button_click
        self._on_button_clear = on_button_clear
        self._on_hover = on_hover

        self._shapes: Dict[str, List[int]] = {}
        self._base_fill: Dict[str, str] = {}
        self._on_fill: Dict[str, str] = {}      # kolor wcisnietej kontrolki
        self._glyph: Dict[str, List[Tuple[List[int], str, str]]] = {}
        self._region: Dict[int, str] = {}      # id elementu -> nazwa czesci
        self._active: Dict[str, bool] = {c: False for c in self._CONTROLS}
        self._assigned: Dict[str, bool] = {c: False for c in self._CONTROLS}
        self._learning: Optional[str] = None
        self._pulse_on = True
        self._highlight: Optional[str] = None
        self._hover: Optional[str] = None

        self._trig_fill: Dict[str, int] = {}
        self._trig_text: Dict[str, int] = {}
        self._trig_box: Dict[str, Tuple[float, float, float, float]] = {}
        self._knob: Dict[str, int] = {}
        self._knob_mark: Dict[str, int] = {}
        self._knob_shadow: Dict[str, int] = {}
        self._knob_inner: Dict[str, int] = {}

        self._build()
        self.bind("<Button-1>", self._on_click)
        self.bind("<Button-3>", self._on_right_click)
        self.bind("<Motion>", self._on_motion)
        self.bind("<Leave>", self._on_leave)

    # ----------------------------------------------------------- budowanie

    def _part(self, name: str, items: Sequence[int], base_fill: str = "",
              clickable: bool = True, on_fill: str = "") -> None:
        """Rejestruje czesc rysunku. Nie nazywac tego `_register` - tak
        nazywa sie wewnetrzna metoda tkinter.Misc, ktorej uzywa bind()."""
        if base_fill:
            self._shapes.setdefault(name, []).extend(items)
            self._base_fill[name] = base_fill
            if on_fill:
                self._on_fill[name] = on_fill
        if clickable:
            for item in items:
                self._region[item] = name

    def _mark(self, name: str, items: Sequence[int], normal: str,
              active: str) -> None:
        """Napis albo piktogram na kontrolce - zmienia kolor razem z nia.

        Kazde wywolanie to osobna grupa z wlasna para kolorow: napis na
        przycisku moze zmieniac sie inaczej niz piktogram obok niego.
        """
        self._glyph.setdefault(name, []).append((list(items), normal, active))
        for item in items:
            self._region[item] = name

    def _round_rect(self, x1, y1, x2, y2, r, **kw) -> int:
        return self.create_polygon(round_points(x1, y1, x2, y2, r),
                                   smooth=True, **kw)

    def _build(self) -> None:
        self._build_body()
        self._build_triggers()
        self._build_sticks()
        self._build_dpad()
        self._build_face()
        self._build_center()
        self._refresh()

    def _build_body(self) -> None:
        poly = spline(SILHOUETTE)
        abs_poly = [(CX + x, CY + y) for x, y in poly]

        # cien pod korpusem - lekko przesuniety obrys w ciemniejszym tonie
        self.create_polygon([c for p in abs_poly for c in (p[0], p[1] + 8)],
                            fill=theme.mix(theme.PANEL, theme.SHADOW, 0.5),
                            outline="")

        # barki: blok pod spustem i bumperem, wtopiony w korpus - bez niego
        # oba przyciski wisialy nad rysunkiem bez zadnego oparcia
        for sign in (-1, 1):
            lo = CX + sign * (_SHOULDER_X[0] - 4)
            hi = CX + sign * (_SHOULDER_X[1] + 4)
            lo, hi = min(lo, hi), max(lo, hi)
            self._round_rect(lo, CY + _TRIGGER_Y[0] - 5, hi, CY - 40, 16,
                             fill=theme.darken(_SHELL_MID, 0.18), outline="")

        # gradient korpusu: linia po linii, obcieta do ksztaltu obrysu
        ys = [p[1] for p in abs_poly]
        y0, y1 = min(ys), max(ys)
        y = y0
        while y < y1:
            t = (y - y0) / max(1.0, y1 - y0)
            color = theme.mix(_SHELL_TOP, _SHELL_BOTTOM, t ** 0.75)
            for xa, xb in _spans(abs_poly, y + 0.5):
                self.create_line(xa, y + 0.5, xb, y + 0.5, fill=color)
            y += 1.0

        # obrys na wierzchu, zeby krawedz byla rowna mimo linii gradientu
        self.create_polygon([c for p in abs_poly for c in p], fill="",
                            outline=theme.darken(_SHELL_BOTTOM, 0.25), width=1)

        # swiatlo na gornej krawedzi i cien na dolnej - rysowane po samym
        # obrysie, wiec zawsze trzymaja sie ksztaltu korpusu
        self._edge_light([i for i, (_, oy) in enumerate(poly) if oy < -55],
                         poly, theme.lighten(_SHELL_TOP, 0.22), 2)
        self._edge_light([i for i, (_, oy) in enumerate(poly) if oy > 120],
                         poly, theme.darken(_SHELL_BOTTOM, 0.3), 2)

    def _edge_light(self, indices: Sequence[int],
                    poly: Sequence[Tuple[float, float]],
                    color: str, width: int) -> None:
        """Rysuje fragmenty obrysu o podanych indeksach jako osobne kreski."""
        runs: List[List[int]] = []
        for i in indices:
            if runs and i == runs[-1][-1] + 1:
                runs[-1].append(i)
            else:
                runs.append([i])
        # pierwszy i ostatni fragment sa w rzeczywistosci jednym - obrys
        # jest zamkniety i zaczyna sie w polowie gornej krawedzi
        if len(runs) > 1 and runs[0][0] == 0 and runs[-1][-1] == len(poly) - 1:
            runs[0] = runs[-1] + runs[0]
            runs.pop()
        for run in runs:
            if len(run) < 2:
                continue
            pts: List[float] = []
            for i in run:
                pts += [CX + poly[i][0], CY + poly[i][1]]
            self.create_line(*pts, fill=color, width=width, capstyle="round",
                             smooth=True)

    def _build_triggers(self) -> None:
        for name in ("LT", "RT"):
            sign = -1 if name == "LT" else 1
            lo = CX + sign * _SHOULDER_X[0]
            hi = CX + sign * _SHOULDER_X[1]
            lo, hi = min(lo, hi), max(lo, hi)
            box = (lo, CY + _TRIGGER_Y[0], hi, CY + _TRIGGER_Y[1])
            bg = self._round_rect(*box, 10, fill=theme.TRACK,
                                  outline=theme.BORDER_HI)
            fill = self.create_polygon(
                round_points(lo + 4, box[1] + 4, lo + 4, box[3] - 4, 6),
                smooth=True, fill=theme.mix(theme.ACCENT, "#000000", 0.25),
                outline="")
            text = self.create_text((lo + hi) / 2, (box[1] + box[3]) / 2,
                                    text=name, font=self._fonts.bold,
                                    fill=theme.TEXT)
            self._trig_fill[name] = fill
            self._trig_text[name] = text
            self._trig_box[name] = box
            self._part(name, [bg], theme.TRACK)
            self._mark(name, [text], theme.TEXT, theme.ACCENT_INK)

        for name in ("LB", "RB"):
            sign = -1 if name == "LB" else 1
            lo = CX + sign * _SHOULDER_X[0]
            hi = CX + sign * _SHOULDER_X[1]
            lo, hi = min(lo, hi), max(lo, hi)
            box = (lo, CY + _BUMPER_Y[0], hi, CY + _BUMPER_Y[1])
            item = self._round_rect(*box, 10, fill=_CAP,
                                    outline=theme.BORDER_HI)
            text = self.create_text((lo + hi) / 2, (box[1] + box[3]) / 2,
                                    text=name, font=self._fonts.small_bold,
                                    fill=theme.TEXT_DIM)
            self._part(name, [item], _CAP)
            self._mark(name, [text], theme.TEXT_DIM, theme.ACCENT_INK)

    def _build_sticks(self) -> None:
        for side, (ox, oy) in _STICKS.items():
            sx, sy = CX + ox, CY + oy
            ring = self.create_oval(sx - _STICK_RING, sy - _STICK_RING,
                                    sx + _STICK_RING, sy + _STICK_RING,
                                    fill=_WELL, outline=theme.BORDER)
            self.create_oval(sx - _STICK_RING + 6, sy - _STICK_RING + 6,
                             sx + _STICK_RING - 6, sy + _STICK_RING - 6,
                             fill=theme.darken(theme.TRACK, 0.2), outline="")
            self.create_oval(sx - _STICK_RING + 12, sy - _STICK_RING + 12,
                             sx + _STICK_RING - 12, sy + _STICK_RING - 12,
                             fill=theme.darken(theme.TRACK, 0.38), outline="")
            # cztery kreski przy krawedzi - od razu widac, ktory kierunek
            # to ktora os, a nic nie przecina samej galki
            guide = theme.lighten(theme.TRACK, 0.22)
            for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
                self.create_line(sx + dx * 25, sy + dy * 25,
                                 sx + dx * 32, sy + dy * 32,
                                 fill=guide, width=2, capstyle="round")

            name = "LS" if side == "L" else "RS"
            shadow = self.create_oval(sx - _STICK_KNOB, sy - _STICK_KNOB + 3,
                                      sx + _STICK_KNOB, sy + _STICK_KNOB + 3,
                                      fill=theme.darken(theme.TRACK, 0.6),
                                      outline="")
            knob = self.create_oval(sx - _STICK_KNOB, sy - _STICK_KNOB,
                                    sx + _STICK_KNOB, sy + _STICK_KNOB,
                                    fill=_CAP, outline=theme.BORDER_HI,
                                    width=2)
            inner = self.create_oval(sx - _STICK_KNOB + 6, sy - _STICK_KNOB + 6,
                                     sx + _STICK_KNOB - 6, sy + _STICK_KNOB - 6,
                                     fill=theme.darken(theme.PANEL_HI, 0.25),
                                     outline="")
            mark = self.create_text(sx, sy, text="L3" if side == "L" else "R3",
                                    font=self._fonts.tiny, fill=theme.TEXT_DIM)
            self._knob[side] = knob
            self._knob_mark[side] = mark
            self._knob_shadow[side] = shadow
            self._knob_inner[side] = inner
            self._part(name, [knob], _CAP)
            self._mark(name, [mark], theme.TEXT_DIM, theme.ACCENT_INK)
            self._region[inner] = name
            # pierscien nie uruchamia nauki, ale najechanie na niego
            # podswietla wiersze wszystkich kierunkow tego drazka
            self._part("STICK_L" if side == "L" else "STICK_R", [ring], _WELL)

    def _build_dpad(self) -> None:
        dx, dy = CX + _DPAD[0], CY + _DPAD[1]
        arm, thick = _DPAD_ARM, _DPAD_THICK
        specs = {
            "DPAD_UP": (dx - thick / 2, dy - arm, dx + thick / 2, dy + 2),
            "DPAD_DOWN": (dx - thick / 2, dy - 2, dx + thick / 2, dy + arm),
            "DPAD_LEFT": (dx - arm, dy - thick / 2, dx + 2, dy + thick / 2),
            "DPAD_RIGHT": (dx - 2, dy - thick / 2, dx + arm, dy + thick / 2),
        }
        self.create_oval(dx - arm - 9, dy - arm - 9, dx + arm + 9, dy + arm + 9,
                         fill=_WELL, outline=theme.BORDER)
        self.create_oval(dx - arm - 4, dy - arm - 4, dx + arm + 4, dy + arm + 4,
                         fill=theme.darken(theme.TRACK, 0.25), outline="")
        # strzalki rysujemy osobno, zeby zmienialy kolor razem z ramieniem
        tip = thick * 0.26
        arrows = {
            "DPAD_UP": (dx, dy - arm + 9, dx - tip, dy - arm + 9 + tip * 1.4,
                        dx + tip, dy - arm + 9 + tip * 1.4),
            "DPAD_DOWN": (dx, dy + arm - 9, dx - tip, dy + arm - 9 - tip * 1.4,
                          dx + tip, dy + arm - 9 - tip * 1.4),
            "DPAD_LEFT": (dx - arm + 9, dy, dx - arm + 9 + tip * 1.4, dy - tip,
                          dx - arm + 9 + tip * 1.4, dy + tip),
            "DPAD_RIGHT": (dx + arm - 9, dy, dx + arm - 9 - tip * 1.4, dy - tip,
                           dx + arm - 9 - tip * 1.4, dy + tip),
        }
        for name, box in specs.items():
            item = self._round_rect(*box, 6, fill=_CAP,
                                    outline=theme.BORDER_HI)
            self._part(name, [item], _CAP)
        self._round_rect(dx - thick / 2 + 1, dy - thick / 2 + 1,
                         dx + thick / 2 - 1, dy + thick / 2 - 1, 4,
                         fill=_CAP, outline="")
        self.create_oval(dx - 3, dy - 3, dx + 3, dy + 3,
                         fill=theme.darken(theme.PANEL_HI, 0.3), outline="")
        for name, pts in arrows.items():
            glyph = self.create_polygon(*pts, fill=theme.MUTED, outline="")
            self._mark(name, [glyph], theme.MUTED, theme.ACCENT_INK)

    def _build_face(self) -> None:
        fx, fy = CX + _FACE[0], CY + _FACE[1]
        r, off = _FACE_R, _FACE_OFFSET
        for name, ox, oy in (("Y", 0, -off), ("X", -off, 0),
                             ("B", off, 0), ("A", 0, off)):
            x, y = fx + ox, fy + oy
            color = FACE_COLORS[name]
            self.create_oval(x - r - 2, y - r + 1, x + r + 2, y + r + 4,
                             fill=theme.darken(theme.TRACK, 0.55), outline="")
            base = theme.mix(_CAP, color, 0.12)
            item = self.create_oval(x - r, y - r, x + r, y + r,
                                    fill=base, outline=color, width=2)
            text = self.create_text(x, y, text=name, font=self._fonts.bold,
                                    fill=color)
            self._part(name, [item], base, on_fill=color)
            self._mark(name, [text], color, theme.darken(color, 0.78))

    def _build_center(self) -> None:
        r = _CENTER_BTN_R
        for name, sign, label in (("BACK", -1, "Back"), ("START", 1, "Start")):
            x = CX + sign * _CENTER_BTN_X
            y = CY + _CENTER_BTN_Y
            item = self.create_oval(x - r, y - r, x + r, y + r,
                                    fill=_CAP, outline=theme.BORDER_HI)
            if name == "BACK":
                glyphs = [self.create_rectangle(x - 5, y - 3, x - 1, y + 3,
                                                fill=theme.MUTED, outline=""),
                          self.create_rectangle(x + 1, y - 3, x + 5, y + 3,
                                                fill=theme.MUTED, outline="")]
            else:
                glyphs = [self.create_rectangle(x - 5, y - 4 + i * 3,
                                                x + 5, y - 3 + i * 3,
                                                fill=theme.MUTED, outline="")
                          for i in range(3)]
            text = self.create_text(x, y + r + 11, text=label,
                                    font=self._fonts.tiny, fill=theme.MUTED)
            self._part(name, [item], _CAP)
            self._mark(name, glyphs, theme.MUTED, theme.ACCENT_INK)
            self._mark(name, [text], theme.MUTED, theme.ACCENT)

        x, y = CX + _GUIDE[0], CY + _GUIDE[1]
        self.create_oval(x - _GUIDE_R - 3, y - _GUIDE_R - 3,
                         x + _GUIDE_R + 3, y + _GUIDE_R + 3,
                         fill=theme.darken(theme.TRACK, 0.35), outline="")
        item = self.create_oval(x - _GUIDE_R, y - _GUIDE_R,
                                x + _GUIDE_R, y + _GUIDE_R,
                                fill=_CAP, outline=theme.BORDER_HI, width=2)
        self._part("GUIDE", [item], _CAP)
        self.create_oval(x - 13, y - 13, x + 13, y + 13,
                         fill=theme.darken(_CAP, 0.3), outline="")
        # znak "X" rysowany kreskami - niezaleznie od tego, czy czcionka
        # systemowa ma akurat taki znak w zestawie
        cross = [self.create_line(x - 7, y - 7, x + 7, y + 7, width=3,
                                  capstyle="round", fill=theme.TEXT_DIM),
                 self.create_line(x - 7, y + 7, x + 7, y - 7, width=3,
                                  capstyle="round", fill=theme.TEXT_DIM)]
        self._mark("GUIDE", cross, theme.TEXT_DIM, theme.ACCENT_INK)

    # ---------------------------------------------------------- interakcja

    def _hit(self, x: float, y: float) -> Optional[str]:
        for item in reversed(self.find_overlapping(x - 1, y - 1, x + 1, y + 1)):
            name = self._region.get(item)
            if name is not None:
                return name
        return None

    def _on_click(self, event) -> None:
        name = self._hit(event.x, event.y)
        if name in self._CONTROLS:
            self._on_button_click(name)
        else:
            self._on_button_click("")        # klik w tlo anuluje nauke

    def _on_right_click(self, event) -> None:
        name = self._hit(event.x, event.y)
        if name in self._CONTROLS:
            self._on_button_clear(name)

    def _on_motion(self, event) -> None:
        name = self._hit(event.x, event.y)
        if name == self._hover:
            return
        self._hover = name
        self.configure(cursor="hand2" if name in self._CONTROLS else "")
        self._on_hover(name)

    def _on_leave(self, _event) -> None:
        self._hover = None
        self.configure(cursor="")
        self._on_hover(None)

    # ---------------------------------------------------------------- stan

    def set_learning(self, name: Optional[str]) -> None:
        if self._learning != name:
            self._learning = name
            self._refresh()

    def set_pulse(self, on: bool) -> None:
        """Faza migania celu nauki. Zegar trzyma PadView, zeby rysunek i
        wiersz listy migaly dokladnie razem."""
        if self._pulse_on != on:
            self._pulse_on = on
            if self._learning is not None:
                self._refresh()

    def set_assigned(self, assigned: Dict[str, bool]) -> None:
        changed = False
        for name in self._CONTROLS:
            value = bool(assigned.get(name))
            if self._assigned.get(name) != value:
                self._assigned[name] = value
                changed = True
        if changed:
            self._refresh()

    def set_active(self, active: Dict[str, bool]) -> None:
        changed = False
        for name in self._CONTROLS:
            value = bool(active.get(name, False))
            if self._active.get(name) != value:
                self._active[name] = value
                changed = True
        if changed:
            self._refresh()

    def highlight(self, part: Optional[str]) -> None:
        if self._highlight != part:
            self._highlight = part
            self._refresh()

    def set_sticks(self, lx: float, ly: float, rx: float, ry: float) -> None:
        for side, (dx, dy) in (("L", (lx, ly)), ("R", (rx, ry))):
            ox, oy = _STICKS[side]
            px = CX + ox + max(-1.0, min(1.0, dx)) * _STICK_TRAVEL
            py = CY + oy - max(-1.0, min(1.0, dy)) * _STICK_TRAVEL
            k = _STICK_KNOB
            self.coords(self._knob[side], px - k, py - k, px + k, py + k)
            self.coords(self._knob_shadow[side], px - k, py - k + 3,
                        px + k, py + k + 3)
            self.coords(self._knob_inner[side], px - k + 6, py - k + 6,
                        px + k - 6, py + k - 6)
            self.coords(self._knob_mark[side], px, py)

    def set_triggers(self, lt: int, rt: int) -> None:
        for name, raw in (("LT", lt), ("RT", rt)):
            raw = max(0, min(255, int(raw)))
            x1, y1, x2, y2 = self._trig_box[name]
            width = (x2 - x1 - 8) * raw / 255.0
            self.itemconfigure(self._trig_fill[name],
                               state="hidden" if width < 2 else "normal")
            if width >= 2:
                set_round_rect(self, self._trig_fill[name], x1 + 4, y1 + 4,
                               x1 + 4 + width, y2 - 4, 6)
            self.itemconfigure(self._trig_text[name],
                               text=name if raw == 0 else f"{name}  {raw}")

    def _refresh(self) -> None:
        for name, items in self._shapes.items():
            base = self._base_fill.get(name)
            if not base or not items:
                continue
            on = False
            if self._learning == name:
                fill = (theme.WARN if self._pulse_on
                        else theme.mix(theme.WARN, theme.PANEL_HI, 0.55))
                on = self._pulse_on
            elif self._active.get(name):
                fill = self._on_fill.get(name, theme.ACCENT)
                on = True
            elif self._highlight == name:
                fill = theme.mix(base, theme.ACCENT, 0.40)
            elif self._assigned.get(name):
                fill = theme.mix(base, theme.ACCENT, 0.20)
            else:
                fill = base
            for item in items:
                self.itemconfigure(item, fill=fill)
            for marks, normal, active in self._glyph.get(name, ()):
                color = active if on else normal
                for item in marks:
                    self.itemconfigure(item, fill=color)


# ----------------------------------------------------------- lista wierszy

class _Row:
    """Jeden wiersz listy przypisan: nazwa, klikalna komorka, pasek."""

    CELL_W = 17          # w znakach - miejsce na "Kierownica w prawo"
    BAR_W = 46
    BAR_H = 8

    def __init__(self, parent: tk.Misc, grid_row: int, fonts: Fonts,
                 slot: str, kind: str,
                 on_click: Callable[[str, int, int], None],
                 on_clear: Callable[[str], None],
                 on_hover: Callable[[Optional[str]], None],
                 label: Optional[str] = None) -> None:
        self.slot = slot
        self.kind = kind
        self._on_click = on_click
        self._on_clear = on_clear
        self._on_hover = on_hover
        self.state = "empty"          # empty | set | active | learning

        self.name = tk.Label(parent, text=(label if label is not None
                                          else ROW_LABELS.get(slot, slot)),
                             font=fonts.small, bg=theme.PANEL,
                             fg=theme.TEXT_DIM, anchor="w")
        self.name.grid(row=grid_row, column=0, sticky="w", padx=(4, 9))

        self.cell = tk.Label(parent, text=EMPTY, font=fonts.small,
                             bg=theme.PANEL_HI, fg=theme.MUTED, anchor="w",
                             width=self.CELL_W, padx=8, pady=3,
                             bd=0, highlightthickness=1,
                             highlightbackground=theme.BORDER,
                             highlightcolor=theme.BORDER, cursor="hand2")
        self.cell.grid(row=grid_row, column=1, sticky="ew", pady=1)

        self.bar: Optional[tk.Canvas] = None
        self._bar_fill = 0
        if kind == "axis":
            w, h = self.BAR_W, self.BAR_H
            self.bar = tk.Canvas(parent, width=w, height=h, bg=theme.PANEL,
                                 highlightthickness=0, bd=0)
            self.bar.create_polygon(round_points(0, 1, w, h - 1, (h - 2) / 2),
                                    smooth=True, fill=theme.TRACK, outline="")
            self._bar_fill = self.bar.create_polygon(
                round_points(0, 1, 0, h - 1, (h - 2) / 2), smooth=True,
                fill=theme.ACCENT, outline="", state="hidden")
            self.bar.grid(row=grid_row, column=2, sticky="w", padx=(8, 2))
        self._value = -1.0

        for w in (self.name, self.cell) + ((self.bar,) if self.bar else ()):
            w.bind("<Button-1>", self._click)
            w.bind("<Button-3>", self._right_click)
            w.bind("<Enter>", lambda _e: self._on_hover(self.slot))
            w.bind("<Leave>", lambda _e: self._on_hover(None))

    # --- zdarzenia ---

    def _click(self, event) -> None:
        self._on_click(self.slot, event.x_root, event.y_root)

    def _right_click(self, _event) -> None:
        self._on_clear(self.slot)

    # --- wyglad ---

    def set_text(self, text: str) -> None:
        self.cell.configure(text=text or EMPTY)

    def set_state(self, state: str, hover: bool = False,
                  pulse: bool = True) -> None:
        self.state = state
        if state == "learning":
            bg = theme.WARN if pulse else theme.mix(theme.WARN,
                                                    theme.PANEL_HI, 0.55)
            fg, border = ("#20160a" if pulse else theme.TEXT), theme.WARN
        elif state == "active":
            bg = theme.mix(theme.PANEL_HI, theme.ACCENT, 0.45)
            fg, border = "#ffffff", theme.ACCENT
        elif state == "set":
            bg, fg = theme.PANEL_HI, theme.TEXT
            border = theme.mix(theme.BORDER, theme.ACCENT, 0.42)
        else:
            bg, fg, border = theme.PANEL_HI, theme.MUTED, theme.BORDER
        if hover and state not in ("learning", "active"):
            bg = theme.lighten(bg, 0.07)
            border = theme.mix(border, theme.ACCENT, 0.55)
        self.cell.configure(bg=bg, fg=fg, highlightbackground=border,
                            highlightcolor=border)
        self.name.configure(fg=theme.TEXT if hover else theme.TEXT_DIM)

    def set_value(self, value: float) -> None:
        if self.bar is None:
            return
        value = 0.0 if value < 0.0 else (1.0 if value > 1.0 else value)
        if abs(self._value - value) < 0.004:
            return
        self._value = value
        w, h = self.BAR_W, self.BAR_H
        if value < 0.02:
            self.bar.itemconfigure(self._bar_fill, state="hidden")
            return
        self.bar.itemconfigure(self._bar_fill, state="normal")
        self.bar.coords(self._bar_fill,
                        *round_points(0, 1, w * value, h - 1, (h - 2) / 2))


# ------------------------------------------------------------------ widok

class PadView(ttk.Frame):
    """Rysunek pada + lista przypisan. Jedno gniazdo = jeden wiersz.

    Widok kierownicy G29 dziedziczy calosc - podmienia tylko atrybuty
    ponizej: rysunek, gniazda, grupy listy i nazwy.
    """

    _CANVAS_CLS = PadCanvas
    _CONTROLS: Tuple[str, ...] = CONTROLS
    _AXES: Tuple[str, ...] = AXES
    _GROUPS = GROUPS
    _TITLES: Dict[str, str] = TITLES
    _ROW_LABELS: Dict[str, str] = ROW_LABELS
    _PART_OF_SLOT: Dict[str, str] = _PART_OF_SLOT
    _SLOTS_OF_PART: Dict[str, Tuple[str, ...]] = _SLOTS_OF_PART
    _IDLE_HINT = ("Kliknij wiersz: kierunki i spusty otwieraja liste "
                  "zrodel, przyciski ucza sie z kierownicy. Prawy przycisk "
                  "myszy czysci gniazdo.")
    _NAME_MINSIZE = 86

    def __init__(self, master: tk.Misc, fonts: Fonts,
                 on_assign_request: Optional[Callable[[str], None]] = None,
                 on_clear: Optional[Callable[[str], None]] = None,
                 on_axis_click: Optional[Callable[[str, int, int], None]] = None,
                 on_axis_clear: Optional[Callable[[str], None]] = None) -> None:
        super().__init__(master, style="Card.TFrame")
        self._fonts = fonts
        self._on_assign_request = on_assign_request
        self._on_clear = on_clear
        self._on_axis_click = on_axis_click
        self._on_axis_clear = on_axis_clear

        self._learning: Optional[str] = None
        self._wizard: Optional[Tuple[int, int]] = None
        self._held: Tuple[int, ...] = ()
        self._pulse_on = True
        self._pulse_job: Optional[str] = None
        self._hover_slots: Tuple[str, ...] = ()
        self._assign: Dict[str, int] = {c: -1 for c in self._CONTROLS}
        self._axis_labels: Dict[str, str] = {a: "" for a in self._AXES}
        self._active: Dict[str, bool] = {c: False for c in self._CONTROLS}

        # Podpowiedz jest NAD rysunkiem - pod spodem wypadala poza widoczny
        # obszar zakladki i w trybie nauki nie bylo jej widac. Pasek ma
        # wlasne tlo i kolorowa kreske, wiec w trybie nauki rzuca sie w oczy.
        self._hint_box = tk.Frame(self, bg=theme.PANEL_HI,
                                  highlightthickness=1,
                                  highlightbackground=theme.BORDER)
        self._hint_box.grid(row=0, column=0, columnspan=2, sticky="ew",
                            pady=(0, 12))
        self._hint_tick = tk.Frame(self._hint_box, bg=theme.MUTED, width=3)
        self._hint_tick.pack(side="left", fill="y")
        self.hint = tk.Label(self._hint_box, text="", font=fonts.small,
                             bg=theme.PANEL_HI, fg=theme.TEXT_DIM, anchor="w",
                             justify="left", padx=11, pady=7,
                             wraplength=self._CANVAS_CLS._SIZE[0] + 540)
        self.hint.pack(side="left", fill="both", expand=True)

        self.canvas = self._CANVAS_CLS(self, fonts,
                                on_button_click=self._on_canvas_click,
                                on_button_clear=self._clear_button,
                                on_hover=self._on_canvas_hover)
        self.canvas.grid(row=1, column=0, sticky="n", padx=(0, 18))

        self._legend = self._build_legend()
        self._legend.grid(row=2, column=0, sticky="n", pady=(10, 0))

        panel = ttk.Frame(self, style="Card.TFrame")
        panel.grid(row=1, column=1, rowspan=2, sticky="nsew")
        self.grid_columnconfigure(1, weight=1)

        self._rows: Dict[str, _Row] = {}
        self._build_panel(panel)

        self._set_hint_idle()

    # ----------------------------------------------------------- budowanie

    def _build_legend(self) -> tk.Frame:
        """Legenda kolorow - bez niej trzeba zgadywac, co znaczy podswietlenie."""
        box = tk.Frame(self, bg=theme.PANEL)
        items = (("wcisniete", theme.ACCENT),
                 ("przypisane", theme.mix(theme.PANEL_HI, theme.ACCENT, 0.20)),
                 ("uczy sie", theme.WARN),
                 ("wolne", theme.PANEL_HI))
        for i, (text, color) in enumerate(items):
            cell = tk.Frame(box, bg=theme.PANEL)
            cell.pack(side="left", padx=(0 if i == 0 else 14, 0))
            dot = tk.Canvas(cell, width=10, height=10, bg=theme.PANEL,
                            highlightthickness=0, bd=0)
            dot.create_oval(0, 0, 9, 9, fill=color,
                            outline=theme.BORDER_HI)
            dot.pack(side="left", padx=(0, 6))
            tk.Label(cell, text=text, bg=theme.PANEL, fg=theme.MUTED,
                     font=self._fonts.tiny).pack(side="left")
        return box

    def _build_panel(self, panel: ttk.Frame) -> None:
        for col, column_groups in enumerate(self._GROUPS):
            frame = ttk.Frame(panel, style="Card.TFrame")
            frame.grid(row=0, column=col, sticky="new", padx=(0, 22))
            frame.grid_columnconfigure(1, weight=1)
            # staly slup na nazwy i na pasek podgladu - dzieki temu komorki
            # z przypisaniem maja w obu kolumnach dokladnie te sama szerokosc
            frame.grid_columnconfigure(0, minsize=self._NAME_MINSIZE)
            frame.grid_columnconfigure(2, minsize=_Row.BAR_W + 10)
            # obie kolumny rozchodza sie na cala szerokosc karty - inaczej
            # po prawej stronie listy zostaje szeroki pas pustego tla
            panel.grid_columnconfigure(col, weight=1, uniform="pad")
            row = 0
            for title, kind, slots in column_groups:
                head = tk.Frame(frame, bg=theme.PANEL)
                head.grid(row=row, column=0, columnspan=3, sticky="ew",
                          pady=(11 if row else 0, 4))
                tk.Label(head, text=title.upper(), font=self._fonts.tiny_bold,
                         bg=theme.PANEL, fg=theme.MUTED, anchor="w").pack(
                    side="left", padx=(4, 8))
                tk.Frame(head, bg=theme.BORDER, height=1).pack(
                    side="left", fill="x", expand=True, pady=(1, 0))
                row += 1
                for slot in slots:
                    self._rows[slot] = _Row(
                        frame, row, self._fonts, slot, kind,
                        on_click=self._on_row_click,
                        on_clear=self._on_row_clear,
                        on_hover=self._on_row_hover,
                        label=self._ROW_LABELS.get(slot, slot))
                    row += 1

    # ---------------------------------------------------------- interakcja

    def _on_row_click(self, slot: str, x_root: int, y_root: int) -> None:
        if slot in self._AXES:
            self.cancel_learning()
            if self._on_axis_click:
                self._on_axis_click(slot, x_root, y_root)
        else:
            self._start_learning(slot)

    def _on_row_clear(self, slot: str) -> None:
        if slot in self._AXES:
            if self._on_axis_clear:
                self._on_axis_clear(slot)
        else:
            self._clear_button(slot)

    def _clear_button(self, name: str) -> None:
        if self._on_clear:
            self._on_clear(name)

    def _on_canvas_click(self, name: str) -> None:
        if not name:
            self.cancel_learning()
        else:
            self._start_learning(name)

    def start_learning(self, name: str) -> None:
        """Wlacza tryb nauki dla kontrolki - uzywane takze przez kreator."""
        self._start_learning(name)

    def _start_learning(self, name: str) -> None:
        self._learning = name
        self._held = ()
        self.canvas.set_learning(name)
        self._start_pulse()
        self._refresh_rows()
        self._update_hint()
        if self._on_assign_request:
            self._on_assign_request(name)

    # --- miganie celu ---

    def _start_pulse(self) -> None:
        self._stop_pulse()
        self._pulse_on = True
        self.canvas.set_pulse(True)
        self._pulse_job = self.after(430, self._pulse)

    def _stop_pulse(self) -> None:
        if self._pulse_job is not None:
            try:
                self.after_cancel(self._pulse_job)
            except tk.TclError:
                pass
            self._pulse_job = None
        self._pulse_on = True
        self.canvas.set_pulse(True)

    def _pulse(self) -> None:
        self._pulse_job = None
        if self._learning is None:
            return
        self._pulse_on = not self._pulse_on
        try:
            self.canvas.set_pulse(self._pulse_on)
            self._refresh_rows()
            self._pulse_job = self.after(430, self._pulse)
        except tk.TclError:
            # okno zamkniete w trakcie migania - nie ma juz czego odswiezac
            self._pulse_job = None

    def _on_canvas_hover(self, part: Optional[str]) -> None:
        self._apply_hover(self._SLOTS_OF_PART.get(part or "", ()), part)

    def _on_row_hover(self, slot: Optional[str]) -> None:
        if slot is None:
            self._apply_hover((), None)
            return
        part = self._PART_OF_SLOT.get(slot)
        # najechanie na jeden kierunek podswietla caly drazek - od razu
        # widac, ktore cztery wiersze opisuja te sama galke
        self._apply_hover(self._SLOTS_OF_PART.get(part or "", (slot,)), part)

    def _apply_hover(self, slots, part: Optional[str]) -> None:
        slots = tuple(slots)
        if slots == self._hover_slots:
            return
        self._hover_slots = slots
        self.canvas.highlight(part)
        self._refresh_rows()

    def cancel_learning(self) -> None:
        if self._learning is None:
            return
        self._learning = None
        self._held = ()
        self._stop_pulse()
        self.canvas.set_learning(None)
        self._refresh_rows()
        self._update_hint()

    @property
    def learning(self) -> Optional[str]:
        return self._learning

    # ------------------------------------------------------- kreator nauki

    def set_wizard(self, step: Optional[int] = None,
                   total: Optional[int] = None) -> None:
        """Numer kroku pokazywany w podpowiedzi; None konczy kreator."""
        self._wizard = (step, total) if step and total else None
        self._update_hint()

    def set_held_buttons(self, held: Iterable[int]) -> None:
        """Przyciski plytki trzymane w tej chwili - widac, ze lacze dziala."""
        value = tuple(sorted(held))[:6]
        if value != self._held:
            self._held = value
            if self._learning is not None:
                self._update_hint()

    def _update_hint(self) -> None:
        if self._learning is None:
            self.hint.configure(text=self._IDLE_HINT,
                                fg=theme.MUTED, bg=theme.PANEL_HI)
            self._hint_box.configure(bg=theme.PANEL_HI,
                                     highlightbackground=theme.BORDER)
            self._hint_tick.configure(bg=theme.MUTED)
            return

        parts: List[str] = []
        if self._wizard:
            parts.append(f"Krok {self._wizard[0]} z {self._wizard[1]}")
        parts.append("Wcisnij przycisk kierownicy dla: "
                     f"{self._TITLES.get(self._learning, self._learning)}")
        current = self._assign.get(self._learning, -1)
        if current >= 0:
            parts.append(f"teraz: przycisk {current}")
        if self._held:
            parts.append("trzymasz: "
                         + ", ".join(str(i) for i in self._held))
        parts.append("Esc konczy" if self._wizard else "Esc anuluje")
        bg = theme.mix(theme.PANEL_HI, theme.WARN, 0.14)
        self.hint.configure(text="      ".join(parts), fg=theme.WARN, bg=bg)
        self._hint_box.configure(bg=bg, highlightbackground=theme.mix(
            theme.BORDER, theme.WARN, 0.5))
        self._hint_tick.configure(bg=theme.WARN)

    def _set_hint_idle(self) -> None:
        self._update_hint()

    # ---------------------------------------------------------------- stan

    def set_assignments(self, assign: Dict[str, int]) -> None:
        changed = False
        for name in self._CONTROLS:
            value = assign.get(name, -1)
            if self._assign.get(name) != value:
                self._assign[name] = value
                changed = True
                self._rows[name].set_text(
                    "" if value < 0 else f"przycisk {value}")
        if changed:
            self.canvas.set_assigned({n: self._assign[n] >= 0
                                      for n in self._CONTROLS})
            self._refresh_rows()
            if self._learning is not None:
                # podpowiedz pokazuje "teraz: przycisk N" - musi nadazyc
                self._update_hint()

    def set_axis_labels(self, labels: Dict[str, str]) -> None:
        changed = False
        for name in self._AXES:
            text = labels.get(name) or ""
            if self._axis_labels.get(name) != text:
                self._axis_labels[name] = text
                self._rows[name].set_text(text)
                changed = True
        if changed:
            self._refresh_rows()

    def set_active(self, active: Dict[str, bool]) -> None:
        changed = False
        for name in self._CONTROLS:
            value = bool(active.get(name, False))
            if self._active.get(name) != value:
                self._active[name] = value
                changed = True
        if changed:
            self.canvas.set_active(self._active)
            self._refresh_rows()

    def set_sticks(self, lx: float, ly: float, rx: float, ry: float) -> None:
        self.canvas.set_sticks(lx, ly, rx, ry)
        for axis, value in (("LX", lx), ("LY", ly), ("RX", rx), ("RY", ry)):
            self._rows[f"{axis}+"].set_value(max(0.0, value))
            self._rows[f"{axis}-"].set_value(max(0.0, -value))

    def set_triggers(self, lt: int, rt: int) -> None:
        self.canvas.set_triggers(lt, rt)
        self._rows["LT"].set_value(lt / 255.0)
        self._rows["RT"].set_value(rt / 255.0)

    def _refresh_rows(self) -> None:
        hover = self._hover_slots
        for slot, row in self._rows.items():
            if self._learning == slot:
                state = "learning"
            elif row.kind == "button":
                if self._active.get(slot):
                    state = "active"
                else:
                    state = "set" if self._assign.get(slot, -1) >= 0 else "empty"
            else:
                state = "set" if self._axis_labels.get(slot) else "empty"
            row.set_state(state, hover=slot in hover, pulse=self._pulse_on)
