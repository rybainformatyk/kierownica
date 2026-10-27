"""
Backend "Pad" - wirtualny kontroler Xbox 360 przez ViGEmBus.

Po co to jest
    Czesc gier (NFS, wiele gier zrecznosciowych i portow z konsol) czyta
    wylacznie XInput i w ogole nie widzi urzadzen DirectInput - czyli nie
    widzi kierownicy na vJoy. Ten backend pokazuje te sama kierownice jako
    zwyklego pada Xbox: skret idzie na drazek, gaz i hamulec na spusty
    analogowe, a przyciski plytki na A/B/X/Y i reszte.

Czego potrzeba
    * sterownik ViGEmBus (https://github.com/nefarius/ViGEmBus/releases),
    * biblioteka ViGEmClient.dll - lezy w katalogu vigem/ obok aplikacji.

Uwaga o spustach
    Spusty Xboxa sa 8-bitowe (0..255), wiec rozdzielczosc pedalow w trybie
    pada jest mniejsza niz na vJoy (16 bitow). To ograniczenie XInputa, nie
    aplikacji - dlatego tryb kierownicy zostaje dla symulatorow.
"""

from __future__ import annotations

import ctypes
import os
import sys
import time
from ctypes import Structure, c_ubyte, c_short, c_uint, c_ushort, c_void_p
from pathlib import Path
from typing import Dict, List, Optional

from .base import OutputBackend, OutputError

_DLL_NAME = "ViGEmClient.dll"

#: Przerwa miedzy raportami inicjalizujacymi pada (patrz _prime).
PRIME_DELAY = 0.03
#: Ile najwyzej czekamy, az Windows wyliczy nowo podlaczonego pada.
PRIME_TIMEOUT = 3.0


class _XINPUT_GAMEPAD(Structure):
    _fields_ = [("wButtons", c_ushort),
                ("bLeftTrigger", c_ubyte), ("bRightTrigger", c_ubyte),
                ("sThumbLX", c_short), ("sThumbLY", c_short),
                ("sThumbRX", c_short), ("sThumbRY", c_short)]


class _XINPUT_STATE(Structure):
    _fields_ = [("dwPacketNumber", c_uint), ("Gamepad", _XINPUT_GAMEPAD)]


def _load_xinput():
    """XInput sluzy nam tylko do weryfikacji stanu startowego pada."""
    for name in ("XInput1_4.dll", "XInput1_3.dll", "XInput9_1_0.dll"):
        try:
            dll = ctypes.WinDLL(name)
            dll.XInputGetState.argtypes = (c_uint, ctypes.POINTER(_XINPUT_STATE))
            dll.XInputGetState.restype = c_uint
            return dll
        except (OSError, AttributeError):
            continue
    return None


_XINPUT = _load_xinput()


def _xinput_slots() -> set:
    """Numery zajetych gniazd XInput (0-3)."""
    if _XINPUT is None:
        return set()
    out = set()
    st = _XINPUT_STATE()
    for i in range(4):
        try:
            if _XINPUT.XInputGetState(i, ctypes.byref(st)) == 0:
                out.add(i)
        except Exception:
            break
    return out


def _xinput_is_zeroed(slot: int) -> Optional[bool]:
    """True gdy gniazdo raportuje same zera. None gdy nie da sie odczytac."""
    if _XINPUT is None:
        return None
    st = _XINPUT_STATE()
    try:
        if _XINPUT.XInputGetState(slot, ctypes.byref(st)) != 0:
            return None
    except Exception:
        return None
    g = st.Gamepad
    return (g.wButtons == 0 and g.bLeftTrigger == 0 and g.bRightTrigger == 0
            and g.sThumbLX == 0 and g.sThumbLY == 0
            and g.sThumbRX == 0 and g.sThumbRY == 0)

VIGEM_ERROR_NONE = 0x20000000

VIGEM_ERRORS = {
    0x20000000: "OK",
    0xE0000001: "nie znaleziono sterownika ViGEmBus - zainstaluj go",
    0xE0000002: "brak wolnego gniazda na kolejne urzadzenie",
    0xE0000003: "nieprawidlowy cel",
    0xE0000004: "nie udalo sie usunac urzadzenia",
    0xE0000005: "urzadzenie jest juz podlaczone",
    0xE0000006: "urzadzenie niezainicjowane",
    0xE0000007: "urzadzenie nie jest podlaczone",
    0xE0000008: "niezgodna wersja sterownika ViGEmBus - zaktualizuj go",
    0xE0000009: "brak dostepu do sterownika (sprobuj uruchomic jako administrator)",
    0xE0000010: "callback juz zarejestrowany",
    0xE0000011: "nie znaleziono callbacku",
    0xE0000012: "magistrala juz polaczona",
    0xE0000013: "nieprawidlowy uchwyt magistrali",
    0xE0000014: "numer uzytkownika poza zakresem",
    0xE0000015: "nieprawidlowy parametr",
    0xE0000016: "operacja nieobslugiwana",
}

# --- maski przyciskow XUSB (zgodne z XINPUT_GAMEPAD_*) ---
XUSB = {
    "DPAD_UP": 0x0001,
    "DPAD_DOWN": 0x0002,
    "DPAD_LEFT": 0x0004,
    "DPAD_RIGHT": 0x0008,
    "START": 0x0010,
    "BACK": 0x0020,
    "LS": 0x0040,
    "RS": 0x0080,
    "LB": 0x0100,
    "RB": 0x0200,
    "GUIDE": 0x0400,
    "A": 0x1000,
    "B": 0x2000,
    "X": 0x4000,
    "Y": 0x8000,
}

#: Kolejnosc wyswietlania i przypisywania w interfejsie.
PAD_BUTTONS = ("A", "B", "X", "Y", "LB", "RB", "LS", "RS",
               "BACK", "START", "GUIDE",
               "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT")

PAD_BUTTON_LABELS = {
    "A": "A", "B": "B", "X": "X", "Y": "Y",
    "LB": "LB", "RB": "RB", "LS": "L3", "RS": "R3",
    "BACK": "Back", "START": "Start", "GUIDE": "Guide",
    "DPAD_UP": "Krzyzak gora", "DPAD_DOWN": "Krzyzak dol",
    "DPAD_LEFT": "Krzyzak lewo", "DPAD_RIGHT": "Krzyzak prawo",
}

#: Cele dla osi analogowych. LT/RT to spusty (0..255), reszta to drazki.
PAD_AXIS_TARGETS = ("-", "LX", "LY", "RX", "RY", "LT", "RT")
PAD_AXIS_LABELS = {
    "-": "nieuzywane",
    "LX": "Lewy drazek X",
    "LY": "Lewy drazek Y",
    "RX": "Prawy drazek X",
    "RY": "Prawy drazek Y",
    "LT": "Lewy spust",
    "RT": "Prawy spust",
}
STICK_TARGETS = ("LX", "LY", "RX", "RY")
TRIGGER_TARGETS = ("LT", "RT")


class XUSB_REPORT(Structure):
    """Raport zgodny z XINPUT_GAMEPAD (uklad pol wg naglowka ViGEm)."""
    _fields_ = [
        ("wButtons", c_ushort),
        ("bLeftTrigger", c_ubyte),
        ("bRightTrigger", c_ubyte),
        ("sThumbLX", c_short),
        ("sThumbLY", c_short),
        ("sThumbRX", c_short),
        ("sThumbRY", c_short),
    ]


class ViGEmNotAvailable(OutputError):
    """Brak biblioteki ViGEmClient.dll albo sterownika ViGEmBus."""


# ------------------------------------------------------- szukanie biblioteki

def _candidate_dirs() -> List[Path]:
    arch = "x64" if sys.maxsize > 2 ** 32 else "x86"
    dirs: List[Path] = []

    env = os.environ.get("VIGEM_DIR")
    if env:
        dirs.append(Path(env))

    # katalog vigem/ dolaczony do aplikacji - podstawowa sciezka
    app = Path(__file__).resolve().parent.parent.parent
    dirs += [app / "vigem" / arch, app / "vigem", app]

    # jesli uzytkownik ma zainstalowany pakiet vgamepad, uzyjemy jego kopii
    try:
        import vgamepad  # type: ignore
        base = Path(vgamepad.__file__).resolve().parent / "win" / "vigem" / "client"
        dirs += [base / arch, base]
    except Exception:
        pass

    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramW6432", r"C:\Program Files")):
        dirs.append(Path(base) / "Nefarius Software Solutions" / "ViGEm Bus Driver")

    seen, out = set(), []
    for d in dirs:
        key = str(d).lower()
        if key not in seen:
            seen.add(key)
            out.append(d)
    return out


def find_vigem_dll() -> Optional[Path]:
    for d in _candidate_dirs():
        candidate = d / _DLL_NAME
        try:
            if candidate.is_file():
                return candidate
        except OSError:
            continue
    return None


class _ViGEmApi:
    """Deklaracje sygnatur ViGEmClient.dll."""

    def __init__(self, dll_path: Path) -> None:
        if hasattr(os, "add_dll_directory"):
            try:
                os.add_dll_directory(str(dll_path.parent))
            except OSError:
                pass
        self.dll = ctypes.WinDLL(str(dll_path))
        d = self.dll

        d.vigem_alloc.argtypes = ()
        d.vigem_alloc.restype = c_void_p

        d.vigem_free.argtypes = (c_void_p,)
        d.vigem_free.restype = None

        d.vigem_connect.argtypes = (c_void_p,)
        d.vigem_connect.restype = c_uint

        d.vigem_disconnect.argtypes = (c_void_p,)
        d.vigem_disconnect.restype = None

        d.vigem_target_x360_alloc.argtypes = ()
        d.vigem_target_x360_alloc.restype = c_void_p

        d.vigem_target_free.argtypes = (c_void_p,)
        d.vigem_target_free.restype = None

        d.vigem_target_add.argtypes = (c_void_p, c_void_p)
        d.vigem_target_add.restype = c_uint

        d.vigem_target_remove.argtypes = (c_void_p, c_void_p)
        d.vigem_target_remove.restype = c_uint

        d.vigem_target_x360_update.argtypes = (c_void_p, c_void_p, XUSB_REPORT)
        d.vigem_target_x360_update.restype = c_uint


def _err(code: int) -> str:
    return VIGEM_ERRORS.get(code & 0xFFFFFFFF, f"nieznany blad 0x{code:08X}")


def vigem_diagnostics() -> Dict[str, object]:
    """Dane do zakladki diagnostyki - nigdy nie rzuca wyjatkiem."""
    info: Dict[str, object] = {
        "python_bits": 64 if sys.maxsize > 2 ** 32 else 32,
        "dll_path": None,
        "dll_loaded": False,
        "driver_ok": False,
        "error": None,
    }
    path = find_vigem_dll()
    info["dll_path"] = str(path) if path else None
    if path is None:
        info["error"] = (f"Nie znaleziono {_DLL_NAME}. Powinien lezec w katalogu "
                         f"vigem/ obok aplikacji.")
        return info
    try:
        api = _ViGEmApi(path)
    except OSError as exc:
        info["error"] = f"Nie udalo sie zaladowac biblioteki: {exc}"
        return info
    info["dll_loaded"] = True

    client = api.dll.vigem_alloc()
    if not client:
        info["error"] = "vigem_alloc() nie przydzielil pamieci"
        return info
    try:
        code = api.dll.vigem_connect(client)
        if code == VIGEM_ERROR_NONE:
            info["driver_ok"] = True
            api.dll.vigem_disconnect(client)
        else:
            info["error"] = _err(code)
    finally:
        api.dll.vigem_free(client)
    return info


# -------------------------------------------------------------------- stan

class PadState:
    """Gotowy raport pada. Wartosci juz w jednostkach XInputa."""

    __slots__ = ("buttons", "lt", "rt", "lx", "ly", "rx", "ry")

    def __init__(self, buttons: int = 0, lt: int = 0, rt: int = 0,
                 lx: int = 0, ly: int = 0, rx: int = 0, ry: int = 0) -> None:
        self.buttons = buttons
        self.lt = lt
        self.rt = rt
        self.lx = lx
        self.ly = ly
        self.rx = rx
        self.ry = ry

    def as_tuple(self) -> tuple:
        return (self.buttons, self.lt, self.rt, self.lx, self.ly, self.rx, self.ry)

    def pressed_names(self) -> List[str]:
        return [name for name, mask in XUSB.items() if self.buttons & mask]


# ----------------------------------------------------------------- backend

class XInputBackend(OutputBackend):
    """Wirtualny pad Xbox 360. Gry widza go jako zwykly kontroler XInput."""

    name = "pad"
    label = "Pad Xbox 360 (ViGEm)"
    supports_ffb = False

    def __init__(self) -> None:
        self._api: Optional[_ViGEmApi] = None
        self._client: Optional[int] = None
        self._target: Optional[int] = None
        self._open = False
        self._last: Optional[tuple] = None
        #: Gniazdo XInput, w ktorym Windows wyliczyl tego pada (0-3).
        self._slot: Optional[int] = None

    @property
    def is_open(self) -> bool:
        return self._open

    def max_buttons(self) -> int:
        return len(PAD_BUTTONS)

    def has_hat(self) -> bool:
        return True

    # -------------------------------------------------------------- otwarcie

    def open(self) -> None:
        if self._open:
            return
        path = find_vigem_dll()
        if path is None:
            raise ViGEmNotAvailable(
                f"Nie znaleziono {_DLL_NAME}.\n\n"
                f"Biblioteka powinna lezec w katalogu 'vigem' obok aplikacji "
                f"(podkatalog x64 dla 64-bitowego Pythona).\n"
                f"Mozna tez zainstalowac pakiet vgamepad:\n"
                f"    python -m pip install vgamepad"
            )
        try:
            api = _ViGEmApi(path)
        except OSError as exc:
            bits = 64 if sys.maxsize > 2 ** 32 else 32
            raise ViGEmNotAvailable(
                f"Nie udalo sie zaladowac {path}:\n{exc}\n\n"
                f"Najczestsza przyczyna to niezgodnosc architektury - "
                f"uzywasz Pythona {bits}-bit, wiec potrzebna jest biblioteka "
                f"z katalogu {'x64' if bits == 64 else 'x86'}."
            ) from exc

        client = api.dll.vigem_alloc()
        if not client:
            raise ViGEmNotAvailable("Nie udalo sie przydzielic klienta ViGEm.")

        code = api.dll.vigem_connect(client)
        if code != VIGEM_ERROR_NONE:
            api.dll.vigem_free(client)
            raise ViGEmNotAvailable(
                f"Nie udalo sie polaczyc ze sterownikiem ViGEmBus:\n"
                f"{_err(code)}\n\n"
                f"Zainstaluj ViGEmBus:\n"
                f"https://github.com/nefarius/ViGEmBus/releases"
            )

        target = api.dll.vigem_target_x360_alloc()
        if not target:
            api.dll.vigem_disconnect(client)
            api.dll.vigem_free(client)
            raise ViGEmNotAvailable("Nie udalo sie utworzyc wirtualnego pada.")

        slots_before = _xinput_slots()
        code = api.dll.vigem_target_add(client, target)
        if code != VIGEM_ERROR_NONE:
            api.dll.vigem_target_free(target)
            api.dll.vigem_disconnect(client)
            api.dll.vigem_free(client)
            raise ViGEmNotAvailable(
                f"Nie udalo sie podlaczyc wirtualnego pada:\n{_err(code)}"
            )

        self._api = api
        self._client = client
        self._target = target
        self._open = True
        self._last = None
        self._prime(slots_before)

    def close(self) -> None:
        if not self._open or self._api is None:
            self._open = False
            return
        try:
            self.center()
        except Exception:
            pass
        try:
            if self._target is not None:
                self._api.dll.vigem_target_remove(self._client, self._target)
                self._api.dll.vigem_target_free(self._target)
            if self._client is not None:
                self._api.dll.vigem_disconnect(self._client)
                self._api.dll.vigem_free(self._client)
        except Exception:
            pass
        self._api = None
        self._client = None
        self._target = None
        self._open = False
        self._last = None
        self._slot = None

    # --------------------------------------------------------------- wyjscie

    def _prime(self, slots_before: Optional[set] = None) -> None:
        """Doprowadza swiezo podlaczonego pada do stanu wyzerowanego.

        ViGEmBus wypycha raport do urzadzenia dopiero wtedy, gdy rozni sie on
        od zapamietanego - a zapamietany startowo jest zerowy. Zapis samych zer
        zaraz po vigem_target_add zwraca VIGEM_ERROR_NONE, ale niczego nie
        wysyla, wiec XInput czyta z drazkow nieokreslone wartosci (u nas okolo
        -3300). W grze oznaczaloby to auto skrecajace samo z siebie po starcie.

        Lekarstwem jest jeden niezerowy raport, a po nim zera. Samo odczekanie
        nie wystarcza: dopoki Windows nie wyliczy swiezo podlaczonego urzadzenia,
        raporty przepadaja (w testach 2 na 10 uruchomien). Dlatego zamiast
        zgadywac czas, czytamy stan z powrotem przez XInput i powtarzamy
        sekwencje az pad naprawde raportuje zera.
        """
        if not self._open or self._api is None:
            return

        def nudge_then_zero() -> None:
            try:
                self._api.dll.vigem_target_x360_update(
                    self._client, self._target, XUSB_REPORT(sThumbLX=1))
                time.sleep(PRIME_DELAY)
            except Exception:
                return
            self._last = None
            self.center()
            time.sleep(PRIME_DELAY)

        nudge_then_zero()

        if _XINPUT is None or slots_before is None:
            return                      # bez XInputa zostaje sama sekwencja

        deadline = time.monotonic() + PRIME_TIMEOUT
        while time.monotonic() < deadline:
            new_slots = _xinput_slots() - slots_before
            if new_slots:
                slot = min(new_slots)
                self._slot = slot
                if _xinput_is_zeroed(slot):
                    return
                nudge_then_zero()
            else:
                time.sleep(0.05)

    def center(self) -> None:
        self.send(PadState(), force=True)

    def send(self, state: PadState, force: bool = False) -> None:
        """Wysyla raport. Powtorzenia sa pomijane, zeby nie meczyc sterownika."""
        if not self._open or self._api is None:
            return
        snapshot = state.as_tuple()
        if snapshot == self._last and not force:
            return
        self._last = snapshot

        report = XUSB_REPORT(
            wButtons=state.buttons & 0xFFFF,
            bLeftTrigger=_clamp_u8(state.lt),
            bRightTrigger=_clamp_u8(state.rt),
            sThumbLX=_clamp_s16(state.lx),
            sThumbLY=_clamp_s16(state.ly),
            sThumbRX=_clamp_s16(state.rx),
            sThumbRY=_clamp_s16(state.ry),
        )
        self._api.dll.vigem_target_x360_update(self._client, self._target, report)

    # Zgodnosc z interfejsem bazowym - tryb pada uzywa send().
    def update(self, axes, buttons, hat: int = -1) -> None:
        pass

    @property
    def slot(self) -> Optional[int]:
        return self._slot

    def status_text(self) -> str:
        if not self._open:
            return "Pad nieaktywny"
        where = f" (gniazdo {self._slot + 1})" if self._slot is not None else ""
        return ("Pad Xbox 360 podlaczony" + where
                + " - gry widza go jako kontroler XInput")


def _clamp_u8(v: int) -> int:
    v = int(v)
    return 0 if v < 0 else (255 if v > 255 else v)


def _clamp_s16(v: int) -> int:
    v = int(v)
    return -32768 if v < -32768 else (32767 if v > 32767 else v)
