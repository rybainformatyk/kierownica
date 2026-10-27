"""
Backend vJoy - wirtualna kierownica widziana przez gry jako urzadzenie
DirectInput z osiami i Force Feedback.

vJoy to podpisany sterownik jadra (https://sourceforge.net/projects/vjoystick/).
Uzywamy go, bo jest to jedyny sposob, zeby z poziomu aplikacji uzytkownika
utworzyc urzadzenie DirectInput, ktore ma osobne osie pedalow, 32+ przyciskow
ORAZ przyjmuje Force Feedback od gry. Emulatory pada (ViGEm/XInput) tego nie
potrafia - gra widzi wtedy pada, nie kierownice.

Wszystkie wywolania DLL idza przez pojedyncze funkcje SetAxis/SetBtn, a nie
przez strukture JOYSTICK_POSITION - to eliminuje ryzyko rozjechania sie
ukladu pol miedzy wersjami vJoy.
"""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from .base import AXIS_KEYS, OutputBackend, OutputError

# --------------------------------------------------------------- stale vJoy

VJD_STAT_OWN = 0      # urzadzenie nalezy do tej aplikacji
VJD_STAT_FREE = 1     # wolne
VJD_STAT_BUSY = 2     # zajete przez inna aplikacje
VJD_STAT_MISS = 3     # nie istnieje (nie wlaczone w vJoyConf)
VJD_STAT_UNKN = 4

STATUS_TEXT = {
    VJD_STAT_OWN: "zajete przez ta aplikacje",
    VJD_STAT_FREE: "wolne",
    VJD_STAT_BUSY: "zajete przez inny program",
    VJD_STAT_MISS: "nie istnieje - wlacz je w vJoyConf",
    VJD_STAT_UNKN: "stan nieznany",
}

#: HID usage ID osi - zgodne z public.h z SDK vJoy
HID_USAGE = {
    "X": 0x30,
    "Y": 0x31,
    "Z": 0x32,
    "RX": 0x33,
    "RY": 0x34,
    "RZ": 0x35,
    "SL0": 0x36,
    "SL1": 0x37,
    "WHL": 0x38,
}

AXIS_LABELS = {
    "X": "X (kierownica)",
    "Y": "Y",
    "Z": "Z",
    "RX": "Rx",
    "RY": "Ry",
    "RZ": "Rz",
    "SL0": "Slider",
    "SL1": "Dial",
    "WHL": "Wheel",
}

_DLL_NAME = "vJoyInterface.dll"


class VJoyNotAvailable(OutputError):
    """vJoy nie jest zainstalowany albo DLL nie daje sie zaladowac."""


# --------------------------------------------------------- szukanie biblioteki

def _candidate_dirs() -> List[Path]:
    dirs: List[Path] = []
    env = os.environ.get("VJOY_DIR")
    if env:
        dirs.append(Path(env))

    arch = "x64" if sys.maxsize > 2 ** 32 else "x86"
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                 os.environ.get("ProgramW6432", r"C:\Program Files")):
        root = Path(base) / "vJoy"
        dirs += [root / arch, root, root / "x64", root / "x86"]

    # obok aplikacji - przydatne przy wersji przenosnej
    here = Path(__file__).resolve().parent.parent.parent
    dirs += [here, here / "vjoy", here / arch]

    dirs += _registry_dirs()

    seen, out = set(), []
    for d in dirs:
        key = str(d).lower()
        if key not in seen:
            seen.add(key)
            out.append(d)
    return out


def _registry_dirs() -> List[Path]:
    out: List[Path] = []
    try:
        import winreg
    except ImportError:
        return out
    keys = [
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Shaul Eizikovich\vJoy", "InstallLocation"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Shaul Eizikovich\vJoy",
         "InstallLocation"),
    ]
    for root, path, value in keys:
        try:
            with winreg.OpenKey(root, path) as k:
                loc, _ = winreg.QueryValueEx(k, value)
                if loc:
                    p = Path(loc)
                    out += [p, p / ("x64" if sys.maxsize > 2 ** 32 else "x86")]
        except OSError:
            continue
    # wpisy odinstalowywania
    for root, sub in ((winreg.HKEY_LOCAL_MACHINE,
                       r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),):
        try:
            with winreg.OpenKey(root, sub) as k:
                for i in range(winreg.QueryInfoKey(k)[0]):
                    try:
                        name = winreg.EnumKey(k, i)
                        with winreg.OpenKey(k, name) as sk:
                            disp = winreg.QueryValueEx(sk, "DisplayName")[0]
                            if "vjoy" not in str(disp).lower():
                                continue
                            loc = winreg.QueryValueEx(sk, "InstallLocation")[0]
                            if loc:
                                p = Path(loc)
                                out += [p, p / ("x64" if sys.maxsize > 2 ** 32 else "x86")]
                    except OSError:
                        continue
        except OSError:
            continue
    return out


def find_vjoy_dll() -> Optional[Path]:
    for d in _candidate_dirs():
        candidate = d / _DLL_NAME
        try:
            if candidate.is_file():
                return candidate
        except OSError:
            continue
    return None


def vjoy_diagnostics() -> Dict[str, object]:
    """Zbiera informacje do zakladki diagnostyki - nigdy nie rzuca wyjatkiem."""
    info: Dict[str, object] = {
        "python_bits": 64 if sys.maxsize > 2 ** 32 else 32,
        "dll_path": None,
        "dll_loaded": False,
        "enabled": False,
        "version": None,
        "product": None,
        "devices": {},
        "error": None,
    }
    dll_path = find_vjoy_dll()
    info["dll_path"] = str(dll_path) if dll_path else None
    if dll_path is None:
        info["error"] = (f"Nie znaleziono {_DLL_NAME}. Zainstaluj vJoy "
                         f"(wersja {'x64' if info['python_bits'] == 64 else 'x86'} "
                         f"musi odpowiadac Pythonowi).")
        return info
    try:
        api = _VJoyApi(dll_path)
    except Exception as exc:
        info["error"] = f"Nie udalo sie zaladowac DLL: {exc}"
        return info

    info["dll_loaded"] = True
    try:
        info["enabled"] = bool(api.dll.vJoyEnabled())
        info["version"] = api.dll.GetvJoyVersion()
        ptr = api.dll.GetvJoyProductString()
        if ptr:
            info["product"] = ctypes.wstring_at(ptr)
    except Exception as exc:
        info["error"] = f"Blad odpytywania vJoy: {exc}"
        return info

    devices: Dict[int, Dict[str, object]] = {}
    for rid in range(1, 17):
        try:
            status = api.dll.GetVJDStatus(rid)
            if status == VJD_STAT_MISS:
                continue
            devices[rid] = {
                "status": status,
                "status_text": STATUS_TEXT.get(status, "?"),
                "buttons": api.dll.GetVJDButtonNumber(rid),
                "axes": [k for k in AXIS_KEYS
                         if api.dll.GetVJDAxisExist(rid, HID_USAGE[k])],
                "disc_pov": api.dll.GetVJDDiscPovNumber(rid),
                "cont_pov": api.dll.GetVJDContPovNumber(rid),
                "ffb": bool(api.dll.IsDeviceFfb(rid)),
            }
        except Exception:
            continue
    info["devices"] = devices
    return info


# ------------------------------------------------------------ wiazanie z DLL

class _VJoyApi:
    """Deklaracje sygnatur funkcji vJoyInterface.dll."""

    def __init__(self, dll_path: Path) -> None:
        # Katalog DLL dodajemy do sciezki szukania, bo vJoyInterface moze
        # ciagnac za soba zaleznosci z tego samego folderu.
        if hasattr(os, "add_dll_directory"):
            try:
                self._dll_dir_handle = os.add_dll_directory(str(dll_path.parent))
            except OSError:
                self._dll_dir_handle = None
        else:
            self._dll_dir_handle = None

        self.dll = ctypes.WinDLL(str(dll_path))
        d = self.dll

        d.vJoyEnabled.restype = ctypes.c_short
        d.vJoyEnabled.argtypes = []

        d.GetvJoyVersion.restype = ctypes.c_short
        d.GetvJoyVersion.argtypes = []

        d.GetvJoyProductString.restype = ctypes.c_void_p
        d.GetvJoyProductString.argtypes = []

        d.GetVJDStatus.restype = ctypes.c_int
        d.GetVJDStatus.argtypes = [ctypes.c_uint]

        d.AcquireVJD.restype = ctypes.c_int
        d.AcquireVJD.argtypes = [ctypes.c_uint]

        d.RelinquishVJD.restype = None
        d.RelinquishVJD.argtypes = [ctypes.c_uint]

        d.ResetVJD.restype = ctypes.c_int
        d.ResetVJD.argtypes = [ctypes.c_uint]

        d.GetVJDButtonNumber.restype = ctypes.c_int
        d.GetVJDButtonNumber.argtypes = [ctypes.c_uint]

        d.GetVJDDiscPovNumber.restype = ctypes.c_int
        d.GetVJDDiscPovNumber.argtypes = [ctypes.c_uint]

        d.GetVJDContPovNumber.restype = ctypes.c_int
        d.GetVJDContPovNumber.argtypes = [ctypes.c_uint]

        d.GetVJDAxisExist.restype = ctypes.c_int
        d.GetVJDAxisExist.argtypes = [ctypes.c_uint, ctypes.c_uint]

        d.GetVJDAxisMax.restype = ctypes.c_int
        d.GetVJDAxisMax.argtypes = [ctypes.c_uint, ctypes.c_uint,
                                    ctypes.POINTER(ctypes.c_long)]

        d.GetVJDAxisMin.restype = ctypes.c_int
        d.GetVJDAxisMin.argtypes = [ctypes.c_uint, ctypes.c_uint,
                                    ctypes.POINTER(ctypes.c_long)]

        d.SetAxis.restype = ctypes.c_int
        d.SetAxis.argtypes = [ctypes.c_long, ctypes.c_uint, ctypes.c_uint]

        d.SetBtn.restype = ctypes.c_int
        d.SetBtn.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_ubyte]

        d.SetDiscPov.restype = ctypes.c_int
        d.SetDiscPov.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_ubyte]

        d.SetContPov.restype = ctypes.c_int
        d.SetContPov.argtypes = [ctypes.c_ulong, ctypes.c_uint, ctypes.c_ubyte]

        d.IsDeviceFfb.restype = ctypes.c_int
        d.IsDeviceFfb.argtypes = [ctypes.c_uint]

        # Funkcje FFB sa opcjonalne - starsze wersje DLL moga ich nie miec.
        self.has_ffb_api = True
        try:
            d.FfbRegisterGenCB.restype = None
            d.FfbRegisterGenCB.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        except AttributeError:
            self.has_ffb_api = False


_api_cache: Dict[str, _VJoyApi] = {}


def _get_api() -> _VJoyApi:
    dll_path = find_vjoy_dll()
    if dll_path is None:
        bits = "x64" if sys.maxsize > 2 ** 32 else "x86"
        raise VJoyNotAvailable(
            f"Nie znaleziono {_DLL_NAME}.\n\n"
            f"Zainstaluj vJoy (vjoystick na SourceForge), a potem w programie "
            f"vJoyConf wlacz urzadzenie nr 1 z osiami X, Y, Z, Rx, Ry, Rz, "
            f"32 przyciskami i wlaczonym Force Feedback.\n\n"
            f"Uwaga: uzywasz Pythona {bits}, wiec potrzebna jest biblioteka "
            f"vJoy w tej samej architekturze."
        )
    key = str(dll_path).lower()
    api = _api_cache.get(key)
    if api is None:
        try:
            api = _VJoyApi(dll_path)
        except OSError as exc:
            raise VJoyNotAvailable(
                f"Nie udalo sie zaladowac {dll_path}:\n{exc}\n\n"
                f"Najczestsza przyczyna to niezgodnosc architektury "
                f"(Python {'64' if sys.maxsize > 2 ** 32 else '32'}-bit "
                f"vs. biblioteka vJoy)."
            ) from exc
        _api_cache[key] = api
    return api


# -------------------------------------------------------------------- backend

class VJoyBackend(OutputBackend):
    """Jedno urzadzenie vJoy. Osie przyjmuja wartosci 0..1."""

    name = "vjoy"
    supports_ffb = True

    def __init__(self, device_id: int = 1) -> None:
        self.device_id = int(device_id)
        self._api: Optional[_VJoyApi] = None
        self._open = False
        self._axes: Dict[str, Tuple[int, int]] = {}   # klucz -> (min, max)
        self._buttons = 0
        self._disc_pov = 0
        self._cont_pov = 0
        self._ffb_capable = False
        self._pressed: set = set()
        self._last_hat = -2
        self._last_axes: Dict[str, int] = {}
        self._version = 0
        self._product = ""

    # ------------------------------------------------------------ wlasciwosci

    @property
    def label(self) -> str:
        return f"vJoy urzadzenie #{self.device_id}"

    @property
    def is_open(self) -> bool:
        return self._open

    @property
    def ffb_capable(self) -> bool:
        return self._ffb_capable

    @property
    def api(self) -> Optional[_VJoyApi]:
        return self._api

    def available_axes(self) -> List[str]:
        return list(self._axes.keys())

    def max_buttons(self) -> int:
        return self._buttons

    def has_hat(self) -> bool:
        return self._disc_pov > 0 or self._cont_pov > 0

    # ---------------------------------------------------------------- otwarcie

    def open(self) -> None:
        if self._open:
            return
        api = _get_api()

        if not api.dll.vJoyEnabled():
            raise VJoyNotAvailable(
                "Sterownik vJoy jest zainstalowany, ale wylaczony.\n"
                "Uruchom vJoyConf i wlacz vJoy, albo sprawdz w Menedzerze "
                "urzadzen, czy 'vJoy Device' nie ma bledu."
            )

        self._version = int(api.dll.GetvJoyVersion())
        try:
            ptr = api.dll.GetvJoyProductString()
            self._product = ctypes.wstring_at(ptr) if ptr else ""
        except Exception:
            self._product = ""

        rid = self.device_id
        status = api.dll.GetVJDStatus(rid)
        if status == VJD_STAT_MISS:
            raise VJoyNotAvailable(
                f"Urzadzenie vJoy #{rid} nie istnieje.\n"
                f"Otworz vJoyConf, zaznacz 'Device {rid}' i kliknij Apply."
            )
        if status == VJD_STAT_BUSY:
            raise VJoyNotAvailable(
                f"Urzadzenie vJoy #{rid} jest zajete przez inny program.\n"
                f"Zamknij pozostale aplikacje korzystajace z vJoy "
                f"(np. drugie okno tego panelu, x360ce, FreePIE)."
            )
        if status not in (VJD_STAT_FREE, VJD_STAT_OWN):
            raise VJoyNotAvailable(
                f"Urzadzenie vJoy #{rid}: {STATUS_TEXT.get(status, status)}"
            )

        if not api.dll.AcquireVJD(rid):
            raise VJoyNotAvailable(
                f"Nie udalo sie przejac urzadzenia vJoy #{rid}. "
                f"Sprobuj uruchomic panel jako administrator."
            )

        self._api = api
        self._open = True

        # odczyt mozliwosci urzadzenia
        self._axes.clear()
        for key, usage in HID_USAGE.items():
            if not api.dll.GetVJDAxisExist(rid, usage):
                continue
            lo = ctypes.c_long(0)
            hi = ctypes.c_long(32767)
            if not api.dll.GetVJDAxisMin(rid, usage, ctypes.byref(lo)):
                lo = ctypes.c_long(0)
            if not api.dll.GetVJDAxisMax(rid, usage, ctypes.byref(hi)):
                hi = ctypes.c_long(32767)
            if hi.value <= lo.value:
                lo, hi = ctypes.c_long(0), ctypes.c_long(32767)
            self._axes[key] = (lo.value, hi.value)

        self._buttons = max(0, int(api.dll.GetVJDButtonNumber(rid)))
        self._disc_pov = max(0, int(api.dll.GetVJDDiscPovNumber(rid)))
        self._cont_pov = max(0, int(api.dll.GetVJDContPovNumber(rid)))
        try:
            self._ffb_capable = bool(api.dll.IsDeviceFfb(rid)) and api.has_ffb_api
        except Exception:
            self._ffb_capable = False

        api.dll.ResetVJD(rid)
        self._pressed.clear()
        self._last_axes.clear()
        self._last_hat = -2
        self.center()

    def close(self) -> None:
        if not self._open or self._api is None:
            self._open = False
            return
        try:
            self.center()
            self._api.dll.RelinquishVJD(self.device_id)
        except Exception:
            pass
        self._open = False
        self._api = None

    # ---------------------------------------------------------------- wyjscie

    def center(self, neutral: Optional[Dict[str, float]] = None) -> None:
        """Ustawia osie w pozycji spoczynkowej.

        `neutral` to klucz osi -> wartosc 0..1. Dla osi bez wpisu uzywany jest
        srodek zakresu (to wlasciwe zachowanie dla kierownicy; pedaly powinny
        dostac 0.0, dlatego silnik przekazuje tu konkretne wartosci).
        """
        if not self._open or self._api is None:
            return
        neutral = neutral or {}
        for key, (lo, hi) in self._axes.items():
            v = neutral.get(key)
            if v is None:
                raw = (lo + hi) // 2
            else:
                v = 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
                raw = lo + int(round(v * (hi - lo)))
            try:
                self._api.dll.SetAxis(raw, self.device_id, HID_USAGE[key])
            except Exception:
                pass
            self._last_axes[key] = raw
        for btn in list(self._pressed):
            try:
                self._api.dll.SetBtn(0, self.device_id, btn)
            except Exception:
                pass
        self._pressed.clear()
        self._set_hat(-1)

    def update(self, axes: Dict[str, float], buttons: Iterable[int],
               hat: int = -1) -> None:
        if not self._open or self._api is None:
            return
        d = self._api.dll
        rid = self.device_id

        for key, value in axes.items():
            rng = self._axes.get(key)
            if rng is None:
                continue
            lo, hi = rng
            v = 0.0 if value != value else value          # NaN -> 0
            v = 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
            raw = lo + int(round(v * (hi - lo)))
            if self._last_axes.get(key) == raw:
                continue
            self._last_axes[key] = raw
            d.SetAxis(raw, rid, HID_USAGE[key])

        wanted = {b for b in buttons if 1 <= b <= self._buttons}
        for btn in wanted - self._pressed:
            d.SetBtn(1, rid, btn)
        for btn in self._pressed - wanted:
            d.SetBtn(0, rid, btn)
        self._pressed = wanted

        self._set_hat(hat)

    def _set_hat(self, hat: int) -> None:
        if self._api is None or hat == self._last_hat:
            return
        self._last_hat = hat
        d = self._api.dll
        rid = self.device_id
        try:
            if self._disc_pov > 0:
                # vJoy POV dyskretny: 0=gora, 1=prawo, 2=dol, 3=lewo, -1=srodek
                d.SetDiscPov(hat if 0 <= hat <= 3 else -1, rid, 1)
            elif self._cont_pov > 0:
                # ciagly: 0..35999 setne stopnia, -1 = srodek
                angles = {0: 0, 1: 9000, 2: 18000, 3: 27000}
                d.SetContPov(ctypes.c_ulong(angles.get(hat, 0xFFFFFFFF)).value, rid, 1)
        except Exception:
            pass

    # --------------------------------------------------------------- opisy

    def status_text(self) -> str:
        if not self._open:
            return "vJoy nieaktywne"
        axes = ", ".join(self._axes.keys()) or "brak osi"
        ffb = "FFB tak" if self._ffb_capable else "FFB nie"
        return (f"vJoy #{self.device_id} - osie: {axes}; "
                f"przyciski: {self._buttons}; POV: "
                f"{self._disc_pov + self._cont_pov}; {ffb}")

    def version_text(self) -> str:
        if not self._version:
            return ""
        hi, lo = (self._version >> 8) & 0xFF, self._version & 0xFF
        name = self._product or "vJoy"
        return f"{name} (wersja {hi:x}.{lo:02x})"
