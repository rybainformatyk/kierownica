"""Sprawy Windowsa: jedna kopia, autostart, polozenie okna."""

import time
import uuid

import pytest

from wheelbridge import winsys

windows_only = pytest.mark.skipif(not winsys.IS_WINDOWS, reason="tylko Windows")


def test_parse_geometry():
    assert winsys.parse_geometry("1240x880+40+60") == (1240, 880, 40, 60)
    assert winsys.parse_geometry("1240x880+-1700+-5") == (1240, 880, -1700, -5)
    for bad in ("", "zle", "0x10+1+1", "10x10", None):
        assert winsys.parse_geometry(bad) is None


def test_covers():
    screen = (0, 0, 1920, 1080)
    assert winsys.covers(screen, (100, 100, 900, 700))
    assert not winsys.covers(screen, (1800, 100, 2400, 700))    # drugi monitor
    assert winsys.covers((0, 0, 1920, 1040), (0, 0, 1921, 1041))  # luz 2 px


def test_autostart_script():
    cmd = '"C:\\Py\\pythonw.exe" "D:\\gry\\panel\\kierownica.py" --minimized'
    assert winsys.autostart_script(cmd) == "D:\\gry\\panel\\kierownica.py"
    assert winsys.autostart_script(None) is None
    assert winsys.autostart_script("cos.exe") is None


@windows_only
def test_single_instance_mutex():
    name = f"Local\\KierownicaPanelTest-{uuid.uuid4().hex}"
    assert winsys.claim_single_instance(name) is True
    assert winsys.claim_single_instance(name) is False


@windows_only
def test_autostart_roundtrip(monkeypatch, tmp_path):
    import winreg
    key = rf"Software\KierownicaPanelTest-{uuid.uuid4().hex}"
    monkeypatch.setattr(winsys, "RUN_KEY", key)     # nie prawdziwy klucz Run
    try:
        assert winsys.autostart_get() is None
        cmd = winsys.autostart_command(tmp_path / "kierownica.py")
        assert cmd.endswith("--minimized")
        winsys.autostart_set(cmd)
        assert winsys.autostart_get() == cmd
        winsys.autostart_set(None)
        assert winsys.autostart_get() is None
        winsys.autostart_set(None)                  # brak wpisu - bez bledu
    finally:
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key)
        except OSError:
            pass


@windows_only
def test_work_area():
    import tkinter as tk
    root = tk.Tk()
    try:
        w = root.winfo_screenwidth()
        area = winsys.work_area_at(40, 40, 800)
        assert area is not None and area[2] - area[0] <= w
        assert winsys.work_area_at(-20000, 40, 800) is None     # brak monitora
        assert winsys.work_area_at(w - 60, 40, 800) is None     # widac 60 px
    finally:
        root.destroy()


@windows_only
def test_visible_window_is_not_hidden():
    import tkinter as tk
    root = tk.Tk()
    try:
        # Okno z testu nie dostaje pierwszego planu (Windows chroni przed
        # kradzieza fokusu) - bez "topmost" zaslanialby je terminal.
        root.attributes("-topmost", True)
        root.geometry("300x200+100+100")
        root.update()
        time.sleep(0.1)
        root.update()
        hwnd = int(root.wm_frame(), 16)
        assert winsys.window_hidden(hwnd) is False
        assert winsys.window_hidden(0) is False
    finally:
        root.destroy()
