"""Paleta i konfiguracja stylow ttk - ciemny motyw."""

from __future__ import annotations

import tkinter as tk
from tkinter import font as tkfont
from tkinter import ttk

# ---------------------------------------------------------------- paleta
#
# Cztery poziomy tla zamiast dwoch: pulpit okna, karta, pole w karcie i
# rowek paska. Dzieki temu karta odcina sie od tla bez grubych ramek, a
# pola wejsciowe widac wewnatrz karty.

BG = "#0b0e14"              # tlo okna
PANEL = "#151a23"           # karta
PANEL_HI = "#1e2531"        # pole w karcie, naglowek
ELEV = "#273041"            # element uniesiony (przycisk pod kursorem)
BORDER = "#28313f"          # ramka karty
BORDER_HI = "#38445a"       # ramka pola pod kursorem
TRACK = "#222a37"           # rowek paska, nieaktywna dioda
SHADOW = "#05070b"          # cien pod karta

TEXT = "#e9eef7"
TEXT_DIM = "#a3b2c7"
MUTED = "#6d7d93"

ACCENT = "#4cc2ff"
ACCENT_DIM = "#1b5478"
ACCENT_DEEP = "#123a55"
ACCENT_INK = "#06131d"      # tekst na tle akcentu

GOOD = "#35d399"
WARN = "#fbbf24"
BAD = "#fb7185"
VIOLET = "#a78bfa"

LEVEL_COLORS = {
    "info": TEXT_DIM,
    "ok": GOOD,
    "warn": WARN,
    "error": BAD,
}


class Fonts:
    """Czcionki tworzone raz, po utworzeniu okna glownego."""

    def __init__(self, root: tk.Misc) -> None:
        family = "Segoe UI"
        available = set(tkfont.families(root))
        if family not in available:
            family = "Tahoma" if "Tahoma" in available else "TkDefaultFont"
        mono = "Consolas"
        if mono not in available:
            mono = "Courier New" if "Courier New" in available else "TkFixedFont"

        # Ikony zakladek: czcionka systemowa Windows 11, a na Windows 10 jej
        # poprzedniczka o tych samych kodach. Bez nich - same napisy.
        self.icons = next((f for f in ("Segoe Fluent Icons", "Segoe MDL2 Assets")
                           if f in available), "")

        self.family = family
        self.base = (family, 10)
        self.small = (family, 9)
        self.tiny = (family, 8)
        self.bold = (family, 10, "bold")
        self.small_bold = (family, 9, "bold")
        self.tiny_bold = (family, 8, "bold")
        self.heading = (family, 11, "bold")
        self.title = (family, 15, "bold")
        self.big = (family, 26, "bold")
        self.huge = (family, 34, "bold")
        self.mono = (mono, 9)
        self.mono_small = (mono, 8)
        self.mono_bold = (mono, 9, "bold")


def apply_theme(root: tk.Misc, fonts: Fonts) -> ttk.Style:
    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass

    style.configure(".", background=PANEL, foreground=TEXT,
                    fieldbackground=PANEL_HI, bordercolor=BORDER,
                    lightcolor=PANEL_HI, darkcolor=PANEL,
                    focuscolor=ACCENT, font=fonts.base)

    style.configure("TFrame", background=PANEL)
    style.configure("Root.TFrame", background=BG)
    style.configure("Card.TFrame", background=PANEL)
    style.configure("Sunken.TFrame", background=PANEL_HI)
    style.configure("Rule.TFrame", background=BORDER)

    style.configure("TLabel", background=PANEL, foreground=TEXT)
    style.configure("Root.TLabel", background=BG, foreground=TEXT)
    style.configure("Dim.TLabel", background=PANEL, foreground=TEXT_DIM,
                    font=fonts.small)
    style.configure("Muted.TLabel", background=PANEL, foreground=MUTED,
                    font=fonts.small)
    style.configure("Heading.TLabel", background=PANEL, foreground=TEXT,
                    font=fonts.heading)
    style.configure("Title.TLabel", background=BG, foreground=TEXT,
                    font=fonts.title)
    style.configure("Value.TLabel", background=PANEL, foreground=ACCENT,
                    font=fonts.bold)
    style.configure("Good.TLabel", background=PANEL, foreground=GOOD,
                    font=fonts.small)
    style.configure("Warn.TLabel", background=PANEL, foreground=WARN,
                    font=fonts.small)
    style.configure("Bad.TLabel", background=PANEL, foreground=BAD,
                    font=fonts.small)
    style.configure("Mono.TLabel", background=PANEL, foreground=TEXT_DIM,
                    font=fonts.mono)

    # ---- przyciski ----
    style.configure("TButton", background=PANEL_HI, foreground=TEXT,
                    bordercolor=BORDER, relief="flat", padding=(14, 8),
                    font=fonts.base, focusthickness=0)
    style.map("TButton",
              background=[("disabled", PANEL), ("pressed", ACCENT_DEEP),
                          ("active", ELEV)],
              foreground=[("disabled", MUTED), ("active", TEXT)],
              bordercolor=[("active", BORDER_HI)])

    style.configure("Accent.TButton", background=ACCENT, foreground=ACCENT_INK,
                    bordercolor=ACCENT, font=fonts.bold)
    style.map("Accent.TButton",
              background=[("disabled", ACCENT_DIM), ("pressed", "#2f9fd8"),
                          ("active", "#6ccfff")],
              bordercolor=[("active", "#6ccfff")],
              foreground=[("disabled", "#4d6678"), ("active", ACCENT_INK)])

    style.configure("Danger.TButton", background="#3a2028", foreground=BAD,
                    bordercolor="#4a2831")
    style.map("Danger.TButton",
              background=[("pressed", "#52262d"), ("active", "#4a2a31")],
              foreground=[("active", BAD)])

    style.configure("Small.TButton", padding=(11, 5), font=fonts.small)
    style.map("Small.TButton",
              background=[("disabled", PANEL), ("pressed", ACCENT_DEEP),
                          ("active", ELEV)])

    style.configure("Ghost.TButton", background=PANEL, foreground=TEXT_DIM,
                    bordercolor=BORDER, padding=(11, 5), font=fonts.small)
    style.map("Ghost.TButton",
              background=[("pressed", PANEL_HI), ("active", PANEL_HI)],
              foreground=[("active", TEXT)],
              bordercolor=[("active", BORDER_HI)])
    _rounded_buttons(root, style)

    # ---- notatnik ----
    #
    # Okno glowne ma wlasne zakladki (widgets.TabBar + widgets.Pages); styl
    # z pustym ukladem zakladki zostaje dla notatnikow bez systemowego paska.
    style.configure("TNotebook", background=BG, borderwidth=0,
                    tabmargins=(0, 0, 0, 0))
    style.configure("Hidden.TNotebook", background=BG, borderwidth=0,
                    tabmargins=(0, 0, 0, 0))
    try:
        style.layout("Hidden.TNotebook.Tab", [])
    except tk.TclError:
        pass

    # ---- pola ----
    style.configure("TCombobox", fieldbackground=PANEL_HI, background=PANEL,
                    foreground=TEXT, arrowcolor=TEXT_DIM, bordercolor=BORDER,
                    selectbackground=PANEL_HI, selectforeground=TEXT,
                    lightcolor=BORDER, darkcolor=BORDER,
                    padding=(8, 5))
    style.map("TCombobox",
              fieldbackground=[("readonly", PANEL_HI), ("disabled", PANEL)],
              bordercolor=[("focus", ACCENT), ("active", BORDER_HI)],
              arrowcolor=[("active", TEXT)],
              foreground=[("disabled", MUTED)])
    root.option_add("*TCombobox*Listbox.background", PANEL_HI)
    root.option_add("*TCombobox*Listbox.foreground", TEXT)
    root.option_add("*TCombobox*Listbox.selectBackground", ACCENT)
    root.option_add("*TCombobox*Listbox.selectForeground", ACCENT_INK)
    root.option_add("*TCombobox*Listbox.borderWidth", 0)

    style.configure("TEntry", fieldbackground=PANEL_HI, background=PANEL,
                    foreground=TEXT,
                    bordercolor=BORDER, insertcolor=TEXT,
                    lightcolor=BORDER, darkcolor=BORDER, padding=(8, 5))
    style.map("TEntry", bordercolor=[("focus", ACCENT)])

    style.configure("TCheckbutton", background=PANEL, foreground=TEXT_DIM,
                    indicatorbackground=PANEL_HI, indicatorforeground=ACCENT_INK,
                    bordercolor=BORDER, focuscolor=PANEL, padding=(2, 4))
    style.map("TCheckbutton",
              background=[("active", PANEL)],
              foreground=[("active", TEXT), ("selected", TEXT)],
              indicatorbackground=[("selected", ACCENT), ("active", ELEV)],
              bordercolor=[("selected", ACCENT), ("active", BORDER_HI)])

    style.configure("TRadiobutton", background=PANEL, foreground=TEXT_DIM,
                    indicatorbackground=PANEL_HI, focuscolor=PANEL)
    style.map("TRadiobutton", background=[("active", PANEL)],
              foreground=[("active", TEXT), ("selected", TEXT)],
              indicatorbackground=[("selected", ACCENT)])

    # Suwaki rysuje widgets.LabeledSlider na plotnie: w motywie "clam"
    # galka i tlo widgetu biora kolor z tej samej opcji, wiec kolorowa
    # galka zawsze ciagnela za soba kolorowa ramke wokol rowka.
    style.configure("TSeparator", background=BORDER)

    # ---- pasek przewijania ----
    style.configure("Vertical.TScrollbar", background=ELEV, troughcolor=BG,
                    bordercolor=BG, arrowcolor=MUTED, relief="flat",
                    arrowsize=12, width=11)
    style.map("Vertical.TScrollbar",
              background=[("active", "#3e4a60"), ("pressed", ACCENT_DIM)],
              arrowcolor=[("active", TEXT_DIM)])
    style.configure("Horizontal.TScrollbar", background=ELEV, troughcolor=BG,
                    bordercolor=BG, arrowcolor=MUTED, relief="flat",
                    arrowsize=12, width=11)
    style.map("Horizontal.TScrollbar",
              background=[("active", "#3e4a60"), ("pressed", ACCENT_DIM)])

    # Waski pasek bez strzalek - sam uchwyt na ciemnym rowku.
    try:
        style.layout("Slim.Vertical.TScrollbar", [
            ("Vertical.Scrollbar.trough", {"sticky": "ns", "children": [
                ("Vertical.Scrollbar.thumb", {"expand": "1",
                                              "sticky": "nswe"})]})])
    except tk.TclError:
        pass
    style.configure("Slim.Vertical.TScrollbar", background=ELEV,
                    troughcolor=BG, bordercolor=BG, lightcolor=ELEV,
                    darkcolor=ELEV, relief="flat", gripcount=0, width=8)
    style.map("Slim.Vertical.TScrollbar",
              background=[("pressed", ACCENT_DIM), ("active", "#3e4a60")],
              lightcolor=[("pressed", ACCENT_DIM), ("active", "#3e4a60")],
              darkcolor=[("pressed", ACCENT_DIM), ("active", "#3e4a60")])

    style.configure("TSpinbox", fieldbackground=PANEL_HI, foreground=TEXT,
                    bordercolor=BORDER, arrowcolor=TEXT_DIM, padding=(6, 3))

    _rounded_fields(root, style)
    return style


def _rounded_buttons(root: tk.Misc, style: ttk.Style) -> None:
    """Zaokraglone przyciski: tlo z wygladzonego obrazka 9-slice.

    Motyw "clam" rysuje tylko prostokaty. Element "image" z border=8
    zostawia rogi obrazka bez zmian, a rozciaga srodek - przycisk ma
    zaokraglone rogi w kazdym rozmiarze. padding=0 - odstep napisu od
    krawedzi daje dalej "padding" stylu, nie ramka obrazka. Kolory napisow
    zostaja w
    style.map powyzej; tu tylko tlo dla stanow: zwykly, kursor,
    wcisniety, wylaczony.
    """
    from . import art               # art potrzebuje Pillow - tylko tutaj

    keep = getattr(root, "_theme_images", None)
    if keep is None:
        keep = root._theme_images = []

    def image(fill: str, edge: str):
        size, r = 24, 7
        p = art.Paint(size, size)
        p.fill(("rrect", 0, 0, size, size, r), edge)
        p.fill(("rrect", 1, 1, size - 1, size - 1, r - 1), fill)
        photo = art.photo(p.image(), root)
        keep.append(photo)
        return photo

    def make(layout: str, element: str, normal, active, pressed, disabled):
        try:
            style.element_create(element, "image", image(*normal),
                                 ("disabled", image(*disabled)),
                                 ("pressed", image(*pressed)),
                                 ("active", image(*active)),
                                 border=8, padding=0, sticky="nsew")
        except tk.TclError:
            return                      # element juz jest (drugie wywolanie)
        style.layout(layout, [(element, {"sticky": "nswe", "children": [
            ("Button.padding", {"sticky": "nswe", "children": [
                ("Button.label", {"sticky": "nswe"})]})]})])

    # Small.TButton dziedziczy uklad po TButton
    make("TButton", "Rnd.border", (PANEL_HI, BORDER), (ELEV, BORDER_HI),
         (ACCENT_DEEP, ACCENT_DIM), (PANEL, BORDER))
    make("Accent.TButton", "RndAccent.border", (ACCENT, ACCENT),
         ("#6ccfff", "#6ccfff"), ("#2f9fd8", "#2f9fd8"),
         (ACCENT_DIM, ACCENT_DIM))
    make("Danger.TButton", "RndDanger.border", ("#3a2028", "#4a2831"),
         ("#4a2a31", "#5a3039"), ("#52262d", "#5a3039"), (PANEL, BORDER))
    make("Ghost.TButton", "RndGhost.border", (PANEL, BORDER),
         (PANEL_HI, BORDER_HI), (PANEL_HI, BORDER_HI), (PANEL, BORDER))


def _rounded_fields(root: tk.Misc, style: ttk.Style) -> None:
    """Pola wyboru, listy rozwijane i pola tekstowe w tym samym stylu co
    zaokraglone przyciski.

    "clam" rysuje pole wyboru jako kwadrat z krzyzykiem, a liste rozwijana
    jako prostokat z osobnym pudelkiem na strzalke - obok zaokraglonych
    przyciskow wygladaly jak z innego programu. Tlo stylu jest kolorem
    karty: przezroczyste rogi obrazka pokazuja wtedy karte, a nie
    kwadratowe tlo pola.
    """
    from . import art

    keep = root._theme_images

    def keep_photo(paint):
        photo = art.photo(paint.image(), root)
        keep.append(photo)
        return photo

    # ---- pole wyboru: zaokraglony kwadrat, zaznaczone - akcent z ptaszkiem
    box, gap = 16, 8

    def check(fill: str, edge: str, mark: str = ""):
        p = art.Paint(box + gap, box)
        p.fill(("rrect", 0, 0, box, box, 4.5), edge)
        p.fill(("rrect", 1, 1, box - 1, box - 1, 3.5), fill)
        if mark:
            p.line(((4.2, 8.3), (6.9, 11.0), (11.9, 5.3)), mark, 2.0)
        return keep_photo(p)

    try:
        style.element_create(
            "Rnd.Checkbutton.indicator", "image", check(PANEL_HI, BORDER),
            ("disabled", "selected", check(ACCENT_DIM, ACCENT_DIM, "#4d6678")),
            ("disabled", check(PANEL, BORDER)),
            ("pressed", "selected", check("#2f9fd8", "#2f9fd8", ACCENT_INK)),
            ("active", "selected", check("#6ccfff", "#6ccfff", ACCENT_INK)),
            ("selected", check(ACCENT, ACCENT, ACCENT_INK)),
            ("pressed", check(ACCENT_DEEP, ACCENT_DIM)),
            ("active", check(ELEV, BORDER_HI)),
            sticky="")
        style.layout("TCheckbutton", [("Checkbutton.padding", {
            "sticky": "nswe", "children": [
                ("Rnd.Checkbutton.indicator", {"side": "left", "sticky": ""}),
                ("Checkbutton.label", {"side": "left", "sticky": "nswe"})]})])
    except tk.TclError:
        pass                            # element juz jest (drugie wywolanie)

    # ---- tlo pola: 9-slice jak w przyciskach ----
    def field(fill: str, edge: str):
        size, r = 24, 6
        p = art.Paint(size, size)
        p.fill(("rrect", 0, 0, size, size, r), edge)
        p.fill(("rrect", 1, 1, size - 1, size - 1, r - 1), fill)
        return keep_photo(p)

    # strzalka listy: sam szewron, bez osobnego pudelka
    def chevron(color: str):
        w, h = 22, 12
        p = art.Paint(w, h)
        p.line(((7, 4.2), (11, 8.2), (15, 4.2)), color, 1.8)
        return keep_photo(p)

    try:
        style.element_create(
            "Rnd.Combobox.field", "image", field(PANEL_HI, BORDER),
            ("disabled", field(PANEL, BORDER)),
            ("focus", field(PANEL_HI, ACCENT)),
            ("pressed", field(ELEV, BORDER_HI)),
            ("active", field(ELEV, BORDER_HI)),
            ("hover", field(ELEV, BORDER_HI)),
            border=7, padding=0, sticky="nswe")
        style.element_create(
            "Rnd.Combobox.arrow", "image", chevron(TEXT_DIM),
            ("disabled", chevron(MUTED)), ("active", chevron(TEXT)),
            ("hover", chevron(TEXT)), sticky="")
        style.layout("TCombobox", [("Rnd.Combobox.field", {
            "sticky": "nswe", "children": [
                ("Rnd.Combobox.arrow", {"side": "right", "sticky": "ns"}),
                ("Combobox.padding", {"expand": "1", "sticky": "nswe",
                                      "children": [
                    ("Combobox.textarea", {"sticky": "nswe"})]})]})])

        style.element_create(
            "Rnd.Entry.field", "image", field(PANEL_HI, BORDER),
            ("disabled", field(PANEL, BORDER)),
            ("focus", field(PANEL_HI, ACCENT)),
            ("hover", field(PANEL_HI, BORDER_HI)),
            border=7, padding=0, sticky="nswe")
        style.layout("TEntry", [("Rnd.Entry.field", {
            "sticky": "nswe", "children": [
                ("Entry.padding", {"sticky": "nswe", "children": [
                    ("Entry.textarea", {"sticky": "nswe"})]})]})])
    except tk.TclError:
        pass


def mix(color_a: str, color_b: str, t: float) -> str:
    """Interpolacja dwoch kolorow #rrggbb; t = 0 daje color_a."""
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    a = (int(color_a[1:3], 16), int(color_a[3:5], 16), int(color_a[5:7], 16))
    b = (int(color_b[1:3], 16), int(color_b[3:5], 16), int(color_b[5:7], 16))
    r = tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))
    return f"#{r[0]:02x}{r[1]:02x}{r[2]:02x}"


def lighten(color: str, t: float) -> str:
    """Rozjasnienie koloru o ulamek drogi do bieli."""
    return mix(color, "#ffffff", t)


def darken(color: str, t: float) -> str:
    """Przyciemnienie koloru o ulamek drogi do czerni."""
    return mix(color, "#000000", t)
