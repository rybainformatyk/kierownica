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
from dataclasses import replace
from tkinter import ttk
from typing import (Callable, Dict, Iterable, List, Optional, Sequence,
                    Tuple)

from ..config import (PAD_BUTTON_NAMES, PAD_DIRECTIONS, PAD_DIRECTION_LABELS,
                      PAD_DIRECTION_SHORT)
from . import art, theme
from .theme import Fonts
from .widgets import round_points

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
    "DPAD_UP": "▲ góra",
    "DPAD_DOWN": "▼ dół",
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
    "LS": "Klik lewego drążka (L3)", "RS": "Klik prawego drążka (R3)",
    "A": "Przycisk A", "B": "Przycisk B", "X": "Przycisk X", "Y": "Przycisk Y",
    "BACK": "Back", "START": "Start", "GUIDE": "Guide",
    "DPAD_UP": "Krzyżak w górę", "DPAD_DOWN": "Krzyżak w dół",
    "DPAD_LEFT": "Krzyżak w lewo", "DPAD_RIGHT": "Krzyżak w prawo",
})
BOX_TITLES = TITLES          # zgodnosc z poprzednia nazwa

#: Kolumny listy przypisan: (naglowek, rodzaj, gniazda). Dwie kolumny
#: zamiast trzech - inaczej calosc nie miesci sie obok rysunku w oknie
#: 1240 px, a trzecia kolumna wychodzi za krawedz (zakladka przewija sie
#: tylko w pionie).
GROUPS: Tuple[Tuple[Tuple[str, str, Tuple[str, ...]], ...], ...] = (
    (
        ("Lewy drążek", "axis", ("LX-", "LX+", "LY+", "LY-")),
        ("Prawy drążek", "axis", ("RX-", "RX+", "RY+", "RY-")),
        ("Spusty analogowe", "axis", ("LT", "RT")),
        ("Bumpery", "button", ("LB", "RB")),
    ),
    (
        ("Przyciski", "button", ("A", "B", "X", "Y")),
        ("Krzyżak", "button", ("DPAD_UP", "DPAD_DOWN", "DPAD_LEFT",
                               "DPAD_RIGHT")),
        ("Klik drążków i środek", "button", ("LS", "RS", "BACK", "START",
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
_BUMPER_Y = (-118.0, -98.0)            # dol nad przyciskiem Y (gora Y: -96)
_TRIGGER_Y = (-156.0, -122.0)

#: Paleta rysunku: grafitowy plastik, ciemniejsze wglebienia, jasniejsze
#: przyciski. Ciemniejsze od kart, zeby kontroler odcinal sie od tla.
_SHELL_TOP = "#465163"
_SHELL_BOTTOM = "#222934"
_SHOULDER = "#2b323e"
_WELL_TOP = "#0d1117"
_WELL_BOTTOM = "#1f2631"

_INK = "#0b0e14"
_CAP_STYLE = art.CapStyle(
    top="#434d5e", bottom="#272e39", edge="#56627a", on=theme.ACCENT,
    glyph=theme.TEXT_DIM, glyph_on=theme.ACCENT_INK, accent=theme.ACCENT,
    warn=theme.WARN, ink="#20160a")
_DARK_STYLE = replace(_CAP_STYLE, top="#2d3440", bottom="#1b2028",
                      edge="#3e4859")
_WELL_STYLE = replace(_CAP_STYLE, top=_WELL_TOP, bottom=_WELL_BOTTOM,
                      edge="#2a323f", gloss=False)
_KNOB_STYLE = replace(_CAP_STYLE, top="#55617a", bottom="#2c333f",
                      edge="#68758e")

FACE_COLORS = {
    "A": "#5ac25a",
    "B": "#e8534a",
    "X": "#3f9be8",
    "Y": "#f0c419",
}


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
    return art.contains(("poly", tuple(spline(SILHOUETTE))), x, y)


class _Part:
    """Czesc rysunku: ksztalt, wyglad, obrazek na plotnie i stan."""

    __slots__ = ("name", "shape", "style", "glyph", "clickable", "shadow",
                 "item", "state", "dx", "dy")

    def __init__(self, name: str, shape: art.Shape, style: art.CapStyle,
                 glyph: Optional[Callable[[art.Paint, str], None]],
                 clickable: bool, shadow: bool) -> None:
        self.name = name
        self.shape = shape
        self.style = style
        self.glyph = glyph
        self.clickable = clickable
        self.shadow = shadow
        self.item = 0
        self.state = ""
        self.dx = self.dy = 0.0         # przesuniecie (galka drazka)


class PadCanvas(tk.Canvas):
    """Rysowany kontroler: klikalne przyciski, podglad osi na zywo.

    Tlo (korpus) i kazda kontrolka to wygladzone obrazki z art.py. Kontrolka
    ma gotowy obrazek dla kazdego stanu (zwykla, przypisana, wskazana,
    wcisnieta, nauka) - zmiana stanu to tylko podmiana obrazka.

    Podklasa (rysunek kierownicy G29) podmienia _CONTROLS, _SIZE, _paint_body
    i _build_parts().
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

        self._parts: Dict[str, _Part] = {}       # w kolejnosci rysowania
        self._shapes: Dict[str, List[int]] = {}  # nazwa -> [obrazek na plotnie]
        self._sprites: Dict[Tuple[str, str], Tuple[object, Tuple[int, int]]] = {}
        self._images: List[object] = []          # tlo i inne stale obrazki
        self._active: Dict[str, bool] = {c: False for c in self._CONTROLS}
        self._assigned: Dict[str, bool] = {c: False for c in self._CONTROLS}
        self._learning: Optional[str] = None
        self._pulse_on = True
        self._highlight: Optional[str] = None
        self._hover: Optional[str] = None
        # co juz jest narysowane - te same wartosci nie przerysowuja plotna
        self._sticks_shown: Optional[Tuple[float, ...]] = None
        self._triggers_shown: Optional[Tuple[int, int]] = None
        self._angle_shown: Optional[float] = None
        self._pedals_shown: Dict[str, float] = {}

        self._trig_fill: Dict[str, int] = {}
        self._trig_text: Dict[str, int] = {}
        self._trig_box: Dict[str, Tuple[float, float, float, float]] = {}

        self._build()
        self.bind("<Button-1>", self._on_click)
        self.bind("<Button-3>", self._on_right_click)
        self.bind("<Motion>", self._on_motion)
        self.bind("<Leave>", self._on_leave)

    # ----------------------------------------------------------- budowanie

    def _build(self) -> None:
        body = art.Paint(self._SIZE[0], self._SIZE[1])
        self._paint_body(body)
        self._background = art.photo(
            art.opaque(body.image(), theme.PANEL).convert("RGB"), self)
        self.create_image(0, 0, image=self._background, anchor="nw")
        self._build_parts()
        self._build_overlay()
        self._refresh()

    def _add(self, name: str, shape: art.Shape, style: art.CapStyle,
             glyph: Optional[Callable[[art.Paint, str], None]] = None,
             clickable: bool = True, shadow: bool = True) -> _Part:
        """Dodaje kontrolke (albo czesc tylko do podswietlania).

        Nie nazywac tego `_register` - tak nazywa sie wewnetrzna metoda
        tkinter.Misc, ktorej uzywa bind()."""
        part = _Part(name, shape, style, glyph, clickable, shadow)
        part.item = self.create_image(0, 0, anchor="nw")
        self._parts[name] = part
        self._shapes[name] = [part.item]
        self._show(part, "idle")
        return part

    def _sprite(self, part: _Part, state: str):
        key = (part.name, state)
        cached = self._sprites.get(key)
        if cached is None:
            img, pos = art.render_cap(part.shape, state, part.style,
                                      part.glyph, part.shadow)
            cached = (art.photo(img, self), pos)
            self._sprites[key] = cached
        return cached

    def _show(self, part: _Part, state: str) -> None:
        image, (x, y) = self._sprite(part, state)
        self.itemconfigure(part.item, image=image)
        self.coords(part.item, x + part.dx, y + part.dy)
        part.state = state

    def visual_state(self, name: str) -> str:
        """Stan, w jakim kontrolka jest teraz narysowana (do testow)."""
        part = self._parts.get(name)
        return part.state if part else ""

    # --- tlo: korpus pada ---

    def _paint_body(self, p: art.Paint) -> None:
        body = ("poly", tuple((CX + x, CY + y) for x, y in spline(SILHOUETTE)))

        # barki za spustami - pod korpusem, jak w prawdziwym padzie
        for sign in (-1, 1):
            lo, hi = sorted((CX + sign * (_SHOULDER_X[0] - 6),
                             CX + sign * (_SHOULDER_X[1] + 6)))
            shoulder = ("rrect", lo, CY + _TRIGGER_Y[0] + 8, hi, CY - 60, 22)
            p.shadow(shoulder, dy=3, blur=4, alpha=120)
            p.gradient(shoulder, "#353d4a", _SHOULDER)

        p.shadow(body, dy=12, blur=11, alpha=190)
        p.gradient(body, _SHELL_TOP, _SHELL_BOTTOM)
        # swiatlo z gory i lekka poswiata akcentu od dolu - bryla
        p.sheen(body, alpha=38, frac=0.45)
        p.radial(CX, CY + 240, 260, theme.ACCENT, theme.ACCENT, alpha_in=22,
                 alpha_out=0, clip=body)
        self._edge(p, lambda oy: oy < -60, "#ffffff", 1.6, 60)
        self._edge(p, lambda oy: oy > 110, "#000000", 2.0, 90)

        # gniazdo krzyzaka i podstawa krzyza
        dx, dy = CX + _DPAD[0], CY + _DPAD[1]
        arm, thick = _DPAD_ARM, _DPAD_THICK
        well = ("circle", dx, dy, arm + 8)
        p.gradient(well, _WELL_TOP, _WELL_BOTTOM)
        p.stroke(well, "#000000", 1.0, alpha=120)
        for box in ((dx - thick / 2, dy - arm, dx + thick / 2, dy + arm),
                    (dx - arm, dy - thick / 2, dx + arm, dy + thick / 2)):
            p.shadow(("rrect", *box, 5), dy=2, blur=2, alpha=150)
        for box in ((dx - thick / 2, dy - arm, dx + thick / 2, dy + arm),
                    (dx - arm, dy - thick / 2, dx + arm, dy + thick / 2)):
            p.gradient(("rrect", *box, 5), _CAP_STYLE.top, _CAP_STYLE.bottom,
                       y0=dy - arm, y1=dy + arm)
        p.fill(("circle", dx, dy, 3.5), "#1a1f27")

        # podpisy malych przyciskow
        for sign, label in ((-1, "Back"), (1, "Start")):
            p.text(CX + sign * _CENTER_BTN_X,
                   CY + _CENTER_BTN_Y + _CENTER_BTN_R + 11, label, 7.5,
                   theme.MUTED)
        # oprawa przycisku Guide
        gx, gy = CX + _GUIDE[0], CY + _GUIDE[1]
        p.fill(("circle", gx, gy, _GUIDE_R + 4), "#1a1f27", alpha=200)

    def _edge(self, p: art.Paint, which: Callable[[float], bool], color: str,
              width: float, alpha: int) -> None:
        """Swiatlo albo cien na fragmentach obrysu korpusu."""
        poly = spline(SILHOUETTE)
        run: List[Tuple[float, float]] = []
        runs: List[List[Tuple[float, float]]] = []
        # zaczynamy od punktu, ktory sie nie kwalifikuje - zamkniety obrys
        # nie rozpadnie sie wtedy na dwa kawalki tego samego luku
        start = next((i for i, (_x, oy) in enumerate(poly) if not which(oy)), 0)
        n = len(poly)
        for k in range(n + 1):
            x, oy = poly[(start + k) % n]
            if which(oy):
                run.append((CX + x, CY + oy))
            elif run:
                runs.append(run)
                run = []
        if run:
            runs.append(run)
        for r in runs:
            if len(r) >= 2:
                inset = [(x, y + width / 2 if color == "#ffffff" else y - width / 2)
                         for x, y in r]
                p.line(inset, color, width, alpha=alpha)

    # --- kontrolki ---

    def _build_parts(self) -> None:
        f = self._fonts

        # spusty: panel z miernikiem; sam miernik i liczba to elementy plotna
        for name in ("LT", "RT"):
            sign = -1 if name == "LT" else 1
            lo, hi = sorted((CX + sign * _SHOULDER_X[0], CX + sign * _SHOULDER_X[1]))
            box = (lo, CY + _TRIGGER_Y[0], hi, CY + _TRIGGER_Y[1])
            self._trig_box[name] = box
            self._add(name, ("rrect", *box, 11), _DARK_STYLE, clickable=False)

        for name in ("LB", "RB"):
            sign = -1 if name == "LB" else 1
            lo, hi = sorted((CX + sign * _SHOULDER_X[0], CX + sign * _SHOULDER_X[1]))
            box = (lo, CY + _BUMPER_Y[0], hi, CY + _BUMPER_Y[1])
            self._add(name, ("rrect", *box, 12), _CAP_STYLE,
                      glyph=_text_glyph((lo + hi) / 2, (box[1] + box[3]) / 2,
                                        name, 9, bold=True))

        # drazki: wglebienie (tylko podswietlanie) i galka (klik = L3/R3)
        for side, (ox, oy) in _STICKS.items():
            sx, sy = CX + ox, CY + oy
            self._add("STICK_L" if side == "L" else "STICK_R",
                      ("circle", sx, sy, _STICK_RING), _WELL_STYLE,
                      glyph=_stick_ticks(sx, sy), clickable=False, shadow=False)
        for side, (ox, oy) in _STICKS.items():
            sx, sy = CX + ox, CY + oy
            self._add("LS" if side == "L" else "RS",
                      ("circle", sx, sy, _STICK_KNOB), _KNOB_STYLE,
                      glyph=_knob_glyph(sx, sy, "L3" if side == "L" else "R3"))

        # krzyzak: cztery ramiona; srodek krzyza jest w tle
        dx, dy = CX + _DPAD[0], CY + _DPAD[1]
        arm, half = _DPAD_ARM, _DPAD_THICK / 2
        inner = half - 1
        specs = {
            "DPAD_UP": ((dx - half, dy - arm, dx + half, dy - inner), (0, -1)),
            "DPAD_DOWN": ((dx - half, dy + inner, dx + half, dy + arm), (0, 1)),
            "DPAD_LEFT": ((dx - arm, dy - half, dx - inner, dy + half), (-1, 0)),
            "DPAD_RIGHT": ((dx + inner, dy - half, dx + arm, dy + half), (1, 0)),
        }
        for name, (box, direction) in specs.items():
            cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
            self._add(name, ("rrect", *box, 5), _CAP_STYLE,
                      glyph=_arrow_glyph(cx, cy, direction, 4.2), shadow=False)

        # przyciski A B X Y: ciemny plastik i kolorowa litera
        fx, fy = CX + _FACE[0], CY + _FACE[1]
        off = _FACE_OFFSET
        for name, ox, oy in (("Y", 0, -off), ("X", -off, 0),
                             ("B", off, 0), ("A", 0, off)):
            color = FACE_COLORS[name]
            style = replace(_CAP_STYLE, top="#333a46", bottom="#1e232c",
                            edge=art.mix("#3e4859", color, 0.35), on=color,
                            glyph=color, glyph_on=_INK)
            self._add(name, ("circle", fx + ox, fy + oy, _FACE_R), style,
                      glyph=_text_glyph(fx + ox, fy + oy + 0.5, name, 13,
                                        bold=True))

        # Back i Start
        r = _CENTER_BTN_R
        for name, sign in (("BACK", -1), ("START", 1)):
            x, y = CX + sign * _CENTER_BTN_X, CY + _CENTER_BTN_Y
            self._add(name, ("circle", x, y, r), _CAP_STYLE,
                      glyph=_view_glyph(x, y) if name == "BACK"
                      else _menu_glyph(x, y))

        # Guide
        x, y = CX + _GUIDE[0], CY + _GUIDE[1]
        self._add("GUIDE", ("circle", x, y, _GUIDE_R),
                  replace(_KNOB_STYLE, glyph=theme.TEXT),
                  glyph=_guide_glyph(x, y))

    def _build_overlay(self) -> None:
        """Miernik spustow i liczby - zwykle elementy plotna nad obrazkami
        (proste prostokaty nie potrzebuja wygladzania)."""
        for name, (x1, y1, x2, y2) in self._trig_box.items():
            # rowek miernika na dole panelu, nad nim nazwa i wartosc
            self.create_rectangle(x1 + 10, y2 - 9, x2 - 10, y2 - 6,
                                  fill="#0e1116", outline="")
            self._trig_fill[name] = self.create_rectangle(
                x1 + 10, y2 - 9, x1 + 10, y2 - 6, fill=theme.ACCENT,
                outline="", state="hidden")
            self._trig_text[name] = self.create_text(
                (x1 + x2) / 2, (y1 + y2) / 2 - 3, text=name,
                font=self._fonts.small_bold, fill=theme.TEXT_DIM)

    # ---------------------------------------------------------- interakcja

    def parts_at(self, x: float, y: float) -> List[str]:
        """Wszystkie czesci pod punktem, od wierzchniej (do testow i klikania)."""
        out = []
        for part in reversed(list(self._parts.values())):
            if art.contains(part.shape, x - part.dx, y - part.dy):
                out.append(part.name)
        return out

    def _hit(self, x: float, y: float) -> Optional[str]:
        names = self.parts_at(x, y)
        return names[0] if names else None

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
        # co klatke przychodzi to samo, gdy nikt nie rusza drazkiem -
        # przesuwanie obrazkow w miejscu to zbedne przerysowanie
        key = (round(lx, 3), round(ly, 3), round(rx, 3), round(ry, 3))
        if key == self._sticks_shown:
            return
        self._sticks_shown = key
        for side, (dx, dy) in (("L", (lx, ly)), ("R", (rx, ry))):
            part = self._parts["LS" if side == "L" else "RS"]
            part.dx = max(-1.0, min(1.0, dx)) * _STICK_TRAVEL
            part.dy = -max(-1.0, min(1.0, dy)) * _STICK_TRAVEL
            _image, (x, y) = self._sprite(part, part.state)
            self.coords(part.item, x + part.dx, y + part.dy)

    def set_triggers(self, lt: int, rt: int) -> None:
        key = (int(lt), int(rt))
        if key == self._triggers_shown:
            return
        self._triggers_shown = key
        for name, raw in (("LT", lt), ("RT", rt)):
            raw = max(0, min(255, int(raw)))
            x1, y1, x2, y2 = self._trig_box[name]
            width = (x2 - x1 - 20) * raw / 255.0
            self.itemconfigure(self._trig_fill[name],
                               state="hidden" if width < 1 else "normal")
            if width >= 1:
                self.coords(self._trig_fill[name], x1 + 10, y2 - 9,
                            x1 + 10 + width, y2 - 6)
            self.itemconfigure(self._trig_text[name],
                               text=name if raw == 0 else f"{name}  {raw}")

    def _state_of(self, name: str) -> str:
        if self._learning == name:
            return "learn_on" if self._pulse_on else "learn_off"
        if self._active.get(name):
            return "active"
        if self._highlight == name:
            return "hover"
        if self._assigned.get(name):
            return "assigned"
        return "idle"

    def _refresh(self) -> None:
        for name, part in self._parts.items():
            state = self._state_of(name)
            if state != part.state:
                self._show(part, state)


# -------------------------------------------------------------- piktogramy
#
# Kazdy zwraca funkcje (Paint, kolor) -> None, ktora rysuje znak na
# kontrolce. Kolor dostaje od render_cap - inny na wcisnietej kontrolce.

def _text_glyph(x: float, y: float, text: str, size: float,
                bold: bool = False) -> Callable[[art.Paint, str], None]:
    def draw(p: art.Paint, color: str) -> None:
        p.text(x, y, text, size, color, bold=bold)
    return draw


def _arrow_glyph(x: float, y: float, direction: Tuple[int, int],
                 size: float) -> Callable[[art.Paint, str], None]:
    ux, uy = direction
    px, py = -uy, ux                    # prostopadly
    tip = (x + ux * size, y + uy * size)
    left = (x - ux * size * 0.6 + px * size, y - uy * size * 0.6 + py * size)
    right = (x - ux * size * 0.6 - px * size, y - uy * size * 0.6 - py * size)

    def draw(p: art.Paint, color: str) -> None:
        p.fill(("poly", (tip, left, right)), color)
    return draw


def _stick_ticks(x: float, y: float) -> Callable[[art.Paint, str], None]:
    """Cztery kreski przy krawedzi wglebienia - kierunki drazka."""
    def draw(p: art.Paint, color: str) -> None:
        for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
            p.line([(x + dx * 29, y + dy * 29), (x + dx * 35, y + dy * 35)],
                   color, 2.0, alpha=110)
    return draw


def _knob_glyph(x: float, y: float, label: str) -> Callable[[art.Paint, str], None]:
    """Wklesla czasza galki z drobnym napisem."""
    def draw(p: art.Paint, color: str) -> None:
        cup = ("circle", x, y, _STICK_KNOB - 5)
        p.gradient(cup, "#1c222b", "#3a4352")
        p.stroke(cup, "#000000", 1.0, alpha=90)
        p.text(x, y, label, 7, color, bold=True, alpha=200)
    return draw


def _view_glyph(x: float, y: float) -> Callable[[art.Paint, str], None]:
    """Dwa nachodzace prostokaty - przycisk Back/View."""
    def draw(p: art.Paint, color: str) -> None:
        p.stroke(("rrect", x - 5.5, y - 4, x + 1.5, y + 2, 1.2), color, 1.4)
        p.fill(("rrect", x - 1.5, y - 2, x + 5.5, y + 4, 1.2), color)
    return draw


def _menu_glyph(x: float, y: float) -> Callable[[art.Paint, str], None]:
    """Trzy kreski - przycisk Start/Menu."""
    def draw(p: art.Paint, color: str) -> None:
        for dy in (-3.5, 0.0, 3.5):
            p.line([(x - 4.5, y + dy), (x + 4.5, y + dy)], color, 1.6)
    return draw


def _guide_glyph(x: float, y: float) -> Callable[[art.Paint, str], None]:
    """Pierscien z kropka - przycisk Guide (bez cudzych logo)."""
    def draw(p: art.Paint, color: str) -> None:
        ring = ("circle", x, y, 11)
        p.stroke(ring, color, 2.6)
        p.fill(("circle", x, y, 4.2), color)
    return draw


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
    _IDLE_HINT = ("Kliknij wiersz: kierunki i spusty otwierają listę "
                  "źródeł, przyciski uczą się z kierownicy. Prawy przycisk "
                  "myszy czyści gniazdo.")
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
        items = (("wciśnięte", theme.ACCENT),
                 ("przypisane", theme.mix(theme.PANEL_HI, theme.ACCENT, 0.20)),
                 ("uczy się", theme.WARN),
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
        parts.append("Wciśnij przycisk kierownicy dla: "
                     f"{self._TITLES.get(self._learning, self._learning)}")
        current = self._assign.get(self._learning, -1)
        if current >= 0:
            parts.append(f"teraz: przycisk {current}")
        if self._held:
            parts.append("trzymasz: "
                         + ", ".join(str(i) for i in self._held))
        parts.append("Esc kończy" if self._wizard else "Esc anuluje")
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
