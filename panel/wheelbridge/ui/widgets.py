"""Widgety rysowane na Canvas - wskaznik kierownicy, paski, diody."""

from __future__ import annotations

import math
import tkinter as tk
from tkinter import ttk
from typing import Callable, Iterable, List, Optional, Sequence, Tuple

from . import theme
from .theme import Fonts


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


# -------------------------------------------------------------------- karta

class Card(ttk.Frame):
    """Panel z naglowkiem, cienka ramka i linia pod tytulem."""

    def __init__(self, master: tk.Misc, title: str, fonts: Fonts,
                 subtitle: str = "", wrap: int = 360,
                 accent: str = theme.ACCENT, **kw) -> None:
        self.outer = tk.Frame(master, bg=theme.BORDER, bd=0, highlightthickness=0)
        super().__init__(self.outer, style="Card.TFrame",
                         padding=(15, 12, 15, 14), **kw)
        super().pack(fill="both", expand=True, padx=1, pady=1)

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
            ttk.Label(self.body, text=subtitle, style="Muted.TLabel",
                      wraplength=wrap, justify="left").pack(anchor="w",
                                                            pady=(0, 8))

    # Ukladamy zewnetrzna ramke, nie samego siebie.
    def place_card(self, **kw) -> "Card":
        self.outer.pack(**kw)
        return self

    def grid_card(self, **kw) -> "Card":
        self.outer.grid(**kw)
        return self


class TabBar(tk.Frame):
    """Wlasny pasek zakladek - podkreslenie zamiast ramek ttk."""

    def __init__(self, master: tk.Misc, fonts: Fonts, labels: Sequence[str],
                 command: Callable[[int], None]) -> None:
        super().__init__(master, bg=theme.BG)
        self._command = command
        self._selected = 0
        self._labels: List[tk.Label] = []
        self._marks: List[tk.Frame] = []
        self._hover = -1

        for index, text in enumerate(labels):
            item = tk.Frame(self, bg=theme.BG, cursor="hand2")
            item.pack(side="left")
            label = tk.Label(item, text=text, bg=theme.BG, fg=theme.MUTED,
                             font=fonts.small_bold, padx=17, pady=9,
                             cursor="hand2")
            label.pack(fill="x")
            mark = tk.Frame(item, bg=theme.BG, height=2)
            mark.pack(fill="x")
            for widget in (item, label):
                widget.bind("<Button-1>", lambda _e, i=index: self._click(i))
                widget.bind("<Enter>", lambda _e, i=index: self._set_hover(i))
                widget.bind("<Leave>", lambda _e, i=index: self._set_hover(-1))
            self._labels.append(label)
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
        for i, (label, mark) in enumerate(zip(self._labels, self._marks)):
            if i == self._selected:
                label.configure(fg=theme.TEXT)
                mark.configure(bg=theme.ACCENT)
            else:
                label.configure(fg=theme.TEXT_DIM if i == self._hover
                                else theme.MUTED)
                mark.configure(bg=theme.BG)


class StatusDot(tk.Canvas):
    """Kolorowa kropka stanu z delikatna poswiata."""

    def __init__(self, master: tk.Misc, size: int = 12, bg: str = theme.PANEL) -> None:
        super().__init__(master, width=size, height=size, bg=bg,
                         highlightthickness=0, bd=0)
        self._size = size
        pad = max(2, size // 5)
        self._glow = self.create_oval(0, 0, size, size, fill="", outline="")
        self._ring = self.create_oval(pad - 1, pad - 1, size - pad + 1,
                                      size - pad + 1, fill="", outline="")
        self._dot = self.create_oval(pad, pad, size - pad, size - pad,
                                     fill=theme.MUTED, outline="")

    def set(self, color: str, glow: bool = True) -> None:
        bg = self.cget("bg")
        self.itemconfigure(self._dot, fill=color)
        self.itemconfigure(self._glow,
                           fill=theme.mix(bg, color, 0.22) if glow else "")
        self.itemconfigure(self._ring,
                           outline=theme.mix(bg, color, 0.45) if glow else "")


class WheelDial(tk.Canvas):
    """Duzy wskaznik kata obrotu kierownicy z grafika kola."""

    def __init__(self, master: tk.Misc, fonts: Fonts, size: int = 250) -> None:
        super().__init__(master, width=size, height=size, bg=theme.PANEL,
                         highlightthickness=0, bd=0)
        self._fonts = fonts
        self._size = size
        self._range = 900.0
        self._deg = 0.0
        self._items_built = False
        self._build()

    # ------------------------------------------------------------------ build

    def _build(self) -> None:
        s = self._size
        cx = cy = s / 2
        ring_w = max(8, s // 20)
        r_out = s / 2 - 5
        r_in = r_out - ring_w

        # tlo pierscienia
        self.create_oval(cx - r_out, cy - r_out, cx + r_out, cy + r_out,
                         outline=theme.TRACK, width=ring_w, fill="")

        # luk postepu (rysowany od gory, start=90)
        self._arc = self.create_arc(cx - r_out, cy - r_out, cx + r_out, cy + r_out,
                                    start=90, extent=0, style="arc",
                                    outline=theme.ACCENT, width=ring_w)

        # znaczniki co 1/8 zakresu
        for i in range(-4, 5):
            frac = i / 4.0
            ang = math.radians(90 - frac * 180.0)
            length = 10 if i % 2 == 0 else 5
            r1 = r_in - 4
            r0 = r1 - length
            color = theme.TEXT_DIM if i % 2 == 0 else theme.BORDER
            self.create_line(cx + r0 * math.cos(ang), cy - r0 * math.sin(ang),
                             cx + r1 * math.cos(ang), cy - r1 * math.sin(ang),
                             fill=color, width=2 if i % 2 == 0 else 1,
                             capstyle="round")

        # wskaznik zera na szczycie pierscienia
        self.create_polygon(cx, cy - r_out - 1, cx - 5, cy - r_out - 9,
                            cx + 5, cy - r_out - 9,
                            fill=theme.MUTED, outline="")

        # kolo kierownicy
        wheel_r = r_in - 20
        self._wheel_r = wheel_r
        self._cx, self._cy = cx, cy
        rim_w = max(5, s // 34)
        # felga z dwoch okregow - ciemniejszy spod i jasniejsza krawedz
        self.create_oval(cx - wheel_r, cy - wheel_r, cx + wheel_r, cy + wheel_r,
                         outline=theme.darken(theme.PANEL_HI, 0.35),
                         width=rim_w + 3, fill="")
        self.create_oval(cx - wheel_r, cy - wheel_r, cx + wheel_r, cy + wheel_r,
                         outline=theme.mix(theme.PANEL_HI, theme.TEXT, 0.22),
                         width=rim_w, fill="")
        self._spokes: List[int] = []
        for _ in range(3):
            self._spokes.append(self.create_line(
                cx, cy, cx, cy, fill=theme.mix(theme.PANEL_HI, theme.TEXT, 0.14),
                width=rim_w, capstyle="round"))
        hub_r = max(10, s // 22)
        self._hub_ring = self.create_oval(cx - hub_r - 3, cy - hub_r - 3,
                                          cx + hub_r + 3, cy + hub_r + 3,
                                          fill=theme.PANEL_HI, outline="")
        self._hub = self.create_oval(cx - hub_r, cy - hub_r, cx + hub_r,
                                     cy + hub_r, fill=theme.PANEL,
                                     outline=theme.ACCENT_DIM, width=2)
        self._marker = self.create_line(cx, cy, cx, cy, fill=theme.ACCENT,
                                        width=4, capstyle="round")

        # Odczyt kata na wlasnym tle: wskazowka i szprychy przechodza pod
        # nim, wiec liczba jest czytelna w kazdym polozeniu kierownicy.
        ry = cy + wheel_r * 0.50
        round_rect(self, cx - 68, ry - 28, cx + 68, ry + 30, 14,
                   fill=theme.mix(theme.PANEL, theme.BG, 0.5),
                   outline=theme.BORDER)
        self._text = self.create_text(cx, ry - 5, text="0",
                                      font=self._fonts.big, fill=theme.TEXT)
        self._unit = self.create_text(cx, ry + 19, text="stopni",
                                      font=self._fonts.tiny, fill=theme.MUTED)
        gy = cy - wheel_r * 0.62
        round_rect(self, cx - 56, gy - 11, cx + 56, gy + 11, 10,
                   fill=theme.PANEL, outline="")
        self._range_text = self.create_text(cx, gy, text="zakres 900 st.",
                                            font=self._fonts.small,
                                            fill=theme.MUTED)
        self._items_built = True
        self._redraw()

    # ------------------------------------------------------------------- API

    def set_range(self, degrees: float) -> None:
        degrees = max(1.0, float(degrees))
        if abs(degrees - self._range) < 0.5:
            return
        self._range = degrees
        self.itemconfigure(self._range_text, text=f"zakres {int(degrees)} st.")
        self._redraw()

    def set_angle(self, degrees: float) -> None:
        if abs(degrees - self._deg) < 0.05:
            return
        self._deg = degrees
        self._redraw()

    # --------------------------------------------------------------- rysunek

    def _redraw(self) -> None:
        if not self._items_built:
            return
        half = self._range / 2.0
        frac = max(-1.0, min(1.0, self._deg / half))

        # luk: 0 na gorze, w prawo = ujemny extent
        over = abs(self._deg) > half + 0.5
        near = min(1.0, max(0.0, (abs(frac) - 0.75) / 0.25))
        color = theme.BAD if over else theme.mix(theme.ACCENT, theme.WARN, near)
        self.itemconfigure(self._arc, extent=-frac * 180.0, outline=color)

        # obrot kola
        cx, cy, r = self._cx, self._cy, self._wheel_r
        base = math.radians(-self._deg)
        for i, spoke in enumerate(self._spokes):
            a = base + math.radians(90 + i * 120)
            self.coords(spoke, cx, cy,
                        cx + r * math.cos(a), cy - r * math.sin(a))
        a0 = base + math.radians(90)
        self.coords(self._marker, cx, cy,
                    cx + r * 0.92 * math.cos(a0), cy - r * 0.92 * math.sin(a0))
        self.itemconfigure(self._marker, fill=color)
        self.itemconfigure(self._hub, outline=theme.mix(theme.ACCENT_DIM,
                                                        color, 0.5))

        text = "0" if abs(self._deg) < 0.5 else f"{self._deg:+.0f}"
        self.itemconfigure(self._text, text=text)


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
        self._pct = tk.Label(top, text="0 %", bg=theme.PANEL, fg=theme.TEXT,
                             font=fonts.bold)
        self._pct.pack(side="right")
        self._raw = None
        if show_raw:
            self._raw = tk.Label(top, text="", bg=theme.PANEL, fg=theme.MUTED,
                                 font=fonts.mono_small)
            self._raw.pack(side="right", padx=(0, 10))

        self._canvas = tk.Canvas(self, width=width, height=height, bg=theme.PANEL,
                                 highlightthickness=0, bd=0)
        self._canvas.pack(fill="x", pady=(5, 0))
        self._radius = height / 2.0
        self._track = round_rect(self._canvas, 0, 0, width, height,
                                 self._radius, fill=theme.TRACK, outline="")
        self._fill = round_rect(self._canvas, 0, 0, 0, height, self._radius,
                                fill=color, outline="")
        # kreski co 25 % - czytaja sie jak podzialka, takze na wypelnieniu
        self._ticks = [self._canvas.create_line(0, 1, 0, height - 1,
                                                fill=theme.PANEL, width=2)
                       for _ in range(3)]
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
            self._canvas.coords(tick, x, 1, x, h - 1)
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

        if self._raw is not None:
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

    def __init__(self, master: tk.Misc, fonts: Fonts, count: int = 32) -> None:
        super().__init__(master, bg=theme.PANEL)
        self._count = count
        self._state = [False] * count
        self._rows = (count + self.COLS - 1) // self.COLS
        height = self._rows * self.CELL_H + (self._rows - 1) * self.GAP
        self._canvas = tk.Canvas(self, bg=theme.PANEL, highlightthickness=0,
                                 bd=0, height=height, width=self.COLS * 44)
        self._canvas.pack(fill="x", expand=True)
        self._cells: List[int] = []
        self._texts: List[int] = []
        for i in range(count):
            self._cells.append(round_rect(self._canvas, 0, 0, 1, 1, 5,
                                          fill=theme.TRACK, outline=""))
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

    def _layout(self) -> None:
        total = self._cw
        step = (total - self.GAP * (self.COLS - 1)) / self.COLS
        for i in range(self._count):
            col, row = i % self.COLS, i // self.COLS
            x1 = col * (step + self.GAP)
            y1 = row * (self.CELL_H + self.GAP)
            set_round_rect(self._canvas, self._cells[i], x1, y1,
                           x1 + step, y1 + self.CELL_H, 5)
            self._canvas.coords(self._texts[i], x1 + step / 2,
                                y1 + self.CELL_H / 2)

    def set_mask(self, mask: int) -> None:
        for i in range(self._count):
            on = bool((mask >> i) & 1)
            if on != self._state[i]:
                self._state[i] = on
                self._canvas.itemconfigure(
                    self._cells[i], fill=theme.ACCENT if on else theme.TRACK)
                self._canvas.itemconfigure(
                    self._texts[i],
                    fill=theme.ACCENT_INK if on else theme.MUTED)

    def set_highlight(self, indices: Iterable[int], color: str) -> None:
        for i in indices:
            if 0 <= i < self._count and not self._state[i]:
                self._canvas.itemconfigure(self._cells[i], fill=color)
                self._canvas.itemconfigure(self._texts[i], fill=theme.TEXT)


class HatIndicator(tk.Canvas):
    """Krzyzak - cztery trojkaty."""

    def __init__(self, master: tk.Misc, size: int = 74) -> None:
        super().__init__(master, width=size, height=size, bg=theme.PANEL,
                         highlightthickness=0, bd=0)
        s = size
        c = s / 2
        g = s * 0.12
        o = s * 0.46
        self.create_oval(2, 2, s - 2, s - 2,
                         fill=theme.darken(theme.TRACK, 0.35), outline="")
        self._items = [
            self.create_polygon(c, c - o, c - g, c - g, c + g, c - g),       # gora
            self.create_polygon(c + o, c, c + g, c - g, c + g, c + g),       # prawo
            self.create_polygon(c, c + o, c - g, c + g, c + g, c + g),       # dol
            self.create_polygon(c - o, c, c - g, c - g, c - g, c + g),       # lewo
        ]
        for it in self._items:
            self.itemconfigure(it, fill=theme.TRACK, outline="")
        self.create_oval(c - g * 0.7, c - g * 0.7, c + g * 0.7, c + g * 0.7,
                         fill=theme.PANEL_HI, outline=theme.BORDER)
        self._active = -1

    def set(self, direction: int) -> None:
        if direction == self._active:
            return
        self._active = direction
        for i, it in enumerate(self._items):
            self.itemconfigure(it, fill=theme.ACCENT if i == direction else theme.TRACK)


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
        k = self.KNOB_R
        self._knob_glow = self._canvas.create_oval(0, 0, 0, 0, fill="",
                                                   outline="")
        self._knob = self._canvas.create_oval(-k, cy - k, k, cy + k,
                                              fill=theme.TEXT,
                                              outline=theme.ACCENT, width=2)
        for seq, handler in (("<Button-1>", self._on_press),
                             ("<B1-Motion>", self._on_drag),
                             ("<ButtonRelease-1>", self._on_release),
                             ("<Configure>", self._on_resize),
                             ("<Enter>", self._on_enter),
                             ("<Leave>", self._on_leave)):
            self._canvas.bind(seq, handler)

        if hint:
            tk.Label(self, text=hint, bg=theme.PANEL, fg=theme.MUTED,
                     font=fonts.tiny, wraplength=width, justify="left",
                     anchor="w").pack(fill="x", pady=(3, 0))
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
        k = self.KNOB_R
        self._canvas.coords(self._knob, x - k, cy - k, x + k, cy + k)
        self._canvas.coords(self._knob_glow, x - k - 4, cy - k - 4,
                            x + k + 4, cy + k + 4)

    def _on_resize(self, event) -> None:
        self._cw = max(40, event.width)
        self._redraw()

    def _on_enter(self, _event) -> None:
        self._canvas.itemconfigure(self._knob_glow,
                                   fill=theme.mix(theme.PANEL, theme.ACCENT,
                                                  0.25))

    def _on_leave(self, _event) -> None:
        if not self._dragging:
            self._canvas.itemconfigure(self._knob_glow, fill="")

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
        self._canvas.itemconfigure(self._knob_glow, fill="")

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
    """Pionowo przewijany kontener - uzywany w zakladkach z wieloma polami."""

    def __init__(self, master: tk.Misc, **kw) -> None:
        super().__init__(master, **kw)
        self._canvas = tk.Canvas(self, bg=theme.BG, highlightthickness=0, bd=0)
        self._bar = ttk.Scrollbar(self, orient="vertical",
                                  command=self._canvas.yview)
        self._canvas.configure(yscrollcommand=self._bar.set)
        self._canvas.pack(side="left", fill="both", expand=True)
        self._bar.pack(side="right", fill="y")

        self.inner = ttk.Frame(self._canvas, style="Root.TFrame")
        self._win = self._canvas.create_window((0, 0), window=self.inner,
                                               anchor="nw")
        self.inner.bind("<Configure>", self._on_inner)
        self._canvas.bind("<Configure>", self._on_canvas)
        self._canvas.bind("<Enter>", lambda _e: self._bind_wheel(True))
        self._canvas.bind("<Leave>", lambda _e: self._bind_wheel(False))

    def _on_inner(self, _event) -> None:
        self._canvas.configure(scrollregion=self._canvas.bbox("all"))

    def _on_canvas(self, event) -> None:
        self._canvas.itemconfigure(self._win, width=event.width)

    def _bind_wheel(self, on: bool) -> None:
        if on:
            self._canvas.bind_all("<MouseWheel>", self._on_wheel)
        else:
            self._canvas.unbind_all("<MouseWheel>")

    def _on_wheel(self, event) -> None:
        first, last = self._canvas.yview()
        if first <= 0.0 and last >= 1.0:
            return
        self._canvas.yview_scroll(int(-event.delta / 120), "units")
