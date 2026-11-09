"""Widgety rysowane na Canvas - wskaznik kierownicy, paski, diody."""

from __future__ import annotations

import math
import tkinter as tk
from tkinter import ttk
from typing import Callable, Iterable, List, Optional, Sequence, Tuple

from PIL import Image, ImageChops, ImageDraw

from . import art, theme
from .theme import Fonts

#: Linia podgladu na zywo, zanim plytka cos przysle - pusta zostawiala
#: w karcie dziure.
OFFLINE_LIVE = "Podgląd na żywo ruszy po połączeniu z płytką."


# --------------------------------------------------------------- pomocnicze

def round_points(x1: float, y1: float, x2: float, y2: float,
                 r: float) -> List[float]:
    """Punkty zaokraglonego prostokata dla create_polygon(smooth=True).

    Naroza sa zdublowane - krzywa Tk przechodzi wtedy przez bok prosto, a
    zakreca dopiero w rogu. Promien jest przycinany do polowy krotszego
    boku, wiec przy zerowej szerokosci nie wywraca ksztaltu.
    """
    w, h = abs(x2 - x1), abs(y2 - y1)
    r = max(0.0, min(r, w / 2.0, h / 2.0))
    return [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r,
            x2, y2, x2 - r, y2, x1 + r, y2, x1, y2, x1, y2 - r,
            x1, y1 + r, x1, y1]


def round_rect(canvas: tk.Canvas, x1: float, y1: float, x2: float, y2: float,
               r: float, **kw) -> int:
    return canvas.create_polygon(round_points(x1, y1, x2, y2, r),
                                 smooth=True, **kw)


def set_round_rect(canvas: tk.Canvas, item: int, x1: float, y1: float,
                   x2: float, y2: float, r: float) -> None:
    canvas.coords(item, *round_points(x1, y1, x2, y2, r))


def configure_if_changed(widget: tk.Misc, **options) -> None:
    """configure() tylko z opcjami, ktore naprawde sie zmienily.

    Kazde configure to przerysowanie widgetu, a zmiana tekstu - takze
    przeliczenie ukladu calego okna. Odswiezanie 60 razy na sekunde tych
    samych napisow dawalo zauwazalne przyciecia. Porownujemy z cget, nie z
    wlasna pamiecia: napis moze zmienic tez inny fragment kodu.
    """
    changed = {k: v for k, v in options.items()
               if str(widget.cget(k)) != str(v)}
    if changed:
        widget.configure(**changed)


def cached_photo(widget: tk.Misc, key: tuple,
                 render: Callable[[], Image.Image]) -> object:
    """Obrazek z pamieci okna - render() wolany tylko przy pierwszym uzyciu.

    Wygladzone ksztalty z Pillow zamiast wielokatow Tk: Tk na Windowsie nie
    wygladza krawedzi, a jego "smooth" zostawial w rogach pojedyncze piksele.
    """
    root = widget.winfo_toplevel()
    cache = getattr(root, "_photo_cache", None)
    if cache is None:
        cache = root._photo_cache = {}
    photo = cache.get(key)
    if photo is None:
        photo = cache[key] = art.photo(render(), root)
    return photo


def rrect_image(w: int, h: int, r: float, fill: str,
                bg: str = theme.PANEL) -> Image.Image:
    """Wygladzony zaokraglony prostokat na jednolitym tle."""
    p = art.Paint(w, h)
    p.fill(("rrect", 0, 0, w, h, r), fill)
    return art.opaque(p.image(), bg).convert("RGB")


def fit_wrap(label: tk.Misc, cap: int = 900, margin: int = 0) -> tk.Misc:
    """Lamie napis do szerokosci rodzica, ale nie szerzej niz cap.

    Stale wraplength bylo dobrane do jednego rozmiaru okna: w szerokiej
    karcie tekst konczyl sie w polowie, w waskiej - wychodzil za krawedz.
    cap trzyma czytelna dlugosc wiersza na duzym ekranie. Zwraca etykiete,
    zeby dalo sie od razu wywolac pack/grid.
    """
    def on_resize(event) -> None:
        width = max(120, min(cap, event.width - margin))
        configure_if_changed(label, wraplength=width)

    label.master.bind("<Configure>", on_resize, add="+")
    return label


# ----------------------------------------------------------- narozniki

def _corner_images(widget: tk.Misc, inside: str, border: str, outside: str,
                   r: int) -> List[object]:
    """Cztery wygladzone narozniki (lg, pg, ld, pd) - wspolne dla okna."""
    root = widget.winfo_toplevel()
    cache = getattr(root, "_corner_cache", None)
    if cache is None:
        cache = root._corner_cache = {}
    key = (inside, border, outside, r)
    if key not in cache:
        size = 4 * r
        paint = art.Paint(size, size)
        paint.fill(("rrect", 0, 0, size, size, 0), outside)
        paint.fill(("rrect", 0, 0, size, size, r), border)
        paint.fill(("rrect", 1, 1, size - 1, size - 1, r - 1), inside)
        tl = paint.image().crop((0, 0, r, r)).convert("RGB")
        flips = (None, Image.FLIP_LEFT_RIGHT, Image.FLIP_TOP_BOTTOM,
                 Image.ROTATE_180)
        cache[key] = [art.photo(tl if f is None else tl.transpose(f), root)
                      for f in flips]
    return cache[key]


def round_corners(frame: tk.Misc, inside: str, border: str = theme.BORDER,
                  outside: str = theme.BG, r: int = 10) -> None:
    """Zaokragla rogi prostokatnej ramki z cienka obwodka.

    Tk nie umie zaokraglac ramek, wiec na kazdy rog kladziemy maly
    wygladzony obrazek: na zewnatrz luku kolor tla pod ramka, na luku
    obwodka, w srodku kolor ramki. bordermode="outside" - obrazek lezy
    takze na obwodce rysowanej przez highlightthickness.
    """
    images = _corner_images(frame, inside, border, outside, r)
    for image, (relx, rely, anchor) in zip(images, ((0, 0, "nw"), (1, 0, "ne"),
                                                    (0, 1, "sw"), (1, 1, "se"))):
        tk.Label(frame, image=image, bd=0, highlightthickness=0, padx=0,
                 pady=0, bg=outside).place(relx=relx, rely=rely, anchor=anchor,
                                           bordermode="outside")


# -------------------------------------------------------------------- karta

class Card(ttk.Frame):
    """Panel z naglowkiem, cienka ramka i linia pod tytulem."""

    def __init__(self, master: tk.Misc, title: str, fonts: Fonts,
                 subtitle: str = "", wrap: int = 900,
                 accent: str = theme.ACCENT, **kw) -> None:
        self.outer = tk.Frame(master, bg=theme.BORDER, bd=0, highlightthickness=0)
        super().__init__(self.outer, style="Card.TFrame",
                         padding=(15, 12, 15, 14), **kw)
        super().pack(fill="both", expand=True, padx=1, pady=1)
        round_corners(self.outer, theme.PANEL)

        self._fonts = fonts
        header = ttk.Frame(self, style="Card.TFrame")
        header.pack(fill="x")

        # pionowa kreska w kolorze akcentu - tytul od razu wpada w oko
        tick = tk.Frame(header, bg=accent, width=3, height=13)
        tick.pack(side="left", padx=(0, 9))
        tick.pack_propagate(False)

        self.title_label = ttk.Label(header, text=title.upper(),
                                     style="Heading.TLabel")
        self.title_label.configure(font=(fonts.family, 9, "bold"),
                                   foreground=theme.TEXT_DIM)
        self.title_label.pack(side="left")
        self.right_slot = ttk.Frame(header, style="Card.TFrame")
        self.right_slot.pack(side="right")

        rule = tk.Frame(self, bg=theme.BORDER, height=1)
        rule.pack(fill="x", pady=(10, 0))

        self.body = ttk.Frame(self, style="Card.TFrame")
        self.body.pack(fill="both", expand=True, pady=(11, 0))

        if subtitle:
            fit_wrap(ttk.Label(self.body, text=subtitle, style="Muted.TLabel",
                               wraplength=wrap, justify="left"),
                     cap=wrap).pack(anchor="w", pady=(0, 8))

    # Ukladamy zewnetrzna ramke, nie samego siebie.
    def place_card(self, **kw) -> "Card":
        self.outer.pack(**kw)
        return self

    def grid_card(self, **kw) -> "Card":
        self.outer.grid(**kw)
        return self


class TabBar(tk.Frame):
    """Wlasny pasek zakladek - ikona, napis i podkreslenie zamiast ramek ttk.

    icons: znaki z czcionki ikon Windows (fonts.icons); bez tej czcionki
    pasek pokazuje same napisy.
    """

    def __init__(self, master: tk.Misc, fonts: Fonts, labels: Sequence[str],
                 command: Callable[[int], None],
                 icons: Sequence[str] = ()) -> None:
        super().__init__(master, bg=theme.BG)
        self._command = command
        self._selected = 0
        self._labels: List[tk.Label] = []
        self._icons: List[Optional[tk.Label]] = []
        self._marks: List[tk.Frame] = []
        self._hover = -1

        for index, text in enumerate(labels):
            item = tk.Frame(self, bg=theme.BG, cursor="hand2")
            item.pack(side="left")
            row = tk.Frame(item, bg=theme.BG, cursor="hand2")
            row.pack(padx=14, pady=(8, 9))
            glyph = icons[index] if index < len(icons) else ""
            icon = None
            if glyph and fonts.icons:
                icon = tk.Label(row, text=glyph, bg=theme.BG, fg=theme.MUTED,
                                font=(fonts.icons, 11), cursor="hand2", bd=0,
                                padx=0, pady=0)
                icon.pack(side="left", padx=(0, 7))
            label = tk.Label(row, text=text, bg=theme.BG, fg=theme.MUTED,
                             font=fonts.small_bold, cursor="hand2", bd=0,
                             padx=0, pady=0)
            label.pack(side="left")
            mark = tk.Frame(item, bg=theme.BG, height=2)
            mark.pack(fill="x", padx=6)
            for widget in (item, row, label, icon):
                if widget is None:
                    continue
                widget.bind("<Button-1>", lambda _e, i=index: self._click(i))
                widget.bind("<Enter>", lambda _e, i=index: self._set_hover(i))
                widget.bind("<Leave>", lambda _e, i=index: self._set_hover(-1))
            self._labels.append(label)
            self._icons.append(icon)
            self._marks.append(mark)

        self._paint()

    def _click(self, index: int) -> None:
        self._command(index)

    def _set_hover(self, index: int) -> None:
        if self._hover != index:
            self._hover = index
            self._paint()

    def select(self, index: int) -> None:
        if 0 <= index < len(self._labels) and index != self._selected:
            self._selected = index
            self._paint()

    def _paint(self) -> None:
        for i, (label, icon, mark) in enumerate(zip(self._labels, self._icons,
                                                    self._marks)):
            if i == self._selected:
                fg, icon_fg, line = theme.TEXT, theme.ACCENT, theme.ACCENT
            else:
                fg = theme.TEXT_DIM if i == self._hover else theme.MUTED
                icon_fg, line = fg, theme.BG
            configure_if_changed(label, fg=fg)
            if icon is not None:
                configure_if_changed(icon, fg=icon_fg)
            configure_if_changed(mark, bg=line)


class Pages(tk.Frame):
    """Strony zakladek ulozone w stos - zamiennik ttk.Notebook.

    Notebook przy zmianie zakladki chowa stara strone i od nowa pokazuje
    nowa, a na Windowsie kazda kontrolka Tk to osobne okno systemowe:
    przelaczenie ciezkiej zakladki trwalo do 300 ms. Tu wszystkie strony
    sa pokazane caly czas, jedna nad druga, a zmiana zakladki tylko
    podnosi wybrana na wierzch.

    API jak w Notebook: add, select, index, tabs i zdarzenie
    <<NotebookTabChanged>>.
    """

    def __init__(self, master: tk.Misc, bg: str = theme.BG) -> None:
        super().__init__(master, bg=bg, bd=0, highlightthickness=0)
        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(0, weight=1)
        self._pages: List[tk.Misc] = []
        self._current = -1

    def add(self, page: tk.Misc, text: str = "") -> None:
        page.grid(row=0, column=0, sticky="nsew", in_=self)
        self._pages.append(page)
        if self._current < 0:
            self._current = 0
        # nowa strona laduje na wierzchu - wybrana ma tam zostac
        self._pages[self._current].tkraise()

    def tabs(self) -> Tuple[str, ...]:
        return tuple(str(p) for p in self._pages)

    def index(self, tab_id) -> int:
        if isinstance(tab_id, int):
            if not 0 <= tab_id < len(self._pages):
                raise tk.TclError(f"błędny indeks strony: {tab_id}")
            return tab_id
        if tab_id == "current":
            return self._current
        if tab_id == "end":
            return len(self._pages)
        name = str(tab_id)
        for i, page in enumerate(self._pages):
            if str(page) == name:
                return i
        raise tk.TclError(f"nie ma takiej strony: {name}")

    def select(self, tab_id=None):
        """Bez argumentu - sciezka wybranej strony, jak w Notebook."""
        if tab_id is None:
            return str(self._pages[self._current]) if self._pages else ""
        index = self.index(tab_id)
        if index == self._current:
            return None
        old = self._pages[self._current]
        self._current = index
        self._pages[index].tkraise()
        # Fokus z zaslonietej strony przechodzi na stos: inaczej klawisze
        # trafialyby do niewidocznego pola.
        try:
            focus = self.focus_get()
        except (KeyError, tk.TclError):     # np. rozwinieta lista comboboxa
            focus = None
        if focus is not None and str(focus).startswith(str(old) + "."):
            self.focus_set()
        self.event_generate("<<NotebookTabChanged>>")
        return None


class StatusDot(tk.Canvas):
    """Kolorowa kropka stanu z delikatna poswiata."""

    def __init__(self, master: tk.Misc, size: int = 12, bg: str = theme.PANEL) -> None:
        super().__init__(master, width=size, height=size, bg=bg,
                         highlightthickness=0, bd=0)
        self._size = size
        self._shown: Optional[Tuple[str, bool]] = None
        pad = max(2, size // 5)
        self._glow = self.create_oval(0, 0, size, size, fill="", outline="")
        self._ring = self.create_oval(pad - 1, pad - 1, size - pad + 1,
                                      size - pad + 1, fill="", outline="")
        self._dot = self.create_oval(pad, pad, size - pad, size - pad,
                                     fill=theme.MUTED, outline="")

    def set(self, color: str, glow: bool = True) -> None:
        if self._shown == (color, glow):
            return
        self._shown = (color, glow)
        bg = self.cget("bg")
        self.itemconfigure(self._dot, fill=color)
        self.itemconfigure(self._glow,
                           fill=theme.mix(bg, color, 0.22) if glow else "")
        self.itemconfigure(self._ring,
                           outline=theme.mix(bg, color, 0.45) if glow else "")


class WheelDial(tk.Canvas):
    """Duzy wskaznik kata obrotu kierownicy z grafika kola.

    Rysowany w Pillow (art.py), wiec krawedzie sa gladkie. Stale tlo i
    obrazek kola powstaja raz; przy ruchu skladamy tylko klatke: tlo, luk
    zakresu (maska pierscienia) i obrocone kolo - kilka milisekund i tylko
    wtedy, gdy kierownica naprawde sie ruszyla.

    Odczyt kata stoi pod pierscieniem, a nie na kole - nic go nie zaslania.
    """

    READOUT_H = 62              # pas pod pierscieniem na odczyt
    MIN_STEP = 0.2              # mniejsza zmiana kata nie przerysowuje

    def __init__(self, master: tk.Misc, fonts: Fonts, size: int = 250) -> None:
        super().__init__(master, width=size, height=size + self.READOUT_H,
                         bg=theme.PANEL, highlightthickness=0, bd=0)
        self._fonts = fonts
        self._size = size
        self._range = 900.0
        self._deg = 0.0
        self._wheels: dict = {}         # kolor znacznika -> obrazek kola
        self._build()

    # ------------------------------------------------------------------ build

    def _build(self) -> None:
        s = self._size
        c = s / 2
        self._ring_w = ring_w = max(9, s // 19)
        r_out = s / 2 - 6
        self._r_mid = r_out - ring_w / 2
        r_in = r_out - ring_w
        self._wheel_r = r_in - 20

        # tlo: rowek pierscienia, podzialka i znacznik zera
        p = art.Paint(s, s)
        p.fill(("ring", c, c, r_out, r_in, 0.0, 360.0), theme.TRACK)
        p.stroke(("circle", c, c, r_out), "#000000", 1.0, alpha=60)
        for i in range(-4, 5):
            a = math.radians(-90 + i * 45.0)
            major = i % 2 == 0
            r1 = r_in - 5
            r0 = r1 - (10 if major else 5)
            p.line([(c + r0 * math.cos(a), c + r0 * math.sin(a)),
                    (c + r1 * math.cos(a), c + r1 * math.sin(a))],
                   theme.TEXT_DIM if major else theme.BORDER_HI,
                   2.0 if major else 1.2)
        p.fill(("poly", ((c, c - r_out + 1), (c - 5, c - r_out - 6),
                         (c + 5, c - r_out - 6))), theme.MUTED)
        # Klatka jest nieprzezroczysta (tlo karty pod spodem): obrazek z
        # przezroczystoscia Tk przyjmuje kilkadziesiat razy wolniej.
        self._base = art.opaque(p.image(), theme.PANEL)

        # maska pelnego pierscienia postepu i okragly koniec luku
        m = art.Paint(s, s)
        m.fill(("ring", c, c, r_out - 1, r_in + 1, 0.0, 360.0), "#ffffff")
        self._ring_mask = m.image().getchannel("A")
        d = int(math.ceil(ring_w - 2)) + 2
        cap = art.Paint(d, d)
        cap.fill(("circle", d / 2, d / 2, (ring_w - 2) / 2), "#ffffff")
        self._cap = cap.image().getchannel("A")

        self._frame = art.photo(self._base.convert("RGB"), self)
        self.create_image(0, 0, image=self._frame, anchor="nw")

        # odczyt pod pierscieniem
        ry = s + 6
        self._text = self.create_text(c, ry + 16, text="0°",
                                      font=self._fonts.big, fill=theme.TEXT)
        self._range_text = self.create_text(c, ry + 46, text="zakres 900°",
                                            font=self._fonts.small,
                                            fill=theme.MUTED)
        self._redraw()

    def _wheel_image(self, marker: str):
        """Kolo kierownicy (obrecz, ramiona, piasta) - jedno na kolor."""
        img = self._wheels.get(marker)
        if img is not None:
            return img
        r = self._wheel_r
        rim_w = max(7.0, self._size / 26)
        half = int(math.ceil(r + rim_w)) + 4
        p = art.Paint(half * 2, half * 2)
        c = float(half)
        rim = ("ring", c, c, r + rim_w / 2, r - rim_w / 2, 0.0, 360.0)
        # cien obreczy bez przesuniecia - obraca sie razem z kolem
        p.shadow(rim, dy=0, blur=4, alpha=150)
        # ramiona w lewo i w prawo i szprycha w dol - pod obrecza
        spoke = art.mix(theme.PANEL_HI, theme.TEXT, 0.16)
        for deg in (0.0, 180.0, 90.0):
            a = math.radians(deg)
            p.line([(c, c), (c + r * math.cos(a), c + r * math.sin(a))],
                   spoke, rim_w + 3, round_caps=False)
        p.gradient(rim, art.mix(theme.PANEL_HI, theme.TEXT, 0.30),
                   art.darker(theme.PANEL_HI, 0.25))
        p.sheen(rim, alpha=40, frac=0.4)
        hub_r = max(14.0, self._size / 14)
        p.fill(("circle", c, c, hub_r + 4), theme.PANEL_HI)
        p.gradient(("circle", c, c, hub_r), "#2a313d", theme.PANEL)
        p.stroke(("circle", c, c, hub_r), art.mix(theme.ACCENT_DIM, marker, 0.5),
                 2.0)
        # pasek "gora kierownicy" na obreczy
        p.line([(c, c - r - rim_w / 2 + 1), (c, c - r + rim_w / 2 - 1)],
               marker, rim_w + 1, round_caps=False)
        img = p.image()
        if len(self._wheels) > 16:
            self._wheels.clear()
        self._wheels[marker] = img
        return img

    # ------------------------------------------------------------------- API

    def set_range(self, degrees: float) -> None:
        degrees = max(1.0, float(degrees))
        if abs(degrees - self._range) < 0.5:
            return
        self._range = degrees
        self.itemconfigure(self._range_text, text=f"zakres {int(degrees)}°")
        self._redraw()

    def set_angle(self, degrees: float) -> None:
        if degrees != degrees:
            degrees = 0.0
        if abs(degrees - self._deg) < self.MIN_STEP:
            return
        self._deg = degrees
        self._redraw()

    # --------------------------------------------------------------- rysunek

    def _redraw(self) -> None:
        s = self._size
        c = s / 2
        half = self._range / 2.0
        frac = max(-1.0, min(1.0, self._deg / half))
        over = abs(self._deg) > half + 0.5
        # kolor ma kilka stopni - kolo dla kazdego rysujemy tylko raz
        near = round(min(1.0, max(0.0, (abs(frac) - 0.75) / 0.25)) * 4) / 4
        color = theme.BAD if over else theme.mix(theme.ACCENT, theme.WARN, near)

        frame = self._base.copy()
        if abs(frac) > 0.003:
            # luk od gory: w prawo zgodnie ze wskazowkami zegara
            a0, a1 = sorted((-90.0, -90.0 + frac * 180.0))
            pie = Image.new("L", (s, s), 0)
            ImageDraw.Draw(pie).pieslice((0, 0, s - 1, s - 1), a0, a1, fill=255)
            alpha = ImageChops.multiply(self._ring_mask, pie)
            layer = Image.new("RGBA", (s, s), art.rgba(color, 0))
            layer.putalpha(alpha)
            frame.alpha_composite(layer)
            for a in (a0, a1):
                rad = math.radians(a)
                x = c + self._r_mid * math.cos(rad) - self._cap.width / 2
                y = c + self._r_mid * math.sin(rad) - self._cap.height / 2
                dot = Image.new("RGBA", self._cap.size, art.rgba(color, 0))
                dot.putalpha(self._cap)
                frame.alpha_composite(dot, (int(round(x)), int(round(y))))

        wheel = self._wheel_image(color).rotate(-self._deg,
                                                resample=Image.BILINEAR)
        frame.alpha_composite(wheel, (int(round(c - wheel.width / 2)),
                                      int(round(c - wheel.height / 2))))
        self._frame.paste(frame.convert("RGB"))

        text = "0°" if abs(self._deg) < 0.5 else f"{self._deg:+.0f}°"
        self.itemconfigure(self._text, text=text,
                           fill=theme.BAD if over else theme.TEXT)


class BarMeter(tk.Frame):
    """Poziomy pasek z etykieta, procentem i surowa wartoscia."""

    def __init__(self, master: tk.Misc, fonts: Fonts, label: str,
                 color: str = theme.ACCENT, width: int = 230, height: int = 16,
                 show_raw: bool = True, bipolar: bool = False) -> None:
        super().__init__(master, bg=theme.PANEL)
        self._fonts = fonts
        self._color = color
        self._cw = width
        self._ch = height
        self._bipolar = bipolar
        self._value = -1.0

        top = tk.Frame(self, bg=theme.PANEL)
        top.pack(fill="x")
        tk.Label(top, text=label, bg=theme.PANEL, fg=theme.TEXT_DIM,
                 font=fonts.small).pack(side="left")
        # Stala szerokosc: "5 %" i "100 %" maja rozna dlugosc, a kazda zmiana
        # rozmiaru napisu przeliczala uklad calej karty.
        self._pct = tk.Label(top, text="0 %", bg=theme.PANEL, fg=theme.TEXT,
                             font=fonts.bold, width=6, anchor="e")
        self._pct.pack(side="right")
        self._raw = None
        self._raw_value: Optional[int] = None
        if show_raw:
            self._raw = tk.Label(top, text="", bg=theme.PANEL, fg=theme.MUTED,
                                 font=fonts.mono_small, width=5, anchor="e")
            self._raw.pack(side="right", padx=(0, 6))

        self._canvas = tk.Canvas(self, width=width, height=height, bg=theme.PANEL,
                                 highlightthickness=0, bd=0)
        self._canvas.pack(fill="x", pady=(5, 0))
        self._radius = height / 2.0
        self._track = round_rect(self._canvas, 0, 0, width, height,
                                 self._radius, fill=theme.TRACK, outline="")
        # Podzialka co 25 % lezy POD wypelnieniem: widac ja tylko na pustej
        # czesci rowka. Ciemne kreski na kolorowym pasku wygladaly jak bledy.
        self._ticks = [self._canvas.create_line(0, 3, 0, height - 3,
                                                fill=theme.mix(theme.TRACK,
                                                               theme.BORDER_HI,
                                                               0.55),
                                                width=1)
                       for _ in range(3)]
        self._fill = round_rect(self._canvas, 0, 0, 0, height, self._radius,
                                fill=color, outline="")
        if bipolar:
            self._mid = self._canvas.create_line(width / 2, 0, width / 2, height,
                                                 fill=theme.BORDER_HI, width=1)
        else:
            self._mid = None
        self._layout()
        self._canvas.bind("<Configure>", self._on_resize)
        self.set(0.0)

    def _layout(self) -> None:
        w, h = self._cw, self._ch
        set_round_rect(self._canvas, self._track, 0, 0, w, h, self._radius)
        for i, tick in enumerate(self._ticks):
            x = w * (i + 1) / 4.0
            self._canvas.coords(tick, x, 3, x, h - 3)
            self._canvas.itemconfigure(tick, state="normal" if w > 60
                                       else "hidden")
        if self._mid is not None:
            self._canvas.coords(self._mid, w / 2, 0, w / 2, h)

    def _on_resize(self, event) -> None:
        self._cw = max(10, event.width)
        self._layout()
        v, self._value = self._value, -2.0
        self.set(max(0.0, v) if not self._bipolar else v)

    def _draw_fill(self, x1: float, x2: float) -> None:
        if x2 - x1 < 1.5:
            self._canvas.itemconfigure(self._fill, state="hidden")
            return
        self._canvas.itemconfigure(self._fill, state="normal")
        set_round_rect(self._canvas, self._fill, x1, 0, x2, self._ch,
                       self._radius)

    def set(self, value: float, raw: Optional[int] = None) -> None:
        if value != value:
            value = 0.0
        if self._bipolar:
            value = max(-1.0, min(1.0, value))
        else:
            value = max(0.0, min(1.0, value))

        if abs(value - self._value) > 0.0005:
            self._value = value
            if self._bipolar:
                mid = self._cw / 2
                if value >= 0:
                    self._draw_fill(mid, mid + value * mid)
                else:
                    self._draw_fill(mid + value * mid, mid)
                self._canvas.itemconfigure(
                    self._fill,
                    fill=theme.mix(theme.GOOD, theme.BAD, abs(value)))
                self._pct.configure(text=f"{value * 100:+.0f} %")
            else:
                self._draw_fill(0, value * self._cw)
                self._pct.configure(text=f"{value * 100:.0f} %")
                self._pct.configure(fg=theme.TEXT if value > 0.002
                                    else theme.MUTED)

        if self._raw is not None and raw != self._raw_value:
            self._raw_value = raw
            self._raw.configure(text="" if raw is None else f"{raw:>4d}")

    def set_color(self, color: str) -> None:
        self._color = color
        if not self._bipolar:
            self._canvas.itemconfigure(self._fill, fill=color)


class ButtonGrid(tk.Frame):
    """Siatka 32 diod pokazujaca wcisniete przyciski."""

    COLS = 8
    CELL_H = 21
    GAP = 4
    RADIUS = 5

    def __init__(self, master: tk.Misc, fonts: Fonts, count: int = 32) -> None:
        super().__init__(master, bg=theme.PANEL)
        self._count = count
        self._state = [False] * count
        self._fill: List[str] = [theme.TRACK] * count
        self._rows = (count + self.COLS - 1) // self.COLS
        height = self._rows * self.CELL_H + (self._rows - 1) * self.GAP
        self._canvas = tk.Canvas(self, bg=theme.PANEL, highlightthickness=0,
                                 bd=0, height=height, width=self.COLS * 44)
        self._canvas.pack(fill="x", expand=True)
        self._cells: List[int] = []
        self._texts: List[int] = []
        self._widths: List[int] = [1] * count
        # obrazki kafelkow w biezacym rozmiarze; nowy uklad zaczyna od zera,
        # wiec przeciaganie okna nie zbiera obrazkow wszystkich szerokosci
        self._photos: dict = {}
        for i in range(count):
            self._cells.append(self._canvas.create_image(0, 0, anchor="nw"))
            self._texts.append(self._canvas.create_text(
                0, 0, text=str(i), font=fonts.mono_small, fill=theme.MUTED))
        self._cw = self.COLS * 44
        self._layout()
        self._canvas.bind("<Configure>", self._on_resize)

    def _on_resize(self, event) -> None:
        if abs(event.width - self._cw) < 2:
            return
        self._cw = max(self.COLS * 20, event.width)
        self._layout()

    def _paint(self, i: int) -> None:
        key = (self._widths[i], self._fill[i])
        photo = self._photos.get(key)
        if photo is None:
            photo = self._photos[key] = art.photo(
                rrect_image(key[0], self.CELL_H, self.RADIUS, key[1]), self)
        self._canvas.itemconfigure(self._cells[i], image=photo)

    def _layout(self) -> None:
        # Cale piksele: kafelek to obrazek, a jego krawedzie maja lezec
        # rowno na siatce ekranu.
        step = (self._cw - self.GAP * (self.COLS - 1)) / self.COLS
        self._photos = {}
        for i in range(self._count):
            col, row = i % self.COLS, i // self.COLS
            x1 = int(round(col * (step + self.GAP)))
            x2 = int(round(col * (step + self.GAP) + step))
            y1 = row * (self.CELL_H + self.GAP)
            self._widths[i] = max(4, x2 - x1)
            self._canvas.coords(self._cells[i], x1, y1)
            self._canvas.coords(self._texts[i], (x1 + x2) / 2,
                                y1 + self.CELL_H / 2)
            self._paint(i)

    def set_mask(self, mask: int) -> None:
        for i in range(self._count):
            on = bool((mask >> i) & 1)
            if on != self._state[i]:
                self._state[i] = on
                self._fill[i] = theme.ACCENT if on else theme.TRACK
                self._paint(i)
                self._canvas.itemconfigure(
                    self._texts[i],
                    fill=theme.ACCENT_INK if on else theme.MUTED)

    def set_highlight(self, indices: Iterable[int], color: str) -> None:
        for i in indices:
            if 0 <= i < self._count and not self._state[i]:
                self._fill[i] = color
                self._paint(i)
                self._canvas.itemconfigure(self._texts[i], fill=theme.TEXT)


class HatIndicator(tk.Canvas):
    """Krzyzak - cztery strzalki na okraglej podstawie.

    Piec gotowych obrazkow (srodek i cztery kierunki); zmiana kierunku to
    tylko podmiana obrazka.
    """

    def __init__(self, master: tk.Misc, size: int = 74) -> None:
        super().__init__(master, width=size, height=size, bg=theme.PANEL,
                         highlightthickness=0, bd=0)
        self._size = size
        self._item = self.create_image(0, 0, anchor="nw",
                                       image=self._photo(-1))
        self._active = -1

    def _photo(self, direction: int) -> object:
        s = self._size

        def render() -> Image.Image:
            c = s / 2
            g = s * 0.12            # promien srodka
            b = s * 0.17            # podstawa strzalki od srodka
            w = s * 0.13            # polowa szerokosci podstawy - mniej niz b,
                                    # wiec sasiednie strzalki sie nie stykaja
            o = s * 0.42            # czubek strzalki od srodka
            p = art.Paint(s, s)
            p.fill(("circle", c, c, c - 2), theme.darken(theme.TRACK, 0.35))
            arrows = (((c, c - o), (c - w, c - b), (c + w, c - b)),
                      ((c + o, c), (c + b, c - w), (c + b, c + w)),
                      ((c, c + o), (c - w, c + b), (c + w, c + b)),
                      ((c - o, c), (c - b, c - w), (c - b, c + w)))
            for i, pts in enumerate(arrows):
                on = i == direction
                if on:
                    p.glow(("poly", pts), theme.ACCENT, radius=4, alpha=120)
                p.fill(("poly", pts), theme.ACCENT if on else theme.TRACK)
            p.fill(("circle", c, c, g * 0.75), theme.BORDER)
            p.fill(("circle", c, c, g * 0.75 - 1), theme.PANEL_HI)
            return art.opaque(p.image(), theme.PANEL).convert("RGB")

        return cached_photo(self, ("hat", s, direction), render)

    def set(self, direction: int) -> None:
        if direction == self._active:
            return
        self._active = direction
        self.itemconfigure(self._item,
                           image=self._photo(direction if 0 <= direction < 4
                                             else -1))


class Sparkline(tk.Canvas):
    """Maly wykres historii - uzywany do ramek/s i opoznienia."""

    def __init__(self, master: tk.Misc, width: int = 190, height: int = 38,
                 color: str = theme.ACCENT, points: int = 90) -> None:
        super().__init__(master, width=width, height=height, bg=theme.PANEL_HI,
                         highlightthickness=0, bd=0)
        self._cw, self._ch = width, height
        self._n = points
        self._color = color
        self._data: List[float] = []
        self._grid = [self.create_line(0, height * f, width, height * f,
                                       fill=theme.mix(theme.PANEL_HI,
                                                      theme.BORDER, 0.8),
                                       dash=(2, 4))
                      for f in (0.33, 0.66)]
        self._area = self.create_polygon(0, height, width, height,
                                         fill=theme.mix(theme.PANEL_HI, color,
                                                        0.22),
                                         outline="", smooth=True)
        self._line = self.create_line(0, height, width, height, fill=color,
                                      width=1.8, smooth=True)
        self._base = self.create_line(0, height - 1, width, height - 1,
                                      fill=theme.BORDER, width=1)
        self.bind("<Configure>", self._on_resize)

    def _on_resize(self, event) -> None:
        self._cw = max(20, event.width)
        for i, f in enumerate((0.33, 0.66)):
            self.coords(self._grid[i], 0, self._ch * f, self._cw, self._ch * f)
        self.coords(self._base, 0, self._ch - 1, self._cw, self._ch - 1)

    def push(self, value: float, scale_max: Optional[float] = None) -> None:
        self._data.append(max(0.0, float(value)))
        if len(self._data) > self._n:
            del self._data[:len(self._data) - self._n]
        if len(self._data) < 2:
            return
        top = scale_max if scale_max else max(self._data)
        top = max(top, 1e-6)
        step = self._cw / (self._n - 1)
        offset = self._n - len(self._data)
        coords: List[float] = []
        for i, v in enumerate(self._data):
            x = (offset + i) * step
            y = self._ch - 2 - (min(v, top) / top) * (self._ch - 6)
            coords += [x, y]
        self.coords(self._line, *coords)
        self.coords(self._area, coords[0], self._ch, *coords,
                    coords[-2], self._ch)


class TorqueMeter(tk.Frame):
    """Dwukierunkowy wskaznik momentu FFB."""

    def __init__(self, master: tk.Misc, fonts: Fonts, width: int = 300,
                 height: int = 26) -> None:
        super().__init__(master, bg=theme.PANEL)
        self._cw, self._ch = width, height
        self._value = 0.0
        self._radius = height / 2.0
        self._canvas = tk.Canvas(self, width=width, height=height, bg=theme.PANEL,
                                 highlightthickness=0, bd=0)
        self._canvas.pack(fill="x")
        self._track = round_rect(self._canvas, 0, 0, width, height,
                                 self._radius, fill=theme.TRACK, outline="")
        self._fill = round_rect(self._canvas, width / 2, 2, width / 2,
                                height - 2, self._radius - 2,
                                fill=theme.VIOLET, outline="")
        self._tick_at = (0.2, 0.35, 0.65, 0.8)
        self._ticks = [self._canvas.create_line(0, height * 0.3, 0,
                                                height * 0.7,
                                                fill=theme.BORDER_HI, width=1)
                       for _ in self._tick_at]
        self._mid = self._canvas.create_line(width / 2, 0, width / 2, height,
                                             fill=theme.BORDER_HI, width=1)
        self._label = self._canvas.create_text(width / 2, height / 2, text="0 %",
                                               font=fonts.small_bold,
                                               fill=theme.TEXT_DIM)
        self._layout()
        self._canvas.bind("<Configure>", self._on_resize)

    def _layout(self) -> None:
        w, h = self._cw, self._ch
        set_round_rect(self._canvas, self._track, 0, 0, w, h, self._radius)
        for frac, tick in zip(self._tick_at, self._ticks):
            x = w * frac
            self._canvas.coords(tick, x, h * 0.3, x, h * 0.7)
        self._canvas.coords(self._mid, w / 2, 0, w / 2, h)
        self._canvas.coords(self._label, w / 2, h / 2)

    def _on_resize(self, event) -> None:
        self._cw = max(20, event.width)
        self._layout()
        v, self._value = self._value, -2.0
        self.set(v)

    def set(self, value: float) -> None:
        value = 0.0 if value != value else max(-1.0, min(1.0, value))
        if abs(value - self._value) < 0.002:
            return
        self._value = value
        mid = self._cw / 2
        r = max(1.0, self._radius - 2)
        if abs(value) * mid < 2:
            self._canvas.itemconfigure(self._fill, state="hidden")
        else:
            self._canvas.itemconfigure(self._fill, state="normal")
            if value >= 0:
                set_round_rect(self._canvas, self._fill, mid, 2,
                               mid + value * mid, self._ch - 2, r)
            else:
                set_round_rect(self._canvas, self._fill, mid + value * mid, 2,
                               mid, self._ch - 2, r)
        self._canvas.itemconfigure(self._fill,
                                   fill=theme.mix(theme.VIOLET, theme.BAD,
                                                  max(0.0, abs(value) - 0.6) / 0.4))
        self._canvas.itemconfigure(self._label, text=f"{value * 100:+.0f} %",
                                   fill=theme.TEXT if abs(value) > 0.02
                                   else theme.MUTED)


class LabeledSlider(tk.Frame):
    """Suwak z nazwa i aktualna wartoscia; callback przy kazdej zmianie.

    Sam suwak jest rysowany na plotnie, a nie wziety z ttk: motyw "clam"
    maluje galke i tlo widgetu tym samym kolorem, wiec kolorowa galka
    zawsze ciagnela za soba kolorowa ramke wokol calego rowka.
    """

    H = 22                      # wysokosc plotna suwaka
    TRACK_H = 6
    KNOB_R = 8

    def __init__(self, master: tk.Misc, fonts: Fonts, label: str,
                 from_: float, to: float, value: float,
                 command=None, fmt: str = "{:.2f}", resolution: float = 0.0,
                 width: int = 200, hint: str = "") -> None:
        super().__init__(master, bg=theme.PANEL)
        self._fmt = fmt
        self._command = command
        self._resolution = resolution
        self._updating = False
        self._from = float(from_)
        self._to = float(to)
        self._cw = max(60, width)
        self._dragging = False

        head = tk.Frame(self, bg=theme.PANEL)
        head.pack(fill="x")
        tk.Label(head, text=label, bg=theme.PANEL, fg=theme.TEXT_DIM,
                 font=fonts.small).pack(side="left")
        self._value_label = tk.Label(head, text=fmt.format(value),
                                     bg=theme.mix(theme.PANEL, theme.ACCENT,
                                                  0.14),
                                     fg=theme.ACCENT, font=fonts.small_bold,
                                     padx=7, pady=1)
        self._value_label.pack(side="right")

        self.var = tk.DoubleVar(value=value)
        cy = self.H / 2
        self._canvas = tk.Canvas(self, width=width, height=self.H,
                                 bg=theme.PANEL, highlightthickness=0, bd=0,
                                 cursor="hand2")
        self._canvas.pack(fill="x", pady=(4, 0))
        r = self.TRACK_H / 2
        self._track = round_rect(self._canvas, 0, cy - r, width, cy + r, r,
                                 fill=theme.TRACK, outline="")
        self._fill = round_rect(self._canvas, 0, cy - r, 0, cy + r, r,
                                fill=theme.ACCENT, outline="")
        self._knob = self._canvas.create_image(0, cy,
                                               image=self._knob_photo(False))
        for seq, handler in (("<Button-1>", self._on_press),
                             ("<B1-Motion>", self._on_drag),
                             ("<ButtonRelease-1>", self._on_release),
                             ("<Configure>", self._on_resize),
                             ("<Enter>", self._on_enter),
                             ("<Leave>", self._on_leave)):
            self._canvas.bind(seq, handler)

        self._hint = None
        if hint:
            self._hint = tk.Label(self, text=hint, bg=theme.PANEL,
                                  fg=theme.MUTED, font=fonts.tiny,
                                  wraplength=width, justify="left", anchor="w")
            self._hint.pack(fill="x", pady=(3, 0))
        self._redraw()

    # --------------------------------------------------------- przeliczenia

    def _span(self) -> Tuple[float, float]:
        """Lewy i prawy skrajny srodek galki."""
        return self.KNOB_R + 1, max(self.KNOB_R + 2, self._cw - self.KNOB_R - 1)

    def _snap(self, value: float) -> float:
        lo, hi = min(self._from, self._to), max(self._from, self._to)
        value = max(lo, min(hi, value))
        if self._resolution:
            value = round(value / self._resolution) * self._resolution
            value = max(lo, min(hi, value))
        return value

    def _value_at(self, x: float) -> float:
        x0, x1 = self._span()
        t = (x - x0) / max(1.0, x1 - x0)
        t = 0.0 if t < 0 else (1.0 if t > 1 else t)
        return self._snap(self._from + (self._to - self._from) * t)

    def _x_of(self, value: float) -> float:
        x0, x1 = self._span()
        rng = self._to - self._from
        t = 0.0 if abs(rng) < 1e-9 else (value - self._from) / rng
        t = 0.0 if t < 0 else (1.0 if t > 1 else t)
        return x0 + (x1 - x0) * t

    # ------------------------------------------------------------- rysunek

    def _redraw(self) -> None:
        cy = self.H / 2
        r = self.TRACK_H / 2
        x = self._x_of(self.var.get())
        set_round_rect(self._canvas, self._track, 0, cy - r, self._cw, cy + r, r)
        if x <= r:
            self._canvas.itemconfigure(self._fill, state="hidden")
        else:
            self._canvas.itemconfigure(self._fill, state="normal")
            set_round_rect(self._canvas, self._fill, 0, cy - r, x, cy + r, r)
        self._canvas.coords(self._knob, x, cy)

    def _on_resize(self, event) -> None:
        self._cw = max(40, event.width)
        self._redraw()
        if self._hint is not None:
            # podpowiedz konczy sie tam, gdzie rowek suwaka
            configure_if_changed(self._hint, wraplength=self._cw)

    def _knob_photo(self, glow: bool) -> object:
        """Galka: jasne kolko w obwodce akcentu, pod kursorem z poswiata."""
        k = self.KNOB_R

        def render() -> Image.Image:
            size = 2 * (k + 5)
            c = size / 2
            p = art.Paint(size, size)
            if glow:
                p.fill(("circle", c, c, k + 4), theme.ACCENT, alpha=64)
            p.fill(("circle", c, c, k), theme.ACCENT)
            p.fill(("circle", c, c, k - 2), theme.TEXT)
            return p.image()

        return cached_photo(self, ("knob", k, glow), render)

    def _on_enter(self, _event) -> None:
        self._canvas.itemconfigure(self._knob, image=self._knob_photo(True))

    def _on_leave(self, _event) -> None:
        if not self._dragging:
            self._canvas.itemconfigure(self._knob,
                                       image=self._knob_photo(False))

    # ------------------------------------------------------------ zdarzenia

    def _apply(self, value: float) -> None:
        if abs(value - self.var.get()) < 1e-9:
            return
        self.var.set(value)
        self._value_label.configure(text=self._fmt.format(value))
        self._redraw()
        if self._command:
            self._command(value)

    def _on_press(self, event) -> None:
        self._dragging = True
        self._canvas.focus_set()
        self._apply(self._value_at(event.x))

    def _on_drag(self, event) -> None:
        if self._dragging:
            self._apply(self._value_at(event.x))

    def _on_release(self, _event) -> None:
        self._dragging = False
        self._canvas.itemconfigure(self._knob, image=self._knob_photo(False))

    # ----------------------------------------------------------------- API

    def get(self) -> float:
        return self._snap(self.var.get())

    def set(self, value: float) -> None:
        self._updating = True
        try:
            self.var.set(value)
            self._value_label.configure(text=self._fmt.format(value))
            self._redraw()
        finally:
            self._updating = False


class CurvePlot(tk.Canvas):
    """Wykres krzywej odpowiedzi (wejscie -> wyjscie) z kropka pozycji.

    Wykres jest obrazkiem z Pillow (wygladzona linia, wypelnienie pod
    krzywa, zacienione martwe strefy) i powstaje od nowa tylko wtedy, gdy
    zmienia sie krzywa albo szerokosc. Kropka biezacej pozycji to osobny
    maly obrazek przesuwany po plotnie - ruch pedalu nic nie przerysowuje.

    Wspolrzedne funkcji: x i y w zakresie 0..1, a w trybie bipolar -1..1.
    """

    PAD = 1                     # obwodka wokol pola wykresu
    INSET = 5                   # krzywa nie dotyka krawedzi pola
    CAPTION_H = 18              # pas na podpisy pod wykresem
    SAMPLES = 120
    SS = 2                      # powiekszenie przy rysowaniu - wystarcza,
                                # a wykres przerysowuje sie przy suwaku

    def __init__(self, master: tk.Misc, fonts: Fonts, height: int = 112,
                 color: str = theme.ACCENT, bipolar: bool = False,
                 captions: Sequence[str] = ("", "", "")) -> None:
        super().__init__(master, height=height + self.CAPTION_H, width=200,
                         bg=theme.PANEL, highlightthickness=0, bd=0)
        self._ph = height
        self._cw = 200
        self._color = color
        self._bipolar = bipolar
        self._fn: Optional[Callable[[float], float]] = None
        self._shade: Tuple[Tuple[float, float], ...] = ()
        self._key: object = None
        self._drawn: object = None
        self._marker_xy: Optional[Tuple[float, float]] = None
        self._marker_at: Optional[Tuple[int, int]] = None
        self._resize_job: Optional[str] = None

        self._photo = None
        self._image = self.create_image(0, 0, anchor="nw")
        self._marker = self.create_image(0, 0, state="hidden",
                                         image=self._marker_photo())
        y = height + self.CAPTION_H / 2 + 1
        self._captions = [
            self.create_text(1, y, anchor="w", font=fonts.tiny,
                             fill=theme.MUTED),
            self.create_text(100, y, anchor="center", font=fonts.tiny,
                             fill=theme.MUTED),
            self.create_text(199, y, anchor="e", font=fonts.tiny,
                             fill=theme.MUTED)]
        self.set_captions(*captions)
        self.bind("<Configure>", self._on_resize)

    # ------------------------------------------------------------- uklad

    RESIZE_DELAY_MS = 80        # przerysowanie po ustaniu zmian rozmiaru

    def _on_resize(self, event) -> None:
        width = max(80, event.width)
        if width == self._cw and self._drawn is not None:
            return
        self._cw = width
        y = self._ph + self.CAPTION_H / 2 + 1
        self.coords(self._captions[0], 1, y)
        self.coords(self._captions[1], width / 2, y)
        self.coords(self._captions[2], width - 1, y)
        # kropka stoi we wspolrzednych funkcji - po zmianie szerokosci
        # trzeba ja przeliczyc na nowe piksele
        if self._marker_xy is not None:
            self._marker_at = None
            self.set_marker(*self._marker_xy)
        # Wykres kosztuje ~12 ms, a przeciaganie krawedzi okna daje dziesiatki
        # zdarzen na sekunde dla kazdego wykresu (takze na ukrytych
        # zakladkach) - rysujemy raz, gdy rozmiar przestanie sie zmieniac.
        if self._resize_job is not None:
            self.after_cancel(self._resize_job)
        self._resize_job = self.after(self.RESIZE_DELAY_MS, self._resized)

    def _resized(self) -> None:
        self._resize_job = None
        self._render()

    def _to_px(self, x: float, y: float) -> Tuple[float, float]:
        """Wspolrzedne funkcji -> piksele wewnatrz pola wykresu."""
        lo = -1.0 if self._bipolar else 0.0
        w = self._cw - 2 * self.INSET
        h = self._ph - 2 * self.INSET
        tx = (x - lo) / (1.0 - lo)
        ty = (y - lo) / (1.0 - lo)
        return self.INSET + tx * w, self.INSET + (1.0 - ty) * h

    # ----------------------------------------------------------- rysunek

    def _marker_photo(self) -> object:
        color = self._color

        def render() -> Image.Image:
            p = art.Paint(18, 18)
            p.fill(("circle", 9, 9, 8), color, alpha=70)
            p.fill(("circle", 9, 9, 4.5), color)
            p.fill(("circle", 9, 9, 3.0), theme.TEXT)
            return p.image()

        return cached_photo(self, ("plot-marker", color), render)

    def _render(self) -> None:
        if self._fn is None:
            return
        w, h = self._cw, self._ph
        lo = -1.0 if self._bipolar else 0.0
        pad = self.PAD
        p = art.Paint(w, h, ss=self.SS)
        p.fill(("rrect", 0, 0, w, h, 7), theme.BORDER)
        p.fill(("rrect", pad, pad, w - pad, h - pad, 6), theme.PANEL_HI)

        # zacienione martwe strefy - pas na cala wysokosc pola
        for a, b in self._shade:
            if b - a < 0.002:
                continue
            x1 = max(pad + 1.0, self._to_px(a, 0)[0])
            x2 = min(w - pad - 1.0, self._to_px(b, 0)[0])
            if x2 - x1 >= 1.0:
                p.fill(("rrect", x1, pad + 1, x2, h - pad - 1, 3),
                       theme.mix(theme.PANEL_HI, theme.BAD, 0.12))

        grid = theme.mix(theme.PANEL_HI, theme.BORDER_HI, 0.55)
        for f in (0.25, 0.5, 0.75):
            v = lo + (1.0 - lo) * f
            x = self._to_px(v, 0)[0]
            y = self._to_px(0, v)[1]
            color = theme.BORDER_HI if self._bipolar and f == 0.5 else grid
            p.line(((x, pad + 2), (x, h - pad - 2)), color, 1.0,
                   round_caps=False)
            p.line(((pad + 2, y), (w - pad - 2, y)), color, 1.0,
                   round_caps=False)
        # przekatna - tak wyglada odpowiedz liniowa
        p.line((self._to_px(lo, lo), self._to_px(1.0, 1.0)),
               theme.mix(theme.PANEL_HI, theme.TEXT_DIM, 0.30), 1.0)

        pts = []
        for i in range(self.SAMPLES + 1):
            x = lo + (1.0 - lo) * i / self.SAMPLES
            pts.append(self._to_px(x, self._fn(x)))
        base_y = self._to_px(0, 0)[1]
        p.fill(("poly", ((pts[0][0], base_y),) + tuple(pts)
                + ((pts[-1][0], base_y),)), self._color, alpha=38)
        p.line(pts, self._color, 2.2)

        img = art.opaque(p.image(), theme.PANEL).convert("RGB")
        if self._photo is not None and (self._photo.width(),
                                        self._photo.height()) == img.size:
            self._photo.paste(img)
        else:
            self._photo = art.photo(img, self)
            self.itemconfigure(self._image, image=self._photo)
        self._drawn = (self._key, w)
        self.tag_raise(self._marker)

    # ----------------------------------------------------------------- API

    def set_curve(self, key: object, fn: Callable[[float], float],
                  shade: Sequence[Tuple[float, float]] = ()) -> None:
        """Nowa krzywa. key opisuje jej parametry - ten sam key nic nie
        przerysowuje, wiec mozna wolac co klatke."""
        self._fn = fn
        self._shade = tuple(shade)
        if key == self._key and self._drawn is not None:
            return
        self._key = key
        self._render()

    def set_marker(self, x: Optional[float], y: Optional[float] = None) -> None:
        """Kropka w punkcie (x, y) albo ukryta, gdy x lub y jest None."""
        if x is None or y is None:
            self._marker_xy = None
            if self._marker_at is not None:
                self._marker_at = None
                self.itemconfigure(self._marker, state="hidden")
            return
        lo = -1.0 if self._bipolar else 0.0
        x = min(1.0, max(lo, x))
        y = min(1.0, max(lo, y))
        self._marker_xy = (x, y)
        px, py = self._to_px(x, y)
        at = (int(round(px)), int(round(py)))
        if at == self._marker_at:
            return
        if self._marker_at is None:
            self.itemconfigure(self._marker, state="normal")
        self._marker_at = at
        self.coords(self._marker, *at)

    def set_captions(self, left: str, middle: str, right: str) -> None:
        for item, text in zip(self._captions, (left, middle, right)):
            if self.itemcget(item, "text") != text:
                self.itemconfigure(item, text=text)


class PedalGauge(tk.Frame):
    """Pionowy wskaznik pedalu na pulpicie: nazwa, slupek, procent.

    Slupek to zwykly prostokat Tk przykryty wygladzona "maska" z
    zaokraglonymi rogami - wypelnienie zmienia sie co klatke tylko przez
    coords, a krawedzie i tak sa gladkie.
    """

    BAR_W = 44
    RADIUS = 9

    def __init__(self, master: tk.Misc, fonts: Fonts, label: str,
                 color: str = theme.ACCENT, height: int = 140) -> None:
        super().__init__(master, bg=theme.PANEL)
        self._bh = height
        self._value = -1.0
        self._enabled: Optional[bool] = None
        self._raw_value: Optional[int] = None

        self._name = tk.Label(self, text=label, bg=theme.PANEL,
                              fg=theme.TEXT_DIM, font=fonts.small_bold)
        self._name.pack()

        w, h = self.BAR_W, height
        self._canvas = tk.Canvas(self, width=w, height=h, bg=theme.PANEL,
                                 highlightthickness=0, bd=0)
        self._canvas.pack(pady=(6, 4))
        self._canvas.create_rectangle(0, 0, w, h, fill=theme.TRACK, width=0)
        tick = theme.mix(theme.TRACK, theme.BORDER_HI, 0.55)
        for f in (0.25, 0.5, 0.75):
            y = round(h * (1.0 - f))
            self._canvas.create_line(8, y, w - 8, y, fill=tick)
        self._fill = self._canvas.create_rectangle(0, h, w, h, fill=color,
                                                   width=0, state="hidden")
        self._canvas.create_image(0, 0, anchor="nw", image=self._mask_photo())

        # Stala szerokosc - "5 %" i "100 %" maja rozna dlugosc, a zmiana
        # rozmiaru napisu przeliczalaby uklad calej karty.
        self._pct = tk.Label(self, text="0 %", bg=theme.PANEL, fg=theme.MUTED,
                             font=fonts.heading, width=6)
        self._pct.pack()
        self._raw = tk.Label(self, text="", bg=theme.PANEL, fg=theme.MUTED,
                             font=fonts.mono_small)
        self._raw.pack()
        self.set(0.0, None, True)

    def _mask_photo(self) -> object:
        """Tlo karty wszedzie poza zaokraglonym slupkiem."""
        w, h, r = self.BAR_W, self._bh, self.RADIUS

        def render() -> Image.Image:
            p = art.Paint(w, h)
            hole = p.mask(("rrect", 0, 0, w, h, r)).reduce(p.ss)
            img = Image.new("RGBA", (w, h), art.rgba(theme.PANEL))
            img.putalpha(ImageChops.invert(hole))
            return img

        return cached_photo(self, ("gauge-mask", w, h, r), render)

    def set(self, value: float, raw: Optional[int] = None,
            enabled: bool = True) -> None:
        if value != value:
            value = 0.0
        value = max(0.0, min(1.0, value)) if enabled else 0.0
        if enabled != self._enabled:
            self._enabled = enabled
            self._name.configure(fg=theme.TEXT_DIM if enabled else theme.MUTED)
            self._value = -1.0                      # wymus odswiezenie
        if abs(value - self._value) > 0.0005:
            self._value = value
            h = self._bh
            top = int(round(h * (1.0 - value)))
            if h - top < 1:
                self._canvas.itemconfigure(self._fill, state="hidden")
            else:
                self._canvas.itemconfigure(self._fill, state="normal")
                self._canvas.coords(self._fill, 0, top, self.BAR_W, h)
            if enabled:
                self._pct.configure(text=f"{value * 100:.0f} %",
                                    fg=theme.TEXT if value > 0.002
                                    else theme.MUTED)
            else:
                self._pct.configure(text="wył.", fg=theme.MUTED)
        raw = raw if enabled else None
        if raw != self._raw_value:
            self._raw_value = raw
            self._raw.configure(text="" if raw is None else str(raw))


class PopupMenu:
    """Ciemne menu podreczne w kolorach panelu.

    tk.Menu na Windowsie jest systemowe: ignoruje kolory i wyskakiwalo jako
    jasna lista w ciemnym oknie. Tu jest zwykle okno bez ramki z etykietami.
    Lokalny grab kieruje do niego klikniecia z calej aplikacji - klik poza
    menu je zamyka (i nic wiecej nie robi), Esc tez.
    """

    def __init__(self, master: tk.Misc, fonts: Fonts,
                 title: str = "") -> None:
        self._master = master
        self._fonts = fonts
        self._title = title
        self._items: List[Tuple[str, object]] = []
        self.window: Optional[tk.Toplevel] = None

    def add(self, label: str, command: Optional[Callable[[], None]] = None,
            checked: bool = False, enabled: bool = True,
            note: str = "") -> None:
        self._items.append(("item", (label, command, checked, enabled, note)))

    def add_separator(self) -> None:
        self._items.append(("sep", None))

    # ------------------------------------------------------------- pokaz

    def popup(self, x_root: int, y_root: int) -> None:
        win = tk.Toplevel(self._master, bg=theme.BORDER_HI, bd=0,
                          highlightthickness=0)
        self.window = win
        win.withdraw()
        win.overrideredirect(True)
        try:
            win.transient(self._master.winfo_toplevel())
        except tk.TclError:
            pass
        body = tk.Frame(win, bg=theme.PANEL_HI)
        body.pack(fill="both", expand=True, padx=1, pady=1)
        if self._title:
            tk.Label(body, text=self._title, bg=theme.PANEL_HI,
                     fg=theme.MUTED, font=self._fonts.tiny_bold, anchor="w",
                     padx=12, pady=6).pack(fill="x")
            tk.Frame(body, bg=theme.BORDER, height=1).pack(fill="x")
        for kind, data in self._items:
            if kind == "sep":
                tk.Frame(body, bg=theme.BORDER, height=1).pack(fill="x",
                                                               pady=3)
                continue
            self._build_item(body, *data)

        win.update_idletasks()
        w, h = win.winfo_reqwidth(), win.winfo_reqheight()
        # menu zostaje na ekranie, takze przy kliknieciu przy krawedzi
        sw, sh = win.winfo_screenwidth(), win.winfo_screenheight()
        x = max(0, min(x_root, sw - w - 4))
        y = max(0, min(y_root, sh - h - 4))
        win.geometry(f"+{x}+{y}")
        win.deiconify()
        win.lift()
        win.bind("<ButtonPress>", self._on_press)
        win.bind("<Escape>", lambda _e: self.close())
        try:
            win.focus_force()
            win.grab_set()
        except tk.TclError:
            pass                        # inne okno trzyma grab - menu i tak dziala

    def _build_item(self, parent: tk.Misc, label: str,
                    command: Optional[Callable[[], None]], checked: bool,
                    enabled: bool, note: str) -> None:
        fg = theme.TEXT if enabled else theme.MUTED
        row = tk.Frame(parent, bg=theme.PANEL_HI,
                       cursor="hand2" if enabled else "")
        row.pack(fill="x")
        mark = tk.Label(row, text="✓" if checked else "", width=2,
                        bg=theme.PANEL_HI, fg=theme.ACCENT,
                        font=self._fonts.small_bold)
        mark.pack(side="left", padx=(6, 0))
        text = tk.Label(row, text=label, bg=theme.PANEL_HI, fg=fg,
                        font=self._fonts.small_bold if checked
                        else self._fonts.small, anchor="w", pady=5)
        text.pack(side="left", fill="x", expand=True, padx=(0, 6))
        widgets = [row, mark, text]
        if note:
            extra = tk.Label(row, text=note, bg=theme.PANEL_HI,
                             fg=theme.WARN if enabled else theme.MUTED,
                             font=self._fonts.tiny)
            extra.pack(side="right", padx=(8, 12))
            widgets.append(extra)
        else:
            tk.Frame(row, bg=theme.PANEL_HI, width=12).pack(side="right")
        if not enabled or command is None:
            return

        def paint(on: bool) -> None:
            bg = theme.ACCENT_DEEP if on else theme.PANEL_HI
            for w in widgets:
                w.configure(bg=bg)

        for w in widgets:
            w.bind("<Enter>", lambda _e: paint(True))
            w.bind("<Leave>", lambda _e: paint(False))
            w.bind("<ButtonRelease-1>", lambda _e, c=command: self._choose(c))

    # ---------------------------------------------------------- zamkniecie

    def _inside(self, x_root: int, y_root: int) -> bool:
        win = self.window
        if win is None:
            return False
        x, y = win.winfo_rootx(), win.winfo_rooty()
        return (x <= x_root < x + win.winfo_width()
                and y <= y_root < y + win.winfo_height())

    def _on_press(self, event) -> None:
        if not self._inside(event.x_root, event.y_root):
            self.close()

    def _choose(self, command: Callable[[], None]) -> None:
        self.close()
        command()

    def close(self) -> None:
        win, self.window = self.window, None
        if win is None:
            return
        try:
            win.grab_release()
            win.destroy()
        except tk.TclError:
            pass

    @property
    def is_open(self) -> bool:
        return self.window is not None


class StatRow(tk.Frame):
    """Wiersz 'nazwa .... wartosc' dla paneli statystyk."""

    def __init__(self, master: tk.Misc, fonts: Fonts, label: str,
                 value: str = "-", value_color: str = theme.TEXT) -> None:
        super().__init__(master, bg=theme.PANEL)
        tk.Label(self, text=label, bg=theme.PANEL, fg=theme.MUTED,
                 font=fonts.small, anchor="w").pack(side="left")
        self._value = tk.Label(self, text=value, bg=theme.PANEL, fg=value_color,
                               font=fonts.mono_bold, anchor="e")
        self._value.pack(side="right")

    def set(self, value: str, color: Optional[str] = None) -> None:
        if self._value.cget("text") != value:
            self._value.configure(text=value)
        if color is not None and self._value.cget("fg") != color:
            self._value.configure(fg=color)


class ScrollableFrame(ttk.Frame):
    """Pionowo przewijany kontener - uzywany w zakladkach z wieloma polami.

    Waski pasek przewijania pojawia sie tylko wtedy, gdy tresc sie nie
    miesci. Lezy NAD trescia (place), a tresc ma stale miejsce na niego -
    pokazanie paska nie zmienia szerokosci strony, wiec uklad nie skacze.
    """

    BAR_W = 8                   # szerokosc paska
    GUTTER = 14                 # miejsce na pasek po prawej stronie tresci

    def __init__(self, master: tk.Misc, **kw) -> None:
        super().__init__(master, **kw)
        self._canvas = tk.Canvas(self, bg=theme.BG, highlightthickness=0, bd=0,
                                 yscrollincrement=24)
        self._canvas.pack(fill="both", expand=True)
        self._bar = ttk.Scrollbar(self, orient="vertical",
                                  style="Slim.Vertical.TScrollbar",
                                  command=self._canvas.yview)
        self._bar_shown = False
        self._wheel_acc = 0.0
        self._canvas.configure(yscrollcommand=self._on_scroll)
        self._canvas.scrollable_frame = self    # dla wspolnej obslugi kolka

        self.inner = ttk.Frame(self._canvas, style="Root.TFrame")
        self._win = self._canvas.create_window((0, 0), window=self.inner,
                                               anchor="nw")
        self.inner.bind("<Configure>", self._on_inner)
        self._canvas.bind("<Configure>", self._on_canvas)
        _install_wheel(self)

    def _on_inner(self, _event) -> None:
        self._canvas.configure(scrollregion=self._canvas.bbox("all"))

    def _on_canvas(self, event) -> None:
        self._canvas.itemconfigure(self._win,
                                   width=max(1, event.width - self.GUTTER))

    def _on_scroll(self, first: str, last: str) -> None:
        self._bar.set(first, last)
        need = float(first) > 0.0 or float(last) < 1.0
        if need and not self._bar_shown:
            self._bar.place(relx=1.0, x=-1, y=0, relheight=1.0,
                            width=self.BAR_W, anchor="ne")
            self._bar_shown = True
        elif not need and self._bar_shown:
            self._bar.place_forget()
            self._bar_shown = False

    def scroll_wheel(self, delta: int) -> None:
        first, last = self._canvas.yview()
        if first <= 0.0 and last >= 1.0:
            return
        # 120 = jeden zabek kolka = 3 kroki po 24 px. Gladzik wysyla
        # ulamki zabka - zbieramy je, zamiast gubic.
        self._wheel_acc -= delta / 40.0
        steps = int(self._wheel_acc)
        if steps:
            self._wheel_acc -= steps
            self._canvas.yview_scroll(steps, "units")


def _install_wheel(widget: tk.Misc) -> None:
    """Jedna obsluga kolka myszy dla calej aplikacji.

    Przewija strone pod kursorem - nie te, nad ktora kursor wszedl ostatnio
    (strony zakladek leza jedna nad druga). Pomija pola, ktore przewijaja
    sie same (dziennik, listy). Rozwiniete listy comboboxow tez.
    """
    root = widget.winfo_toplevel()
    if getattr(root, "_wheel_installed", False):
        return
    root._wheel_installed = True

    def on_wheel(event) -> None:
        try:
            target = root.winfo_containing(event.x_root, event.y_root)
        except (KeyError, tk.TclError):
            return
        while target is not None:
            if isinstance(target, (tk.Text, tk.Listbox)):
                return
            frame = getattr(target, "scrollable_frame", None)
            if frame is not None:
                frame.scroll_wheel(event.delta)
                return
            target = target.master

    root.bind_all("<MouseWheel>", on_wheel, add="+")
    # Kolko nad comboboxem zmienialo jego wartosc - przy przewijaniu
    # zakladki Przyciski latwo bylo przypadkiem zmienic przypisanie.
    root.unbind_class("TCombobox", "<MouseWheel>")
