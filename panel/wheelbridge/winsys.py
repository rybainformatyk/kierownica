"""
Drobne sprawy Windowsa dla okna panelu - przez ctypes, bez nowych bibliotek.

  * jedna kopia panelu naraz (nazwany mutex + przywrocenie dzialajacego okna),
  * uruchamianie razem z Windowsem (klucz Run w rejestrze uzytkownika),
  * czy zapamietane polozenie okna lezy na ktoryms monitorze,
  * czy okno panelu jest zasloniete (np. gra na pelnym ekranie),
  * mruganie przyciskiem na pasku zadan.

Poza Windowsem albo przy bledzie API kazda funkcja zwraca bezpieczna
wartosc domyslna - nic stad nie moze wywrocic aplikacji.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import Optional, Tuple

IS_WINDOWS = sys.platform == "win32"

#: Nazwa mutexu jednej kopii panelu. "Local\" - osobno dla kazdej sesji
#: Windows, wiec dwoch zalogowanych uzytkownikow sobie nie przeszkadza.
INSTANCE_MUTEX = "Local\\KierownicaPanel"

#: Klucz autostartu i nazwa wpisu (HKEY_CURRENT_USER - bez uprawnien
#: administratora). Atrybuty modulu, zeby testy mogly uzyc innego klucza.
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "KierownicaPanel"

_ERROR_ALREADY_EXISTS = 183
_SW_SHOW = 5
_SW_RESTORE = 9
_GW_HWNDPREV = 3
_MONITOR_DEFAULTTONULL = 0
_DWMWA_EXTENDED_FRAME_BOUNDS = 9
_DWMWA_CLOAKED = 14
_FLASHW_TRAY = 0x2
_FLASHW_TIMERNOFG = 0xC

#: Uchwyt mutexu - otwarty do konca procesu, Windows zamknie go sam
#: (takze po awarii panelu).
_mutex = None

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    try:
        _dwmapi = ctypes.WinDLL("dwmapi")
    except OSError:
        _dwmapi = None

    _WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND,
                                      wintypes.LPARAM)

    class _MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]

    class _FLASHWINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.UINT), ("hwnd", wintypes.HWND),
                    ("dwFlags", wintypes.DWORD), ("uCount", wintypes.UINT),
                    ("dwTimeout", wintypes.DWORD)]

    _kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL,
                                       wintypes.LPCWSTR]
    _kernel32.CreateMutexW.restype = wintypes.HANDLE
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _user32.EnumWindows.argtypes = [_WNDENUMPROC, wintypes.LPARAM]
    _user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR,
                                      ctypes.c_int]
    _user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR,
                                       ctypes.c_int]
    _user32.GetWindowThreadProcessId.argtypes = [
        wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    _user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    _user32.IsIconic.argtypes = [wintypes.HWND]
    _user32.IsWindowVisible.argtypes = [wintypes.HWND]
    _user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    _user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    _user32.GetForegroundWindow.restype = wintypes.HWND
    _user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
    _user32.GetWindow.restype = wintypes.HWND
    _user32.GetWindowRect.argtypes = [wintypes.HWND,
                                      ctypes.POINTER(wintypes.RECT)]
    _user32.MonitorFromRect.argtypes = [ctypes.POINTER(wintypes.RECT),
                                        wintypes.DWORD]
    _user32.MonitorFromRect.restype = wintypes.HMONITOR
    _user32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR,
                                        ctypes.POINTER(_MONITORINFO)]
    _user32.FlashWindowEx.argtypes = [ctypes.POINTER(_FLASHWINFO)]
    if _dwmapi is not None:
        _dwmapi.DwmGetWindowAttribute.argtypes = [
            wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        _dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long


# ------------------------------------------------------ jedna kopia panelu

def claim_single_instance(name: str = INSTANCE_MUTEX) -> bool:
    """True, gdy to jedyna dzialajaca kopia panelu.

    Druga kopia walczylaby z pierwsza o port COM (szukalaby plytki bez
    konca) i o urzadzenie vJoy. Przy bledzie API zwraca True - lepiej
    uruchomic panel niz go zablokowac.
    """
    global _mutex
    if not IS_WINDOWS:
        return True
    try:
        ctypes.set_last_error(0)
        handle = _kernel32.CreateMutexW(None, False, name)
        error = ctypes.get_last_error()
    except Exception:
        return True
    if not handle:
        return True
    if error == _ERROR_ALREADY_EXISTS:
        _kernel32.CloseHandle(handle)
        return False
    _mutex = handle
    return True


def bring_to_front(title_prefix: str) -> bool:
    """Przywraca i wysuwa na wierzch okno Tk innego procesu z takim tytulem.

    False, gdy takiego okna nie ma (np. dzialajacy panel dopiero startuje).
    """
    if not IS_WINDOWS:
        return False
    found = []
    own_pid = os.getpid()

    def visit(hwnd, _lparam):
        try:
            if _class_name(hwnd) != "TkTopLevel":
                return True
            if not _window_text(hwnd).startswith(title_prefix):
                return True
            pid = wintypes.DWORD()
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == own_pid:
                return True
            found.append(hwnd)
            return False                    # mamy - koniec szukania
        except Exception:
            return True

    try:
        _user32.EnumWindows(_WNDENUMPROC(visit), 0)
        if not found:
            return False
        hwnd = found[0]
        _user32.ShowWindow(hwnd, _SW_RESTORE if _user32.IsIconic(hwnd)
                           else _SW_SHOW)
        _user32.SetForegroundWindow(hwnd)
        return True
    except Exception:
        return False


def _class_name(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(64)
    _user32.GetClassNameW(hwnd, buf, len(buf))
    return buf.value


def _window_text(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(256)
    _user32.GetWindowTextW(hwnd, buf, len(buf))
    return buf.value


# ------------------------------------------------- start razem z Windowsem

def autostart_command(script: Path) -> str:
    """Polecenie dla klucza Run: pythonw (bez okna konsoli), skrypt, --minimized."""
    exe = Path(sys.executable)
    gui = exe.with_name("pythonw.exe")
    if exe.name.lower() == "python.exe" and gui.is_file():
        exe = gui
    return f'"{exe}" "{Path(script).resolve()}" --minimized'


def autostart_script(command: Optional[str]) -> Optional[str]:
    """Sciezka skryptu z polecenia autostartu (drugi napis w cudzyslowie)."""
    parts = re.findall(r'"([^"]*)"', command or "")
    return parts[1] if len(parts) >= 2 else None


def autostart_get() -> Optional[str]:
    """Polecenie z klucza Run albo None, gdy panel nie startuje z Windowsem."""
    if not IS_WINDOWS:
        return None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            value, _kind = winreg.QueryValueEx(key, RUN_VALUE)
        return str(value)
    except OSError:
        return None


def autostart_set(command: Optional[str]) -> None:
    """Wpisuje polecenie do klucza Run; None usuwa wpis. Bledy: OSError."""
    if not IS_WINDOWS:
        raise OSError("autostart dziala tylko w Windowsie")
    import winreg
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                            winreg.KEY_SET_VALUE) as key:
        if command is None:
            try:
                winreg.DeleteValue(key, RUN_VALUE)
            except FileNotFoundError:
                pass
        else:
            winreg.SetValueEx(key, RUN_VALUE, 0, winreg.REG_SZ, command)


# ---------------------------------------------------------------- polozenie

_GEOMETRY = re.compile(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)")


def parse_geometry(text: str) -> Optional[Tuple[int, int, int, int]]:
    """"1240x880+40+-10" -> (szer, wys, x, y); None dla czegos innego."""
    m = _GEOMETRY.fullmatch(str(text or "").strip())
    if not m:
        return None
    w, h, x, y = (int(g) for g in m.groups())
    return (w, h, x, y) if w > 0 and h > 0 else None


def work_area_at(x: int, y: int, w: int) -> Optional[Tuple[int, int, int, int]]:
    """Obszar roboczy (l, g, p, d) monitora pod paskiem tytulu okna.

    None, gdy pasek tytulu nie lezy porzadnie na zadnym monitorze - wtedy
    okna nie dalo by sie zlapac i przesunac (np. zapamietane polozenie
    z odlaczonego monitora). Poza Windowsem tez None.
    """
    if not IS_WINDOWS:
        return None
    try:
        strip = wintypes.RECT(x, y, x + w, y + 30)
        monitor = _user32.MonitorFromRect(ctypes.byref(strip),
                                          _MONITOR_DEFAULTTONULL)
        if not monitor:
            return None
        info = _MONITORINFO()
        info.cbSize = ctypes.sizeof(info)
        if not _user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
            return None
        work = info.rcWork
        wide = min(strip.right, work.right) - max(strip.left, work.left)
        tall = min(strip.bottom, work.bottom) - max(strip.top, work.top)
        if wide < 120 or tall < 20:
            return None
        return work.left, work.top, work.right, work.bottom
    except Exception:
        return None


# --------------------------------------------------------- zasloniecie okna

def covers(outer: Tuple[int, int, int, int],
           inner: Tuple[int, int, int, int], slack: int = 2) -> bool:
    """Czy prostokat outer (l, g, p, d) zakrywa caly inner."""
    return (outer[0] - slack <= inner[0] and outer[1] - slack <= inner[1]
            and outer[2] + slack >= inner[2] and outer[3] + slack >= inner[3])


def window_hidden(hwnd: int) -> bool:
    """Czy okna panelu na pewno nie widac.

    Tak jest, gdy lezy na innym pulpicie wirtualnym albo gdy w calosci
    zaslania je okno innego programu, ktore jest na pierwszym planie i nad
    nim (np. gra na pelnym ekranie). Okno na drugim monitorze, czesciowo
    przykryte albo za pulpitem liczy sie jako widoczne - w razie watpliwosci
    lepiej rysowac niepotrzebnie niz nie rysowac.
    """
    if not IS_WINDOWS or not hwnd:
        return False
    try:
        if _cloaked(hwnd):
            return True
        fg = _user32.GetForegroundWindow()
        if not fg or fg == hwnd:
            return False
        pid = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(fg, ctypes.byref(pid))
        if pid.value == os.getpid():
            return False                    # nasze menu albo okno dialogowe
        if (_user32.IsIconic(fg) or not _user32.IsWindowVisible(fg)
                or _cloaked(fg) or not _is_above(fg, hwnd)):
            return False
        outer, inner = _frame(fg), _frame(hwnd)
        return outer is not None and inner is not None and covers(outer, inner)
    except Exception:
        return False


def _is_above(top, hwnd) -> bool:
    """Czy okno top lezy nad hwnd (idziemy w gore po kolejnosci Z)."""
    h = _user32.GetWindow(hwnd, _GW_HWNDPREV)
    for _ in range(4000):                   # zabezpieczenie przed petla
        if not h:
            return False
        if h == top:
            return True
        h = _user32.GetWindow(h, _GW_HWNDPREV)
    return False


def _frame(hwnd) -> Optional[Tuple[int, int, int, int]]:
    """Widoczna ramka okna. GetWindowRect liczy tez niewidoczne brzegi do
    zmiany rozmiaru (kilka px) - pelnoekranowa gra nie "zakrywalaby" wtedy
    zmaksymalizowanego panelu. DWM podaje ramke bez nich."""
    rect = wintypes.RECT()
    if _dwmapi is not None and _dwmapi.DwmGetWindowAttribute(
            hwnd, _DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(rect),
            ctypes.sizeof(rect)) == 0:
        return rect.left, rect.top, rect.right, rect.bottom
    if _user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return rect.left, rect.top, rect.right, rect.bottom
    return None


def _cloaked(hwnd) -> bool:
    """Okno ukryte przez DWM - np. lezy na innym pulpicie wirtualnym."""
    if _dwmapi is None:
        return False
    value = wintypes.DWORD(0)
    ok = _dwmapi.DwmGetWindowAttribute(hwnd, _DWMWA_CLOAKED,
                                       ctypes.byref(value),
                                       ctypes.sizeof(value)) == 0
    return ok and value.value != 0


# --------------------------------------------------------------- pasek zadan

def is_foreground(hwnd: int) -> bool:
    if not IS_WINDOWS or not hwnd:
        return False
    try:
        return _user32.GetForegroundWindow() == hwnd
    except Exception:
        return False


def flash_taskbar(hwnd: int) -> None:
    """Mruga przyciskiem panelu na pasku zadan, az okno wroci na pierwszy plan."""
    if not IS_WINDOWS or not hwnd:
        return
    try:
        info = _FLASHWINFO(ctypes.sizeof(_FLASHWINFO), hwnd,
                           _FLASHW_TRAY | _FLASHW_TIMERNOFG, 0, 0)
        _user32.FlashWindowEx(ctypes.byref(info))
    except Exception:
        pass
