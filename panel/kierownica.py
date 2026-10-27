#!/usr/bin/env python3
"""
Panel kierownicy - punkt wejscia.

Uruchomienie:
    python kierownica.py

Wymagania: Python 3.9+, pyserial. Do tego, zeby gry widzialy kierownice,
potrzebny jest sterownik vJoy - aplikacja dziala takze bez niego
(w trybie podgladu) i powie, co zainstalowac.
"""

import sys
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

MIN_PYTHON = (3, 9)


def _die(title: str, message: str) -> None:
    try:
        import tkinter as tk
        from tkinter import messagebox
        root = tk.Tk()
        root.withdraw()
        messagebox.showerror(title, message)
        root.destroy()
    except Exception:
        pass
    print(f"{title}\n\n{message}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    if sys.version_info < MIN_PYTHON:
        _die("Za stary Python",
             f"Potrzebny jest Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]} lub nowszy.\n"
             f"Masz {sys.version.split()[0]}.")

    try:
        import serial  # noqa: F401
    except ImportError:
        _die("Brak biblioteki pyserial",
             "Zainstaluj zależności:\n\n"
             "    python -m pip install -r requirements.txt\n\n"
             "albo:\n\n"
             "    python -m pip install pyserial")

    try:
        import tkinter  # noqa: F401
    except ImportError:
        _die("Brak Tkinter",
             "Ta instalacja Pythona nie ma modułu tkinter.\n"
             "Zainstaluj Pythona z python.org z domyślnymi opcjami.")

    try:
        from wheelbridge.ui.app import main as run_app
        run_app()
    except Exception:
        _die("Błąd aplikacji",
             "Aplikacja zakończyła się błędem:\n\n" + traceback.format_exc())


if __name__ == "__main__":
    main()
