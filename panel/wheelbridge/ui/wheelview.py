"""
Widok kierownicy Logitech G29 - przypisywanie przyciskow trybu G29.

Dziala dokladnie jak zakladka Pad (dziedziczy PadView): rysunek do klikania
po lewej, lista gniazd po prawej, klikniecie wlacza nauke, a wcisniety
przycisk kierownicy trafia do gniazda. Rozni sie tylko rysunkiem i lista.

Na rysunku na zywo widac to, co dostaje gra: wcisniete przyciski (z
gotowego raportu G29), obrot kierownicy (znacznik na obreczy) i pedaly.

Geometria (stale _RIM_*, _PLATE, _FACE ...) jest sprawdzana testem:
kontrolki nie moga na siebie nachodzic, wychodzic poza plotno ani poza
plyte, na ktorej leza.
"""

from __future__ import annotations

import math
import tkinter as tk
from typing import Dict, List, Tuple

from ..config import G29_CONTROL_LABELS, G29_CONTROLS
from . import theme
from .padview import PadCanvas, PadView
from .theme import Fonts
from .widgets import round_points

CANVAS_W, CANVAS_H = 486, 388
CX, CY = 243.0, 178.0

#: Obrecz: promien zewnetrzny i wewnetrzny.
_RIM_OUT = 150.0
_RIM_IN = 121.0
#: Plyta z przyciskami (poziome ramiona) i dolne ramie - (x1, y1, x2, y2, r).
_PLATE = (-125.0, -50.0, 125.0, 54.0, 24.0)
_SPOKE = (-50.0, 30.0, 50.0, 128.0, 18.0)
#: Lopatki za obrecza - w calosci na zewnatrz, zeby dalo sie je kliknac.
_PADDLE = (-206.0, -86.0, -158.0, -24.0)        # lewa; prawa to odbicie

_DPAD = (-82.0, -12.0)
_DPAD_ARM = 22.0          # od srodka do konca ramienia
_DPAD_HALF = 7.5          # polowa szerokosci ramienia
_DPAD_GAP = 8.0           # srodek krzyzaka zostaje pusty

_FACE = (82.0, -12.0)
_FACE_STEP = 20.0
_FACE_R = 9.5

_SMALL_R = 10.0                                   # L2/L3/R2/R3
_SMALL = {"L2": (-104.0, 28.0), "L3": (-74.0, 36.0),
          "R2": (104.0, 28.0), "R3": (74.0, 36.0)}
_PILL = {"SHARE": (-34.0, -30.0), "OPTIONS": (34.0, -30.0)}
_PILL_W, _PILL_H = 22.0, 10.0
_PS = (0.0, 20.0)
_PS_R = 11.0

_PLUS = (-26.0, 66.0)
_MINUS = (-26.0, 92.0)
_PM_R = 8.0
_DIAL = (16.0, 80.0)
_DIAL_OUT = 22.0
_DIAL_IN = 12.0
_ENTER_R = 9.0

#: Skrzynia biegow (osobne akcesorium G29) - prawy dolny rog, poza obrecza.
_GEARS = {"GEAR_1": (160.0, 146.0), "GEAR_3": (184.0, 146.0),
          "GEAR_5": (208.0, 146.0), "GEAR_R": (232.0, 146.0),
          "GEAR_2": (160.0, 194.0), "GEAR_4": (184.0, 194.0),
          "GEAR_6": (208.0, 194.0)}
_GEAR_R = 9.0
_GEAR_LABELS = {"GEAR_1": "1", "GEAR_2": "2", "GEAR_3": "3", "GEAR_4": "4",
                "GEAR_5": "5", "GEAR_6": "6", "GEAR_R": "R"}

#: Pedaly - lewy dolny rog, poza obrecza: sprzeglo, hamulec, gaz.
_PEDALS = (("clutch", -222.0, "SPR"), ("brake", -196.0, "HAM"),
           ("throttle", -170.0, "GAZ"))
_PEDAL_TOP, _PEDAL_BOTTOM, _PEDAL_HALF = 126.0, 186.0, 7.0

FACE_COLORS = {
    "TRIANGLE": "#3fbf9f",
    "CIRCLE": "#e8534a",
    "CROSS": "#5b8def",
    "SQUARE": "#d97ab8",
}

_RIM = theme.darken(theme.PANEL_HI, 0.30)
_RIM_HI = theme.lighten(theme.PANEL_HI, 0.10)
_PLATE_FILL = theme.lighten(theme.PANEL_HI, 0.16)
_PLATE_EDGE = theme.lighten(theme.PANEL_HI, 0.30)
_CAP = theme.lighten(theme.PANEL_HI, 0.30)
_METAL = theme.mix(theme.lighten(theme.PANEL_HI, 0.42), theme.ACCENT, 0.06)
_DIAL_FILL = "#a8323a"


def _ring_half(cx: float, cy: float, r_out: float, r_in: float,
               a0: float, a1: float, steps: int = 18) -> List[float]:
    """Wycinek pierscienia jako wielokat (katy w stopniach, 0 = w prawo)."""
    pts: List[float] = []
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts += [cx + r_out * math.cos(a), cy + r_out * math.sin(a)]
    for i in range(steps, -1, -1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts += [cx + r_in * math.cos(a), cy + r_in * math.sin(a)]
    return pts


class WheelCanvas(PadCanvas):
    """Rysunek G29: klikalne przyciski, obrot i pedaly na zywo."""

    _CONTROLS: Tuple[str, ...] = G29_CONTROLS
    _SIZE: Tuple[int, int] = (CANVAS_W, CANVAS_H)

    # ------------------------------------------------------------ budowanie

    def _build(self) -> None:
        self._pedal_fill: Dict[str, int] = {}
        #: piktogramy przyciskow twarzowych: (element, opcja koloru) - obrys
        #: trojkata/kwadratu/kolka siedzi w "outline", krzyzyka w "fill"
        self._face_glyphs: Dict[str, List[Tuple[int, str]]] = {}
        self._build_paddles()
        self._build_rim()
        self._build_plate()
        self._build_dpad()
        self._build_face()
        self._build_small()
        self._build_center()
        self._build_dial()
        self._build_shifter()
        self._build_pedals()
        # znacznik obrotu na samej gorze - nie jest klikalny
        self._angle_line = self.create_line(0, 0, 0, 0, fill=theme.ACCENT,
                                            width=6, capstyle="round")
        self._angle_text = self.create_text(CX, CY - 86, text="",
                                            font=self._fonts.heading,
                                            fill=theme.TEXT)
        self.set_angle(0.0)
        self._refresh()

    def _circle(self, x: float, y: float, r: float, **kw) -> int:
        return self.create_oval(CX + x - r, CY + y - r, CX + x + r,
                                CY + y + r, **kw)

    def _build_paddles(self) -> None:
        x1, y1, x2, y2 = _PADDLE
        # _PADDLE opisuje lewa lopatke (ujemne x) - prawa to jej odbicie,
        # dlatego liczymy od wartosci bezwzglednych i strony
        for name, side, text in (("PADDLE_L", -1.0, "L"), ("PADDLE_R", 1.0, "P")):
            # trzonek chowa sie za obrecza - tylko optycznie laczy lopatke
            sx = sorted((side * 140.0, side * 166.0))
            self.create_rectangle(CX + sx[0], CY - 62, CX + sx[1], CY - 48,
                                  fill=theme.darken(_METAL, 0.35), outline="")
            lo, hi = sorted((side * abs(x1), side * abs(x2)))
            body = self._round_rect(CX + lo, CY + y1, CX + hi, CY + y2, 12,
                                    fill=_METAL, outline=theme.BORDER_HI,
                                    width=1)
            groove = self.create_line(CX + (lo + hi) / 2, CY + y1 + 12,
                                      CX + (lo + hi) / 2, CY + y2 - 12,
                                      fill=theme.darken(_METAL, 0.25), width=2)
            label = self.create_text(CX + (lo + hi) / 2, CY + y2 + 12,
                                     text=text, font=self._fonts.tiny_bold,
                                     fill=theme.MUTED)
            self._part(name, [body], base_fill=_METAL)
            self._mark(name, [groove], theme.darken(_METAL, 0.25),
                       theme.ACCENT_INK)
            self._mark(name, [label], theme.MUTED, theme.ACCENT)

    def _build_rim(self) -> None:
        ro, ri = _RIM_OUT, _RIM_IN
        self._circle(0, 6, ro + 2, fill=theme.SHADOW, outline="")     # cien
        self._circle(0, 0, ro, fill=_RIM, outline=theme.BORDER_HI, width=1)
        self._circle(0, 0, ro - 5, fill="", outline=_RIM_HI, width=1)
        self._circle(0, 0, ri, fill=theme.PANEL, outline=theme.BORDER_HI,
                     width=1)

    def _build_plate(self) -> None:
        x1, y1, x2, y2, r = _SPOKE
        self._round_rect(CX + x1, CY + y1, CX + x2, CY + y2, r,
                         fill=_PLATE_FILL, outline=_PLATE_EDGE, width=1)
        x1, y1, x2, y2, r = _PLATE
        self._round_rect(CX + x1, CY + y1, CX + x2, CY + y2, r,
                         fill=_PLATE_FILL, outline=_PLATE_EDGE, width=1)
        # zlaczenie plyt - dolne ramie wchodzi pod gorna bez widocznej kreski
        sx1, _sy1, sx2, _sy2, _r = _SPOKE
        self.create_rectangle(CX + sx1 + 1, CY + y2 - 26, CX + sx2 - 1,
                              CY + y2 + 2, fill=_PLATE_FILL, outline="")
        self.create_text(CX, CY - 14, text="G29", font=self._fonts.small_bold,
                         fill=theme.MUTED)

    def _build_dpad(self) -> None:
        cx, cy = _DPAD
        a, h, g = _DPAD_ARM, _DPAD_HALF, _DPAD_GAP
        self._circle(cx, cy, a + 6, fill=theme.darken(_PLATE_FILL, 0.18),
                     outline="")
        arms = {
            "DPAD_UP": ((cx - h, cy - a, cx + h, cy - g), "▲", (cx, cy - (a + g) / 2)),
            "DPAD_DOWN": ((cx - h, cy + g, cx + h, cy + a), "▼", (cx, cy + (a + g) / 2)),
            "DPAD_LEFT": ((cx - a, cy - h, cx - g, cy + h), "◀", (cx - (a + g) / 2, cy)),
            "DPAD_RIGHT": ((cx + g, cy - h, cx + a, cy + h), "▶", (cx + (a + g) / 2, cy)),
        }
        for name, ((x1, y1, x2, y2), arrow, (tx, ty)) in arms.items():
            body = self._round_rect(CX + x1, CY + y1, CX + x2, CY + y2, 3,
                                    fill=_CAP, outline="")
            glyph = self.create_text(CX + tx, CY + ty, text=arrow,
                                     font=(self._fonts.family, 6),
                                     fill=theme.TEXT_DIM)
            self._part(name, [body], base_fill=_CAP)
            self._mark(name, [glyph], theme.TEXT_DIM, theme.ACCENT_INK)

    def _build_face(self) -> None:
        cx, cy = _FACE
        s, r = _FACE_STEP, _FACE_R
        spots = {"TRIANGLE": (cx, cy - s), "SQUARE": (cx - s, cy),
                 "CIRCLE": (cx + s, cy), "CROSS": (cx, cy + s)}
        for name, (x, y) in spots.items():
            color = FACE_COLORS[name]
            body = self._circle(x, y, r, fill=_CAP, outline=color, width=2)
            px, py = CX + x, CY + y
            if name == "TRIANGLE":
                g = [self.create_polygon(px, py - 5, px + 5, py + 4, px - 5,
                                         py + 4, fill="", outline=color,
                                         width=1.6)]
            elif name == "SQUARE":
                g = [self.create_rectangle(px - 4, py - 4, px + 4, py + 4,
                                           outline=color, width=1.6)]
            elif name == "CIRCLE":
                g = [self.create_oval(px - 4.5, py - 4.5, px + 4.5, py + 4.5,
                                      outline=color, width=1.6)]
            else:
                g = [self.create_line(px - 4, py - 4, px + 4, py + 4,
                                      fill=color, width=1.6),
                     self.create_line(px - 4, py + 4, px + 4, py - 4,
                                      fill=color, width=1.6)]
            self._part(name, [body], base_fill=_CAP, on_fill=color)
            self._face_glyphs[name] = [
                (item, "fill" if self.type(item) == "line" else "outline")
                for item in g]

    def _build_small(self) -> None:
        for name, (x, y) in _SMALL.items():
            body = self._circle(x, y, _SMALL_R, fill=_CAP,
                                outline=theme.BORDER_HI, width=1)
            label = self.create_text(CX + x, CY + y, text=name,
                                     font=(self._fonts.family, 7, "bold"),
                                     fill=theme.TEXT_DIM)
            self._part(name, [body], base_fill=_CAP)
            self._mark(name, [label], theme.TEXT_DIM, theme.ACCENT_INK)

    def _build_center(self) -> None:
        for name, (x, y) in _PILL.items():
            w, h = _PILL_W / 2, _PILL_H / 2
            body = self._round_rect(CX + x - w, CY + y - h, CX + x + w,
                                    CY + y + h, h, fill=_CAP, outline="")
            label = self.create_text(CX + x, CY + y - h - 7,
                                     text=G29_CONTROL_LABELS[name].upper(),
                                     font=(self._fonts.family, 6, "bold"),
                                     fill=theme.MUTED)
            self._part(name, [body], base_fill=_CAP)
            self._mark(name, [label], theme.MUTED, theme.ACCENT)
        x, y = _PS
        body = self._circle(x, y, _PS_R, fill=theme.darken(_PLATE_FILL, 0.35),
                            outline=theme.BORDER_HI, width=1)
        label = self.create_text(CX + x, CY + y, text="PS",
                                 font=(self._fonts.family, 7, "bold"),
                                 fill=theme.TEXT_DIM)
        self._part("PS", [body], base_fill=theme.darken(_PLATE_FILL, 0.35))
        self._mark("PS", [label], theme.TEXT_DIM, theme.ACCENT_INK)

    def _build_dial(self) -> None:
        for name, (x, y), text in (("PLUS", _PLUS, "+"), ("MINUS", _MINUS, "−")):
            body = self._circle(x, y, _PM_R, fill=_CAP,
                                outline=theme.BORDER_HI, width=1)
            label = self.create_text(CX + x, CY + y - 1, text=text,
                                     font=(self._fonts.family, 9, "bold"),
                                     fill=theme.TEXT_DIM)
            self._part(name, [body], base_fill=_CAP)
            self._mark(name, [label], theme.TEXT_DIM, theme.ACCENT_INK)

        x, y = _DIAL
        px, py = CX + x, CY + y
        # dwie polowki pokretla: prawa = obrot w prawo, lewa = w lewo;
        # 4 stopnie przerwy, zeby bylo widac, ze to dwa osobne gniazda
        for name, a0, a1 in (("DIAL_CW", -88.0, 88.0),
                             ("DIAL_CCW", 92.0, 268.0)):
            body = self.create_polygon(
                _ring_half(px, py, _DIAL_OUT, _DIAL_IN, a0, a1),
                fill=_DIAL_FILL, outline="")
            arrow = self.create_text(
                px + (17 if name == "DIAL_CW" else -17), py,
                text="›" if name == "DIAL_CW" else "‹",
                font=(self._fonts.family, 9, "bold"), fill="#f3c9cc")
            self._part(name, [body], base_fill=_DIAL_FILL)
            self._mark(name, [arrow], "#f3c9cc", theme.ACCENT_INK)
        body = self._circle(x, y, _ENTER_R, fill=theme.darken(_DIAL_FILL, 0.30),
                            outline="")
        dot = self._circle(x, y, 2.5, fill="#f3c9cc", outline="")
        self._part("ENTER", [body], base_fill=theme.darken(_DIAL_FILL, 0.30))
        self._mark("ENTER", [dot], "#f3c9cc", theme.ACCENT_INK)

    def _build_shifter(self) -> None:
        xs = sorted({x for x, _y in _GEARS.values()})
        top, bottom = _GEARS["GEAR_1"][1], _GEARS["GEAR_2"][1]
        mid = (top + bottom) / 2
        line = theme.darken(theme.BORDER_HI, 0.1)
        self.create_line(CX + xs[0], CY + mid, CX + xs[-1], CY + mid,
                         fill=line, width=3)
        for x in xs:
            y_end = bottom if any(gx == x and gy == bottom
                                  for gx, gy in _GEARS.values()) else mid
            self.create_line(CX + x, CY + top, CX + x, CY + y_end,
                             fill=line, width=3)
        self.create_text(CX + (xs[0] + xs[-1]) / 2, CY + top - 22,
                         text="SKRZYNIA", font=self._fonts.tiny_bold,
                         fill=theme.MUTED)
        for name, (x, y) in _GEARS.items():
            body = self._circle(x, y, _GEAR_R, fill=_CAP,
                                outline=theme.BORDER_HI, width=1)
            label = self.create_text(CX + x, CY + y, text=_GEAR_LABELS[name],
                                     font=(self._fonts.family, 7, "bold"),
                                     fill=theme.TEXT_DIM)
            self._part(name, [body], base_fill=_CAP)
            self._mark(name, [label], theme.TEXT_DIM, theme.ACCENT_INK)

    def _build_pedals(self) -> None:
        h = _PEDAL_HALF
        for key, x, text in _PEDALS:
            self._round_rect(CX + x - h, CY + _PEDAL_TOP, CX + x + h,
                             CY + _PEDAL_BOTTOM, 5, fill=theme.TRACK,
                             outline="")
            self._pedal_fill[key] = self._round_rect(
                CX + x - h, CY + _PEDAL_BOTTOM - 2, CX + x + h,
                CY + _PEDAL_BOTTOM, 5, fill=theme.ACCENT, outline="",
                state="hidden")
            self.create_text(CX + x, CY + _PEDAL_BOTTOM + 11, text=text,
                             font=(self._fonts.family, 6, "bold"),
                             fill=theme.MUTED)
        self.create_text(CX + _PEDALS[1][1], CY + _PEDAL_TOP - 22,
                         text="PEDAŁY", font=self._fonts.tiny_bold,
                         fill=theme.MUTED)

    # ---------------------------------------------------------------- stan

    def set_angle(self, deg: float) -> None:
        """Znacznik na obreczy obraca sie razem z kierownica."""
        deg = 0.0 if deg != deg else deg
        if self._angle_shown is not None and abs(deg - self._angle_shown) < 0.05:
            return
        self._angle_shown = deg
        a = math.radians(-90.0 + deg)
        r1, r2 = _RIM_IN + 3, _RIM_OUT - 3
        self.coords(self._angle_line,
                    CX + r1 * math.cos(a), CY + r1 * math.sin(a),
                    CX + r2 * math.cos(a), CY + r2 * math.sin(a))
        text = f"{deg:+.0f}°"
        if self.itemcget(self._angle_text, "text") != text:
            self.itemconfigure(self._angle_text, text=text)

    def set_pedals(self, values: Dict[str, float]) -> None:
        h = _PEDAL_HALF
        span = _PEDAL_BOTTOM - _PEDAL_TOP
        shown = self._pedals_shown
        for key, x, _text in _PEDALS:
            v = values.get(key, 0.0)
            v = 0.0 if v != v else max(0.0, min(1.0, v))
            if abs(v - shown.get(key, -1.0)) < 0.002:
                continue
            shown[key] = v
            item = self._pedal_fill[key]
            if v * span < 3:
                self.itemconfigure(item, state="hidden")
                continue
            self.coords(item, *round_points(CX + x - h, CY + _PEDAL_BOTTOM - v * span,
                                            CX + x + h, CY + _PEDAL_BOTTOM, 5))
            self.itemconfigure(item, state="normal")

    def _refresh(self) -> None:
        super()._refresh()
        # Piktogramy przyciskow twarzowych maja kolor w roznych opcjach
        # (obrys albo wypelnienie), wiec nie obsluguje ich _mark z PadCanvas.
        # Ta sama logika "wlaczenia" co w PadCanvas._refresh.
        for name, items in getattr(self, "_face_glyphs", {}).items():
            if self._learning == name:
                on = self._pulse_on
            else:
                on = self._active.get(name, False)
            color = theme.ACCENT_INK if on else FACE_COLORS[name]
            for item, key in items:
                self.itemconfigure(item, **{key: color})


# ------------------------------------------------------------------ widok

#: Krotkie nazwy w wierszach listy.
ROW_LABELS = dict(G29_CONTROL_LABELS)
ROW_LABELS.update({
    "DPAD_UP": "▲ góra", "DPAD_DOWN": "▼ dół",
    "DPAD_LEFT": "◀ lewo", "DPAD_RIGHT": "▶ prawo",
    "ENTER": "Enter",
})

GROUPS = (
    (
        ("Przyciski", "button", ("CROSS", "SQUARE", "CIRCLE", "TRIANGLE")),
        ("Łopatki", "button", ("PADDLE_L", "PADDLE_R")),
        ("Krzyżak", "button", ("DPAD_UP", "DPAD_DOWN", "DPAD_LEFT",
                               "DPAD_RIGHT")),
        ("L2 / R2 / L3 / R3", "button", ("L2", "R2", "L3", "R3")),
    ),
    (
        ("Środek", "button", ("SHARE", "OPTIONS", "PS")),
        ("Pokrętło", "button", ("PLUS", "MINUS", "DIAL_CW", "DIAL_CCW",
                                "ENTER")),
        ("Skrzynia biegów", "button", ("GEAR_1", "GEAR_2", "GEAR_3", "GEAR_4",
                                       "GEAR_5", "GEAR_6", "GEAR_R")),
    ),
)

#: Kolejnosc kreatora "po kolei": najpierw to, czego uzywa sie w kazdej grze.
WIZARD_ORDER = ("CROSS", "CIRCLE", "SQUARE", "TRIANGLE",
                "PADDLE_L", "PADDLE_R",
                "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
                "L2", "R2", "L3", "R3", "SHARE", "OPTIONS", "PS",
                "PLUS", "MINUS", "DIAL_CW", "DIAL_CCW", "ENTER",
                "GEAR_1", "GEAR_2", "GEAR_3", "GEAR_4", "GEAR_5", "GEAR_6",
                "GEAR_R")


class WheelView(PadView):
    """Rysunek G29 + lista przypisan. Cala logika nauki z PadView."""

    _CANVAS_CLS = WheelCanvas
    _CONTROLS = G29_CONTROLS
    _AXES: Tuple[str, ...] = ()
    _GROUPS = GROUPS
    _TITLES = G29_CONTROL_LABELS
    _ROW_LABELS = ROW_LABELS
    _PART_OF_SLOT = {c: c for c in G29_CONTROLS}
    _SLOTS_OF_PART = {c: (c,) for c in G29_CONTROLS}
    _IDLE_HINT = ("Kliknij przycisk na rysunku albo wiersz na liście, a potem "
                  "wciśnij przycisk na swojej kierownicy. Prawy przycisk myszy "
                  "czyści gniazdo.")
    _NAME_MINSIZE = 112

    def set_angle(self, deg: float) -> None:
        self.canvas.set_angle(deg)

    def set_pedals(self, values: Dict[str, float]) -> None:
        self.canvas.set_pedals(values)
