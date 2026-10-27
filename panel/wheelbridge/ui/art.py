"""
Wygladzona grafika dla rysunkow pada i kierownicy.

Tk na Windowsie rysuje ksztalty bez wygladzania krawedzi - okregi i luki
sa poszarpane. Tu rysujemy w Pillow w powiekszeniu SS i zmniejszamy, co
daje gladkie krawedzie, a do tego gradienty i miekkie cienie.

Kontrolki sa gotowymi obrazkami w kilku stanach (CapStyle + STATES);
zmiana stanu na plotnie to tylko podmiana obrazka, wiec podglad na zywo
nic nie kosztuje. Obrazki powstaja przy pierwszym uzyciu i zostaja w
pamieci podrecznej.

Ksztalty to krotki we wspolrzednych plotna:
    ("circle", cx, cy, r)
    ("rrect", x1, y1, x2, y2, r)
    ("poly", ((x, y), ...))
    ("ring", cx, cy, r_out, r_in, a0, a1)     - wycinek pierscienia, katy w
                                                stopniach, 0 = w prawo,
                                                rosna zgodnie ze wskazowkami
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from functools import lru_cache
from typing import Callable, Optional, Sequence, Tuple

from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageTk

SS = 4                      # powiekszenie przy rysowaniu

#: Stany kontrolki - w kolejnosci waznosci (pierwszy wygrywa).
STATES = ("learn_on", "learn_off", "active", "hover", "assigned", "idle")

Shape = tuple
RGBA = Tuple[int, int, int, int]


def rgba(color: str, alpha: int = 255) -> RGBA:
    """"#rrggbb" -> (r, g, b, a)."""
    return (int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16),
            max(0, min(255, int(alpha))))


def mix(a: str, b: str, t: float) -> str:
    ca, cb = rgba(a), rgba(b)
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return "#%02x%02x%02x" % tuple(int(round(ca[i] + (cb[i] - ca[i]) * t))
                                   for i in range(3))


def lighter(color: str, t: float) -> str:
    return mix(color, "#ffffff", t)


def darker(color: str, t: float) -> str:
    return mix(color, "#000000", t)


@lru_cache(maxsize=64)
def font(size: float, bold: bool = False) -> ImageFont.ImageFont:
    """Czcionka Segoe UI w rozmiarze dla rysowania w powiekszeniu."""
    px = max(4, int(round(size * SS)))
    names = (("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf") if bold
             else ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"))
    for name in names:
        try:
            return ImageFont.truetype(name, px)
        except OSError:
            continue
    try:
        return ImageFont.load_default(px)
    except TypeError:                               # stary Pillow
        return ImageFont.load_default()


# ------------------------------------------------------------- ksztalty

def shape_bbox(shape: Shape) -> Tuple[float, float, float, float]:
    kind = shape[0]
    if kind == "circle":
        _k, cx, cy, r = shape
        return cx - r, cy - r, cx + r, cy + r
    if kind == "rrect":
        return shape[1], shape[2], shape[3], shape[4]
    if kind == "poly":
        xs = [p[0] for p in shape[1]]
        ys = [p[1] for p in shape[1]]
        return min(xs), min(ys), max(xs), max(ys)
    if kind == "ring":
        _k, cx, cy, ro, ri, a0, a1 = shape
        if a1 - a0 >= 360.0:
            return cx - ro, cy - ro, cx + ro, cy + ro
        pts = ring_points(cx, cy, ro, ri, a0, a1, 64)
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        return min(xs), min(ys), max(xs), max(ys)
    raise ValueError(kind)


def grow(shape: Shape, d: float) -> Shape:
    """Ten sam ksztalt powiekszony o d (ujemne - pomniejszony)."""
    kind = shape[0]
    if kind == "circle":
        return ("circle", shape[1], shape[2], max(0.5, shape[3] + d))
    if kind == "rrect":
        _k, x1, y1, x2, y2, r = shape
        return ("rrect", x1 - d, y1 - d, x2 + d, y2 + d, max(0.0, r + d))
    if kind == "ring":
        _k, cx, cy, ro, ri, a0, a1 = shape
        span = max(1.0, (ro + ri) / 2)
        da = math.degrees(d / span)
        return ("ring", cx, cy, ro + d, max(0.0, ri - d), a0 - da, a1 + da)
    if kind == "poly":
        # odsuniecie wierzcholkow od srodka ciezkosci - wystarcza dla
        # wypuklych ksztaltow (lopatki, piktogramy)
        pts = shape[1]
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        out = []
        for x, y in pts:
            dx, dy = x - cx, y - cy
            n = math.hypot(dx, dy) or 1.0
            out.append((x + dx / n * d, y + dy / n * d))
        return ("poly", tuple(out))
    raise ValueError(kind)


def ring_points(cx: float, cy: float, ro: float, ri: float, a0: float,
                a1: float, steps: int = 48) -> Tuple[Tuple[float, float], ...]:
    pts = []
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append((cx + ro * math.cos(a), cy + ro * math.sin(a)))
    for i in range(steps, -1, -1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append((cx + ri * math.cos(a), cy + ri * math.sin(a)))
    return tuple(pts)


def contains(shape: Shape, x: float, y: float) -> bool:
    """Czy punkt plotna lezy w ksztalcie - do klikania."""
    kind = shape[0]
    if kind == "circle":
        return math.hypot(x - shape[1], y - shape[2]) <= shape[3]
    if kind == "rrect":
        _k, x1, y1, x2, y2, r = shape
        if not (x1 <= x <= x2 and y1 <= y <= y2):
            return False
        r = min(r, (x2 - x1) / 2, (y2 - y1) / 2)
        cx = min(max(x, x1 + r), x2 - r)
        cy = min(max(y, y1 + r), y2 - r)
        return math.hypot(x - cx, y - cy) <= r + 1e-9
    if kind == "ring":
        _k, cx, cy, ro, ri, a0, a1 = shape
        d = math.hypot(x - cx, y - cy)
        if not ri <= d <= ro:
            return False
        a = math.degrees(math.atan2(y - cy, x - cx))
        while a < a0:
            a += 360.0
        while a > a0 + 360.0:
            a -= 360.0
        return a <= a1
    if kind == "poly":
        pts = shape[1]
        hit = False
        j = len(pts) - 1
        for i, (xi, yi) in enumerate(pts):
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                hit = not hit
            j = i
        return hit
    raise ValueError(kind)


# ------------------------------------------------------------- malowanie

def _scale_alpha(mask: Image.Image, alpha: int) -> Image.Image:
    if alpha >= 255:
        return mask
    return mask.point(lambda v: v * alpha // 255)


class Paint:
    """Obrazek RGBA rysowany w powiekszeniu SS.

    Wspolrzedne podajemy w pikselach plotna; origin to lewy gorny rog
    obrazka na plotnie. Kazda operacja pracuje na wycinku wokol swojego
    ksztaltu (_sub), nie na calym obrazku - tlo pada w powiekszeniu ma
    kilka milionow pikseli i inaczej start trwalby sekundy.
    """

    def __init__(self, w: int, h: int, origin: Tuple[float, float] = (0, 0),
                 ss: int = SS) -> None:
        self.w, self.h = max(1, int(w)), max(1, int(h))
        self.ox, self.oy = origin
        self.ss = ss
        self.img = Image.new("RGBA", (self.w * ss, self.h * ss), (0, 0, 0, 0))

    # --- przeliczenia ---
    def _x(self, x: float) -> float:
        return (x - self.ox) * self.ss

    def _y(self, y: float) -> float:
        return (y - self.oy) * self.ss

    def _flat(self, pts: Sequence[Tuple[float, float]]) -> list:
        return [(self._x(x), self._y(y)) for x, y in pts]

    def _sub(self, bbox: Tuple[float, float, float, float],
             margin: float = 2.0) -> "Paint":
        """Pusty wycinek obejmujacy bbox (we wspolrzednych plotna)."""
        x1 = max(self.ox, math.floor(bbox[0] - margin))
        y1 = max(self.oy, math.floor(bbox[1] - margin))
        x2 = min(self.ox + self.w, math.ceil(bbox[2] + margin))
        y2 = min(self.oy + self.h, math.ceil(bbox[3] + margin))
        return Paint(max(1, x2 - x1), max(1, y2 - y1), (x1, y1), self.ss)

    def paste(self, other: "Paint") -> None:
        self.img.alpha_composite(other.img,
                                 (int(round((other.ox - self.ox) * self.ss)),
                                  int(round((other.oy - self.oy) * self.ss))))

    def mask(self, shape: Shape) -> Image.Image:
        """Maska "L" ksztaltu w rozdzielczosci rysowania."""
        m = Image.new("L", self.img.size, 0)
        self._draw_shape(ImageDraw.Draw(m), shape, fill=255)
        return m

    def _draw_shape(self, d: ImageDraw.ImageDraw, shape: Shape,
                    fill=None) -> None:
        kind = shape[0]
        if kind == "circle":
            _k, cx, cy, r = shape
            d.ellipse((self._x(cx - r), self._y(cy - r), self._x(cx + r),
                       self._y(cy + r)), fill=fill)
        elif kind == "rrect":
            _k, x1, y1, x2, y2, r = shape
            r = max(0.0, min(r, (x2 - x1) / 2, (y2 - y1) / 2))
            d.rounded_rectangle((self._x(x1), self._y(y1), self._x(x2),
                                 self._y(y2)), radius=r * self.ss, fill=fill)
        elif kind == "poly":
            d.polygon(self._flat(shape[1]), fill=fill)
        elif kind == "ring":
            _k, cx, cy, ro, ri, a0, a1 = shape
            d.polygon(self._flat(ring_points(cx, cy, ro, ri, a0, a1)),
                      fill=fill)
        else:
            raise ValueError(kind)

    def _tinted(self, color: str, alpha_mask: Image.Image) -> Image.Image:
        layer = Image.new("RGBA", self.img.size, rgba(color, 0))
        layer.putalpha(alpha_mask)
        return layer

    # --- operacje ---
    def fill(self, shape: Shape, color: str, alpha: int = 255) -> None:
        sub = self._sub(shape_bbox(shape))
        m = _scale_alpha(sub.mask(shape), alpha)
        sub.img.alpha_composite(sub._tinted(color, m))
        self.paste(sub)

    def stroke(self, shape: Shape, color: str, width: float,
               alpha: int = 255) -> None:
        """Obrys PO WEWNETRZNEJ stronie ksztaltu (nie wychodzi poza niego)."""
        sub = self._sub(shape_bbox(shape))
        outer = sub.mask(shape)
        inner = sub.mask(grow(shape, -width))
        ring = Image.composite(Image.new("L", outer.size, 0), outer, inner)
        sub.img.alpha_composite(sub._tinted(color, _scale_alpha(ring, alpha)))
        self.paste(sub)

    def gradient(self, shape: Shape, top: str, bottom: str,
                 alpha: int = 255, y0: Optional[float] = None,
                 y1: Optional[float] = None) -> None:
        """Wypelnienie gradientem z gory na dol (y0..y1 - zakres gradientu;
        domyslnie wysokosc ksztaltu)."""
        bx1, by1, bx2, by2 = shape_bbox(shape)
        y0 = by1 if y0 is None else y0
        y1 = by2 if y1 is None else y1
        sub = self._sub((bx1, by1, bx2, by2))
        h = sub.img.size[1]
        t_a, t_b = rgba(top), rgba(bottom)
        span = max(1e-6, (y1 - y0) * self.ss)
        start = sub._y(y0)
        rows = []
        for row in range(h):
            t = (row - start) / span
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            rows.append(tuple(int(round(t_a[i] + (t_b[i] - t_a[i]) * t))
                              for i in range(3)) + (255,))
        col = Image.new("RGBA", (1, h))
        col.putdata(rows)
        layer = col.resize(sub.img.size, Image.NEAREST)
        layer.putalpha(_scale_alpha(sub.mask(shape), alpha))
        sub.img.alpha_composite(layer)
        self.paste(sub)

    def sheen(self, shape: Shape, alpha: int = 40, frac: float = 0.5,
              color: str = "#ffffff") -> None:
        """Polysk: kolor od gornej krawedzi ksztaltu, zanikajacy do frac
        jego wysokosci."""
        bx1, by1, bx2, by2 = shape_bbox(shape)
        sub = self._sub((bx1, by1, bx2, by2))
        h = sub.img.size[1]
        start = sub._y(by1)
        span = max(1e-6, (by2 - by1) * frac * self.ss)
        col = Image.new("L", (1, h))
        col.putdata([int(alpha * max(0.0, 1.0 - max(0.0, row - start) / span))
                     for row in range(h)])
        fade = col.resize(sub.img.size, Image.NEAREST)
        m = Image.composite(fade, Image.new("L", fade.size, 0), sub.mask(shape))
        sub.img.alpha_composite(sub._tinted(color, m))
        self.paste(sub)

    def radial(self, cx: float, cy: float, r: float, inner: str, outer: str,
               alpha_in: int = 255, alpha_out: int = 0,
               clip: Optional[Shape] = None) -> None:
        """Przejscie od srodka (inner) do brzegu kola r (outer)."""
        sub = self._sub((cx - r, cy - r, cx + r, cy + r))
        size = max(2, int(round(r * 2 * self.ss)))
        g = Image.radial_gradient("L").resize((size, size), Image.BILINEAR)
        a_in, a_out = rgba(inner, alpha_in), rgba(outer, alpha_out)
        luts = []
        for i in range(4):
            luts += [int(round(a_in[i] + (a_out[i] - a_in[i]) * v / 255))
                     for v in range(256)]
        disc = Image.merge("RGBA", (g, g, g, g)).point(luts)
        # gradient siega 255 dopiero w rogach kwadratu - przycinamy do kola
        circle = Image.new("L", (size, size), 0)
        ImageDraw.Draw(circle).ellipse((0, 0, size - 1, size - 1), fill=255)
        disc.putalpha(Image.composite(disc.getchannel("A"),
                                      Image.new("L", (size, size), 0), circle))
        layer = Image.new("RGBA", sub.img.size, (0, 0, 0, 0))
        layer.alpha_composite(disc, (int(round(sub._x(cx - r))),
                                     int(round(sub._y(cy - r)))))
        if clip is not None:
            a = layer.getchannel("A")
            layer.putalpha(Image.composite(a, Image.new("L", a.size, 0),
                                           sub.mask(clip)))
        sub.img.alpha_composite(layer)
        self.paste(sub)

    def line(self, pts: Sequence[Tuple[float, float]], color: str,
             width: float, alpha: int = 255, round_caps: bool = True) -> None:
        xs = [x for x, _ in pts]
        ys = [y for _, y in pts]
        sub = self._sub((min(xs), min(ys), max(xs), max(ys)), width + 2)
        m = Image.new("L", sub.img.size, 0)
        d = ImageDraw.Draw(m)
        flat = sub._flat(pts)
        w = max(1, int(round(width * self.ss)))
        d.line(flat, fill=255, width=w, joint="curve")
        if round_caps:
            r = w / 2
            for x, y in (flat[0], flat[-1]):
                d.ellipse((x - r, y - r, x + r, y + r), fill=255)
        sub.img.alpha_composite(sub._tinted(color, _scale_alpha(m, alpha)))
        self.paste(sub)

    def arc(self, cx: float, cy: float, r: float, a0: float, a1: float,
            color: str, width: float, alpha: int = 255) -> None:
        """Luk o grubosci width wysrodkowany na promieniu r, z okraglymi
        koncami. Katy jak w "ring"."""
        if a1 - a0 <= 0.05:
            return
        sub = self._sub((cx - r - width, cy - r - width, cx + r + width,
                         cy + r + width))
        m = sub.mask(("ring", cx, cy, r + width / 2, r - width / 2, a0, a1))
        d = ImageDraw.Draw(m)
        for a in (a0, a1):
            rad = math.radians(a)
            sub._draw_shape(d, ("circle", cx + r * math.cos(rad),
                                cy + r * math.sin(rad), width / 2), fill=255)
        sub.img.alpha_composite(sub._tinted(color, _scale_alpha(m, alpha)))
        self.paste(sub)

    def text(self, x: float, y: float, s: str, size: float, color: str,
             bold: bool = False, alpha: int = 255) -> None:
        half_w = size * max(1, len(s)) * 0.75 + 4
        sub = self._sub((x - half_w, y - size * 1.4, x + half_w, y + size * 1.4))
        m = Image.new("L", sub.img.size, 0)
        ImageDraw.Draw(m).text((sub._x(x), sub._y(y)), s, font=font(size * self.ss / SS, bold),
                               fill=255, anchor="mm")
        sub.img.alpha_composite(sub._tinted(color, _scale_alpha(m, alpha)))
        self.paste(sub)

    def shadow(self, shape: Shape, dy: float = 4, blur: float = 6,
               alpha: int = 150, color: str = "#000000") -> None:
        """Miekki cien pod ksztaltem (rozmywany w zwyklej rozdzielczosci)."""
        bx1, by1, bx2, by2 = shape_bbox(shape)
        sub = self._sub((bx1, by1, bx2, by2), blur * 3 + abs(dy))
        small = sub.mask(shape).reduce(self.ss)
        shifted = Image.new("L", small.size, 0)
        shifted.paste(small, (0, int(round(dy))))
        soft = _scale_alpha(shifted.filter(ImageFilter.GaussianBlur(blur)), alpha)
        sub.img.alpha_composite(
            sub._tinted(color, soft.resize(sub.img.size, Image.BILINEAR)))
        self.paste(sub)

    def glow(self, shape: Shape, color: str, radius: float = 6,
             alpha: int = 170) -> None:
        self.shadow(grow(shape, radius * 0.35), dy=0, blur=radius,
                    alpha=alpha, color=color)

    def image(self) -> Image.Image:
        return self.img.reduce(self.ss)


def photo(img: Image.Image, master) -> ImageTk.PhotoImage:
    return ImageTk.PhotoImage(img, master=master)


def opaque(img: Image.Image, background: str) -> Image.Image:
    """Obrazek polozony na jednolitym tle - bez przezroczystosci.

    Tk wyswietla i podmienia obrazki z przezroczystoscia wielokrotnie
    wolniej, wiec duze tla (korpus pada, kierownica) robimy nieprzezroczyste.
    """
    out = Image.new("RGBA", img.size, rgba(background))
    out.alpha_composite(img)
    return out


# ------------------------------------------------------------ kontrolki

@dataclass(frozen=True)
class CapStyle:
    """Wyglad jednej kontrolki w zwyklym stanie i po wcisnieciu."""
    top: str                    # gradient plastiku
    bottom: str
    edge: str                   # cienki obrys
    on: str                     # kolor wcisnietej
    glyph: str                  # kolor napisu/piktogramu
    glyph_on: str               # kolor napisu na wcisnietej
    accent: str                 # kolor obwodki przypisanej/wskazanej
    warn: str                   # kolor nauki
    ink: str                    # ciemny napis na jasnym tle
    gloss: bool = True          # polysk wypuklego plastiku (wglebienie - nie)


GLOW_MARGIN = 10                # zapas wokol kontrolki na poswiate


def render_cap(shape: Shape, state: str, style: CapStyle,
               glyph: Optional[Callable[[Paint, str], None]] = None,
               shadow: bool = True) -> Tuple[Image.Image, Tuple[int, int]]:
    """Obrazek kontrolki w danym stanie + polozenie lewego gornego rogu."""
    x1, y1, x2, y2 = shape_bbox(shape)
    m = GLOW_MARGIN
    ox, oy = math.floor(x1 - m), math.floor(y1 - m)
    w, h = math.ceil(x2 + m) - ox, math.ceil(y2 + m) - oy
    p = Paint(w, h, (ox, oy))

    if state in ("active", "learn_on"):
        p.glow(shape, style.warn if state == "learn_on" else style.on,
               radius=6, alpha=210)
    elif state == "hover":
        p.glow(shape, style.accent, radius=5, alpha=140)
    if shadow:
        p.shadow(shape, dy=2.5, blur=2.5, alpha=160)

    if state == "active":
        p.gradient(shape, lighter(style.on, 0.22), darker(style.on, 0.10))
        glyph_color = style.glyph_on
    elif state == "learn_on":
        p.gradient(shape, lighter(style.warn, 0.18), darker(style.warn, 0.08))
        glyph_color = style.ink
    else:
        p.gradient(shape, style.top, style.bottom)
        glyph_color = style.glyph
        # odblask na gornej polowie - wypukly plastik
        if style.gloss:
            p.sheen(shape, alpha=34, frac=0.6)
            p.stroke(shape, "#ffffff", 1.0, alpha=22)

    edge, edge_w, edge_a = style.edge, 1.0, 255
    if state == "learn_off":
        edge, edge_w = style.warn, 2.0
        glyph_color = style.warn
    elif state == "hover":
        edge, edge_w = style.accent, 2.0
    elif state == "assigned":
        edge, edge_w, edge_a = style.accent, 1.5, 170
    elif state in ("active", "learn_on"):
        edge, edge_a = "#ffffff", 70
    p.stroke(shape, edge, edge_w, alpha=edge_a)

    if glyph is not None:
        glyph(p, glyph_color)
    return p.image(), (ox, oy)
