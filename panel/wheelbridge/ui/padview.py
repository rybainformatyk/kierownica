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

Geometria (stale _STICKS, _DPAD, _FACE, _BUMPER_X ...) ma dwa warunki:
nic nie wychodzi poza korpus i nic nie nachodzi na sasiada (gniazda co
najmniej 3 px od siebie, galka wychylona do konca zostaje w gniezdzie).
Warstwa malarska moze sie zmieniac dowolnie, te warunki - nie.
"""

from __future__ import annotations

import math
import tkinter as tk
from dataclasses import replace
from tkinter import ttk
from typing import (Callable, Dict, Iterable, List, Optional, Sequence,
                    Tuple)

from ..config import (PAD_BUTTON_NAMES, PAD_DIRECTIONS, PAD_DIRECTION_LABELS,
                      PAD_DIRECTION_SHORT)
from . import art, theme
from .theme import Fonts
from .widgets import PopupMenu, fit_wrap, round_points

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
#
# Wzor: zdjecia pada Elite w _archiwum/panel. Czarny korpus z gumowymi,
# fakturowanymi uchwytami, jasniejsza wpuszczona "przylbica" wokol
# przycisku glownego, metalowy fasetowany krzyzak-talerz z podzialka,
# drazki w okraglych gniazdach z rantem, szare bumpery pogrubiajace sie ku
# rogom i spusty-skrzydelka wystajace zza nich. Kolor akcentu panelu tylko
# przy stanach: wcisniete, wskazane, nauka.

CANVAS_W, CANVAS_H = 560, 420
CX, CY = 280.0, 196.0

#: Wszystko ponizej jest mierzone na zdjeciu (powiekszenie 3x, srodek pada
#: x = 640, gora korpusu y = 185) i przeliczane jedna funkcja - widac, skad
#: pochodzi kazda liczba.
_PHOTO_S = 0.47
_PHOTO_X, _PHOTO_Y = 640.0, 185.0 + 136.0 / 0.47


def _ph(x: float, y: float) -> Tuple[float, float]:
    """Punkt ze zdjecia -> wspolrzedne wzgledem srodka pada."""
    return ((x - _PHOTO_X) * _PHOTO_S, (y - _PHOTO_Y) * _PHOTO_S)


#: Prawa polowa obrysu, od srodka gornej krawedzi zgodnie ze wskazowkami
#: zegara do srodka luku miedzy uchwytami. Lewa polowa to odbicie.
SILHOUETTE = tuple(_ph(x, y) for x, y in (
    (640, 185), (700, 186), (780, 182), (860, 178), (930, 185), (970, 198),
    (992, 215), (1010, 250), (1035, 310), (1062, 390), (1088, 470),
    (1110, 550), (1128, 630), (1140, 700), (1142, 755), (1128, 800),
    (1100, 830), (1065, 842), (1030, 835), (1000, 815), (965, 780),
    (925, 735), (890, 695), (862, 664), (840, 652), (760, 650), (640, 650),
))

_STICKS = {"L": _ph(375, 345), "R": _ph(775, 500)}
_STICK_RING = 80 * _PHOTO_S          # gniazdo z rantem (klik: wybor zrodla)
_STICK_KNOB = 26.0                   # gumowa czasza (klik: L3/R3)
_STICK_TRAVEL = 8.0

_DPAD = _ph(503, 508)                # metalowy talerz z czterema fasetami
_DPAD_R = 85 * _PHOTO_S
_DPAD_HUB = 8.0                      # srodek talerza (w tle, nie klikalny)

_FACE = _ph(903, 343)
_FACE_OFFSET, _FACE_R = 68 * _PHOTO_S, 33 * _PHOTO_S

_GUIDE = _ph(640, 240)
_GUIDE_R = 38 * _PHOTO_S
_VIEW = _ph(563, 343)                # Menu - odbicie wzgledem srodka
_SMALL_R = 22 * _PHOTO_S

#: Ozdoby ze zdjecia bez odpowiednika w padzie Xbox (nie sa klikalne):
#: przycisk Share i trzy diody profilu obok krzyzaka.
_SHARE = _ph(640, 397) + (25 * _PHOTO_S, 11 * _PHOTO_S)
_LEDS = tuple(_ph(640, y) for y in (492, 508, 524))
_LED_HALF = 11 * _PHOTO_S

#: Jasniejsza, wpuszczona "przylbica" wokol przycisku glownego.
_VISOR = tuple(_ph(x, y) for x, y in ((450, 192), (830, 192), (765, 288),
                                      (515, 288)))
#: Linia podzialu gumowej czesci uchwytu (prawa strona).
_GRIP_SEAM = tuple(_ph(x, y) for x, y in ((1045, 380), (990, 460),
                                          (935, 540), (880, 615), (858, 648)))

#: Bumper - smukly polksiezyc wzdluz barku: ostry przy srodku, najgrubszy
#: na rogu, zwezajacy sie w dol boku. Spust to garb wyrastajacy zza niego
#: i biegnacy rownolegle - jak na zdjeciu z przodu.
_BUMPER_X = (54.0, 184.0)
_BUMPER_THICK = (2.0, 17.0)          # na koncach, najgrubsze miejsce
_BUMPER_PEAK = 0.6                   # gdzie najgrubszy (0 srodek, 1 bok)
#: Spust: odcinek bumpera (ulamki jego dlugosci), wysokosc garbu ponad
#: bumperem i jak gleboko dolna krawedz chowa sie pod bumper.
_TRIGGER_SPAN = (0.08, 0.64)
_TRIGGER_RISE = 23.0
_TRIGGER_TUCK = 3.0
_TRIGGER_STEPS = 24                  # poziomy wypelnienia spustu

#: Czesci osi (spusty, gniazda drazkow) - klik otwiera wybor zrodla.
AXIS_PARTS = ("LT", "RT", "STICK_L", "STICK_R")

# --- paleta ze zdjecia (czern #1d1e20, szare bumpery #4d4e50-#616365) ---
_BODY_TOP = "#2c2e32"
_BODY_BOTTOM = "#191a1d"
_VISOR_TOP = "#34363a"
_VISOR_BOTTOM = "#222326"
_GRIP = "#24262a"
_RECESS = "#0d0e10"
_LETTER = "#8b8e94"

_INK = "#0b0e14"
_GLOSS_STYLE = art.CapStyle(
    top="#2e3034", bottom="#08090a", edge="#000000", on=theme.ACCENT,
    glyph=_LETTER, glyph_on=theme.ACCENT_INK, accent=theme.ACCENT,
    warn=theme.WARN, ink="#20160a", gloss=True,
    assigned=art.mix("#000000", theme.ACCENT, 0.30))
_SMALL_STYLE = replace(_GLOSS_STYLE, glyph="#c4c6ca")
_GUIDE_STYLE = replace(_GLOSS_STYLE, top="#eeeef0", bottom="#b4b6ba",
                       edge="#5d5f63", glyph="#2a2b2e",
                       assigned=art.mix("#5d5f63", theme.ACCENT, 0.30))
_METAL_STYLE = replace(_GLOSS_STYLE, top="#74767b", bottom="#3a3c40",
                       edge="#202124", glyph="#e6e7ea", gloss=False,
                       assigned=art.mix("#202124", theme.ACCENT, 0.30))
#: Bumper blizej oka: jasniejszy, blyszczacy grafit z refleksem wzdluz
#: grzbietu (glyph). Spust glebiej: ciemniejszy i matowy.
_BUMPER_STYLE = replace(_GLOSS_STYLE, top="#62656b", bottom="#232428",
                        edge="#000000", glyph="#d4d6da", gloss=False)
_TRIGGER_STYLE = replace(_METAL_STYLE, top="#45474c", bottom="#18191b",
                         edge="#050506")
_WELL_STYLE = replace(_GLOSS_STYLE, top=_RECESS, bottom="#222327",
                      edge="#2f3034", gloss=False, glyph="#3a3c40",
                      assigned=art.mix("#2f3034", theme.ACCENT, 0.30))
_CAP_STYLE = replace(_GLOSS_STYLE, top="#45474c", bottom="#141517",
                     edge="#050506", gloss=False, glyph="#000000",
                     assigned=art.mix("#050506", theme.ACCENT, 0.30))
#: Fasety talerza krzyzaka - swiatlo z lewej gory: gorna najjasniejsza.
_DPAD_FACETS = {
    "DPAD_UP": ("#d2d3d6", "#8d8f93"),
    "DPAD_LEFT": ("#a9abaf", "#6c6e72"),
    "DPAD_RIGHT": ("#83858a", "#4f5155"),
    "DPAD_DOWN": ("#64666a", "#38393d"),
}
#: Sektor talerza: (kat poczatku, kat konca) - 0 w prawo, zgodnie z zegarem.
_DPAD_SECTORS = {
    "DPAD_UP": (-135.0, -45.0), "DPAD_RIGHT": (-45.0, 45.0),
    "DPAD_DOWN": (45.0, 135.0), "DPAD_LEFT": (135.0, 225.0),
}


def _smooth(points: Sequence[Tuple[float, float]],
            passes: int = 1) -> List[Tuple[float, float]]:
    """Wygladzenie szumu odczytu (1/4, 1/2, 1/4) - konce zostaja."""
    pts = list(points)
    for _ in range(passes):
        pts = ([pts[0]]
               + [((a[0] + 2 * b[0] + c[0]) / 4, (a[1] + 2 * b[1] + c[1]) / 4)
                  for a, b, c in zip(pts, pts[1:], pts[2:])]
               + [pts[-1]])
    return pts


def _catmull(points: Sequence[Tuple[float, float]],
             steps: int = 6) -> List[Tuple[float, float]]:
    """Zamknieta krzywa Catmulla-Roma przez wszystkie punkty."""
    n = len(points)
    out: List[Tuple[float, float]] = []
    for i in range(n):
        p0, p1 = points[(i - 1) % n], points[i]
        p2, p3 = points[(i + 1) % n], points[(i + 2) % n]
        for step in range(steps):
            t = step / steps
            t2, t3 = t * t, t * t * t
            out.append(tuple(
                0.5 * (2 * p1[k] + (-p0[k] + p2[k]) * t
                       + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2
                       + (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3)
                for k in (0, 1)))
    return out


_OUTLINE: List[Tuple[float, float]] = []


def body_outline() -> List[Tuple[float, float]]:
    """Obrys korpusu jako punkty wzgledem srodka, zgodnie z ruchem
    wskazowek zegara od srodka gornej krawedzi."""
    if not _OUTLINE:
        right = _smooth(SILHOUETTE)
        left = [(-x, y) for x, y in reversed(right[1:-1])]
        _OUTLINE.extend(_catmull(right + left))
    return _OUTLINE


def inside_body(x: float, y: float) -> bool:
    """Czy punkt (wzgledem srodka) lezy w obrysie korpusu."""
    return art.contains(("poly", tuple(body_outline())), x, y)


_TRACK: List[Tuple[Tuple[float, float], Tuple[float, float], float]] = []
_BUMPER_INNER = -1.5                 # bumper wsuniety pod krawedz korpusu


def _bumper_track():
    """Punkty wzdluz barku (prawego): (punkt na krawedzi korpusu, normalna
    na zewnatrz, grubosc bumpera) - od srodka pada ku bokowi, zageszczone,
    zeby odsuniete krawedzie byly gladkie i na rogu."""
    if _TRACK:
        return _TRACK
    pts = body_outline()
    n = len(pts)
    idx = [i for i in range(n)
           if _BUMPER_X[0] <= pts[i][0] <= _BUMPER_X[1] and pts[i][1] < -40]
    raw = []
    for i in idx:
        ax, ay = pts[(i - 1) % n]
        bx, by = pts[(i + 1) % n]
        tx, ty = bx - ax, by - ay
        length = math.hypot(tx, ty) or 1.0
        nx, ny = ty / length, -tx / length
        px, py = pts[i]
        if nx * px + ny * (py - 40.0) < 0:        # na zewnatrz korpusu
            nx, ny = -nx, -ny
        raw.append(((px, py), (nx, ny)))
    dense = []
    for ((ax, ay), (anx, any_)), ((bx, by), (bnx, bny)) in zip(raw, raw[1:]):
        for s in range(4):
            t = s / 4
            nx, ny = anx + (bnx - anx) * t, any_ + (bny - any_) * t
            nl = math.hypot(nx, ny) or 1.0
            dense.append(((ax + (bx - ax) * t, ay + (by - ay) * t),
                          (nx / nl, ny / nl)))
    dense.append(raw[-1])
    t_end, t_max = _BUMPER_THICK
    # pierwiastek sinusa: tepy srodek, ostre konce; potega przesuwa
    # najgrubsze miejsce ku rogowi
    power = math.log(0.5) / math.log(_BUMPER_PEAK)
    for k, (p, nrm) in enumerate(dense):
        u = k / (len(dense) - 1)
        thick = t_end + (t_max - t_end) * math.sin(math.pi * u ** power) ** 0.6
        _TRACK.append((p, nrm, thick))
    return _TRACK


def _offset(k: int, d: float, sign: int = 1) -> Tuple[float, float]:
    """Punkt k toru barku odsuniety o d na zewnatrz (sign -1: lewy bark)."""
    (px, py), (nx, ny), _thick = _bumper_track()[k]
    return sign * (px + nx * d), py + ny * d


def bumper_edge(sign: int, inset: float = 0.0) -> List[Tuple[float, float]]:
    """Zewnetrzna krawedz bumpera (wsunieta o inset do srodka) - od srodka
    pada ku bokowi."""
    return [_offset(k, max(0.0, thick - inset), sign)
            for k, (_p, _n, thick) in enumerate(_bumper_track())]


def bumper_outline(sign: int) -> Tuple[Tuple[float, float], ...]:
    """Bumper jako wielokat wzgledem srodka. sign = 1 prawy, -1 lewy."""
    track = _bumper_track()
    outer = bumper_edge(1)
    inner = [_offset(k, _BUMPER_INNER) for k in range(len(track))]
    poly = outer + inner[::-1]
    if sign < 0:
        poly = [(-x, y) for x, y in reversed(poly)]
    return tuple(poly)


def bumper_center(sign: int) -> Tuple[float, float]:
    """Napis LB/RB - w najgrubszym miejscu bumpera."""
    track = _bumper_track()
    k = max(range(len(track)), key=lambda i: track[i][2])
    return _offset(k, (track[k][2] + _BUMPER_INNER) / 2, sign)


def _trigger_range() -> range:
    n = len(_bumper_track())
    a, b = _TRIGGER_SPAN
    return range(int(round(a * (n - 1))), int(round(b * (n - 1))) + 1)


def _trigger_rise(v: float) -> float:
    """Wysokosc garbu spustu nad bumperem; v 0..1 wzdluz spustu. Strome,
    zaokraglone konce i plaski grzbiet, lekko wyzszy blizej srodka pada."""
    s = math.sin(math.pi * min(1.0, max(0.0, v)))
    return _TRIGGER_RISE * s ** 0.45 * (1.0 - 0.18 * v)


def trigger_outline(sign: int) -> Tuple[Tuple[float, float], ...]:
    """Spust wzgledem srodka: garb nad bumperem. Gorna krawedz biegnie
    rownolegle do bumpera, dolna lezy pod nim (wsunieta o _TRIGGER_TUCK),
    wiec spod bumpera nie wystaje zadna dolna krawedz."""
    track = _bumper_track()
    ks = list(_trigger_range())
    top, bottom = [], []
    for j, k in enumerate(ks):
        thick = track[k][2]
        v = j / (len(ks) - 1)
        top.append(_offset(k, thick + _trigger_rise(v)))
        bottom.append(_offset(k, thick - min(_TRIGGER_TUCK, thick * 0.6)))
    poly = top + bottom[::-1]
    if sign < 0:
        poly = [(-x, y) for x, y in reversed(poly)]
    return tuple(poly)


def trigger_label(sign: int) -> Tuple[float, float]:
    """Napis LT/RT: w najwyzszym miejscu garbu, w polowie jego wysokosci."""
    track = _bumper_track()
    ks = list(_trigger_range())
    j = max(range(len(ks)), key=lambda i: _trigger_rise(i / (len(ks) - 1)))
    k = ks[j]
    return _offset(k, track[k][2] + _trigger_rise(j / (len(ks) - 1)) * 0.5,
                   sign)



def trigger_ridge(sign: int, ox: float = 0.0,
                  oy: float = 0.0) -> List[Tuple[float, float]]:
    """Grzbiet garbu: gorna krawedz wsunieta o 1.8 px, bez stromych
    koncow (tam jasna linia wygladalaby jak zadzior). ox, oy - przesuniecie
    (srodek pada na plotnie)."""
    track = _bumper_track()
    ks = list(_trigger_range())
    out = []
    for j, k in enumerate(ks):
        rise = _trigger_rise(j / (len(ks) - 1))
        if rise > 0.45 * _TRIGGER_RISE:
            x, y = _offset(k, track[k][2] + rise - 1.8, sign)
            out.append((ox + x, oy + y))
    return out

def _grip_region(sign: int) -> art.Shape:
    """Gumowa czesc uchwytu: od linii podzialu na zewnatrz (przycinana
    potem do korpusu)."""
    seam = [(sign * x, y) for x, y in _GRIP_SEAM]
    far = [(sign * 300, seam[-1][1] + 200), (sign * 300, seam[0][1] - 40)]
    return ("poly", tuple((CX + x, CY + y) for x, y in seam + far))


def _clipped(p: art.Paint, clip: art.Shape,
             draw: Callable[[art.Paint], None], ss: int = 1) -> None:
    """Rysuje na osobnej warstwie i przycina ja do ksztaltu (korpusu).

    Warstwa w powiekszeniu ss i dopiero potem do rozmiaru p - gladkie
    przejscia wystarczy liczyc w zwyklej rozdzielczosci, a pelne
    powiekszenie calego plotna kosztowalo ~100 ms na warstwe.
    """
    from PIL import Image, ImageChops
    layer = art.Paint(p.w, p.h, (p.ox, p.oy), ss=ss)
    draw(layer)
    img = layer.img
    if img.size != p.img.size:
        img = img.resize(p.img.size, Image.BILINEAR)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), p.mask(clip)))
    p.img.alpha_composite(img)


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
    _AXIS_PARTS: Tuple[str, ...] = AXIS_PARTS
    _SIZE: Tuple[int, int] = (CANVAS_W, CANVAS_H)

    def __init__(self, master: tk.Misc, fonts: Fonts,
                 on_button_click: Callable[[str], None],
                 on_button_clear: Callable[[str], None],
                 on_hover: Callable[[Optional[str]], None],
                 on_axis_click: Optional[Callable[[str, int, int], None]] = None,
                 on_axis_clear: Optional[Callable[[str], None]] = None) -> None:
        super().__init__(master, width=self._SIZE[0], height=self._SIZE[1],
                         bg=theme.PANEL, highlightthickness=0, bd=0)
        self._fonts = fonts
        self._on_button_click = on_button_click
        self._on_button_clear = on_button_clear
        self._on_hover = on_hover
        self._on_axis_click = on_axis_click
        self._on_axis_clear = on_axis_clear

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

        #: spusty: wypelnienie (obrazek na poziom) i napis z wartoscia
        self._trig_level: Dict[str, int] = {}
        self._trig_text: Dict[str, int] = {}
        self._trig_photos: Dict[Tuple[str, int], Tuple[object, Tuple[int, int]]] = {}
        self._trig_step: Dict[str, int] = {}

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
        body = ("poly", tuple((CX + x, CY + y) for x, y in body_outline()))
        top = CY + min(y for _x, y in body_outline())
        bottom = CY + max(y for _x, y in body_outline())

        p.shadow(body, dy=10, blur=14, alpha=220)
        p.gradient(body, _BODY_TOP, _BODY_BOTTOM, y0=top, y1=bottom)

        def light(q: art.Paint) -> None:
            # swiatlo z gory na srodku i ciemniejsze boki - bryla, nie plyta
            q.radial(CX, top - 30, 300, "#ffffff", "#ffffff", alpha_in=22,
                     alpha_out=0)
            for sign in (-1, 1):
                q.radial(CX + sign * 250, CY + 60, 170, "#000000", "#000000",
                         alpha_in=80, alpha_out=0)
        _clipped(p, body, light, ss=1)

        # gumowe uchwyty: jasniejsza czern i drobne ukosne prazki
        for sign in (-1, 1):
            region = _grip_region(sign)

            def grip(q: art.Paint, region=region, sign=sign) -> None:
                from PIL import ImageChops, ImageDraw
                q.fill(region, _GRIP)
                d = ImageDraw.Draw(q.img)
                step = 3.2 * q.ss
                w, h = q.img.size
                k = -sign                   # prazki w strone srodka pada
                x = -h
                while x < w + h:
                    d.line([(x, 0), (x + k * h * 0.6, h)],
                           fill=(255, 255, 255, 14), width=max(1, q.ss // 2))
                    x += step
                q.img.putalpha(ImageChops.multiply(q.img.getchannel("A"),
                                                   q.mask(region)))
            _clipped(p, body, grip, ss=2)
            # linia podzialu: ciemny rowek i jasny brzeg obok
            seam = [(CX + sign * x, CY + y) for x, y in _GRIP_SEAM]
            p.line(seam, "#000000", 2.0, alpha=170)
            p.line([(x - sign * 1.6, y) for x, y in seam], "#ffffff", 1.0,
                   alpha=26)

        # przylbica wokol przycisku glownego: jasniejsza i wpuszczona
        visor = ("poly", _rounded_poly([(CX + x, CY + y, 14.0)
                                        for x, y in _VISOR], steps=6))
        p.gradient(visor, _VISOR_TOP, _VISOR_BOTTOM)
        p.stroke(visor, "#000000", 1.2, alpha=150)

        # krawedz korpusu: ciemny obrys i jasny blask na gorze
        p.stroke(body, "#000000", 1.4, alpha=220)
        edge_top = sorted((CX + x, CY + y + 1.3) for x, y in body_outline()
                          if y < -120)
        p.line(edge_top, "#ffffff", 1.1, alpha=60)

        # gniazdo krzyzaka: podzialka dookola talerza
        dx, dy = CX + _DPAD[0], CY + _DPAD[1]
        p.shadow(("circle", dx, dy, _DPAD_R), dy=4, blur=4, alpha=200)
        for k in range(16):
            a = math.radians(k * 22.5)
            r0, r1 = _DPAD_R + 4.5, _DPAD_R + (9.0 if k % 2 == 0 else 7.0)
            p.line([(dx + r0 * math.cos(a), dy + r0 * math.sin(a)),
                    (dx + r1 * math.cos(a), dy + r1 * math.sin(a))],
                   "#5c5e63", 1.1, alpha=200, round_caps=False)
        # srodek talerza
        p.gradient(("circle", dx, dy, _DPAD_HUB), "#9a9ca0", "#55575b")
        p.stroke(("circle", dx, dy, _DPAD_HUB), "#2a2b2e", 1.0)

        # ozdoby: Share i diody profilu
        sx, sy, hw, hh = _SHARE
        share = ("rrect", CX + sx - hw, CY + sy - hh, CX + sx + hw,
                 CY + sy + hh, hh)
        p.shadow(share, dy=1.5, blur=2, alpha=170)
        p.gradient(share, "#2e3034", "#0a0b0c")
        p.stroke(share, "#000000", 1.0)
        p.sheen(share, alpha=40, frac=0.5)
        for lx, ly in _LEDS:
            bar = ("rrect", CX + lx - _LED_HALF, CY + ly - 1.6,
                   CX + lx + _LED_HALF, CY + ly + 1.6, 1.6)
            p.glow(bar, "#ffffff", radius=3, alpha=90)
            p.fill(bar, "#f2f3f5")

        # cienie drazkow w gniazdach - padaja w dol
        for ox, oy in _STICKS.values():
            p.shadow(("circle", CX + ox, CY + oy, _STICK_RING), dy=2,
                     blur=3, alpha=160)

    # --- kontrolki ---

    def _build_parts(self) -> None:
        # spusty: garby wyrastajace zza bumperow
        for name in ("LT", "RT"):
            sign = -1 if name == "LT" else 1
            shape = ("poly", tuple((CX + x, CY + y)
                                   for x, y in trigger_outline(sign)))
            self._add(name, shape, _TRIGGER_STYLE,
                      glyph=_trigger_glyph(sign, shape), clickable=False,
                      shadow=False)

        # bumpery: blyszczace polksiezyce wzdluz barkow
        for name in ("LB", "RB"):
            sign = -1 if name == "LB" else 1
            shape = ("poly", tuple((CX + x, CY + y)
                                   for x, y in bumper_outline(sign)))
            self._add(name, shape, _BUMPER_STYLE,
                      glyph=_bumper_glyph(sign, name), shadow=True)

        # drazki: gniazdo z rantem (klik: zrodla) i gumowa czasza (klik:
        # L3/R3), ktora rusza sie z drazkiem
        for side, (ox, oy) in _STICKS.items():
            sx, sy = CX + ox, CY + oy
            self._add("STICK_L" if side == "L" else "STICK_R",
                      ("circle", sx, sy, _STICK_RING), _WELL_STYLE,
                      glyph=_stick_ticks(sx, sy), clickable=False,
                      shadow=False)
        for side, (ox, oy) in _STICKS.items():
            sx, sy = CX + ox, CY + oy
            self._add("LS" if side == "L" else "RS",
                      ("circle", sx, sy, _STICK_KNOB), _CAP_STYLE,
                      glyph=_knob_glyph(sx, sy, ""), shadow=True)

        # krzyzak: metalowy talerz z czterema fasetami (kazda swieci osobno)
        dx, dy = CX + _DPAD[0], CY + _DPAD[1]
        for name in ("DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT"):
            a0, a1 = _DPAD_SECTORS[name]
            top_c, bottom_c = _DPAD_FACETS[name]
            style = replace(_METAL_STYLE, top=top_c, bottom=bottom_c,
                            edge="#1f2023",
                            assigned=art.mix("#1f2023", theme.ACCENT, 0.30))
            self._add(name, ("ring", dx, dy, _DPAD_R, _DPAD_HUB, a0, a1),
                      style, glyph=_facet_glyph(dx, dy, a0, a1),
                      shadow=False)

        # przyciski A B X Y: blyszczaca czern z wytloczona litera
        fx, fy = CX + _FACE[0], CY + _FACE[1]
        off = _FACE_OFFSET
        for name, ox, oy in (("Y", 0, -off), ("X", -off, 0),
                             ("B", off, 0), ("A", 0, off)):
            x, y = fx + ox, fy + oy
            self._add(name, ("circle", x, y, _FACE_R), _GLOSS_STYLE,
                      glyph=_text_glyph(x, y + 0.5, name, 12, bold=True))

        # View i Menu: male blyszczace przyciski z jasnymi ikonami
        for name, sign in (("BACK", -1), ("START", 1)):
            x, y = CX + sign * abs(_VIEW[0]), CY + _VIEW[1]
            self._add(name, ("circle", x, y, _SMALL_R), _SMALL_STYLE,
                      glyph=_view_glyph(x, y) if name == "BACK"
                      else _menu_glyph(x, y))

        # przycisk glowny: jasny, blyszczacy (bez cudzego logo)
        x, y = CX + _GUIDE[0], CY + _GUIDE[1]
        self._add("GUIDE", ("circle", x, y, _GUIDE_R), _GUIDE_STYLE,
                  glyph=_guide_glyph(x, y))

    def _build_overlay(self) -> None:
        """Wypelnienie spustow i ich napisy - nad obrazkiem spustu, pod
        bumperem (dolna czesc spustu i tak chowa sie pod nim)."""
        for name in ("LT", "RT"):
            part = self._parts.get(name)
            if part is None:
                continue
            self._trig_level[name] = self.create_image(0, 0, anchor="nw",
                                                       state="hidden")
            self.tag_raise(self._trig_level[name], part.item)
            lx, ly = trigger_label(-1 if name == "LT" else 1)
            # napis na grzbiecie garbu, ponad bumperem
            self._trig_text[name] = self.create_text(
                CX + lx, CY + ly, text=name,
                font=self._fonts.small_bold, fill="#eef0f3")
        # bumpery leza na spustach - ich obrazki maja byc nad wypelnieniem
        for name in ("LB", "RB"):
            part = self._parts.get(name)
            if part is not None:
                self.tag_raise(part.item)
        for item in self._trig_text.values():
            self.tag_raise(item)

    def _trigger_photo(self, name: str, step: int):
        """Spust wypelniony akcentem od wewnatrz na zewnatrz - step/STEPS."""
        key = (name, step)
        cached = self._trig_photos.get(key)
        if cached is None:
            from PIL import Image, ImageChops, ImageDraw
            shape = self._parts[name].shape
            x1, y1, x2, y2 = art.shape_bbox(shape)
            ox, oy = math.floor(x1) - 1, math.floor(y1) - 1
            w, h = math.ceil(x2) + 1 - ox, math.ceil(y2) + 1 - oy
            p = art.Paint(w, h, (ox, oy))
            frac = step / _TRIGGER_STEPS
            sign = -1 if name == "LT" else 1
            # od strony srodka pada ku zewnetrznemu koncowi
            if sign > 0:
                cut = (x1, y1 - 2, x1 + (x2 - x1) * frac, y2 + 2)
            else:
                cut = (x2 - (x2 - x1) * frac, y1 - 2, x2, y2 + 2)
            r = Image.new("L", p.img.size, 0)
            ImageDraw.Draw(r).rectangle(
                (p._x(cut[0]), p._y(cut[1]), p._x(cut[2]), p._y(cut[3])),
                fill=255)
            # akcent z gory jasniejszy - jak podswietlony plastik - i jasny
            # grzbiet garbu, przyciete do wcisnietej czesci
            p.gradient(shape, art.lighter(theme.ACCENT, 0.25),
                       art.darker(theme.ACCENT, 0.15))
            p.line(trigger_ridge(sign, CX, CY), "#ffffff", 1.1, alpha=120)
            p.img.putalpha(ImageChops.multiply(p.img.getchannel("A"), r))
            cached = (art.photo(p.image(), self), (ox, oy))
            self._trig_photos[key] = cached
        return cached

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

    def _clickable(self, name: Optional[str]) -> bool:
        return name in self._CONTROLS or (
            name in self._AXIS_PARTS and self._on_axis_click is not None)

    def _on_click(self, event) -> None:
        name = self._hit(event.x, event.y)
        if name in self._CONTROLS:
            self._on_button_click(name)
        elif name in self._AXIS_PARTS and self._on_axis_click is not None:
            self._on_axis_click(name, event.x_root, event.y_root)
        else:
            self._on_button_click("")        # klik w tlo anuluje nauke

    def _on_right_click(self, event) -> None:
        name = self._hit(event.x, event.y)
        if name in self._CONTROLS:
            self._on_button_clear(name)
        elif name in self._AXIS_PARTS and self._on_axis_clear is not None:
            self._on_axis_clear(name)

    def _on_motion(self, event) -> None:
        name = self._hit(event.x, event.y)
        if name == self._hover:
            return
        self._hover = name
        self.configure(cursor="hand2" if self._clickable(name) else "")
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
            if name not in self._trig_level:
                continue
            raw = max(0, min(255, int(raw)))
            # wypelnienie skrzydelka - kilkadziesiat poziomow, kazdy obrazek
            # liczony raz
            step = int(round(raw / 255.0 * _TRIGGER_STEPS))
            if self._trig_step.get(name) != step:
                self._trig_step[name] = step
                item = self._trig_level[name]
                if step == 0:
                    self.itemconfigure(item, state="hidden")
                else:
                    photo, (x, y) = self._trigger_photo(name, step)
                    self.itemconfigure(item, image=photo, state="normal")
                    self.coords(item, x, y)
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

def _rounded_poly(corners: Sequence[Tuple[float, float, float]],
                  steps: int = 5) -> Tuple[Tuple[float, float], ...]:
    """Wielokat z zaokraglonymi wybranymi rogami: (x, y, promien)."""
    n = len(corners)
    out: List[Tuple[float, float]] = []
    for i, (x, y, r) in enumerate(corners):
        if r <= 0:
            out.append((x, y))
            continue
        px, py, _ = corners[(i - 1) % n]
        nx, ny, _ = corners[(i + 1) % n]
        # punkty na obu bokach w odleglosci r od rogu, luk miedzy nimi
        def toward(ax: float, ay: float) -> Tuple[float, float]:
            dx, dy = ax - x, ay - y
            d = math.hypot(dx, dy) or 1.0
            return x + dx / d * r, y + dy / d * r
        a = toward(px, py)
        b = toward(nx, ny)
        for s in range(steps + 1):
            t = s / steps
            # krzywa Beziera drugiego stopnia z rogiem jako punktem kontrolnym
            out.append(((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * x + t * t * b[0],
                        (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * y + t * t * b[1]))
    return tuple(out)


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
    """Gniazdo drazka: cien przy gornej krawedzi wglebienia i jasny rant
    od dolu - jak okragle wyciecie w korpusie."""
    ring = _STICK_RING

    def draw(p: art.Paint, color: str) -> None:
        p.arc(x, y, ring - 2.0, 200, 340, "#000000", 4.0, alpha=170)
        p.arc(x, y, ring - 1.0, 25, 155, "#ffffff", 1.3, alpha=55)
        p.stroke(("circle", x, y, ring), "#4a4c50", 1.0, alpha=110)
    return draw


def _knob_glyph(x: float, y: float, label: str) -> Callable[[art.Paint, str], None]:
    """Gumowa czasza drazka: wklesly srodek, radelkowane obrzeze i blask
    na krawedzi. Na wcisnietej / uczacej sie czaszy bez ciemnego srodka -
    kolor stanu ma byc widac."""
    r = _STICK_KNOB

    def draw(p: art.Paint, color: str) -> None:
        idle = color == "#000000"
        if idle:
            # metalowy rant czaszy (jasny u gory) i wklesly srodek: ciemny
            # u gory, jasniejszy u dolu
            p.gradient(("circle", x, y, r - 3.5), "#7a7d83", "#2c2e32")
            p.gradient(("circle", x, y, r - 5.5), "#0b0c0d", "#2e3034")
        for k in range(44):
            a = 2 * math.pi * k / 44
            ca, sa = math.cos(a), math.sin(a)
            p.line([(x + ca * (r - 4.0), y + sa * (r - 4.0)),
                    (x + ca * (r - 1.0), y + sa * (r - 1.0))],
                   "#000000", 1.0, alpha=110, round_caps=False)
        p.arc(x, y, r - 1.0, 200, 300, "#ffffff", 1.4, alpha=90)
        if label:
            p.text(x, y, label, 6.5, color, bold=True, alpha=200)
    return draw


def _facet_glyph(x: float, y: float, a0: float,
                 a1: float) -> Callable[[art.Paint, str], None]:
    """Faseta talerza krzyzaka: jasniejsza wewnetrzna plaszczyzna, jak
    szlifowany metal."""
    def draw(p: art.Paint, color: str) -> None:
        p.fill(("ring", x, y, _DPAD_R - 8.0, _DPAD_HUB + 6.0, a0 + 9.0,
                a1 - 9.0), "#ffffff", alpha=26)
        p.arc(x, y, _DPAD_R - 1.2, a0 + 2.0, a1 - 2.0, "#ffffff", 1.0,
              alpha=50)
    return draw



def _bumper_glyph(sign: int, name: str) -> Callable[[art.Paint, str], None]:
    """Bumper: ostry refleks wzdluz grzbietu i szersza, miekka poswiata pod
    nim - wypukly, blyszczacy plastik. Napis w najgrubszym miejscu."""
    track = _bumper_track()
    keep = [k for k in range(len(track)) if track[k][2] > 6.0]
    ridge = [(CX + x, CY + y) for x, y in
             (_offset(k, track[k][2] - 2.0, sign) for k in keep)]
    soft = [(CX + x, CY + y) for x, y in
            (_offset(k, track[k][2] * 0.55, sign) for k in keep)]
    lx, ly = bumper_center(sign)

    def draw(p: art.Paint, color: str) -> None:
        p.line(soft, "#ffffff", 4.0, alpha=16)
        p.line(ridge, "#ffffff", 1.2, alpha=120)
        p.text(CX + lx, CY + ly + 0.5, name, 7.5, color, bold=True)
    return draw


def _trigger_glyph(sign: int,
                   shape: art.Shape) -> Callable[[art.Paint, str], None]:
    """Spust: cien rzucany przez bumper u podstawy garbu, jasny grzbiet i
    kilka zlobien na zewnetrznym koncu (chwyt palca)."""
    bumper = ("poly", tuple((CX + x, CY + y) for x, y in bumper_outline(sign)))
    ridge = trigger_ridge(sign, CX, CY)
    track = _bumper_track()
    ks = list(_trigger_range())
    grooves = []
    for j in range(len(ks)):
        v = j / (len(ks) - 1)
        if 0.66 <= v <= 0.94 and j % 5 == 0:
            k, rise = ks[j], _trigger_rise(v)
            base = track[k][2]
            grooves.append([(CX + x, CY + y) for x, y in
                            (_offset(k, base + 3.0, sign),
                             _offset(k, base + rise - 4.5, sign))])

    def draw(p: art.Paint, color: str) -> None:
        _clipped(p, shape, lambda q: q.shadow(bumper, dy=-1.0, blur=2.5,
                                              alpha=210), ss=p.ss)
        for line in grooves:
            p.line(line, "#000000", 1.4, alpha=110)
            p.line([(x + sign * 1.0, y) for x, y in line], "#ffffff", 0.8,
                   alpha=30)
        p.line(ridge, "#ffffff", 1.1, alpha=80)
    return draw

def _view_glyph(x: float, y: float) -> Callable[[art.Paint, str], None]:
    """Dwa nachodzace prostokaty - przycisk Back/View."""
    def draw(p: art.Paint, color: str) -> None:
        p.stroke(("rrect", x - 4.6, y - 3.4, x + 1.2, y + 1.6, 1.0), color, 1.2)
        p.fill(("rrect", x - 1.2, y - 1.6, x + 4.6, y + 3.4, 1.0), color)
    return draw


def _menu_glyph(x: float, y: float) -> Callable[[art.Paint, str], None]:
    """Trzy kreski - przycisk Start/Menu."""
    def draw(p: art.Paint, color: str) -> None:
        for dy in (-2.8, 0.0, 2.8):
            p.line([(x - 3.8, y + dy), (x + 3.8, y + dy)], color, 1.3)
    return draw


def _guide_glyph(x: float, y: float) -> Callable[[art.Paint, str], None]:
    """Przycisk glowny: grawerowany pierscien z kropka (bez cudzego logo)."""
    def draw(p: art.Paint, color: str) -> None:
        p.stroke(("circle", x, y, _GUIDE_R * 0.56), color, 2.2)
        p.fill(("circle", x, y, _GUIDE_R * 0.2), color)
    return draw


# ----------------------------------------------------------- lista wierszy

class _Row:
    """Jeden wiersz listy przypisan: nazwa, klikalna komorka, pasek."""

    CELL_W = 17          # w znakach - miejsce na "Kierownica w prawo"
    BAR_W = 46
    BAR_H = 8

    LEARN_TEXT = "wciśnij przycisk…"
    FLASH_MS = 900

    def __init__(self, parent: tk.Misc, grid_row: int, fonts: Fonts,
                 slot: str, kind: str,
                 on_click: Callable[[str, int, int], None],
                 on_clear: Callable[[str], None],
                 on_hover: Callable[[Optional[str]], None],
                 label: Optional[str] = None,
                 name_color: Optional[str] = None) -> None:
        self.slot = slot
        self.kind = kind
        self._on_click = on_click
        self._on_clear = on_clear
        self._on_hover = on_hover
        self.state = "empty"          # empty | set | active | learning
        self._text = ""               # przypisanie (bez tekstu nauki)
        self._hover = False
        self._pulse = True
        self._flash_job: Optional[str] = None
        self._name_color = name_color

        self.name = tk.Label(parent, text=(label if label is not None
                                          else ROW_LABELS.get(slot, slot)),
                             font=fonts.small_bold if name_color else fonts.small,
                             bg=theme.PANEL, fg=name_color or theme.TEXT_DIM,
                             anchor="w")
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
        self._text = text
        self._show_text()

    def _show_text(self) -> None:
        # w trakcie nauki komorka mowi, co zrobic - a nie co bylo
        text = (self.LEARN_TEXT if self.state == "learning"
                else (self._text or EMPTY))
        if self.cell.cget("text") != text:
            self.cell.configure(text=text)

    def flash(self) -> None:
        """Krotkie zielone potwierdzenie - przypisanie sie udalo."""
        if self._flash_job is not None:
            self.cell.after_cancel(self._flash_job)
        self._flash_job = self.cell.after(self.FLASH_MS, self._flash_end)
        self._paint()

    def _flash_end(self) -> None:
        self._flash_job = None
        try:
            self._paint()
        except tk.TclError:
            pass                        # okno zamkniete w trakcie blysku

    def set_state(self, state: str, hover: bool = False,
                  pulse: bool = True) -> None:
        if (state, hover, pulse) == (self.state, self._hover, self._pulse):
            return
        self.state, self._hover, self._pulse = state, hover, pulse
        self._show_text()
        self._paint()

    def _paint(self) -> None:
        state, hover, pulse = self.state, self._hover, self._pulse
        if self._flash_job is not None and state != "learning":
            bg = theme.mix(theme.PANEL_HI, theme.GOOD, 0.30)
            self.cell.configure(bg=bg, fg="#ffffff",
                                highlightbackground=theme.GOOD,
                                highlightcolor=theme.GOOD)
            return
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
        if self._name_color:
            name_fg = (theme.lighten(self._name_color, 0.25) if hover
                       else self._name_color)
        else:
            name_fg = theme.TEXT if hover else theme.TEXT_DIM
        self.name.configure(fg=name_fg)

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
    _IDLE_HINT = ("Kliknij przycisk na rysunku albo wiersz i wciśnij "
                  "przycisk kierownicy. Spust lub drążek na rysunku otwiera "
                  "wybór źródła (kierownica, gaz, hamulec…). Prawy przycisk "
                  "myszy czyści.")
    _NAME_MINSIZE = 86
    #: Najwezsza lista, przy ktorej stoi jeszcze obok rysunku. Ponizej
    #: komorki ucinaly tekst albo znikaly za krawedzia - lista idzie wtedy
    #: pod rysunek.
    _LIST_MIN_W = 560
    #: Kolor nazwy wiersza - litery ABXY w kolorach z rysunku.
    _ROW_COLORS: Dict[str, str] = FACE_COLORS
    #: Gniazdo drazka -> jego cztery kierunki (menu po kliknieciu gniazda).
    _STICK_SLOTS: Dict[str, Tuple[str, ...]] = {
        "STICK_L": ("LX-", "LX+", "LY+", "LY-"),
        "STICK_R": ("RX-", "RX+", "RY+", "RY-"),
    }

    def __init__(self, master: tk.Misc, fonts: Fonts,
                 on_assign_request: Optional[Callable[[str], None]] = None,
                 on_clear: Optional[Callable[[str], None]] = None,
                 on_axis_click: Optional[Callable[[str, int, int], None]] = None,
                 on_axis_clear: Optional[Callable[[str], None]] = None,
                 on_cancel: Optional[Callable[[], None]] = None) -> None:
        super().__init__(master, style="Card.TFrame")
        self._fonts = fonts
        self._on_assign_request = on_assign_request
        self._on_clear = on_clear
        self._on_axis_click = on_axis_click
        self._on_axis_clear = on_axis_clear
        # uzytkownik przerwal nauke (klik obok kontrolki) - kreator musi
        # sie wtedy skonczyc, inaczej zostaje wlaczony bez celu
        self._on_cancel = on_cancel
        self._menu: Optional[PopupMenu] = None

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
        # podpowiedz lamie sie do szerokosci paska - w waskim oknie byla ucieta
        fit_wrap(self.hint, cap=4000, margin=30)

        self.canvas = self._CANVAS_CLS(self, fonts,
                                on_button_click=self._on_canvas_click,
                                on_button_clear=self._clear_button,
                                on_hover=self._on_canvas_hover,
                                on_axis_click=self._on_canvas_axis,
                                on_axis_clear=self._on_canvas_axis_clear)
        self.canvas.grid(row=1, column=0, sticky="n", padx=(0, 18))

        self._legend = self._build_legend()
        self._legend.grid(row=2, column=0, sticky="n", pady=(10, 0))

        panel = ttk.Frame(self, style="Card.TFrame")
        self._panel = panel
        self._stacked: Optional[bool] = None
        self._place_panel(stacked=False)

        self._rows: Dict[str, _Row] = {}
        self._build_panel(panel)
        self.bind("<Configure>", self._on_resize, add="+")

        self._set_hint_idle()

    # ----------------------------------------------------------- budowanie

    def _place_panel(self, stacked: bool) -> None:
        """Lista obok rysunku albo - w waskim oknie - pod nim."""
        if stacked == self._stacked:
            return
        self._stacked = stacked
        if stacked:
            self._panel.grid(row=3, column=0, columnspan=2, rowspan=1,
                             sticky="nsew", pady=(16, 0))
            self.grid_columnconfigure(0, weight=1)
            self.grid_columnconfigure(1, weight=0)
            self.canvas.grid_configure(padx=0)
        else:
            self._panel.grid(row=1, column=1, columnspan=1, rowspan=2,
                             sticky="nsew", pady=0)
            self.grid_columnconfigure(0, weight=0)
            self.grid_columnconfigure(1, weight=1)
            self.canvas.grid_configure(padx=(0, 18))

    def _on_resize(self, event) -> None:
        if event.widget is not self:
            return
        need = self.canvas.winfo_reqwidth() + 18 + self._LIST_MIN_W
        self._place_panel(stacked=event.width < need)

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
                        label=self._ROW_LABELS.get(slot, slot),
                        name_color=self._ROW_COLORS.get(slot))
                    row += 1

    # ---------------------------------------------------------- interakcja

    def _on_row_click(self, slot: str, x_root: int, y_root: int) -> None:
        if slot in self._AXES:
            self._user_cancel()
            if self._on_axis_click:
                self._on_axis_click(slot, x_root, y_root)
        else:
            self._start_learning(slot)

    def _user_cancel(self) -> None:
        """Nauka przerwana kliknieciem obok - informujemy wlasciciela."""
        was = self._learning is not None
        self.cancel_learning()
        if was and self._on_cancel:
            self._on_cancel()

    def _on_canvas_axis(self, part: str, x_root: int, y_root: int) -> None:
        """Klik w spust albo gniazdo drazka na rysunku."""
        self._user_cancel()
        if not self._on_axis_click:
            return
        if part in self._AXES:              # spust: od razu wybor zrodla
            self._on_axis_click(part, x_root, y_root)
            return
        slots = self._STICK_SLOTS.get(part)
        if not slots:
            return
        # drazek ma cztery gniazda - najpierw kierunek, potem zrodlo
        menu = PopupMenu(self, self._fonts,
                         title=("LEWY" if part == "STICK_L" else "PRAWY")
                         + " DRĄŻEK - KIERUNEK")
        for slot in slots:
            menu.add(self._ROW_LABELS.get(slot, slot),
                     command=lambda s=slot: self._on_axis_click(
                         s, x_root, y_root),
                     note=self._axis_labels.get(slot) or EMPTY)
        self._menu = menu
        menu.popup(x_root, y_root)

    def _on_canvas_axis_clear(self, part: str) -> None:
        if not self._on_axis_clear:
            return
        for slot in self._STICK_SLOTS.get(part, (part,)):
            if slot in self._AXES:
                self._on_axis_clear(slot)

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
            self._user_cancel()
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
                # nauczony przycisk - wiersz potwierdza to na zielono
                if name == self._learning and value >= 0:
                    self._rows[name].flash()
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
