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
from dataclasses import replace
from typing import Dict, List, Tuple

from ..config import G29_CONTROL_LABELS, G29_CONTROLS
from . import art, theme
from .padview import PadCanvas, PadView, _arrow_glyph, _text_glyph

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
_PILL = {"SHARE": (-34.0, -27.0), "OPTIONS": (34.0, -27.0)}
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
#: Odsunieta od prawej krawedzi plotna, zeby poswiata wcisnietego biegu
#: sie nie ucinala.
_GEARS = {"GEAR_1": (152.0, 140.0), "GEAR_3": (176.0, 140.0),
          "GEAR_5": (200.0, 140.0), "GEAR_R": (224.0, 140.0),
          "GEAR_2": (152.0, 188.0), "GEAR_4": (176.0, 188.0),
          "GEAR_6": (200.0, 188.0)}
_GEAR_R = 9.0
_GEAR_LABELS = {"GEAR_1": "1", "GEAR_2": "2", "GEAR_3": "3", "GEAR_4": "4",
                "GEAR_5": "5", "GEAR_6": "6", "GEAR_R": "R"}

#: Pedaly - lewy dolny rog, poza obrecza: sprzeglo, hamulec, gaz.
_PEDALS = (("clutch", -222.0, "SPR"), ("brake", -196.0, "HAM"),
           ("throttle", -170.0, "GAZ"))
_PEDAL_TOP, _PEDAL_BOTTOM, _PEDAL_HALF = 126.0, 186.0, 7.0
_PEDAL_COLORS = {"clutch": theme.WARN, "brake": theme.BAD,
                 "throttle": theme.GOOD}

FACE_COLORS = {
    "TRIANGLE": "#3fbf9f",
    "CIRCLE": "#e8534a",
    "CROSS": "#5b8def",
    "SQUARE": "#d97ab8",
}

_INK = "#0b0e14"
_RUBBER_TOP, _RUBBER_BOTTOM = "#3a4250", "#161a21"
_PLATE_TOP, _PLATE_BOTTOM = "#4b5568", "#262d38"
_DIAL_RED = "#b3343d"

_CAP_STYLE = art.CapStyle(
    top="#3c4555", bottom="#232a34", edge="#566279", on=theme.ACCENT,
    glyph=theme.TEXT_DIM, glyph_on=theme.ACCENT_INK, accent=theme.ACCENT,
    warn=theme.WARN, ink="#20160a")
_METAL_STYLE = replace(_CAP_STYLE, top="#7d889c", bottom="#4a5465",
                       edge="#98a3b6", glyph="#2a313c")
_RED_STYLE = replace(_CAP_STYLE, top="#c94850", bottom="#8e252d",
                     edge="#d9636b", glyph="#f6d4d7")
_RED_DARK_STYLE = replace(_RED_STYLE, top="#8a2730", bottom="#5e1820",
                          edge="#a8323a")


def _ring_half(cx: float, cy: float, r_out: float, r_in: float,
               a0: float, a1: float, steps: int = 18) -> List[float]:
    """Wycinek pierscienia jako plaska lista punktow (katy w stopniach)."""
    return [c for pt in art.ring_points(cx, cy, r_out, r_in, a0, a1, steps)
            for c in pt]


class WheelCanvas(PadCanvas):
    """Rysunek G29: klikalne przyciski, obrot i pedaly na zywo."""

    _CONTROLS: Tuple[str, ...] = G29_CONTROLS
    _SIZE: Tuple[int, int] = (CANVAS_W, CANVAS_H)
    _AXIS_PARTS: Tuple[str, ...] = ()       # kierownica nie ma osi do klikania

    # ------------------------------------------------------------ budowanie

    def _paint_body(self, p: art.Paint) -> None:
        # trzonki lopatek - chowaja sie za obrecza
        for side in (-1.0, 1.0):
            lo, hi = sorted((CX + side * 138.0, CX + side * 166.0))
            stem = ("rrect", lo, CY - 62, hi, CY - 48, 3)
            p.gradient(stem, "#4a5465", "#2b323e")

        # obrecz z gumy: cien, gradient, polysk i przeszycie
        rim = ("ring", CX, CY, _RIM_OUT, _RIM_IN, -90.0, 270.0)
        p.shadow(("circle", CX, CY, _RIM_OUT), dy=10, blur=10, alpha=170)
        p.fill(("circle", CX, CY, _RIM_IN), theme.PANEL)
        p.gradient(rim, _RUBBER_TOP, _RUBBER_BOTTOM)
        p.sheen(rim, alpha=40, frac=0.35)
        mid = (_RIM_OUT + _RIM_IN) / 2
        for k in range(120):
            a = math.radians(k * 3.0)
            b = math.radians(k * 3.0 + 1.4)
            p.line([(CX + mid * math.cos(a), CY + mid * math.sin(a)),
                    (CX + mid * math.cos(b), CY + mid * math.sin(b))],
                   "#ffffff", 0.9, alpha=28)
        p.stroke(("circle", CX, CY, _RIM_OUT), "#ffffff", 1.0, alpha=30)

        # plyta z przyciskami i dolne ramie - jedna bryla
        x1, y1, x2, y2, r = _PLATE
        plate = ("rrect", CX + x1, CY + y1, CX + x2, CY + y2, r)
        x1, y1, x2, y2, r = _SPOKE
        spoke = ("rrect", CX + x1, CY + y1, CX + x2, CY + y2, r)
        for shape in (spoke, plate):
            p.shadow(shape, dy=6, blur=7, alpha=170)
        top, bottom = CY + _PLATE[1], CY + _SPOKE[3]
        for shape in (spoke, plate):
            p.gradient(shape, _PLATE_TOP, _PLATE_BOTTOM, y0=top, y1=bottom)
        p.sheen(plate, alpha=34, frac=0.5)
        p.stroke(plate, "#ffffff", 1.0, alpha=30)
        p.text(CX, CY - 10, "G29", 9, "#8a95a8", bold=True)

        # wglebienia pod krzyzakiem i pokretlem
        dx, dy = CX + _DPAD[0], CY + _DPAD[1]
        well = ("circle", dx, dy, _DPAD_ARM + 6)
        p.gradient(well, "#151a21", "#2a313c")
        p.fill(("circle", dx, dy, _DPAD_GAP - 1), "#1c222b")
        x, y = CX + _DIAL[0], CY + _DIAL[1]
        p.fill(("circle", x, y, _DIAL_OUT + 3), "#14181f")

        # skrzynia biegow: plyta i prowadnica H
        xs = sorted({gx for gx, _gy in _GEARS.values()})
        top_y, bottom_y = _GEARS["GEAR_1"][1], _GEARS["GEAR_2"][1]
        gate = ("rrect", CX + xs[0] - 18, CY + top_y - 18, CX + xs[-1] + 18,
                CY + bottom_y + 18, 14)
        p.shadow(gate, dy=4, blur=5, alpha=150)
        p.gradient(gate, "#2e3541", "#1b2028")
        p.stroke(gate, "#ffffff", 1.0, alpha=24)
        mid_y = (top_y + bottom_y) / 2
        slot = "#0e1116"
        p.line([(CX + xs[0], CY + mid_y), (CX + xs[-1], CY + mid_y)], slot, 4)
        for gx in xs:
            y_end = bottom_y if any(x0 == gx and y0 == bottom_y
                                    for x0, y0 in _GEARS.values()) else mid_y
            p.line([(CX + gx, CY + top_y), (CX + gx, CY + y_end)], slot, 4)

        # pedaly: rowki pod paskami
        for _key, x, _text in _PEDALS:
            track = ("rrect", CX + x - _PEDAL_HALF, CY + _PEDAL_TOP,
                     CX + x + _PEDAL_HALF, CY + _PEDAL_BOTTOM, _PEDAL_HALF)
            p.gradient(track, "#0e1116", "#1d232c")
            p.stroke(track, "#ffffff", 1.0, alpha=20)

    def _build_parts(self) -> None:
        self._angle_sprites: Dict[int, Tuple[object, Tuple[int, int]]] = {}

        # lopatki
        x1, y1, x2, y2 = _PADDLE
        # _PADDLE opisuje lewa lopatke (ujemne x) - prawa to jej odbicie,
        # dlatego liczymy od wartosci bezwzglednych i strony
        for name, side in (("PADDLE_L", -1.0), ("PADDLE_R", 1.0)):
            lo, hi = sorted((side * abs(x1), side * abs(x2)))
            cx = CX + (lo + hi) / 2
            self._add(name, ("rrect", CX + lo, CY + y1, CX + hi, CY + y2, 12),
                      _METAL_STYLE,
                      glyph=_groove_glyph(cx, CY + y1 + 13, CY + y2 - 13))

        # krzyzak
        cx, cy = CX + _DPAD[0], CY + _DPAD[1]
        a, h, g = _DPAD_ARM, _DPAD_HALF, _DPAD_GAP
        arms = {
            "DPAD_UP": ((cx - h, cy - a, cx + h, cy - g), (0, -1)),
            "DPAD_DOWN": ((cx - h, cy + g, cx + h, cy + a), (0, 1)),
            "DPAD_LEFT": ((cx - a, cy - h, cx - g, cy + h), (-1, 0)),
            "DPAD_RIGHT": ((cx + g, cy - h, cx + a, cy + h), (1, 0)),
        }
        for name, (box, direction) in arms.items():
            mx, my = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
            self._add(name, ("rrect", *box, 3.5), _CAP_STYLE,
                      glyph=_arrow_glyph(mx, my, direction, 3.2))

        # przyciski twarzowe z piktogramami
        fx, fy = CX + _FACE[0], CY + _FACE[1]
        s = _FACE_STEP
        spots = {"TRIANGLE": (fx, fy - s), "SQUARE": (fx - s, fy),
                 "CIRCLE": (fx + s, fy), "CROSS": (fx, fy + s)}
        for name, (x, y) in spots.items():
            color = FACE_COLORS[name]
            style = replace(_CAP_STYLE, top="#2f3642", bottom="#1c2129",
                            edge=art.mix("#3e4859", color, 0.4), on=color,
                            glyph=color, glyph_on=_INK)
            self._add(name, ("circle", x, y, _FACE_R), style,
                      glyph=_face_glyph(name, x, y))

        # L2/L3/R2/R3
        for name, (x, y) in _SMALL.items():
            self._add(name, ("circle", CX + x, CY + y, _SMALL_R), _CAP_STYLE,
                      glyph=_text_glyph(CX + x, CY + y, name, 6.5, bold=True))

        # Share / Options / PS
        for name, (x, y) in _PILL.items():
            w, ph = _PILL_W / 2, _PILL_H / 2
            self._add(name, ("rrect", CX + x - w, CY + y - ph, CX + x + w,
                             CY + y + ph, ph), _CAP_STYLE)
        x, y = _PS
        self._add("PS", ("circle", CX + x, CY + y, _PS_R),
                  replace(_CAP_STYLE, top="#2a313c", bottom="#171b22"),
                  glyph=_text_glyph(CX + x, CY + y, "PS", 7, bold=True))

        # plus, minus, pokretlo i Enter
        for name, (x, y), text in (("PLUS", _PLUS, "+"), ("MINUS", _MINUS, "−")):
            self._add(name, ("circle", CX + x, CY + y, _PM_R), _CAP_STYLE,
                      glyph=_text_glyph(CX + x, CY + y - 0.5, text, 9, bold=True))
        x, y = CX + _DIAL[0], CY + _DIAL[1]
        # dwie polowki pokretla: prawa = obrot w prawo, lewa = w lewo;
        # 4 stopnie przerwy, zeby bylo widac, ze to dwa osobne gniazda
        for name, a0, a1 in (("DIAL_CW", -88.0, 88.0),
                             ("DIAL_CCW", 92.0, 268.0)):
            ax = x + (17 if name == "DIAL_CW" else -17)
            self._add(name, ("ring", x, y, _DIAL_OUT, _DIAL_IN, a0, a1),
                      _RED_STYLE,
                      glyph=_text_glyph(ax, y - 0.5,
                                        "›" if name == "DIAL_CW" else "‹",
                                        9, bold=True))
        self._add("ENTER", ("circle", x, y, _ENTER_R), _RED_DARK_STYLE,
                  glyph=_dot_glyph(x, y, 2.6))

        # biegi
        for name, (gx, gy) in _GEARS.items():
            self._add(name, ("circle", CX + gx, CY + gy, _GEAR_R), _METAL_STYLE,
                      glyph=_text_glyph(CX + gx, CY + gy, _GEAR_LABELS[name],
                                        7.5, bold=True))

    def _build_overlay(self) -> None:
        f = self._fonts
        x1, _y1, x2, y2 = _PADDLE
        for side, text in ((-1.0, "L"), (1.0, "P")):
            lo, hi = sorted((side * abs(x1), side * abs(x2)))
            self.create_text(CX + (lo + hi) / 2, CY + y2 + 13, text=text,
                             font=f.tiny_bold, fill=theme.MUTED)
        for name, (x, y) in _PILL.items():
            self.create_text(CX + x, CY + y - _PILL_H / 2 - 6,
                             text=G29_CONTROL_LABELS[name].upper(),
                             font=(f.family, 6, "bold"), fill=theme.MUTED)
        xs = sorted({x for x, _y in _GEARS.values()})
        self.create_text(CX + (xs[0] + xs[-1]) / 2,
                         CY + _GEARS["GEAR_1"][1] - 30, text="SKRZYNIA",
                         font=f.tiny_bold, fill=theme.MUTED)

        self._pedal_fill: Dict[str, int] = {}
        h = _PEDAL_HALF - 2
        for key, x, text in _PEDALS:
            self._pedal_fill[key] = self.create_rectangle(
                CX + x - h, CY + _PEDAL_BOTTOM - 3, CX + x + h,
                CY + _PEDAL_BOTTOM - 3, fill=_PEDAL_COLORS[key], outline="",
                state="hidden")
            self.create_text(CX + x, CY + _PEDAL_BOTTOM + 12, text=text,
                             font=(f.family, 6, "bold"), fill=theme.MUTED)
        self.create_text(CX + _PEDALS[1][1], CY + _PEDAL_TOP - 16,
                         text="PEDAŁY", font=f.tiny_bold, fill=theme.MUTED)

        # znacznik obrotu na obreczy i odczyt kata - nie sa klikalne
        self._angle_item = self.create_image(0, 0, anchor="nw")
        self._angle_text = self.create_text(CX, CY - 84, text="",
                                            font=f.heading, fill=theme.TEXT)
        self.set_angle(0.0)

    # ---------------------------------------------------------------- stan

    def set_angle(self, deg: float) -> None:
        """Znacznik na obreczy obraca sie razem z kierownica."""
        deg = 0.0 if deg != deg else deg
        if self._angle_shown is not None and abs(deg - self._angle_shown) < 0.05:
            return
        self._angle_shown = deg
        key = int(round(deg)) % 360
        sprite = self._angle_sprites.get(key)
        if sprite is None:
            sprite = self._angle_sprite(key)
            self._angle_sprites[key] = sprite
        image, (x, y) = sprite
        self.itemconfigure(self._angle_item, image=image)
        self.coords(self._angle_item, x, y)
        text = f"{deg:+.0f}°"
        if self.itemcget(self._angle_text, "text") != text:
            self.itemconfigure(self._angle_text, text=text)

    def _angle_sprite(self, deg: int):
        """Kreska akcentu na obreczy - jak pasek na gorze prawdziwego kola."""
        a = math.radians(-90.0 + deg)
        r1, r2 = _RIM_IN + 4, _RIM_OUT - 4
        pts = [(CX + r1 * math.cos(a), CY + r1 * math.sin(a)),
               (CX + r2 * math.cos(a), CY + r2 * math.sin(a))]
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        ox, oy = math.floor(min(xs) - 10), math.floor(min(ys) - 10)
        p = art.Paint(math.ceil(max(xs) + 10) - ox, math.ceil(max(ys) + 10) - oy,
                      (ox, oy))
        p.line(pts, theme.ACCENT, 9, alpha=70)
        p.line(pts, theme.ACCENT, 5.5)
        return art.photo(p.image(), self), (ox, oy)

    def set_pedals(self, values: Dict[str, float]) -> None:
        h = _PEDAL_HALF - 2
        span = _PEDAL_BOTTOM - _PEDAL_TOP - 6
        shown = self._pedals_shown
        for key, x, _text in _PEDALS:
            v = values.get(key, 0.0)
            v = 0.0 if v != v else max(0.0, min(1.0, v))
            if abs(v - shown.get(key, -1.0)) < 0.002:
                continue
            shown[key] = v
            item = self._pedal_fill[key]
            if v * span < 2:
                self.itemconfigure(item, state="hidden")
                continue
            self.coords(item, CX + x - h, CY + _PEDAL_BOTTOM - 3 - v * span,
                        CX + x + h, CY + _PEDAL_BOTTOM - 3)
            self.itemconfigure(item, state="normal")


# -------------------------------------------------------------- piktogramy

def _groove_glyph(x: float, y1: float, y2: float):
    def draw(p: art.Paint, color: str) -> None:
        p.line([(x, y1), (x, y2)], color, 2.2, alpha=150)
    return draw


def _dot_glyph(x: float, y: float, r: float):
    def draw(p: art.Paint, color: str) -> None:
        p.fill(("circle", x, y, r), color)
    return draw


def _face_glyph(name: str, x: float, y: float):
    """Trojkat, kwadrat, kolko albo krzyzyk - rysowane, nie z czcionki."""
    def draw(p: art.Paint, color: str) -> None:
        if name == "TRIANGLE":
            pts = ((x, y - 5.2), (x + 5.2, y + 3.8), (x - 5.2, y + 3.8))
            p.line(list(pts) + [pts[0]], color, 1.5)
        elif name == "SQUARE":
            p.stroke(("rrect", x - 4.3, y - 4.3, x + 4.3, y + 4.3, 0.8),
                     color, 1.5)
        elif name == "CIRCLE":
            p.stroke(("circle", x, y, 4.8), color, 1.5)
        else:
            p.line([(x - 4.2, y - 4.2), (x + 4.2, y + 4.2)], color, 1.5)
            p.line([(x - 4.2, y + 4.2), (x + 4.2, y - 4.2)], color, 1.5)
    return draw


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
    _ROW_COLORS: Dict[str, str] = {}        # kolory liter ABXY tylko w padzie

    def set_angle(self, deg: float) -> None:
        self.canvas.set_angle(deg)

    def set_pedals(self, values: Dict[str, float]) -> None:
        self.canvas.set_pedals(values)
