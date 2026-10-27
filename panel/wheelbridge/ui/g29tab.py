"""
Zakladka G29 - przypisywanie przyciskow kierownicy dla trybu G29.

Tryb G29 ma WLASNE przypisania (cfg.g29.buttons: kontrolka G29 -> przycisk
plytki), niezalezne od zakladki Pad i od listy vJoy z zakladki Przyciski.
Wczesniej tryb G29 bral przyciski z listy vJoy i ignorowal to, co ktos
ustawil w zakladce Pad - stad ta zakladka.

Nauka i kreator dzialaja jak w zakladce Pad: klikniecie kontrolki wlacza
nauke, pierwszy swiezo wcisniety przycisk kierownicy trafia do gniazda,
a ten sam przycisk nie moze sterowac dwiema kontrolkami naraz.
"""

from __future__ import annotations

import time
import tkinter as tk
from tkinter import ttk
from typing import TYPE_CHECKING, Dict, List, Optional

from .. import protocol
from ..config import (DEFAULT_PAD_BUTTONS, G29_CONTROL_LABELS, G29_CONTROLS,
                      OUTPUT_MODE_LABELS, OUTPUT_MODES, PAD_TO_G29,
                      g29_buttons_from_pad)
from ..processing import build_g29_report, g29_active
from . import theme
from .wheelview import WIZARD_ORDER, WheelView
from .widgets import Card

if TYPE_CHECKING:                       # tylko dla podpowiedzi typow
    from .app import PanelApp


class G29Tab:
    """Widzety i logika zakladki. Tworzy ja PanelApp i wola tick() co klatke."""

    def __init__(self, app: "PanelApp") -> None:
        self.app = app
        self.view: Optional[WheelView] = None
        self._prev_buttons = 0
        self._notice_until = 0.0
        self._wizard: List[str] = []
        self._wizard_i = 0

    # ---------------------------------------------------------- skroty

    @property
    def cfg(self):
        return self.app.cfg

    def _log(self, level: str, text: str) -> None:
        self.app._log(level, text)

    @staticmethod
    def _label(name: str) -> str:
        return G29_CONTROL_LABELS.get(name, name)

    # ---------------------------------------------------------- budowa

    def build(self, parent: ttk.Frame) -> None:
        app = self.app
        fonts = app.fonts
        parent.grid_columnconfigure(0, weight=1)

        # pasek u gory - jak w zakladce Pad: jeden wiersz, stan i tryb
        strip = tk.Frame(parent, bg=theme.PANEL, highlightthickness=1,
                         highlightbackground=theme.BORDER)
        strip.grid(row=0, column=0, sticky="ew", pady=(0, 12))
        bar = tk.Frame(strip, bg=theme.PANEL)
        bar.pack(fill="x", padx=15, pady=9)
        tick = tk.Frame(bar, bg=theme.ACCENT, width=3, height=13)
        tick.pack(side="left", padx=(0, 9))
        tick.pack_propagate(False)
        tk.Label(bar, text="KIEROWNICA G29", bg=theme.PANEL, fg=theme.TEXT_DIM,
                 font=(fonts.family, 9, "bold")).pack(side="left")

        app.var_mode_g29 = tk.StringVar(
            value=OUTPUT_MODE_LABELS[self.cfg.output.mode])
        cb = ttk.Combobox(bar, textvariable=app.var_mode_g29, state="readonly",
                          values=[OUTPUT_MODE_LABELS[m] for m in OUTPUT_MODES],
                          width=44)
        cb.pack(side="right")
        cb.bind("<<ComboboxSelected>>",
                lambda _e: app._on_mode_change(app.var_mode_g29))
        tk.Label(bar, text="Tryb wyjscia", bg=theme.PANEL, fg=theme.TEXT_DIM,
                 font=fonts.small).pack(side="right", padx=(0, 9))
        self.lbl_state = tk.Label(bar, text="", bg=theme.PANEL,
                                  fg=theme.TEXT_DIM, font=fonts.small)
        self.lbl_state.pack(side="right", padx=(16, 24))

        view = Card(parent, "Przyciski kierownicy G29", fonts)
        view.grid_card(row=1, column=0, sticky="ew")

        self._tools = ttk.Frame(view.right_slot, style="Card.TFrame")
        self._tools.pack(side="left")
        ttk.Button(self._tools, text="Przypisz po kolei", style="Accent.TButton",
                   command=self.wizard_start).pack(side="left")
        ttk.Button(self._tools, text="Przepisz z pada", style="Small.TButton",
                   command=self.copy_from_pad).pack(side="left", padx=(6, 0))
        ttk.Button(self._tools, text="Domyslne", style="Small.TButton",
                   command=self.defaults).pack(side="left", padx=(6, 0))
        ttk.Button(self._tools, text="Wyczysc wszystko", style="Small.TButton",
                   command=self.clear_all).pack(side="left", padx=(6, 0))

        self._wizard_tools = ttk.Frame(view.right_slot, style="Card.TFrame")
        ttk.Button(self._wizard_tools, text="Wstecz", style="Small.TButton",
                   command=self.wizard_back).pack(side="left")
        ttk.Button(self._wizard_tools, text="Pomin", style="Small.TButton",
                   command=self.wizard_skip).pack(side="left", padx=(6, 0))
        ttk.Button(self._wizard_tools, text="Wyczysc i pomin",
                   style="Small.TButton",
                   command=self.wizard_clear).pack(side="left", padx=(6, 0))
        ttk.Button(self._wizard_tools, text="Zakoncz", style="Accent.TButton",
                   command=self.wizard_stop).pack(side="left", padx=(6, 0))

        self.lbl_live = ttk.Label(view.body, text="", style="Good.TLabel")
        self.lbl_live.pack(anchor="w", pady=(0, 6))

        self.view = WheelView(view.body, fonts,
                              on_assign_request=self._on_assign_request,
                              on_clear=self._on_clear)
        self.view.pack(fill="x")
        self.view.set_assignments(self.cfg.g29.buttons)

        ttk.Label(view.body, style="Muted.TLabel", wraplength=1080,
                  justify="left",
                  text="Te przypisania dzialaja tylko w trybie Kierownica G29 i "
                       "nie zaleza od zakladki Pad ani od listy vJoy w zakladce "
                       "Przyciski. Krzyzak bez przypisan bierze kierunki z "
                       "krzyzaka ustawionego w zakladce Przyciski. Biegi to "
                       "przyciski skrzyni G29 - przypisz je, jesli masz "
                       "wlasna skrzynie albo manetki."
                  ).pack(anchor="w", pady=(12, 0))

    # ---------------------------------------------------------- stan

    def _notify(self, text: str, seconds: float = 4.0) -> None:
        self._notice_until = time.monotonic() + seconds
        self.lbl_live.configure(text=text)

    def _sync(self) -> None:
        if self.view is not None:
            self.view.set_assignments(self.cfg.g29.buttons)

    def _status(self, connected: bool):
        st = self.app.engine.status
        if not connected:
            return "Brak polaczenia z plytka", theme.MUTED
        if not st.g29_fw:
            return "Firmware bez trybu G29 - wgraj nowy (wgraj_firmware.bat)", theme.BAD
        if not self.cfg.output.g29_on:
            return "Tryb G29 wylaczony - gra nie dostaje tych przypisan", theme.WARN
        if not st.g29_usb:
            return "Podlacz kabel do portu Native USB", theme.BAD
        return "Windows widzi G29 - przypisania dzialaja w grze", theme.GOOD

    def tick(self, state_obj, connected: bool, visible: bool) -> None:
        """Wolane z PanelApp._refresh co klatke."""
        if state_obj is not None and connected:
            self._capture(state_obj.buttons_effective)
        if not visible or self.view is None:
            return

        text, color = self._status(connected)
        if self.lbl_state.cget("text") != text:
            self.lbl_state.configure(text=text, fg=color)
        self._sync()

        if state_obj is None or not connected:
            self.view.set_active({})
            self.view.set_angle(0.0)
            self.view.set_pedals({})
            self._set_live(None)
            return

        # pokazujemy to, co faktycznie idzie do gry; poza trybem G29 -
        # to, co by poszlo
        report = self.app.engine.last_g29 if self.cfg.output.g29_on else None
        if report is None:
            report = build_g29_report(state_obj, self.cfg)
        active = g29_active(report)
        self.view.set_active(active)
        self.view.set_angle(state_obj.steer_deg)
        self.view.set_pedals(state_obj.pedals)
        self._set_live(active)

    def _set_live(self, active: Optional[Dict[str, bool]]) -> None:
        """Linia nad rysunkiem: potwierdzenie, oczekiwanie albo podglad."""
        if time.monotonic() < self._notice_until:
            return                      # swiezy komunikat ma pierwszenstwo
        learning = self.view.learning
        if learning is not None and not self._wizard:
            text = f"Czekam na przycisk kierownicy dla: {self._label(learning)}"
        elif learning is not None or active is None:
            # w kreatorze krok widac w podpowiedzi nad rysunkiem
            text = ""
        else:
            names = [self._label(n) for n in G29_CONTROLS if active.get(n)]
            text = "Wcisniete na G29: " + (", ".join(names) if names else "nic")
        if self.lbl_live.cget("text") != text:
            self.lbl_live.configure(text=text)

    # ---------------------------------------------------------- nauka

    def _on_assign_request(self, control: str) -> None:
        app = self.app
        app.nb.select(app.TAB_G29)
        # druga zakladka nie moze uczyc sie w tym samym czasie - jeden
        # wcisniety przycisk trafilby do obu
        if app._pad_wizard:
            app._pad_wizard_stop()
        app.pad_view.cancel_learning()
        # przycisk trzymany w chwili klikniecia (albo z poprzedniego kroku
        # kreatora) nie moze od razu wpasc do tej kontrolki
        self._prev_buttons = (app.engine.last_state.buttons_effective
                              if app.engine.last_state else 0)
        self._notice_until = 0.0

    def _on_clear(self, control: str) -> None:
        self.cfg.g29.buttons[control] = -1
        self.cfg.normalize()
        self._sync()
        self._log("info", f"G29: wyczyszczono {self._label(control)}")

    def _capture(self, mask: int) -> None:
        """Tryb nauki: pierwszy swiezo wcisniety przycisk trafia do kontrolki."""
        view = self.view
        control = view.learning if view is not None else None
        if control is None:
            self._prev_buttons = mask
            return

        view.set_held_buttons(
            i for i in range(protocol.NUM_BUTTONS) if (mask >> i) & 1)

        fresh = mask & ~self._prev_buttons
        self._prev_buttons = mask
        if not fresh:
            return
        index = (fresh & -fresh).bit_length() - 1

        buttons = self.cfg.g29.buttons
        stolen = [n for n, b in buttons.items() if b == index and n != control]
        for n in stolen:
            buttons[n] = -1
        buttons[control] = index
        self.cfg.normalize()
        self._sync()

        note = ""
        if stolen:
            note = "  (zabrany z: " + ", ".join(self._label(n) for n in stolen) + ")"
        self._notify(f"Przypisano przycisk {index} do: {self._label(control)}{note}")
        self._log("ok", f"G29: przycisk {index} -> {self._label(control)}{note}")

        if self._wizard:
            self._wizard_advance(+1)
        else:
            view.cancel_learning()

    def escape(self) -> None:
        if self._wizard:
            self.wizard_stop()
        elif self.view is not None:
            self.view.cancel_learning()

    def stop(self) -> None:
        """Konczy nauke i kreator - np. przy wyjsciu z zakladki."""
        if self._wizard:
            self.wizard_stop()
        elif self.view is not None and self.view.learning is not None:
            self.view.cancel_learning()

    # ---------------------------------------------------- calosciowe

    def copy_from_pad(self) -> None:
        """Przepisuje przypisania z zakladki Pad na odpowiedniki G29.

        Kontrolki bez odpowiednika na padzie (biegi, pokretlo, L2/R2...)
        zostaja, chyba ze ich przycisk zajal wlasnie odpowiednik z pada.
        """
        self.stop()
        buttons = self.cfg.g29.buttons
        from_pad = g29_buttons_from_pad(self.cfg.pad.buttons)
        taken = set()
        for g29_name in PAD_TO_G29.values():
            buttons[g29_name] = from_pad[g29_name]
            if from_pad[g29_name] >= 0:
                taken.add(from_pad[g29_name])
        for name in G29_CONTROLS:
            if name not in PAD_TO_G29.values() and buttons.get(name, -1) in taken:
                buttons[name] = -1
        self.cfg.normalize()
        self._sync()
        filled = sum(1 for v in self.cfg.g29.buttons.values() if v >= 0)
        self._notify(f"Przepisano przypisania z pada - przypisanych kontrolek: "
                     f"{filled} z {len(G29_CONTROLS)}")
        self._log("ok", "G29: przepisano przypisania z zakladki Pad")

    def defaults(self) -> None:
        self.stop()
        self.cfg.g29.buttons = g29_buttons_from_pad(DEFAULT_PAD_BUTTONS)
        self.cfg.normalize()
        self._sync()
        self._log("ok", "G29: przywrocono domyslne przypisania")

    def clear_all(self) -> None:
        self.stop()
        self.cfg.g29.buttons = {name: -1 for name in G29_CONTROLS}
        self.cfg.normalize()
        self._sync()
        self._log("info", "G29: wyczyszczono wszystkie przypisania")

    # ------------------------------------------------- kreator "po kolei"

    def wizard_start(self) -> None:
        self.app.nb.select(self.app.TAB_G29)
        self._wizard = list(WIZARD_ORDER)
        self._wizard_i = 0
        self._tools.pack_forget()
        self._wizard_tools.pack(side="left")
        self._log("info", "G29: kreator przypisan - wciskaj kolejne przyciski "
                          "kierownicy, Esc konczy")
        self._wizard_show()

    def _wizard_show(self) -> None:
        if not self._wizard:
            return
        if not (0 <= self._wizard_i < len(self._wizard)):
            self.wizard_stop(done=True)
            return
        control = self._wizard[self._wizard_i]
        self.view.set_wizard(self._wizard_i + 1, len(self._wizard))
        self.view.start_learning(control)

    def _wizard_advance(self, delta: int) -> None:
        if not self._wizard:
            return
        self._wizard_i += delta
        self._wizard_show()

    def wizard_skip(self) -> None:
        self._wizard_advance(+1)

    def wizard_back(self) -> None:
        if self._wizard and self._wizard_i > 0:
            self._wizard_advance(-1)

    def wizard_clear(self) -> None:
        """Zostawia gniazdo puste i idzie dalej."""
        if not self._wizard:
            return
        self.cfg.g29.buttons[self._wizard[self._wizard_i]] = -1
        self.cfg.normalize()
        self._sync()
        self._wizard_advance(+1)

    def wizard_stop(self, done: bool = False) -> None:
        if not self._wizard:
            return
        self._wizard = []
        self.view.set_wizard(None, None)
        self.view.cancel_learning()
        self._wizard_tools.pack_forget()
        self._tools.pack(side="left")
        filled = sum(1 for v in self.cfg.g29.buttons.values() if v >= 0)
        self._notify(f"Kreator zakonczony - przypisanych kontrolek: "
                     f"{filled} z {len(G29_CONTROLS)}", 6.0)
        self._log("ok" if done else "info",
                  f"G29: kreator zakonczony ({filled} z {len(G29_CONTROLS)} "
                  f"kontrolek przypisanych)")

    @property
    def wizard_active(self) -> bool:
        return bool(self._wizard)
