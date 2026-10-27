"""Okno glowne panelu kierownicy."""

from __future__ import annotations

import queue
import time
import tkinter as tk
import webbrowser
from tkinter import messagebox, ttk
from typing import Dict, List

from .. import link as link_mod
from .. import protocol
from ..config import (AppConfig, G29_CONTROL_LABELS, G29_CONTROLS,
                      OUTPUT_MODE_LABELS, OUTPUT_MODES,
                      PAD_BUTTON_NAMES, PAD_DIRECTION_LABELS, PAD_DIRECTIONS,
                      PAD_PROFILE_LABELS, PAD_PROFILE_ORDER,
                      PAD_SOURCE_KEYS, PAD_SOURCE_LABELS, PAD_SOURCE_PEDAL,
                      PEDAL_KEYS, PEDAL_LABELS, VJOY_AXIS_CHOICES,
                      config_path)
from ..engine import WheelEngine
from ..ffb import DIRECTION_LABELS, DIRECTION_MODES
from ..processing import HAT_DIRECTIONS, build_g29_report, g29_active
from ..link import list_ports
from ..outputs import VJoyBackend, vjoy_diagnostics
from ..outputs.vjoy import AXIS_LABELS
from ..outputs.xinput import PAD_BUTTON_LABELS, XUSB, vigem_diagnostics
from . import theme
from .padview import CONTROLS as PAD_CONTROLS
from .padview import PadView
from .g29tab import G29Tab
from .theme import Fonts, apply_theme
from .widgets import (BarMeter, ButtonGrid, Card, HatIndicator, LabeledSlider,
                      ScrollableFrame, Sparkline, StatRow, StatusDot, TabBar,
                      TorqueMeter, WheelDial)

APP_TITLE = "Panel kierownicy"
APP_VERSION = "2.1"
VJOY_URL = "https://sourceforge.net/projects/vjoystick/"

RANGE_PRESETS = (120, 270, 360, 540, 720, 900, 1080)

#: Nazwy zakladek - w tej samej kolejnosci co stale TAB_* ponizej.
TAB_NAMES = ("Pulpit", "Kierownica", "Pedaly", "Przyciski", "G29", "Pad",
             "Force Feedback", "Diagnostyka")
UI_PERIOD_MS = 33            # ~30 klatek na sekunde


#: Czego wymaga ktora rodzina gier. Kolejnosc ma znaczenie - pierwszy wiersz
#: dostaje akcent, bo to najczestszy powod, dla ktorego gra "nie widzi
#: kierownicy".
GAME_COMPAT = (
    ("Forza, NFS Unbound, F1, ETS2 - kazda gra z kierownica", "kierownica G29",
     "Plytka sama przedstawia sie jako Logitech G29 (046D:C24F), wiec gry z "
     "lista obslugiwanych kierownic rozpoznaja ja bez zadnej konfiguracji."),
    ("Gry bez obslugi kierownic (porty z konsol)", "tryb pada",
     "Czytaja wylacznie XInput, czyli pada. Kierownicy nie zobacza w zadnym "
     "trybie."),
    ("Symulatory z Force Feedback (AC, rFactor, BeamNG)", "kierownica vJoy",
     "G29 tez w nich dziala, ale Force Feedback z gry idzie tylko przez vJoy. "
     "Lokalne efekty (sprezyna, tlumienie) dzialaja w kazdym trybie."),
    ("Gry, ktore umieja jedno i drugie", "oba naraz",
     "Tryb \"Oba naraz\" wystawia vJoy i pada jednoczesnie; w grze wybierasz, "
     "ktore urzadzenie ma byc aktywne."),
)

#: Gdzie ustawia sie przyciski trybu G29.
G29_BUTTON_HELP = (
    "Przyciski G29 przypisujesz w zakladce G29 - kliknij przycisk na rysunku "
    "kierownicy i wcisnij go na swojej. Te przypisania nie zaleza od "
    "zakladki Pad ani od listy vJoy w zakladce Przyciski.")


class PanelApp:
    # Indeksy zakladek w notatniku - uzywane przy odswiezaniu.
    TAB_DASH, TAB_STEER, TAB_PEDALS, TAB_BUTTONS = 0, 1, 2, 3
    TAB_G29, TAB_PAD, TAB_FFB, TAB_DIAG = 4, 5, 6, 7

    def __init__(self) -> None:
        self.cfg = AppConfig.load()
        self._log_queue: "queue.Queue[tuple]" = queue.Queue(maxsize=2000)
        self.engine = WheelEngine(self.cfg, on_log=self._enqueue_log)

        self.root = tk.Tk()
        self.root.title(f"{APP_TITLE} {APP_VERSION}")
        self.root.minsize(940, 600)
        self.root.configure(bg=theme.BG)
        self._place_window(1240, 880)
        self._set_icon()

        self.fonts = Fonts(self.root)
        self.style = apply_theme(self.root, self.fonts)

        self._port_labels: Dict[str, str] = {}
        self._button_map_vars: List[tk.StringVar] = []
        self._button_invert_vars: List[tk.BooleanVar] = []
        self._hat_vars: List[tk.StringVar] = []
        self._pedal_widgets: Dict[str, dict] = {}
        self._pad_prev_buttons = 0
        self._pad_wizard: List[str] = []
        self._pad_wizard_i = 0
        self._pad_notice_until = 0.0
        self._closing = False

        self._build()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self.engine.start()
        self._log("info", f"Konfiguracja: {config_path()}")
        self.root.after(UI_PERIOD_MS, self._tick)

    def run(self) -> None:
        self.root.mainloop()

    def _place_window(self, want_w: int, want_h: int) -> None:
        """Dopasowuje okno do ekranu i ustawia je na srodku.

        Na malym laptopie (np. 1366x768) okno nie moze byc wieksze niz pulpit,
        inaczej dolna czesc interfejsu wychodzi poza ekran.
        """
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        w = max(940, min(want_w, screen_w - 80))
        h = max(600, min(want_h, screen_h - 120))
        x = max(0, (screen_w - w) // 2)
        y = max(0, (screen_h - h) // 3)
        self.root.geometry(f"{w}x{h}+{x}+{y}")

    def _set_icon(self) -> None:
        # Prosta ikona rysowana w locie - brak zewnetrznych plikow.
        try:
            img = tk.PhotoImage(width=32, height=32)
            cx = cy = 15.5
            for y in range(32):
                for x in range(32):
                    d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
                    if 11.5 <= d <= 15.0:
                        img.put(theme.ACCENT, (x, y))
                    elif d <= 3.5:
                        img.put(theme.ACCENT, (x, y))
            for t in range(4, 12):
                img.put(theme.ACCENT, (int(cx), int(cy) - t))
                img.put(theme.ACCENT, (int(cx) - t, int(cy) + t // 2))
                img.put(theme.ACCENT, (int(cx) + t, int(cy) + t // 2))
            self.root.iconphoto(True, img)
            self._icon = img            # trzymamy referencje
        except Exception:
            pass

    # ================================================================ budowa

    def _build(self) -> None:
        root = ttk.Frame(self.root, style="Root.TFrame", padding=(16, 14))
        root.pack(fill="both", expand=True)

        self._build_header(root)

        # Pasek zakladek rysujemy sami - ttk potrafi tylko ramki w stylu
        # Windows 95. Zakladki notatnika sa ukryte stylem Hidden.TNotebook,
        # ale sam notatnik zostaje: to on przelacza strony.
        nav = tk.Frame(root, bg=theme.BG)
        nav.pack(fill="x", pady=(16, 0))
        tk.Frame(root, bg=theme.BORDER, height=1).pack(fill="x")

        self.nb = ttk.Notebook(root, style="Hidden.TNotebook")
        self.nb.pack(fill="both", expand=True, pady=(12, 0))

        pad_kw = {"style": "Root.TFrame", "padding": 0}
        self.tab_dash = ttk.Frame(self.nb, **pad_kw)
        self.tab_steer = ttk.Frame(self.nb, **pad_kw)
        self.tab_pedals = ttk.Frame(self.nb, **pad_kw)
        self.tab_buttons = ttk.Frame(self.nb, **pad_kw)
        self.tab_g29 = ttk.Frame(self.nb, **pad_kw)
        self.tab_pad = ttk.Frame(self.nb, **pad_kw)
        self.tab_ffb = ttk.Frame(self.nb, **pad_kw)
        self.tab_diag = ttk.Frame(self.nb, **pad_kw)

        for tab, name in zip((self.tab_dash, self.tab_steer, self.tab_pedals,
                              self.tab_buttons, self.tab_g29, self.tab_pad,
                              self.tab_ffb, self.tab_diag), TAB_NAMES):
            self.nb.add(tab, text=name)

        self.tabbar = TabBar(nav, self.fonts, TAB_NAMES, command=self._go_tab)
        self.tabbar.pack(side="left")
        self.nb.bind("<<NotebookTabChanged>>", self._on_tab_changed)

        dash_scroll = ScrollableFrame(self.tab_dash, style="Root.TFrame")
        dash_scroll.pack(fill="both", expand=True)
        self._build_dashboard(dash_scroll.inner)
        self._build_steer_tab(self.tab_steer)
        self._build_pedals_tab(self.tab_pedals)
        self._build_buttons_tab(self.tab_buttons)
        g29_scroll = ScrollableFrame(self.tab_g29, style="Root.TFrame")
        g29_scroll.pack(fill="both", expand=True)
        self.g29tab = G29Tab(self)
        self.g29tab.build(g29_scroll.inner)
        pad_scroll = ScrollableFrame(self.tab_pad, style="Root.TFrame")
        pad_scroll.pack(fill="both", expand=True)
        self._build_pad_tab(pad_scroll.inner)
        self._build_ffb_tab(self.tab_ffb)
        diag_scroll = ScrollableFrame(self.tab_diag, style="Root.TFrame")
        diag_scroll.pack(fill="both", expand=True)
        self._build_diag_tab(diag_scroll.inner)

        self._build_statusbar(root)

    def _go_tab(self, index: int) -> None:
        self.nb.select(index)

    def _on_tab_changed(self, _event=None) -> None:
        """Notatnik zmienil strone - podkreslenie na pasku ma nadazyc."""
        bar = getattr(self, "tabbar", None)
        if bar is None:
            return
        try:
            index = self.nb.index(self.nb.select())
        except tk.TclError:
            return
        bar.select(index)
        g29 = getattr(self, "g29tab", None)
        if g29 is not None and index != self.TAB_G29:
            g29.stop()

    # ------------------------------------------------------------- naglowek

    def _build_header(self, parent: ttk.Frame) -> None:
        bar = tk.Frame(parent, bg=theme.PANEL, highlightthickness=1,
                       highlightbackground=theme.BORDER)
        bar.pack(fill="x")
        inner = tk.Frame(bar, bg=theme.PANEL)
        inner.pack(fill="x", padx=16, pady=12)

        left = tk.Frame(inner, bg=theme.PANEL)
        left.pack(side="left")
        logo = tk.Canvas(left, width=30, height=30, bg=theme.PANEL,
                         highlightthickness=0, bd=0)
        logo.pack(side="left", padx=(0, 11))
        # kierownica: obrecz, poziomy drazek i kolumna w dol - trzy promienie
        # rozchodzace sie na boki wygladaly jak znaczek samochodu
        logo.create_oval(2, 2, 28, 28, outline=theme.ACCENT, width=3)
        logo.create_line(5, 15, 25, 15, fill=theme.ACCENT, width=3,
                         capstyle="round")
        logo.create_line(15, 15, 15, 26, fill=theme.ACCENT, width=3,
                         capstyle="round")
        logo.create_oval(11, 11, 19, 19, fill=theme.PANEL,
                         outline=theme.ACCENT, width=2)
        self._logo = logo
        tk.Label(left, text=APP_TITLE, bg=theme.PANEL, fg=theme.TEXT,
                 font=self.fonts.title).pack(side="left")
        tk.Label(left, text=f"v{APP_VERSION}", bg=theme.PANEL_HI,
                 fg=theme.TEXT_DIM, font=self.fonts.tiny_bold, padx=7,
                 pady=2).pack(side="left", padx=(10, 0), pady=(4, 0))

        right = tk.Frame(inner, bg=theme.PANEL)
        right.pack(side="right")

        # --- stan lacza i wyjscia: jeden kafelek, dwa wiersze ---
        status = tk.Frame(right, bg=theme.PANEL_HI, highlightthickness=1,
                          highlightbackground=theme.BORDER)
        status.pack(side="right", padx=(16, 0))

        lk = tk.Frame(status, bg=theme.PANEL_HI)
        lk.pack(anchor="w", padx=11, pady=(7, 1))
        self.dot_link = StatusDot(lk, 10, bg=theme.PANEL_HI)
        self.dot_link.pack(side="left", padx=(0, 7))
        self.lbl_link = tk.Label(lk, text="Rozlaczono", bg=theme.PANEL_HI,
                                 fg=theme.TEXT_DIM, font=self.fonts.small,
                                 anchor="w")
        self.lbl_link.pack(side="left")

        vj = tk.Frame(status, bg=theme.PANEL_HI)
        vj.pack(anchor="w", padx=11, pady=(1, 7))
        self.dot_vjoy = StatusDot(vj, 10, bg=theme.PANEL_HI)
        self.dot_vjoy.pack(side="left", padx=(0, 7))
        self.lbl_vjoy = tk.Label(vj, text="vJoy: -", bg=theme.PANEL_HI,
                                 fg=theme.TEXT_DIM, font=self.fonts.small,
                                 anchor="w")
        self.lbl_vjoy.pack(side="left")

        # --- wybor portu ---
        ctl = tk.Frame(right, bg=theme.PANEL)
        ctl.pack(side="right")
        tk.Label(ctl, text="PORT", bg=theme.PANEL, fg=theme.MUTED,
                 font=self.fonts.tiny_bold).pack(side="left", padx=(0, 8))
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(ctl, textvariable=self.port_var, width=26,
                                       state="readonly")
        self.port_combo.pack(side="left", padx=(0, 7))
        ttk.Button(ctl, text="Odswiez", style="Ghost.TButton",
                   command=self._refresh_ports).pack(side="left", padx=(0, 7))
        self.btn_connect = ttk.Button(ctl, text="Polacz", style="Accent.TButton",
                                      command=self._toggle_connect)
        self.btn_connect.pack(side="left")

        self._refresh_ports()

    # -------------------------------------------------------------- pulpit

    def _build_dashboard(self, parent: ttk.Frame) -> None:
        parent.grid_columnconfigure(0, weight=0)
        parent.grid_columnconfigure(1, weight=1)

        # ---------------- lewa kolumna: kierownica ----------------
        left = ttk.Frame(parent, style="Root.TFrame")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 10))

        card = Card(left, "Kierownica", self.fonts)
        card.place_card(fill="x")
        self.dial = WheelDial(card.body, self.fonts, size=258)
        self.dial.pack()
        self.dial.set_range(self.cfg.steer.range_deg)

        info = ttk.Frame(card.body, style="Card.TFrame")
        info.pack(fill="x", pady=(10, 0))
        self.row_counts = StatRow(info, self.fonts, "Impulsy enkodera")
        self.row_counts.pack(fill="x")
        self.row_axis = StatRow(info, self.fonts, "Os wyjsciowa")
        self.row_axis.pack(fill="x")
        self.row_velocity = StatRow(info, self.fonts, "Predkosc obrotu")
        self.row_velocity.pack(fill="x")

        btns = ttk.Frame(card.body, style="Card.TFrame")
        btns.pack(fill="x", pady=(10, 0))
        ttk.Button(btns, text="Ustaw srodek", style="Accent.TButton",
                   command=self._center_wheel).pack(side="left")
        ttk.Button(btns, text="Reset plytki", style="Small.TButton",
                   command=self._reset_board).pack(side="left", padx=(8, 0))

        quick = Card(left, "Szybki zakres obrotu", self.fonts)
        quick.place_card(fill="x", pady=(10, 0))
        grid = ttk.Frame(quick.body, style="Card.TFrame")
        grid.pack(fill="x")
        self._range_buttons: Dict[int, ttk.Button] = {}
        for i, deg in enumerate(RANGE_PRESETS):
            b = ttk.Button(grid, text=f"{deg}°", width=7,
                           command=lambda d=deg: self._set_range(d))
            b.grid(row=i // 4, column=i % 4, padx=3, pady=3, sticky="ew")
            self._range_buttons[deg] = b
        for c in range(4):
            grid.grid_columnconfigure(c, weight=1)

        # ---------------- prawa kolumna ----------------
        right = ttk.Frame(parent, style="Root.TFrame")
        right.grid(row=0, column=1, sticky="nsew")
        right.grid_columnconfigure(0, weight=1)

        ped = Card(right, "Pedaly i osie pomocnicze", self.fonts)
        ped.grid_card(row=0, column=0, sticky="ew")
        self.dash_bars: Dict[str, BarMeter] = {}
        colors = {"throttle": theme.GOOD, "brake": theme.BAD,
                  "clutch": theme.WARN, "aux1": theme.ACCENT,
                  "aux2": theme.ACCENT, "aux3": theme.ACCENT}
        for key in PEDAL_KEYS:
            bar = BarMeter(ped.body, self.fonts, PEDAL_LABELS[key],
                           color=colors.get(key, theme.ACCENT), height=14)
            bar.pack(fill="x", pady=2)
            self.dash_bars[key] = bar

        btn_card = Card(right, "Przyciski", self.fonts)
        btn_card.grid_card(row=1, column=0, sticky="ew", pady=(10, 0))
        row = ttk.Frame(btn_card.body, style="Card.TFrame")
        row.pack(fill="x")
        self.button_grid = ButtonGrid(row, self.fonts)
        self.button_grid.pack(side="left", fill="x", expand=True)
        hat_box = ttk.Frame(row, style="Card.TFrame")
        hat_box.pack(side="right", padx=(14, 0))
        ttk.Label(hat_box, text="Krzyzak", style="Muted.TLabel").pack()
        self.hat_indicator = HatIndicator(hat_box, 72)
        self.hat_indicator.pack()
        self.lbl_dash_out = ttk.Label(btn_card.body, text="", style="Dim.TLabel",
                                      wraplength=560, justify="left")
        self.lbl_dash_out.pack(anchor="w", pady=(8, 0))

        out_card = Card(right, "Wyjscie do gier", self.fonts)
        out_card.grid_card(row=2, column=0, sticky="ew", pady=(10, 0))
        self.lbl_output = ttk.Label(out_card.body, text="-", style="Dim.TLabel",
                                    wraplength=560, justify="left")
        self.lbl_output.pack(anchor="w")
        self.lbl_output_hint = ttk.Label(out_card.body, text="", style="Warn.TLabel",
                                         wraplength=560, justify="left")
        self.lbl_output_hint.pack(anchor="w", pady=(6, 0))

        torq = ttk.Frame(out_card.body, style="Card.TFrame")
        torq.pack(fill="x", pady=(8, 0))
        ttk.Label(torq, text="Moment Force Feedback", style="Muted.TLabel"
                  ).pack(side="left", padx=(0, 10))
        self.torque_meter = TorqueMeter(torq, self.fonts, height=22)
        self.torque_meter.pack(side="left", fill="x", expand=True)

    # ---------------------------------------------------------- kierownica

    def _build_steer_tab(self, parent: ttk.Frame) -> None:
        scroll = ScrollableFrame(parent, style="Root.TFrame")
        scroll.pack(fill="both", expand=True)
        body = scroll.inner
        body.grid_columnconfigure(0, weight=1, uniform="s")
        body.grid_columnconfigure(1, weight=1, uniform="s")

        # --- zakres i enkoder ---
        c1 = Card(body, "Zakres obrotu i enkoder", self.fonts)
        c1.grid_card(row=0, column=0, sticky="nsew", padx=(0, 6), pady=(0, 10))

        self.sl_range = LabeledSlider(
            c1.body, self.fonts, "Zakres lock-to-lock", 90, 1800,
            self.cfg.steer.range_deg, command=self._on_range_slider,
            fmt="{:.0f}°", resolution=10, width=300,
            hint="Tyle stopni ma pelny obrot od oporu do oporu. "
                 "G29 ma fabrycznie 900°.")
        self.sl_range.pack(fill="x", pady=(0, 10))

        cpr = ttk.Frame(c1.body, style="Card.TFrame")
        cpr.pack(fill="x", pady=(0, 8))
        ttk.Label(cpr, text="Impulsy enkodera na obrot kierownicy (x4)",
                  style="Dim.TLabel").pack(anchor="w")
        row = ttk.Frame(cpr, style="Card.TFrame")
        row.pack(fill="x", pady=(3, 0))
        self.var_cpr = tk.StringVar(value=str(self.cfg.steer.counts_per_rev))
        ttk.Entry(row, textvariable=self.var_cpr, width=12).pack(side="left")
        ttk.Button(row, text="Zapisz", style="Small.TButton",
                   command=self._apply_cpr).pack(side="left", padx=6)
        ttk.Button(row, text="Wykryj automatycznie", style="Small.TButton",
                   command=self._detect_cpr).pack(side="left")
        ttk.Label(c1.body, style="Muted.TLabel", wraplength=330, justify="left",
                  text="Wykrywanie: ustaw srodek, przekrec kierownice do jednego "
                       "oporu, potem do drugiego i kliknij przycisk. Aplikacja "
                       "policzy impulsy z przejechanego zakresu.").pack(anchor="w")

        # --- krzywa i filtr ---
        c2 = Card(body, "Charakterystyka", self.fonts)
        c2.grid_card(row=0, column=1, sticky="nsew", padx=(6, 0), pady=(0, 10))

        self.sl_lin = LabeledSlider(
            c2.body, self.fonts, "Liniowosc", 0.4, 2.5,
            self.cfg.steer.linearity, command=self._on_steer_lin,
            fmt="{:.2f}", resolution=0.05, width=300,
            hint="1.00 = liniowo. Ponizej 1 kierownica reaguje ostrzej przy "
                 "srodku, powyzej 1 lagodniej.")
        self.sl_lin.pack(fill="x", pady=(0, 10))

        self.sl_steer_smooth = LabeledSlider(
            c2.body, self.fonts, "Wygladzanie", 0.0, 0.9,
            self.cfg.steer.smoothing, command=self._on_steer_smooth,
            fmt="{:.2f}", resolution=0.05, width=300,
            hint="Filtr drgan enkodera. Wieksza wartosc = gladziej, "
                 "ale z wiekszym opoznieniem. Zwykle 0.")
        self.sl_steer_smooth.pack(fill="x", pady=(0, 10))

        self.var_steer_inv = tk.BooleanVar(value=self.cfg.steer.invert)
        ttk.Checkbutton(c2.body, text="Odwroc kierunek obrotu",
                        variable=self.var_steer_inv,
                        command=self._on_steer_invert).pack(anchor="w")

        axis_row = ttk.Frame(c2.body, style="Card.TFrame")
        axis_row.pack(fill="x", pady=(10, 0))
        ttk.Label(axis_row, text="Os vJoy", style="Dim.TLabel").pack(side="left")
        self.var_steer_axis = tk.StringVar(value=self.cfg.steer.vjoy_axis)
        cb = ttk.Combobox(axis_row, textvariable=self.var_steer_axis, width=8,
                          state="readonly", values=list(VJOY_AXIS_CHOICES))
        cb.pack(side="left", padx=8)
        cb.bind("<<ComboboxSelected>>", lambda _e: self._on_steer_axis())
        ttk.Label(axis_row, text="G29 uzywa osi X", style="Muted.TLabel").pack(side="left")

        # --- miekki ogranicznik ---
        c3 = Card(body, "Miekki ogranicznik konca zakresu", self.fonts,
                     wrap=820,
                  subtitle="Dziala w firmware, niezaleznie od gry - przy "
                           "przekroczeniu zakresu silnik odpycha kierownice. "
                           "Wymaga podlaczonego mostka H.")
        c3.grid_card(row=1, column=0, columnspan=2, sticky="ew")
        self.var_endstop = tk.BooleanVar(value=self.cfg.steer.soft_endstop)
        ttk.Checkbutton(c3.body, text="Wlacz miekki ogranicznik",
                        variable=self.var_endstop,
                        command=self._on_endstop_toggle).pack(anchor="w")
        self.sl_endstop = LabeledSlider(
            c3.body, self.fonts, "Sila ogranicznika", 1, 200,
            self.cfg.steer.endstop_gain, command=self._on_endstop_gain,
            fmt="{:.0f}", resolution=1, width=300,
            hint="Moment na kazdy impuls przekroczenia zakresu.")
        self.sl_endstop.pack(fill="x", pady=(8, 0))

    # -------------------------------------------------------------- pedaly

    def _build_pedals_tab(self, parent: ttk.Frame) -> None:
        scroll = ScrollableFrame(parent, style="Root.TFrame")
        scroll.pack(fill="both", expand=True)
        body = scroll.inner
        body.grid_columnconfigure(0, weight=1, uniform="p")
        body.grid_columnconfigure(1, weight=1, uniform="p")

        head = Card(body, "Kreator kalibracji", self.fonts,
                     wrap=820,
                    subtitle="1. Kliknij 'Zacznij nasluchiwanie'.  "
                             "2. Wcisnij kazdy pedal do konca i puscic.  "
                             "3. Kliknij 'Zapisz zakresy'. "
                             "Aplikacja zapamietuje najmniejsza i najwieksza "
                             "zaobserwowana wartosc kazdego kanalu.")
        head.grid_card(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 10))
        hrow = ttk.Frame(head.body, style="Card.TFrame")
        hrow.pack(fill="x")
        ttk.Button(hrow, text="Zacznij nasluchiwanie", style="Accent.TButton",
                   command=self._calib_start).pack(side="left")
        ttk.Button(hrow, text="Zapisz zakresy",
                   command=self._calib_apply_all).pack(side="left", padx=8)
        ttk.Button(hrow, text="Przywroc pelny zakres", style="Small.TButton",
                   command=self._calib_reset).pack(side="left")
        self.lbl_calib = ttk.Label(head.body, text="", style="Dim.TLabel")
        self.lbl_calib.pack(anchor="w", pady=(8, 0))

        for i, key in enumerate(PEDAL_KEYS):
            card = Card(body, PEDAL_LABELS[key], self.fonts)
            card.grid_card(row=1 + i // 2, column=i % 2, sticky="nsew",
                           padx=(0, 6) if i % 2 == 0 else (6, 0), pady=(0, 10))
            self._pedal_widgets[key] = self._build_pedal_card(card, key)

    def _build_pedal_card(self, card: Card, key: str) -> dict:
        cfg = self.cfg.pedals[key]
        w: dict = {}

        src = ttk.Frame(card.body, style="Card.TFrame")
        src.pack(fill="x")
        ttk.Label(src, text="Kanal", style="Dim.TLabel").pack(side="left")
        values = ["wylaczony"] + [f"A{i}" for i in range(protocol.NUM_AXES)]
        var_src = tk.StringVar(value="wylaczony" if cfg.source < 0 else f"A{cfg.source}")
        cb = ttk.Combobox(src, textvariable=var_src, width=11, state="readonly",
                          values=values)
        cb.pack(side="left", padx=(8, 14))
        cb.bind("<<ComboboxSelected>>",
                lambda _e, k=key, v=var_src: self._on_pedal_source(k, v))
        w["src"] = var_src

        ttk.Label(src, text="Os vJoy", style="Dim.TLabel").pack(side="left")
        var_axis = tk.StringVar(value=cfg.vjoy_axis)
        cb2 = ttk.Combobox(src, textvariable=var_axis, width=7, state="readonly",
                           values=list(VJOY_AXIS_CHOICES))
        cb2.pack(side="left", padx=8)
        cb2.bind("<<ComboboxSelected>>",
                 lambda _e, k=key, v=var_axis: self._on_pedal_axis(k, v))
        w["axis"] = var_axis

        bar = BarMeter(card.body, self.fonts, "Wartosc wyjsciowa",
                       color=theme.GOOD if key == "throttle" else theme.ACCENT)
        bar.pack(fill="x", pady=(10, 2))
        w["bar"] = bar

        w["range"] = StatRow(card.body, self.fonts, "Zakres surowy")
        w["range"].pack(fill="x")
        w["seen"] = StatRow(card.body, self.fonts, "Zaobserwowano")
        w["seen"].pack(fill="x", pady=(0, 6))

        w["dz_lo"] = LabeledSlider(
            card.body, self.fonts, "Martwa strefa - poczatek", 0.0, 0.80,
            cfg.deadzone_low, fmt="{:.0%}", resolution=0.01, width=260,
            command=lambda v, k=key: self._on_pedal_field(k, "deadzone_low", v))
        w["dz_lo"].pack(fill="x", pady=2)

        w["dz_hi"] = LabeledSlider(
            card.body, self.fonts, "Martwa strefa - koniec", 0.0, 0.80,
            cfg.deadzone_high, fmt="{:.0%}", resolution=0.01, width=260,
            command=lambda v, k=key: self._on_pedal_field(k, "deadzone_high", v))
        w["dz_hi"].pack(fill="x", pady=2)

        w["gamma"] = LabeledSlider(
            card.body, self.fonts, "Krzywa (gamma)", 0.4, 4, cfg.gamma,
            fmt="{:.2f}", resolution=0.05, width=260,
            command=lambda v, k=key: self._on_pedal_field(k, "gamma", v))
        w["gamma"].pack(fill="x", pady=2)

        w["smooth"] = LabeledSlider(
            card.body, self.fonts, "Wygladzanie", 0.0, 0.9, cfg.smoothing,
            fmt="{:.2f}", resolution=0.05, width=260,
            command=lambda v, k=key: self._on_pedal_field(k, "smoothing", v))
        w["smooth"].pack(fill="x", pady=2)

        var_inv = tk.BooleanVar(value=cfg.invert)
        ttk.Checkbutton(card.body, text="Odwroc kierunek", variable=var_inv,
                        command=lambda k=key, v=var_inv:
                        self._on_pedal_field(k, "invert", bool(v.get()))
                        ).pack(anchor="w", pady=(6, 0))
        w["invert"] = var_inv
        return w

    # ----------------------------------------------------------- przyciski

    def _build_buttons_tab(self, parent: ttk.Frame) -> None:
        scroll = ScrollableFrame(parent, style="Root.TFrame")
        scroll.pack(fill="both", expand=True)
        body = scroll.inner
        body.grid_columnconfigure(0, weight=1)

        info = Card(body, "Mapowanie przyciskow", self.fonts,
                     wrap=820,
                    subtitle="Lewa kolumna to numer przycisku z plytki "
                             "(0 = pin 4, 1..31 = piny 22..52). Prawa to numer "
                             "przycisku w urzadzeniu vJoy widzianym przez gre. "
                             "0 oznacza 'nie wysylaj'.")
        info.grid_card(row=0, column=0, sticky="ew", pady=(0, 10))

        tools = ttk.Frame(info.body, style="Card.TFrame")
        tools.pack(fill="x")
        ttk.Button(tools, text="Mapowanie 1:1", style="Small.TButton",
                   command=self._buttons_identity).pack(side="left")
        ttk.Button(tools, text="Wyczysc wszystko", style="Small.TButton",
                   command=self._buttons_clear).pack(side="left", padx=6)
        ttk.Button(tools, text="Zdejmij wszystkie NC", style="Small.TButton",
                   command=self._buttons_clear_invert).pack(side="left")
        self.lbl_btn_live = ttk.Label(info.body, text="", style="Dim.TLabel")
        self.lbl_btn_live.pack(anchor="w", pady=(8, 0))
        self.lbl_btn_out = ttk.Label(info.body, text="", style="Good.TLabel")
        self.lbl_btn_out.pack(anchor="w")
        ttk.Label(info.body, style="Muted.TLabel", wraplength=820, justify="left",
                  text="Kolumna NC odwraca logike przycisku - dla stykow "
                       "normalnie zwartych (zwarty = puszczony). Diody i wiersz "
                       "'Wcisniete teraz' pokazuja stan JUZ po odwroceniu, wiec "
                       "efekt widac od razu."
                  ).pack(anchor="w", pady=(6, 0))

        hat = Card(body, "Krzyzak (POV)", self.fonts,
                     wrap=820,
                   subtitle="Przycisk przypisany do krzyzaka przestaje byc "
                            "wysylany jako zwykly przycisk. -1 = nieuzywany.")
        hat.grid_card(row=1, column=0, sticky="ew", pady=(0, 10))
        hrow = ttk.Frame(hat.body, style="Card.TFrame")
        hrow.pack(fill="x")
        choices = ["-1"] + [str(i) for i in range(protocol.NUM_BUTTONS)]
        for i, name in enumerate(("Gora", "Prawo", "Dol", "Lewo")):
            cell = ttk.Frame(hrow, style="Card.TFrame")
            cell.pack(side="left", padx=(0, 16))
            ttk.Label(cell, text=name, style="Dim.TLabel").pack(anchor="w")
            var = tk.StringVar(value=str(self.cfg.buttons.hat[i]))
            cb = ttk.Combobox(cell, textvariable=var, width=6, state="readonly",
                              values=choices)
            cb.pack()
            cb.bind("<<ComboboxSelected>>", lambda _e: self._on_hat_change())
            self._hat_vars.append(var)

        grid_card = Card(body, "Przyciski 0 - 31", self.fonts)
        grid_card.grid_card(row=2, column=0, sticky="ew")
        g = ttk.Frame(grid_card.body, style="Card.TFrame")
        g.pack(fill="x")
        self._btn_dots: List[StatusDot] = []
        vals = ["0"] + [str(i) for i in range(1, 129)]
        for i in range(protocol.NUM_BUTTONS):
            col = i // 8
            rowi = i % 8
            cell = ttk.Frame(g, style="Card.TFrame")
            cell.grid(row=rowi, column=col, padx=(0, 14), pady=2, sticky="w")
            dot = StatusDot(cell, 10)
            dot.pack(side="left", padx=(0, 6))
            self._btn_dots.append(dot)
            ttk.Label(cell, text=f"{i:>2}", style="Mono.TLabel",
                      width=3).pack(side="left")
            ttk.Label(cell, text="→", style="Muted.TLabel").pack(side="left",
                                                                      padx=4)
            var = tk.StringVar(value=str(self.cfg.buttons.mapping[i]))
            cb = ttk.Combobox(cell, textvariable=var, width=5, state="readonly",
                              values=vals)
            cb.pack(side="left")
            cb.bind("<<ComboboxSelected>>", lambda _e: self._on_button_map_change())
            self._button_map_vars.append(var)

            inv_var = tk.BooleanVar(value=self.cfg.buttons.invert[i])
            ttk.Checkbutton(cell, text="NC", variable=inv_var,
                            command=self._on_button_invert_change
                            ).pack(side="left", padx=(6, 0))
            self._button_invert_vars.append(inv_var)

    # ------------------------------------------------------------- tryb pada

    def _build_pad_tab(self, parent: ttk.Frame) -> None:
        parent.grid_columnconfigure(0, weight=1)

        # Pasek u gory jest celowo plaski - jeden wiersz zamiast karty.
        # Cala wysokosc zakladki idzie na liste przypisan, zeby miescila sie
        # bez przewijania; dluzsze wyjasnienie siedzi w karcie na dole.
        strip = tk.Frame(parent, bg=theme.PANEL, highlightthickness=1,
                         highlightbackground=theme.BORDER)
        strip.grid(row=0, column=0, sticky="ew", pady=(0, 12))
        bar = tk.Frame(strip, bg=theme.PANEL)
        bar.pack(fill="x", padx=15, pady=9)
        tick = tk.Frame(bar, bg=theme.ACCENT, width=3, height=13)
        tick.pack(side="left", padx=(0, 9))
        tick.pack_propagate(False)
        tk.Label(bar, text="TRYB PADA", bg=theme.PANEL, fg=theme.TEXT_DIM,
                 font=(self.fonts.family, 9, "bold")).pack(side="left")

        self.var_mode_pad = tk.StringVar(
            value=OUTPUT_MODE_LABELS[self.cfg.output.mode])
        cb = ttk.Combobox(bar, textvariable=self.var_mode_pad,
                          state="readonly",
                          values=[OUTPUT_MODE_LABELS[m] for m in OUTPUT_MODES],
                          width=44)
        cb.pack(side="right")
        cb.bind("<<ComboboxSelected>>", lambda _e: self._on_mode_change(
            self.var_mode_pad))
        tk.Label(bar, text="Tryb wyjscia", bg=theme.PANEL, fg=theme.TEXT_DIM,
                 font=self.fonts.small).pack(side="right", padx=(0, 9))
        # Stan pada stoi w tym samym wierszu co wybor trybu - osobny wiersz
        # pod spodem zabieral wysokosc liscie przypisan.
        self.lbl_pad_state = ttk.Label(bar, text="", style="Dim.TLabel")
        self.lbl_pad_state.pack(side="right", padx=(16, 24))

        # --- rysowany kontroler ---
        view = Card(parent, "Przypisania", self.fonts)
        view.grid_card(row=1, column=0, sticky="ew", pady=(0, 12))
        # Narzedzia ida do naglowka karty - pod rysunkiem wypadaly poza
        # widoczny obszar zakladki. Dwa zestawy wymieniaja sie w miejscu,
        # wiec wlaczenie kreatora nie zmienia wysokosci zakladki.
        self._pad_tools = ttk.Frame(view.right_slot, style="Card.TFrame")
        self._pad_tools.pack(side="left")
        ttk.Button(self._pad_tools, text="Przypisz po kolei",
                   style="Accent.TButton",
                   command=self._pad_wizard_start).pack(side="left")
        ttk.Button(self._pad_tools, text="Domyslne",
                   style="Small.TButton",
                   command=self._pad_defaults).pack(side="left", padx=(6, 0))
        ttk.Button(self._pad_tools, text="Wyczysc wszystko",
                   style="Small.TButton",
                   command=self._pad_clear_all).pack(side="left", padx=(6, 0))

        self._pad_wizard_tools = ttk.Frame(view.right_slot,
                                           style="Card.TFrame")
        ttk.Button(self._pad_wizard_tools, text="Wstecz",
                   style="Small.TButton",
                   command=self._pad_wizard_back).pack(side="left")
        ttk.Button(self._pad_wizard_tools, text="Pomin",
                   style="Small.TButton",
                   command=self._pad_wizard_skip).pack(side="left", padx=(6, 0))
        ttk.Button(self._pad_wizard_tools, text="Wyczysc i pomin",
                   style="Small.TButton",
                   command=self._pad_wizard_clear).pack(side="left", padx=(6, 0))
        ttk.Button(self._pad_wizard_tools, text="Zakoncz",
                   style="Accent.TButton",
                   command=self._pad_wizard_stop).pack(side="left", padx=(6, 0))

        self.lbl_pad_live = ttk.Label(view.body, text="", style="Good.TLabel")
        self.lbl_pad_live.pack(anchor="w", pady=(0, 6))

        # Ostrzezenia pokazujemy tylko wtedy, gdy jest co powiedziec - pusta
        # etykieta zjadalaby wiersz, a zakladka jest wypelniona co do piksela.
        self.lbl_pad_conflict = ttk.Label(view.body, text="",
                                          style="Warn.TLabel",
                                          wraplength=1080, justify="left")

        holder = ttk.Frame(view.body, style="Card.TFrame")
        holder.pack(fill="x")
        self._pad_holder = holder
        self.pad_view = PadView(holder, self.fonts,
                                on_assign_request=self._on_pad_assign_request,
                                on_clear=self._on_pad_clear,
                                on_axis_click=self._on_pad_axis_click,
                                on_axis_clear=self._on_pad_axis_clear)
        self.pad_view.pack(fill="x")
        self.root.bind("<Escape>", lambda _e: self._on_escape())


        # --- osie ---
        axes = Card(parent, "Osie analogowe", self.fonts)
        axes.grid_card(row=2, column=0, sticky="ew")
        cols = ttk.Frame(axes.body, style="Card.TFrame")
        cols.pack(fill="x")
        cols.grid_columnconfigure(0, weight=1, uniform="pa")
        cols.grid_columnconfigure(1, weight=1, uniform="pa")

        left = ttk.Frame(cols, style="Card.TFrame")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 18))
        right = ttk.Frame(cols, style="Card.TFrame")
        right.grid(row=0, column=1, sticky="nsew")

        self.sl_pad_range = LabeledSlider(
            left, self.fonts, "Zakres kierownicy na pelne wychylenie drazka",
            45, 1080, self.cfg.pad.steer_range_deg, fmt="{:.0f}\u00b0",
            resolution=15, width=320,
            command=lambda v: self._on_pad_field("steer_range_deg", int(v)),
            hint="Pad ma maly zakres drazka. Dla gier zrecznosciowych 270-450 "
                 "stopni daje naturalne prowadzenie; pelne 900 sprawi, ze auto "
                 "ledwo skreca.")
        self.sl_pad_range.pack(fill="x", pady=(0, 10))

        self.sl_pad_dz = LabeledSlider(
            left, self.fonts, "Martwa strefa wokol srodka", 0.0, 0.40,
            self.cfg.pad.steer_deadzone, fmt="{:.0%}", resolution=0.01, width=320,
            command=lambda v: self._on_pad_field("steer_deadzone", v),
            hint="Wlasna martwa strefa aplikacji. Zostaw 0, jesli auto nie "
                 "drga, gdy trzymasz kierownice na srodku.")
        self.sl_pad_dz.pack(fill="x", pady=(0, 10))

        self.sl_pad_anti = LabeledSlider(
            left, self.fonts, "Kompensacja martwej strefy gry", 0.0, 0.60,
            self.cfg.pad.steer_antideadzone, fmt="{:.0%}", resolution=0.01,
            width=320,
            command=lambda v: self._on_pad_field("steer_antideadzone", v),
            hint="Na tym choruje Forza: gra sama ucina poczatek wychylenia "
                 "drazka, wiec pierwsze stopnie obrotu kierownicy nic nie "
                 "robia. Tu najmniejszy ruch od razu przebija prog gry. "
                 "Najpierw ustaw w grze martwa strefe wewnetrzna skretu na 0, "
                 "potem podnos ten suwak, az auto reaguje natychmiast.")
        self.sl_pad_anti.pack(fill="x", pady=(0, 10))

        self.sl_pad_lin = LabeledSlider(
            left, self.fonts, "Krzywa skretu", 0.3, 3.0,
            self.cfg.pad.steer_linearity, fmt="{:.2f}", resolution=0.05,
            width=320,
            command=lambda v: self._on_pad_field("steer_linearity", v),
            hint="1.00 = liniowo. Ponizej 1 srodek jest czulszy, powyzej 1 "
                 "spokojniejszy przy srodku, a agresywny na koncach.")
        self.sl_pad_lin.pack(fill="x")

        # --- prawa kolumna: profil gry i pozostale nastawy ---
        prof = ttk.Frame(right, style="Card.TFrame")
        prof.pack(fill="x")
        ttk.Label(prof, text="Profil gry", style="Dim.TLabel").pack(side="left")
        self.var_pad_profile = tk.StringVar(
            value=PAD_PROFILE_LABELS[self.cfg.pad.detect_profile()])
        prof_box = ttk.Combobox(
            prof, textvariable=self.var_pad_profile, state="readonly", width=28,
            values=[PAD_PROFILE_LABELS[k] for k in PAD_PROFILE_ORDER])
        prof_box.pack(side="left", padx=(10, 0))
        prof_box.bind("<<ComboboxSelected>>", lambda _e: self._on_pad_profile())

        ttk.Label(right, style="Muted.TLabel", wraplength=520, justify="left",
                  text="Profil ustawia tylko skalowanie skretu i pedalow pod "
                       "dana gre. Przypisania przyciskow i kierunkow zostaja "
                       "nietkniete - zmiana profilu ich nie rusza."
                  ).pack(anchor="w", pady=(8, 0))

        self.sl_pad_trig = LabeledSlider(
            right, self.fonts, "Kompensacja martwej strefy spustow", 0.0, 0.60,
            self.cfg.pad.trigger_antideadzone, fmt="{:.0%}", resolution=0.01,
            width=320,
            command=lambda v: self._on_pad_field("trigger_antideadzone", v),
            hint="To samo dla gazu i hamulca na LT/RT - przydaje sie, gdy gra "
                 "ignoruje pierwsze procenty wcisniecia pedalu.")
        self.sl_pad_trig.pack(fill="x", pady=(10, 0))

        self.var_pad_inv = tk.BooleanVar(value=self.cfg.pad.steer_invert)
        ttk.Checkbutton(right, text="Odwroc kierunek skretu na padzie",
                        variable=self.var_pad_inv,
                        command=self._on_pad_invert).pack(anchor="w",
                                                          pady=(10, 0))

        ttk.Label(right, style="Muted.TLabel", wraplength=520, justify="left",
                  text="Spusty Xboxa maja 8 bitow (0-255), wiec pedaly sa tu "
                       "mniej precyzyjne niz w trybie kierownicy. To "
                       "ograniczenie XInputa, nie aplikacji."
                  ).pack(anchor="w", pady=(8, 0))
        ttk.Label(right, style="Muted.TLabel", wraplength=520, justify="left",
                  text="Forza, NFS Unbound i inne gry z obsluga kierownic "
                       "najlepiej dzialaja w trybie Kierownica G29 - widza "
                       "wtedy prawdziwa kierownice i ustawienia pada nie sa "
                       "potrzebne. Tryb pada zostaw dla gier bez kierownic."
                  ).pack(anchor="w", pady=(8, 0))


    # ------------------------------------------------------- force feedback

    def _build_ffb_tab(self, parent: ttk.Frame) -> None:
        scroll = ScrollableFrame(parent, style="Root.TFrame")
        scroll.pack(fill="both", expand=True)
        body = scroll.inner
        body.grid_columnconfigure(0, weight=1, uniform="f")
        body.grid_columnconfigure(1, weight=1, uniform="f")

        warn = Card(body, "Zanim wlaczysz", self.fonts,
                     wrap=820,
                    subtitle="Force Feedback steruje silnikiem przez mostek H "
                             "na pinach 5 (EN), 6 (RPWM) i 7 (LPWM). Jesli nie "
                             "masz podlaczonego silnika, mozesz wlaczyc FFB "
                             "bezpiecznie - nic sie nie stanie. Przy pierwszym "
                             "uruchomieniu z silnikiem zacznij od malego "
                             "wzmocnienia i trzymaj rece na kierownicy.")
        warn.grid_card(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 10))
        self.var_ffb_on = tk.BooleanVar(value=self.cfg.ffb.enabled)
        ttk.Checkbutton(warn.body, text="Wlacz Force Feedback",
                        variable=self.var_ffb_on,
                        command=self._on_ffb_toggle).pack(anchor="w")
        self.lbl_ffb_state = ttk.Label(warn.body, text="", style="Dim.TLabel",
                                       wraplength=820, justify="left")
        self.lbl_ffb_state.pack(anchor="w", pady=(6, 0))

        live = ttk.Frame(warn.body, style="Card.TFrame")
        live.pack(fill="x", pady=(10, 0))
        ttk.Label(live, text="Moment wysylany do plytki",
                  style="Muted.TLabel").pack(anchor="w")
        self.ffb_meter = TorqueMeter(live, self.fonts)
        self.ffb_meter.pack(fill="x", pady=(3, 0))

        # --- sila ---
        c1 = Card(body, "Sila", self.fonts)
        c1.grid_card(row=1, column=0, sticky="nsew", padx=(0, 6), pady=(0, 10))
        f = self.cfg.ffb
        self.sl_gain = LabeledSlider(
            c1.body, self.fonts, "Wzmocnienie globalne", 0.0, 1.0,
            f.overall_gain, fmt="{:.0%}", resolution=0.01, width=300,
            command=lambda v: self._on_ffb_field("overall_gain", v))
        self.sl_gain.pack(fill="x", pady=(0, 8))

        self.sl_maxf = LabeledSlider(
            c1.body, self.fonts, "Ograniczenie maksimum", 0.05, 1.0,
            f.max_force, fmt="{:.0%}", resolution=0.01, width=300,
            command=lambda v: self._on_ffb_field("max_force", v),
            hint="Chroni silnik i zasilacz - obcina szczytowy moment.")
        self.sl_maxf.pack(fill="x", pady=(0, 8))

        self.sl_minf = LabeledSlider(
            c1.body, self.fonts, "Sila minimalna", 0.0, 0.5, f.min_force,
            fmt="{:.0%}", resolution=0.01, width=300,
            command=lambda v: self._on_ffb_field("min_force", v),
            hint="Dodawana do kazdego niezerowego momentu, zeby pokonac "
                 "tarcie przekladni. Za duza wartosc daje szarpanie.")
        self.sl_minf.pack(fill="x", pady=(0, 8))

        self.sl_dead = LabeledSlider(
            c1.body, self.fonts, "Martwa strefa momentu", 0.0, 0.2, f.deadband,
            fmt="{:.1%}", resolution=0.005, width=300,
            command=lambda v: self._on_ffb_field("deadband", v))
        self.sl_dead.pack(fill="x", pady=(0, 8))

        self.var_ffb_inv = tk.BooleanVar(value=f.invert)
        ttk.Checkbutton(c1.body, text="Odwroc kierunek dzialania silnika",
                        variable=self.var_ffb_inv,
                        command=self._on_ffb_invert).pack(anchor="w")

        # --- efekty wlasne ---
        c2 = Card(body, "Efekty generowane lokalnie", self.fonts,
                  subtitle="Dzialaja takze w grach, ktore nie wysylaja FFB.")
        c2.grid_card(row=1, column=1, sticky="nsew", padx=(6, 0), pady=(0, 10))
        self.sl_spring = LabeledSlider(
            c2.body, self.fonts, "Sprezyna (samocentrowanie)", 0.0, 1.0,
            f.spring_gain, fmt="{:.0%}", resolution=0.01, width=300,
            command=lambda v: self._on_ffb_field("spring_gain", v))
        self.sl_spring.pack(fill="x", pady=(0, 8))
        self.sl_damper = LabeledSlider(
            c2.body, self.fonts, "Tlumienie", 0.0, 1.0, f.damper_gain,
            fmt="{:.0%}", resolution=0.01, width=300,
            command=lambda v: self._on_ffb_field("damper_gain", v))
        self.sl_damper.pack(fill="x", pady=(0, 8))
        self.sl_friction = LabeledSlider(
            c2.body, self.fonts, "Tarcie", 0.0, 1.0, f.friction_gain,
            fmt="{:.0%}", resolution=0.01, width=300,
            command=lambda v: self._on_ffb_field("friction_gain", v))
        self.sl_friction.pack(fill="x", pady=(0, 8))

        # --- efekty z gry ---
        c3 = Card(body, "Skalowanie efektow z gry", self.fonts)
        c3.grid_card(row=2, column=0, sticky="nsew", padx=(0, 6), pady=(0, 10))
        for label, name in (("Sila stala / narastanie", "game_constant_gain"),
                            ("Efekty okresowe (wibracje)", "game_periodic_gain"),
                            ("Sprezyna z gry", "game_spring_gain"),
                            ("Tlumienie i tarcie z gry", "game_damper_gain")):
            sl = LabeledSlider(c3.body, self.fonts, label, 0.0, 2.0,
                               getattr(f, name), fmt="{:.0%}", resolution=0.05,
                               width=300,
                               command=lambda v, n=name: self._on_ffb_field(n, v))
            sl.pack(fill="x", pady=(0, 8))

        dirrow = ttk.Frame(c3.body, style="Card.TFrame")
        dirrow.pack(fill="x", pady=(4, 0))
        ttk.Label(dirrow, text="Interpretacja kierunku", style="Dim.TLabel").pack(anchor="w")
        self.var_dir_mode = tk.StringVar(
            value=DIRECTION_LABELS.get(self.engine.ffb.direction_mode,
                                       DIRECTION_LABELS["auto"]))
        cb = ttk.Combobox(dirrow, textvariable=self.var_dir_mode, state="readonly",
                          values=[DIRECTION_LABELS[m] for m in DIRECTION_MODES],
                          width=36)
        cb.pack(anchor="w", pady=(3, 0))
        cb.bind("<<ComboboxSelected>>", lambda _e: self._on_dir_mode())
        ttk.Label(c3.body, style="Muted.TLabel", wraplength=330, justify="left",
                  text="Gry roznie koduja kierunek sily. Jesli FFB dziala "
                       "odwrotnie albo tylko w jedna strone, zmien ten tryb."
                  ).pack(anchor="w", pady=(4, 0))

        # --- mostek H ---
        c4 = Card(body, "Mostek H i bezpieczenstwo", self.fonts)
        c4.grid_card(row=2, column=1, sticky="nsew", padx=(6, 0), pady=(0, 10))
        self.sl_minduty = LabeledSlider(
            c4.body, self.fonts, "PWM minimalne", 0, 254, f.min_duty,
            fmt="{:.0f}", resolution=1, width=300,
            command=lambda v: self._on_ffb_field("min_duty", int(v)))
        self.sl_minduty.pack(fill="x", pady=(0, 8))
        self.sl_maxduty = LabeledSlider(
            c4.body, self.fonts, "PWM maksymalne", 1, 255, f.max_duty,
            fmt="{:.0f}", resolution=1, width=300,
            command=lambda v: self._on_ffb_field("max_duty", int(v)))
        self.sl_maxduty.pack(fill="x", pady=(0, 8))
        self.sl_slew = LabeledSlider(
            c4.body, self.fonts, "Max zmiana momentu na ms", 256, 65280,
            f.slew_per_ms, fmt="{:.0f}", resolution=256, width=300,
            command=lambda v: self._on_ffb_field("slew_per_ms", int(v)),
            hint="Ogranicza szarpniecia. 65280 praktycznie wylacza limit.")
        self.sl_slew.pack(fill="x", pady=(0, 8))
        self.sl_timeout = LabeledSlider(
            c4.body, self.fonts, "Watchdog plytki", 20, 1000, f.timeout_ms,
            fmt="{:.0f} ms", resolution=10, width=300,
            command=lambda v: self._on_ffb_field("timeout_ms", int(v)),
            hint="Po tym czasie bez komendy z PC firmware zwalnia silnik.")
        self.sl_timeout.pack(fill="x", pady=(0, 8))
        self.sl_rate = LabeledSlider(
            c4.body, self.fonts, "Czestotliwosc FFB", 50, 1000, f.update_hz,
            fmt="{:.0f} Hz", resolution=10, width=300,
            command=lambda v: self._on_ffb_field("update_hz", int(v)))
        self.sl_rate.pack(fill="x")

    # ---------------------------------------------------------- diagnostyka

    def _build_diag_tab(self, parent: ttk.Frame) -> None:
        parent.grid_columnconfigure(0, weight=0)
        parent.grid_columnconfigure(1, weight=1)

        # --- lacze ---
        c1 = Card(parent, "Lacze szeregowe", self.fonts)
        c1.grid_card(row=0, column=0, sticky="new", padx=(0, 10))
        self.diag_rows: Dict[str, StatRow] = {}
        for key, label in (("port", "Port"), ("fw", "Firmware"),
                           ("fps", "Ramki na sekunde"), ("bps", "Przeplyw"),
                           ("lat", "Opoznienie"), ("crc", "Bledy CRC"),
                           ("drop", "Zgubione ramki"), ("resync", "Resynchronizacje"),
                           ("total", "Ramek lacznie"), ("up", "Czas pracy")):
            r = StatRow(c1.body, self.fonts, label)
            r.pack(fill="x")
            self.diag_rows[key] = r

        ttk.Label(c1.body, text="Ramki / s", style="Muted.TLabel").pack(
            anchor="w", pady=(10, 2))
        self.spark_fps = Sparkline(c1.body, width=250, height=40, color=theme.GOOD)
        self.spark_fps.pack(fill="x")
        ttk.Label(c1.body, text="Opoznienie [ms]", style="Muted.TLabel").pack(
            anchor="w", pady=(8, 2))
        self.spark_lat = Sparkline(c1.body, width=250, height=40, color=theme.WARN)
        self.spark_lat.pack(fill="x")

        rate_row = ttk.Frame(c1.body, style="Card.TFrame")
        rate_row.pack(fill="x", pady=(10, 0))
        self.sl_report = LabeledSlider(
            rate_row, self.fonts, "Czestotliwosc ramek plytki", 50, 500,
            self.cfg.link.report_hz, fmt="{:.0f} Hz", resolution=
            10, width=250, command=self._on_report_hz)
        self.sl_report.pack(fill="x")

        # --- prawa kolumna: G29 nad vJoy ---
        rightcol = ttk.Frame(parent, style="Root.TFrame")
        rightcol.grid(row=0, column=1, sticky="new")
        self._build_g29_card(rightcol)

        # --- vJoy ---
        c2 = Card(rightcol, "Urzadzenie wirtualne (vJoy)", self.fonts)
        c2.place_card(fill="x")
        row = ttk.Frame(c2.body, style="Card.TFrame")
        row.pack(fill="x")
        ttk.Label(row, text="Numer urzadzenia", style="Dim.TLabel").pack(side="left")
        self.var_vjoy_id = tk.StringVar(value=str(self.cfg.output.vjoy_device))
        cb = ttk.Combobox(row, textvariable=self.var_vjoy_id, width=5,
                          state="readonly",
                          values=[str(i) for i in range(1, 17)])
        cb.pack(side="left", padx=8)
        cb.bind("<<ComboboxSelected>>", lambda _e: self._on_vjoy_device())
        ttk.Button(row, text="Podlacz ponownie", style="Small.TButton",
                   command=lambda: self._reopen_output(show_errors=True)
                   ).pack(side="left", padx=(0, 6))
        ttk.Button(row, text="Sprawdz vJoy", style="Small.TButton",
                   command=self._show_vjoy_diag).pack(side="left")

        br = ttk.Frame(c2.body, style="Card.TFrame")
        br.pack(fill="x", pady=(8, 0))
        ttk.Label(br, text="Tryb wyjscia", style="Dim.TLabel").pack(anchor="w")
        self.var_mode_diag = tk.StringVar(
            value=OUTPUT_MODE_LABELS[self.cfg.output.mode])
        mode_box = ttk.Combobox(br, textvariable=self.var_mode_diag,
                                state="readonly", width=46,
                                values=[OUTPUT_MODE_LABELS[m] for m in OUTPUT_MODES])
        mode_box.pack(anchor="w", pady=(3, 0))
        mode_box.bind("<<ComboboxSelected>>", lambda _e: self._on_backend())

        self.txt_vjoy = tk.Text(c2.body, height=9, bg=theme.PANEL_HI,
                                fg=theme.TEXT_DIM, font=self.fonts.mono_small,
                                relief="flat", bd=0, wrap="word",
                                insertbackground=theme.TEXT, padx=10, pady=8)
        self.txt_vjoy.pack(fill="both", expand=True, pady=(10, 0))
        self.txt_vjoy.configure(state="disabled")

        hint = ttk.Frame(c2.body, style="Card.TFrame")
        hint.pack(fill="x", pady=(8, 0))
        ttk.Button(hint, text="Pobierz vJoy", style="Small.TButton",
                   command=lambda: webbrowser.open(VJOY_URL)).pack(side="left")
        ttk.Label(hint, text="Potrzebne, zeby gry widzialy kierownice",
                  style="Muted.TLabel").pack(side="left", padx=8)

        # --- co ktora gra potrafi zobaczyc ---
        c25 = Card(parent, "Zgodnosc z grami", self.fonts)
        c25.grid_card(row=1, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        table = ttk.Frame(c25.body, style="Card.TFrame")
        table.pack(fill="x")
        # rozciaga sie opis, nie nazwa gry - inaczej tryb odjezdzal
        # na srodek karty, daleko od gry, ktorej dotyczy
        table.grid_columnconfigure(1, minsize=300)
        table.grid_columnconfigure(3, weight=1)
        for row, (games, mode, why) in enumerate(GAME_COMPAT):
            tk.Frame(table, bg=theme.ACCENT if row == 0 else theme.BORDER_HI,
                     width=3, height=14).grid(row=row, column=0, sticky="w",
                                              padx=(0, 9), pady=3)
            ttk.Label(table, text=games, style="Dim.TLabel").grid(
                row=row, column=1, sticky="w", pady=3)
            ttk.Label(table, text=mode, style="Value.TLabel").grid(
                row=row, column=2, sticky="w", padx=(14, 14), pady=3)
            ttk.Label(table, text=why, style="Muted.TLabel", wraplength=560,
                      justify="left").grid(row=row, column=3, sticky="w",
                                           pady=3)
        ttk.Label(c25.body, style="Muted.TLabel", wraplength=1040,
                  justify="left",
                  text="Force Feedback z gry dziala tylko w trybie vJoy - "
                       "tamta droga idzie HID PID przez sterownik vJoy. W "
                       "trybie G29 gra wysyla FFB do sterownika Logitecha, "
                       "ktorego tu nie ma, a w trybie pada XInput w ogole nie "
                       "ma kanalu zwrotnego. W obu zostaja efekty wyliczane "
                       "lokalnie (sprezyna, tlumienie, tarcie) z zakladki "
                       "Force Feedback."
                  ).pack(anchor="w", pady=(10, 0))

        # --- dziennik ---
        c3 = Card(parent, "Dziennik", self.fonts)
        c3.grid_card(row=2, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        wrap = ttk.Frame(c3.body, style="Card.TFrame")
        wrap.pack(fill="both", expand=True)
        self.txt_log = tk.Text(wrap, height=9, bg=theme.PANEL_HI, fg=theme.TEXT_DIM,
                               font=self.fonts.mono_small, relief="flat", bd=0,
                               wrap="word", padx=10, pady=8)
        bar = ttk.Scrollbar(wrap, orient="vertical", command=self.txt_log.yview)
        self.txt_log.configure(yscrollcommand=bar.set, state="disabled")
        self.txt_log.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        for level, color in theme.LEVEL_COLORS.items():
            self.txt_log.tag_configure(level, foreground=color)

        tools = ttk.Frame(c3.body, style="Card.TFrame")
        tools.pack(fill="x", pady=(8, 0))
        ttk.Button(tools, text="Wyczysc", style="Small.TButton",
                   command=self._clear_log).pack(side="left")

    def _build_g29_card(self, parent: ttk.Frame) -> None:
        card = Card(parent, "Kierownica G29 (Native USB)", self.fonts)
        card.place_card(fill="x", pady=(0, 10))
        self.g29_rows: Dict[str, StatRow] = {}
        for key, label in (("fw", "Firmware z trybem G29"),
                           ("usb", "Windows widzi G29"),
                           ("feed", "Raporty z aplikacji"),
                           ("game", "Komendy od gry")):
            r = StatRow(card.body, self.fonts, label)
            r.pack(fill="x")
            self.g29_rows[key] = r
        self.lbl_g29_report = ttk.Label(card.body, text="", style="Mono.TLabel")
        self.lbl_g29_report.pack(anchor="w", pady=(8, 0))

        self.var_g29_inv = tk.BooleanVar(value=self.cfg.g29.pedals_inverted)
        ttk.Checkbutton(card.body,
                        text="Pedaly jak w oryginalnym G29 (255 = puszczony)",
                        variable=self.var_g29_inv,
                        command=self._on_g29_invert).pack(anchor="w",
                                                          pady=(8, 0))
        ttk.Label(card.body, style="Muted.TLabel", wraplength=600,
                  justify="left",
                  text="Podlacz dwa kable: Programming Port (lacze z ta "
                       "aplikacja) i Native USB (kierownica dla gier). Gdy "
                       "aplikacja jest zamknieta, G29 stoi w pozycji "
                       "spoczynkowej - kalibracje i mapowanie liczy tylko ona."
                  ).pack(anchor="w", pady=(8, 0))
        ttk.Label(card.body, style="Muted.TLabel", wraplength=600,
                  justify="left", text=G29_BUTTON_HELP
                  ).pack(anchor="w", pady=(6, 0))

    def _refresh_g29_card(self, connected: bool) -> None:
        st = self.engine.status
        rows = self.g29_rows
        if not connected:
            for r in rows.values():
                r.set("-", theme.MUTED)
            self.lbl_g29_report.configure(text="")
            return
        rows["fw"].set("tak" if st.g29_fw else "nie - wgraj firmware 2.1",
                       theme.GOOD if st.g29_fw else theme.BAD)
        if not st.g29_fw:
            rows["usb"].set("-", theme.MUTED)
        else:
            rows["usb"].set("tak" if st.g29_usb
                            else "nie - podlacz kabel do Native USB",
                            theme.GOOD if st.g29_usb else theme.BAD)
        if not self.cfg.output.g29_on:
            rows["feed"].set("wylaczone - inny tryb wyjscia", theme.MUTED)
        elif st.g29_fed:
            rows["feed"].set("docieraja", theme.GOOD)
        else:
            rows["feed"].set("brak", theme.WARN)
        game = str(st.g29_out_packets)
        if st.g29_game_range:
            game += f"  (zakres {st.g29_game_range} st.)"
        rows["game"].set(game)
        rep = self.engine.last_g29 if self.cfg.output.g29_on else None
        self.lbl_g29_report.configure(
            text=("raport: " + rep.hex(" ")) if rep else "")

    def _on_g29_invert(self) -> None:
        self.cfg.g29.pedals_inverted = bool(self.var_g29_inv.get())

    # --------------------------------------------------------- pasek stanu

    def _build_statusbar(self, parent: ttk.Frame) -> None:
        tk.Frame(parent, bg=theme.BORDER, height=1).pack(fill="x", pady=(12, 0))
        bar = tk.Frame(parent, bg=theme.BG)
        bar.pack(fill="x", pady=(10, 0))
        self.lbl_status = tk.Label(bar, text="", bg=theme.BG, fg=theme.MUTED,
                                   font=self.fonts.small, anchor="w")
        self.lbl_status.pack(side="left")
        ttk.Button(bar, text="Zapisz ustawienia", style="Accent.TButton",
                   command=self._save_config).pack(side="right")

    # ================================================== obsluga interfejsu

    def _refresh_ports(self) -> None:
        ports = list_ports()
        labels = ["Automatycznie (wykryj plytke)"]
        self._port_labels = {labels[0]: ""}
        for p in ports:
            labels.append(p.label)
            self._port_labels[p.label] = p.device
        self.port_combo.configure(values=labels)

        current = self.cfg.link.port
        chosen = labels[0]
        for label, dev in self._port_labels.items():
            if dev and dev == current:
                chosen = label
                break
        self.port_var.set(chosen)

    def _selected_port(self) -> str:
        return self._port_labels.get(self.port_var.get(), "")

    def _toggle_connect(self) -> None:
        if self.engine.link.state in (link_mod.CONNECTED, link_mod.SCANNING):
            self.engine.disconnect()
            self._log("info", "Rozlaczono recznie")
        else:
            self.cfg.link.port = self._selected_port()
            self.engine.connect(self.cfg.link.port or None)

    def _center_wheel(self) -> None:
        self.engine.center_wheel()
        self.engine.reset_raw_tracking()

    def _reset_board(self) -> None:
        if messagebox.askyesno("Reset plytki",
                               "Wyslac reset do Arduino?\n\n"
                               "Force Feedback zostanie wylaczony, a srodek "
                               "kierownicy ustawiony na biezaca pozycje.",
                               parent=self.root):
            self.engine.reset_board()
            self.var_ffb_on.set(False)
            self.cfg.ffb.enabled = False

    def _set_range(self, degrees: int) -> None:
        self.cfg.steer.range_deg = degrees
        self.sl_range.set(degrees)
        self.dial.set_range(degrees)
        self.engine.apply_config()
        self._log("ok", f"Zakres obrotu: {degrees} stopni")

    def _on_range_slider(self, value: float) -> None:
        self.cfg.steer.range_deg = int(value)
        self.dial.set_range(value)
        self.engine.apply_config()

    def _on_steer_lin(self, value: float) -> None:
        self.cfg.steer.linearity = float(value)

    def _on_steer_smooth(self, value: float) -> None:
        self.cfg.steer.smoothing = float(value)

    def _on_steer_invert(self) -> None:
        self.cfg.steer.invert = bool(self.var_steer_inv.get())
        self.engine.processor.steer.reset()

    def _on_steer_axis(self) -> None:
        self.cfg.steer.vjoy_axis = self.var_steer_axis.get()
        self.engine.apply_config()

    def _on_endstop_toggle(self) -> None:
        self.cfg.steer.soft_endstop = bool(self.var_endstop.get())
        self.engine.apply_config()

    def _on_endstop_gain(self, value: float) -> None:
        self.cfg.steer.endstop_gain = int(value)
        self.engine.apply_config()

    def _apply_cpr(self) -> None:
        try:
            value = int(self.var_cpr.get())
        except ValueError:
            messagebox.showwarning("Impulsy enkodera",
                                   "Podaj liczbe calkowita.", parent=self.root)
            return
        if value < 4:
            messagebox.showwarning("Impulsy enkodera",
                                   "Wartosc musi byc co najmniej 4.",
                                   parent=self.root)
            return
        self.cfg.steer.counts_per_rev = value
        self.engine.apply_config()
        self.var_cpr.set(str(self.cfg.steer.counts_per_rev))
        self._log("ok", f"Impulsy na obrot: {self.cfg.steer.counts_per_rev}")

    def _detect_cpr(self) -> None:
        cpr = self.engine.detect_counts_per_rev()
        if cpr is None:
            messagebox.showinfo(
                "Wykrywanie enkodera",
                "Za maly przejechany zakres.\n\n"
                "Ustaw srodek, przekrec kierownice do jednego oporu, potem do "
                "drugiego i kliknij ponownie.", parent=self.root)
            return
        self.cfg.steer.counts_per_rev = cpr
        self.var_cpr.set(str(cpr))
        self.engine.apply_config()
        self._log("ok", f"Wykryto {cpr} impulsow na obrot kierownicy")

    # --- pedaly ---

    def _on_pedal_source(self, key: str, var: tk.StringVar) -> None:
        text = var.get()
        self.cfg.pedals[key].source = -1 if text == "wylaczony" else int(text[1:])
        self.engine.processor.pedals[key].reset()
        self.engine.apply_config()

    def _on_pedal_axis(self, key: str, var: tk.StringVar) -> None:
        self.cfg.pedals[key].vjoy_axis = var.get()
        self.engine.apply_config()

    def _on_pedal_field(self, key: str, field: str, value) -> None:
        setattr(self.cfg.pedals[key], field, value)
        self.cfg.normalize()

    def _calib_start(self) -> None:
        self.engine.reset_raw_tracking()
        self.lbl_calib.configure(
            text="Nasluchiwanie... wcisnij kazdy pedal do konca i puscic, "
                 "potem kliknij 'Zapisz zakresy'.")
        self._log("info", "Rozpoczeto nasluchiwanie kalibracji pedalow")

    def _calib_apply_all(self) -> None:
        ok, skipped = [], []
        for key in PEDAL_KEYS:
            if self.cfg.pedals[key].source < 0:
                continue
            if self.engine.calibrate_pedal_from_seen(key):
                ok.append(PEDAL_LABELS[key])
            else:
                skipped.append(PEDAL_LABELS[key])
        self._sync_pedal_widgets()
        if ok:
            self._log("ok", "Zapisano kalibracje: " + ", ".join(ok))
        if skipped:
            self._log("warn", "Za maly zakres, pominieto: " + ", ".join(skipped))
        self.lbl_calib.configure(
            text=f"Zapisano: {', '.join(ok) if ok else 'nic'}"
                 + (f" | pominieto: {', '.join(skipped)}" if skipped else ""))

    def _calib_reset(self) -> None:
        for key in PEDAL_KEYS:
            p = self.cfg.pedals[key]
            p.raw_min, p.raw_max = 0, 4095
        self.engine.reset_raw_tracking()
        self._sync_pedal_widgets()
        self._log("info", "Przywrocono pelny zakres surowy (0-4095)")

    def _sync_pedal_widgets(self) -> None:
        for key, w in self._pedal_widgets.items():
            cfg = self.cfg.pedals[key]
            w["range"].set(f"{cfg.raw_min} - {cfg.raw_max}")
            w["dz_lo"].set(cfg.deadzone_low)
            w["dz_hi"].set(cfg.deadzone_high)
            w["gamma"].set(cfg.gamma)
            w["smooth"].set(cfg.smoothing)
            w["invert"].set(cfg.invert)

    # --- przyciski ---

    def _on_button_map_change(self) -> None:
        mapping = []
        for var in self._button_map_vars:
            try:
                mapping.append(int(var.get()))
            except ValueError:
                mapping.append(0)
        self.cfg.buttons.mapping = mapping
        self.cfg.normalize()

    def _on_button_invert_change(self) -> None:
        self.cfg.buttons.invert = [bool(v.get()) for v in self._button_invert_vars]
        self.cfg.normalize()

    def _on_hat_change(self) -> None:
        hat = []
        for var in self._hat_vars:
            try:
                hat.append(int(var.get()))
            except ValueError:
                hat.append(-1)
        self.cfg.buttons.hat = hat
        self.cfg.normalize()

    def _buttons_identity(self) -> None:
        for i, var in enumerate(self._button_map_vars):
            var.set(str(i + 1))
        self._on_button_map_change()
        self._log("ok", "Przyciski ustawione 1:1")

    def _buttons_clear(self) -> None:
        for var in self._button_map_vars:
            var.set("0")
        self._on_button_map_change()
        self._log("info", "Mapowanie przyciskow wyczyszczone")

    def _buttons_clear_invert(self) -> None:
        for var in self._button_invert_vars:
            var.set(False)
        self._on_button_invert_change()
        self._log("info", "Zdjeto odwrocenie ze wszystkich przyciskow")

    # --- FFB ---

    def _on_ffb_toggle(self) -> None:
        enabled = bool(self.var_ffb_on.get())
        self.engine.set_ffb_enabled(enabled)
        self._log("ok" if enabled else "info",
                  "Force Feedback wlaczony" if enabled else "Force Feedback wylaczony")

    def _on_ffb_field(self, name: str, value) -> None:
        setattr(self.cfg.ffb, name, value)
        self.cfg.normalize()

    def _on_ffb_invert(self) -> None:
        self.cfg.ffb.invert = bool(self.var_ffb_inv.get())

    def _on_dir_mode(self) -> None:
        label = self.var_dir_mode.get()
        for mode, text in DIRECTION_LABELS.items():
            if text == label:
                self.engine.ffb.direction_mode = mode
                self._log("info", f"Tryb kierunku FFB: {text}")
                return

    # --- tryb pada ---

    def _on_mode_change(self, var: tk.StringVar) -> None:
        label = var.get()
        for mode, text in OUTPUT_MODE_LABELS.items():
            if text == label:
                self.cfg.output.mode = mode
                break
        self._sync_mode_widgets()
        self._reopen_output()
        self._log("info", f"Tryb wyjscia: {OUTPUT_MODE_LABELS[self.cfg.output.mode]}")

    def _sync_mode_widgets(self) -> None:
        label = OUTPUT_MODE_LABELS[self.cfg.output.mode]
        for var in (getattr(self, "var_mode_pad", None),
                    getattr(self, "var_mode_g29", None),
                    getattr(self, "var_mode_diag", None)):
            if var is not None and var.get() != label:
                var.set(label)

    def _on_pad_field(self, name: str, value) -> None:
        setattr(self.cfg.pad, name, value)
        self.cfg.normalize()
        self._sync_pad_profile_label()

    def _sync_pad_profile_label(self) -> None:
        """Reczny ruch suwaka przestawia nazwe profilu na wlasny."""
        var = getattr(self, "var_pad_profile", None)
        if var is None:
            return
        label = PAD_PROFILE_LABELS[self.cfg.pad.detect_profile()]
        if var.get() != label:
            var.set(label)

    def _on_pad_profile(self) -> None:
        chosen = self.var_pad_profile.get()
        key = next((k for k in PAD_PROFILE_ORDER
                    if PAD_PROFILE_LABELS[k] == chosen), "")
        if not key:
            # Wlasny nie jest zestawem wartosci - nie ma czego ustawiac,
            # a nazwa musi wrocic do tego, co naprawde jest na suwakach.
            self._sync_pad_profile_label()
            return
        self.cfg.pad.apply_profile(key)
        self.cfg.normalize()
        pad = self.cfg.pad
        # set() nie wywoluje callbacku, wiec nie przestawi profilu na wlasny
        self.sl_pad_range.set(pad.steer_range_deg)
        self.sl_pad_dz.set(pad.steer_deadzone)
        self.sl_pad_anti.set(pad.steer_antideadzone)
        self.sl_pad_lin.set(pad.steer_linearity)
        self.sl_pad_trig.set(pad.trigger_antideadzone)
        self._log("info", f"Profil pada: {PAD_PROFILE_LABELS[key]}")

    def _on_pad_invert(self) -> None:
        self.cfg.pad.steer_invert = bool(self.var_pad_inv.get())

    #: Zrodla analogowe, ktore mozna wpiac w kierunek pada.
    PAD_SOURCES = tuple((k, PAD_SOURCE_LABELS[k]) for k in PAD_SOURCE_KEYS)

    def _pad_source_available(self, source: str) -> bool:
        """Czy zrodlo ma w ogole skad brac dane."""
        pedal = PAD_SOURCE_PEDAL.get(source)
        return pedal is None or self.cfg.pedals[pedal].source >= 0

    def _pad_axis_label(self, direction: str) -> str:
        source = self.cfg.pad.axes.get(direction, "")
        if not source:
            return ""
        label = PAD_SOURCE_LABELS.get(source, source)
        return label if self._pad_source_available(source) else f"{label} (!)"

    def _set_pad_axis_source(self, direction: str, source: str) -> None:
        """Wpina zrodlo w kierunek pada.

        Jedno gniazdo trzyma jedno zrodlo, ale to samo zrodlo moze
        obslugiwac kilka gniazd - np. gaz jednoczesnie na prawym spuscie
        i na drazku w gore.
        """
        self.cfg.pad.axes[direction] = source or ""
        self.cfg.normalize()
        self._sync_pad_axis_labels()
        name = PAD_DIRECTION_LABELS.get(direction, direction)
        if source:
            self._log("ok", f"Pad: {PAD_SOURCE_LABELS[source]} -> {name}")
        else:
            self._log("info", f"Pad: wyczyszczono {name}")

    def _sync_pad_axis_labels(self) -> None:
        self.pad_view.set_axis_labels(
            {d: self._pad_axis_label(d) for d in PAD_DIRECTIONS})

    def _on_pad_axis_click(self, direction: str, x_root: int,
                           y_root: int) -> None:
        current = self.cfg.pad.axes.get(direction, "")
        menu = tk.Menu(self.root, tearoff=0, bg=theme.PANEL_HI, fg=theme.TEXT,
                       activebackground=theme.ACCENT, activeforeground="#08121c",
                       bd=0, relief="flat")
        menu.add_command(
            label=f"  {PAD_DIRECTION_LABELS.get(direction, direction)}",
            state="disabled")
        menu.add_separator()
        for key, label in self.PAD_SOURCES:
            mark = "\u2713 " if key == current else "    "
            extra = "" if self._pad_source_available(key) else "  (kanal wylaczony)"
            menu.add_command(
                label=mark + label + extra,
                command=lambda k=key: self._set_pad_axis_source(direction, k))
        menu.add_separator()
        menu.add_command(label="    Nie uzywaj",
                         command=lambda: self._set_pad_axis_source(direction, ""))
        try:
            menu.tk_popup(x_root, y_root)
        finally:
            menu.grab_release()

    def _on_pad_axis_clear(self, direction: str) -> None:
        self._set_pad_axis_source(direction, "")

    def _pad_notify(self, text: str, seconds: float = 4.0) -> None:
        """Komunikat w linii pod naglowkiem, ktorego nie zasloni od razu
        podglad aktywnych przyciskow (odswiezany 30 razy na sekunde)."""
        self._pad_notice_until = time.monotonic() + seconds
        self.lbl_pad_live.configure(text=text)

    def _on_escape(self) -> None:
        try:
            on_g29 = self.nb.index(self.nb.select()) == self.TAB_G29
        except tk.TclError:
            on_g29 = False
        if on_g29:
            self.g29tab.escape()
        else:
            self._pad_escape()

    def _pad_escape(self) -> None:
        """Esc konczy kreator, a poza nim tylko anuluje pojedyncza nauke."""
        if self._pad_wizard:
            self._pad_wizard_stop()
        else:
            self.pad_view.cancel_learning()

    def _on_pad_assign_request(self, control: str) -> None:
        self.nb.select(self.TAB_PAD)
        self.g29tab.stop()
        if self._pad_wizard:
            # w kreatorze biezacy krok widac w podpowiedzi nad rysunkiem,
            # wiec ta linia zostaje na potwierdzeniu poprzedniego kroku
            return
        self._pad_notify(f"Czekam na przycisk kierownicy dla: "
                         f"{PAD_BUTTON_LABELS.get(control, control)}")

    def _on_pad_clear(self, control: str) -> None:
        self.cfg.pad.buttons[control] = -1
        self.cfg.normalize()
        self.pad_view.set_assignments(self.cfg.pad.buttons)
        self._log("info", f"Wyczyszczono przypisanie: "
                          f"{PAD_BUTTON_LABELS.get(control, control)}")

    def _pad_defaults(self) -> None:
        from ..config import DEFAULT_PAD_AXES, DEFAULT_PAD_BUTTONS
        self.cfg.pad.buttons = dict(DEFAULT_PAD_BUTTONS)
        self.cfg.pad.axes = dict(DEFAULT_PAD_AXES)
        self.cfg.normalize()
        self.pad_view.set_assignments(self.cfg.pad.buttons)
        self._sync_pad_axis_labels()
        self._log("ok", "Pad: przywrocono domyslne przypisania (skret na "
                        "lewym drazku, gaz na RT, hamulec na LT)")

    def _pad_clear_all(self) -> None:
        self.cfg.pad.buttons = {name: -1 for name in PAD_BUTTON_NAMES}
        self.cfg.pad.axes = {d: "" for d in PAD_DIRECTIONS}
        self.cfg.normalize()
        self.pad_view.set_assignments(self.cfg.pad.buttons)
        self._sync_pad_axis_labels()
        self._log("info", "Wyczyszczono wszystkie przypisania pada")

    def _pad_capture(self, mask: int) -> None:
        """Tryb nauki: pierwszy swiezo wcisniety przycisk trafia do kontrolki."""
        control = self.pad_view.learning
        if control is None:
            self._pad_prev_buttons = mask
            return

        # podglad na zywo - widac, ze plytka w ogole cos wysyla
        self.pad_view.set_held_buttons(
            i for i in range(protocol.NUM_BUTTONS) if (mask >> i) & 1)

        fresh = mask & ~self._pad_prev_buttons
        self._pad_prev_buttons = mask
        if not fresh:
            return
        index = (fresh & -fresh).bit_length() - 1

        # ten sam przycisk nie moze sterowac dwiema kontrolkami naraz
        stolen = [name for name, assigned in self.cfg.pad.buttons.items()
                  if assigned == index and name != control]
        for name in stolen:
            self.cfg.pad.buttons[name] = -1
        self.cfg.pad.buttons[control] = index
        self.cfg.normalize()
        self.pad_view.set_assignments(self.cfg.pad.buttons)

        label = PAD_BUTTON_LABELS.get(control, control)
        note = ""
        if stolen:
            note = ("  (zabrany z: "
                    + ", ".join(PAD_BUTTON_LABELS.get(n, n) for n in stolen)
                    + ")")
        self._pad_notify(f"Przypisano przycisk {index} do {label}{note}")
        self._log("ok", f"Pad: przycisk {index} -> {label}{note}")

        if self._pad_wizard:
            self._pad_wizard_advance(+1)
        else:
            self.pad_view.cancel_learning()

    # ----------------------------------------------- kreator "po kolei"

    #: Kolejnosc kroków kreatora: najpierw to, czego uzywa sie w kazdej grze.
    PAD_WIZARD_ORDER = ("A", "B", "X", "Y", "LB", "RB",
                        "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
                        "START", "BACK", "LS", "RS", "GUIDE")

    def _pad_wizard_start(self) -> None:
        self.nb.select(self.TAB_PAD)
        self._pad_wizard = list(self.PAD_WIZARD_ORDER)
        self._pad_wizard_i = 0
        self._pad_tools.pack_forget()
        self._pad_wizard_tools.pack(side="left")
        self._log("info", "Pad: kreator przypisan - wciskaj kolejne przyciski "
                          "kierownicy, Esc konczy")
        self._pad_wizard_show()

    def _pad_wizard_show(self) -> None:
        """Ustawia nauke na biezacym kroku albo konczy kreator."""
        if not self._pad_wizard:
            return
        if not (0 <= self._pad_wizard_i < len(self._pad_wizard)):
            self._pad_wizard_stop(done=True)
            return
        control = self._pad_wizard[self._pad_wizard_i]
        self.pad_view.set_wizard(self._pad_wizard_i + 1, len(self._pad_wizard))
        self.pad_view.start_learning(control)
        # zeby przycisk trzymany z poprzedniego kroku nie wpadl od razu
        # do nastepnej kontrolki
        self._pad_prev_buttons = (self.engine.last_state.buttons_effective
                                  if self.engine.last_state else 0)

    def _pad_wizard_advance(self, delta: int) -> None:
        if not self._pad_wizard:
            return
        self._pad_wizard_i += delta
        self._pad_wizard_show()

    def _pad_wizard_skip(self) -> None:
        self._pad_wizard_advance(+1)

    def _pad_wizard_back(self) -> None:
        if self._pad_wizard and self._pad_wizard_i > 0:
            self._pad_wizard_advance(-1)

    def _pad_wizard_clear(self) -> None:
        """Zostawia gniazdo puste i idzie dalej."""
        if not self._pad_wizard:
            return
        control = self._pad_wizard[self._pad_wizard_i]
        self.cfg.pad.buttons[control] = -1
        self.cfg.normalize()
        self.pad_view.set_assignments(self.cfg.pad.buttons)
        self._pad_wizard_advance(+1)

    def _pad_wizard_stop(self, done: bool = False) -> None:
        if not self._pad_wizard:
            return
        self._pad_wizard = []
        self.pad_view.set_wizard(None, None)
        self.pad_view.cancel_learning()
        self._pad_wizard_tools.pack_forget()
        self._pad_tools.pack(side="left")
        filled = sum(1 for n in PAD_BUTTON_NAMES
                     if self.cfg.pad.buttons.get(n, -1) >= 0)
        self._pad_notify(f"Kreator zakonczony - przypisanych kontrolek: "
                         f"{filled} z {len(PAD_BUTTON_NAMES)}", 6.0)
        self._log("ok" if done else "info",
                  f"Pad: kreator zakonczony ({filled} z "
                  f"{len(PAD_BUTTON_NAMES)} kontrolek przypisanych)")

    def _pad_axis_warnings(self) -> List[str]:
        """Rzeczy, ktore na pewno nie beda dzialac tak, jak wyglada."""
        pad = self.cfg.pad
        out: List[str] = []

        for direction in PAD_DIRECTIONS:
            source = pad.axes.get(direction, "")
            if source and not self._pad_source_available(source):
                out.append(
                    f"{PAD_DIRECTION_LABELS[direction]}: "
                    f"{PAD_SOURCE_LABELS[source]} ma wylaczony kanal "
                    f"- ten kierunek nic nie wysyla")

        # to samo zrodlo na obu polowkach jednej osi znosi sie do zera
        for axis in ("LX", "LY", "RX", "RY"):
            minus = pad.axes.get(f"{axis}-", "")
            plus = pad.axes.get(f"{axis}+", "")
            if minus and minus == plus:
                out.append(
                    f"{PAD_SOURCE_LABELS[minus]} steruje oboma kierunkami "
                    f"osi {axis} naraz - wychylenia znosza sie do zera")

        if not pad.axes.get("LX-") and not pad.axes.get("LX+"):
            out.append("Lewy drazek nie ma wpietego skretu - w grze auto nie "
                       "bedzie skrecac")
        return out

    # --- wyjscie ---

    def _on_backend(self) -> None:
        self._on_mode_change(self.var_mode_diag)

    def _on_vjoy_device(self) -> None:
        try:
            self.cfg.output.vjoy_device = int(self.var_vjoy_id.get())
        except ValueError:
            return
        self._reopen_output()

    def _reopen_output(self, show_errors: bool = False) -> None:
        """Otwiera wyjscia na nowo.

        `show_errors` tylko dla jawnego klikniecia uzytkownika - przy zwyklej
        zmianie trybu modalne okno byloby uciazliwe, a tresc bledu i tak
        trafia do podpowiedzi pod statusem oraz do dziennika.
        """
        ok = self.engine.open_output()
        if not ok and show_errors:
            msg = self.engine.output_error or self.engine.pad_error
            if msg:
                messagebox.showwarning("Wyjscie", msg, parent=self.root)
        if self.cfg.ffb.enabled:
            self.engine.set_ffb_enabled(True)
        self._update_vjoy_text()

    def _show_vjoy_diag(self) -> None:
        self._update_vjoy_text()
        self.nb.select(self.tab_diag)

    def _update_vjoy_text(self) -> None:
        info = vjoy_diagnostics()
        lines: List[str] = []
        lines.append(f"Python: {info['python_bits']}-bit")
        lines.append(f"Biblioteka: {info['dll_path'] or 'nie znaleziono'}")
        if info["error"]:
            lines.append("")
            lines.append(str(info["error"]))
        else:
            ver = info.get("version")
            if ver:
                lines.append(f"Wersja vJoy: {(ver >> 8) & 0xFF:x}.{ver & 0xFF:02x}")
            if info.get("product"):
                lines.append(f"Produkt: {info['product']}")
            lines.append(f"Sterownik wlaczony: {'tak' if info['enabled'] else 'NIE'}")
            devices = info.get("devices") or {}
            if not devices:
                lines.append("")
                lines.append("Brak wlaczonych urzadzen vJoy. Uruchom vJoyConf, "
                             "zaznacz Device 1 i kliknij Apply.")
            for rid, d in sorted(devices.items()):
                lines.append("")
                lines.append(f"Urzadzenie #{rid}: {d['status_text']}")
                lines.append(f"  osie      : {', '.join(d['axes']) or 'brak'}")
                lines.append(f"  przyciski : {d['buttons']}")
                lines.append(f"  POV       : {d['disc_pov']} dyskr. / "
                             f"{d['cont_pov']} ciagl.")
                lines.append(f"  FFB       : {'tak' if d['ffb'] else 'nie'}")

        lines.append("")
        lines.append("Zalecana konfiguracja dla kierownicy (vJoyConf):")
        lines.append("  osie X, Y, Z, Rx, Ry, Rz   32 przyciski   1x POV")
        lines.append("  zaznaczone 'Enable Effects' (Force Feedback)")

        # --- tryb pada ---
        vg = vigem_diagnostics()
        lines.append("")
        lines.append("-" * 46)
        lines.append("TRYB PADA (ViGEm)")
        lines.append(f"  biblioteka : {vg['dll_path'] or 'nie znaleziono'}")
        lines.append(f"  zaladowana : {'tak' if vg['dll_loaded'] else 'NIE'}")
        lines.append(f"  sterownik  : {'dziala' if vg['driver_ok'] else 'NIE DZIALA'}")
        if vg["error"]:
            lines.append(f"  blad       : {vg['error']}")
        pad = self.engine.pad_output
        if pad is not None and pad.is_open:
            slot = pad.slot
            lines.append(f"  pad        : podlaczony"
                         + (f", gniazdo XInput {slot + 1}" if slot is not None else ""))
        elif self.cfg.output.pad_on:
            lines.append("  pad        : nie wystartowal")
        if not vg["driver_ok"]:
            lines.append("")
            lines.append("  Zainstaluj ViGEmBus:")
            lines.append("  https://github.com/nefarius/ViGEmBus/releases")

        self.txt_vjoy.configure(state="normal")
        self.txt_vjoy.delete("1.0", "end")
        self.txt_vjoy.insert("1.0", "\n".join(lines))
        self.txt_vjoy.configure(state="disabled")

    def _on_report_hz(self, value: float) -> None:
        self.cfg.link.report_hz = int(value)
        self.engine.apply_config()

    # --- dziennik i konfiguracja ---

    def _enqueue_log(self, level: str, msg: str) -> None:
        try:
            self._log_queue.put_nowait((level, msg))
        except queue.Full:
            pass

    def _log(self, level: str, msg: str) -> None:
        self._enqueue_log(level, msg)

    def _drain_log(self) -> None:
        import time as _time
        wrote = False
        for _ in range(60):
            try:
                level, msg = self._log_queue.get_nowait()
            except queue.Empty:
                break
            if not wrote:
                self.txt_log.configure(state="normal")
                wrote = True
            stamp = _time.strftime("%H:%M:%S")
            self.txt_log.insert("end", f"{stamp}  {msg}\n", level)
        if wrote:
            # ograniczenie dziennika do 600 linii
            lines = int(self.txt_log.index("end-1c").split(".")[0])
            if lines > 600:
                self.txt_log.delete("1.0", f"{lines - 600}.0")
            self.txt_log.see("end")
            self.txt_log.configure(state="disabled")

    def _clear_log(self) -> None:
        self.txt_log.configure(state="normal")
        self.txt_log.delete("1.0", "end")
        self.txt_log.configure(state="disabled")

    def _save_config(self) -> None:
        try:
            path = self.cfg.save()
        except OSError as exc:
            messagebox.showerror("Zapis ustawien", str(exc), parent=self.root)
            return
        self._log("ok", f"Ustawienia zapisane: {path}")

    def _on_close(self) -> None:
        if self._closing:
            return
        self._closing = True
        try:
            self.cfg.save()
        except OSError:
            pass
        try:
            self.engine.stop()
        except Exception:
            pass
        self.root.destroy()

    # ================================================== odswiezanie widoku

    def _tick(self) -> None:
        if self._closing:
            return
        try:
            self._refresh()
        except Exception as exc:                 # interfejs nie moze przerwac petli
            self._enqueue_log("error", f"Blad odswiezania: {exc!r}")
        finally:
            if not self._closing:
                self.root.after(UI_PERIOD_MS, self._tick)

    def _refresh(self) -> None:
        self._drain_log()

        link = self.engine.link
        state = link.state
        stats = self.engine.link_stats()
        connected = state == link_mod.CONNECTED

        # --- naglowek ---
        if connected:
            self.dot_link.set(theme.GOOD)
            self.lbl_link.configure(text=f"{stats.port} - {stats.frames_per_sec:.0f} Hz",
                                    fg=theme.GOOD)
            self.btn_connect.configure(text="Rozlacz", style="TButton")
        elif state == link_mod.SCANNING:
            self.dot_link.set(theme.WARN)
            detail = self.engine.status.link_detail or "szukanie"
            self.lbl_link.configure(text=detail, fg=theme.WARN)
            self.btn_connect.configure(text="Przerwij", style="TButton")
        else:
            self.dot_link.set(theme.MUTED)
            self.lbl_link.configure(text=link_mod.STATE_LABELS.get(state, state),
                                    fg=theme.TEXT_DIM)
            self.btn_connect.configure(text="Polacz", style="Accent.TButton")

        vjoy_ok = isinstance(self.engine.output, VJoyBackend) and self.engine.output.is_open
        pad_ok = self.engine.pad_output is not None and self.engine.pad_output.is_open
        mode = self.cfg.output.mode
        if mode == "none":
            self.dot_vjoy.set(theme.MUTED)
            self.lbl_vjoy.configure(text="podglad", fg=theme.TEXT_DIM)
        else:
            parts, all_ok = [], True
            if self.cfg.output.wheel_on:
                parts.append(f"vJoy #{self.cfg.output.vjoy_device}"
                             if vjoy_ok else "vJoy blad")
                all_ok = all_ok and vjoy_ok
            if self.cfg.output.pad_on:
                parts.append("pad" if pad_ok else "pad blad")
                all_ok = all_ok and pad_ok
            if self.cfg.output.g29_on:
                st = self.engine.status
                if not connected:
                    parts.append("G29: brak plytki")
                elif not st.g29_fw:
                    parts.append("G29: stary firmware")
                elif not st.g29_usb:
                    parts.append("G29: podlacz Native USB")
                else:
                    parts.append("G29")
                all_ok = all_ok and connected and st.g29_fw and st.g29_usb
            self.dot_vjoy.set(theme.GOOD if all_ok else theme.BAD)
            self.lbl_vjoy.configure(text=" + ".join(parts),
                                    fg=theme.GOOD if all_ok else theme.BAD)

        # --- pulpit ---
        state_obj = self.engine.last_state
        if state_obj is not None and connected:
            self.dial.set_angle(state_obj.steer_deg)
            self.row_counts.set(f"{state_obj.raw_counts:+d}")
            self.row_axis.set(f"{state_obj.steer_axis:+.3f}")
            self.row_velocity.set(f"{state_obj.steer_velocity:+.0f} st/s")
            for key, bar in self.dash_bars.items():
                src = self.cfg.pedals[key].source
                raw = state_obj.raw_axes[src] if 0 <= src < len(state_obj.raw_axes) else None
                bar.set(state_obj.pedals[key], raw)
            # Pokazujemy stan PO odwroceniu (NC) - inaczej kolumna NC
            # wygladalaby, jakby nic nie robila.
            self.button_grid.set_mask(state_obj.buttons_effective)
            self.hat_indicator.set(state_obj.hat)
            for i, dot in enumerate(self._btn_dots):
                on = bool((state_obj.buttons_effective >> i) & 1)
                dot.set(theme.ACCENT if on else theme.TRACK, glow=on)
            pressed = [str(i) for i in range(protocol.NUM_BUTTONS)
                       if (state_obj.buttons_effective >> i) & 1]
            out_txt = (", ".join(str(b) for b in sorted(state_obj.out_buttons))
                       or "nic")
            hat_txt = ("" if state_obj.hat < 0
                       else f"   krzyzak: {HAT_DIRECTIONS[state_obj.hat]}")
            self.lbl_btn_live.configure(
                text="Wcisniete teraz: " + (", ".join(pressed) if pressed else "brak"))
            if self.cfg.output.g29_on:
                # G29 ma wlasne przypisania - lista vJoy tutaj nic nie zmienia
                rep = (self.engine.last_g29
                       or build_g29_report(state_obj, self.cfg))
                act = g29_active(rep)
                g29_txt = ", ".join(G29_CONTROL_LABELS[n] for n in G29_CONTROLS
                                    if act.get(n)) or "nic"
                self.lbl_btn_out.configure(
                    text=f"Tryb G29 - przypisania sa w zakladce G29. "
                         f"Wcisniete na G29: {g29_txt}")
                self.lbl_dash_out.configure(text=f"Do G29: {g29_txt}")
            else:
                self.lbl_btn_out.configure(
                    text=f"Wysylane do vJoy: {out_txt}{hat_txt}")
                self.lbl_dash_out.configure(text=f"Do vJoy: {out_txt}{hat_txt}")
        elif not connected:
            self.dial.set_angle(0.0)
            self.button_grid.set_mask(0)
            self.hat_indicator.set(-1)
            self.lbl_dash_out.configure(text="")
            for bar in self.dash_bars.values():
                bar.set(0.0, None)
            self.row_counts.set("-")
            self.row_axis.set("-")
            self.row_velocity.set("-")

        # --- wyjscie / FFB ---
        out_lines = []
        if self.cfg.output.g29_on:
            out_lines.append(self.engine.status.output_text)
        if self.cfg.output.wheel_on:
            out_lines.append(self.engine.output.status_text())
        if self.cfg.output.pad_on:
            out_lines.append(self.engine.status.pad_text)
        if not out_lines:
            out_lines.append("Tryb podgladu - gry nie widza kierownicy")
        self.lbl_output.configure(text="\n".join(out_lines))
        hint = ""
        g29 = self.engine.status
        if self.cfg.output.g29_on and connected and not g29.g29_fw:
            hint = ("Firmware plytki nie ma trybu G29 - wgraj nowy "
                    "(firmware/kierownica/wgraj_firmware.bat).")
        elif self.cfg.output.g29_on and connected and not g29.g29_usb:
            hint = ("Windows nie widzi G29 - podlacz drugi kabel do portu "
                    "Native USB plytki.")
        elif self.cfg.output.pad_on and not pad_ok and self.engine.pad_error:
            hint = self.engine.pad_error.splitlines()[0]
        elif self.cfg.output.wheel_on and not vjoy_ok:
            hint = (self.engine.output_error or
                    "vJoy nie jest aktywne - gry nie widza kierownicy. "
                    "Sprawdz zakladke Diagnostyka.")
        elif self._axis_conflicts():
            hint = ("Ta sama os vJoy jest przypisana do kilku zrodel: "
                    + ", ".join(self._axis_conflicts()))
        self.lbl_output_hint.configure(text=hint)

        torque = self.engine.status.ffb_torque
        self.torque_meter.set(torque)
        self.ffb_meter.set(torque)

        if self.cfg.ffb.enabled:
            if self.engine.ffb.registered:
                txt = (f"Aktywne. Pakietow z gry: {self.engine.ffb.packets}, "
                       f"wzmocnienie urzadzenia: "
                       f"{self.engine.ffb.device_gain() * 100:.0f} %, "
                       f"efekty: {len(self.engine.ffb.active_effects())}")
                if self.engine.ffb.parse_errors:
                    txt += f", odrzucone pakiety: {self.engine.ffb.parse_errors}"
            else:
                txt = ("Wlaczone, ale bez odbioru z gry - dzialaja tylko efekty "
                       "lokalne (sprezyna, tlumienie, tarcie).")
            if not self.engine.status.board_has_ffb and connected:
                txt += "  UWAGA: firmware zglosil brak obslugi FFB."
        else:
            txt = "Wylaczone - silnik jest zwolniony."
        self.lbl_ffb_state.configure(text=txt)

        # --- diagnostyka ---
        if self.nb.index(self.nb.select()) == self.TAB_DIAG:
            self.diag_rows["port"].set(stats.port or "-")
            hello = link.hello
            self.diag_rows["fw"].set(
                f"{hello.name} {hello.firmware}" if hello else "-")
            self.diag_rows["fps"].set(f"{stats.frames_per_sec:.0f} Hz",
                                      theme.GOOD if stats.frames_per_sec > 50
                                      else theme.WARN)
            self.diag_rows["bps"].set(f"{stats.bytes_per_sec / 1024:.1f} kB/s")
            self.diag_rows["lat"].set(f"{stats.latency_ms:.1f} ms",
                                      theme.GOOD if stats.latency_ms < 15
                                      else theme.WARN)
            self.diag_rows["crc"].set(str(stats.crc_errors),
                                      theme.GOOD if stats.crc_errors == 0 else theme.BAD)
            self.diag_rows["drop"].set(str(stats.dropped_frames),
                                       theme.GOOD if stats.dropped_frames == 0
                                       else theme.WARN)
            self.diag_rows["resync"].set(str(stats.resyncs))
            self.diag_rows["total"].set(str(stats.total_frames))
            self.diag_rows["up"].set(_fmt_duration(stats.uptime))
            self.spark_fps.push(stats.frames_per_sec, scale_max=520)
            self.spark_lat.push(stats.latency_ms, scale_max=40)
            self._refresh_g29_card(connected)

        # --- pedaly: podglad zakresow ---
        if self.nb.index(self.nb.select()) == self.TAB_PEDALS:
            for key, w in self._pedal_widgets.items():
                cfg = self.cfg.pedals[key]
                w["range"].set(f"{cfg.raw_min} - {cfg.raw_max}")
                if 0 <= cfg.source < protocol.NUM_AXES:
                    lo = self.engine.raw_seen_min[cfg.source]
                    hi = self.engine.raw_seen_max[cfg.source]
                    w["seen"].set(f"{lo} - {hi}" if hi >= lo else "-")
                else:
                    w["seen"].set("kanal wylaczony")
                if state_obj is not None and connected:
                    src = cfg.source
                    raw = state_obj.raw_axes[src] if 0 <= src < len(state_obj.raw_axes) else None
                    w["bar"].set(state_obj.pedals[key], raw)

        # --- zakladka G29 ---
        self.g29tab.tick(state_obj, connected,
                         self.nb.index(self.nb.select()) == self.TAB_G29)

        # --- zakladka Pad ---
        if state_obj is not None and connected:
            self._pad_capture(state_obj.buttons_effective)
        if self.nb.index(self.nb.select()) == self.TAB_PAD:
            self.pad_view.set_assignments(self.cfg.pad.buttons)
            self._sync_pad_axis_labels()
            pad_state = self.engine.last_pad
            if pad_state is None and state_obj is not None:
                from ..processing import build_pad_state
                pad_state = build_pad_state(state_obj, self.cfg)
            if pad_state is not None:
                self.pad_view.set_active(
                    {name: bool(pad_state.buttons & XUSB[name])
                     for name in PAD_CONTROLS})
                self.pad_view.set_triggers(pad_state.lt, pad_state.rt)
                self.pad_view.set_sticks(pad_state.lx / 32767.0,
                                         pad_state.ly / 32767.0,
                                         pad_state.rx / 32767.0,
                                         pad_state.ry / 32767.0)
                if (self.pad_view.learning is None
                        and time.monotonic() >= self._pad_notice_until):
                    names = [PAD_BUTTON_LABELS.get(n, n) for n in PAD_CONTROLS
                             if pad_state.buttons & XUSB[n]]
                    self.lbl_pad_live.configure(
                        text="Aktywne na padzie: "
                             + (", ".join(names) if names else "nic"))
            else:
                self.pad_view.set_active({n: False for n in PAD_CONTROLS})
            warnings = self._pad_axis_warnings()
            if warnings:
                self.lbl_pad_conflict.configure(
                    text="\n".join("• " + w for w in warnings))
                if not self.lbl_pad_conflict.winfo_ismapped():
                    self.lbl_pad_conflict.pack(anchor="w", pady=(0, 6),
                                               before=self._pad_holder)
            elif self.lbl_pad_conflict.winfo_ismapped():
                self.lbl_pad_conflict.pack_forget()
            if self.cfg.output.pad_on:
                self.lbl_pad_state.configure(
                    text=self.engine.status.pad_text if pad_ok
                    else (self.engine.pad_error.splitlines()[0]
                          if self.engine.pad_error else "Pad nie wystartowal"))
            else:
                self.lbl_pad_state.configure(
                    text="Tryb pada wylaczony - powyzej wybierz 'Pad' albo "
                         "'Oba naraz', zeby gry zobaczyly kontroler.")

        # --- przyciski presetow zakresu ---
        for deg, btn in self._range_buttons.items():
            btn.configure(style="Accent.TButton"
                          if deg == self.cfg.steer.range_deg else "TButton")

        # --- pasek stanu ---
        flags = []
        if state_obj is not None and connected:
            if state_obj.flags & protocol.FLAG_ENCODER_ERROR:
                flags.append("bledy kwadratury enkodera")
            if state_obj.flags & protocol.FLAG_ENDSTOP_HIT:
                flags.append("na ograniczniku")
            if state_obj.flags & protocol.FLAG_FFB_WATCHDOG:
                flags.append("watchdog FFB zadzialal")
        base = f"Konfiguracja: {config_path()}"
        self.lbl_status.configure(
            text=base + ("   |   " + ", ".join(flags) if flags else ""),
            fg=theme.WARN if flags else theme.MUTED)

    def _axis_conflicts(self) -> List[str]:
        used: Dict[str, List[str]] = {}
        if self.cfg.steer.vjoy_axis != "-":
            used.setdefault(self.cfg.steer.vjoy_axis, []).append("kierownica")
        for key in PEDAL_KEYS:
            p = self.cfg.pedals[key]
            if p.vjoy_axis != "-" and p.source >= 0:
                used.setdefault(p.vjoy_axis, []).append(PEDAL_LABELS[key])
        return [f"{AXIS_LABELS.get(axis, axis)} ({', '.join(names)})"
                for axis, names in used.items() if len(names) > 1]


def _fmt_duration(seconds: float) -> str:
    seconds = int(max(0, seconds))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def main() -> None:
    app = PanelApp()
    app._update_vjoy_text()
    app._sync_pedal_widgets()
    app.run()
