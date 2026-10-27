"""Wspolny interfejs backendow wyjsciowych."""

from __future__ import annotations

from typing import Dict, Iterable, List

#: Klucze osi uzywane w calej aplikacji (identyczne z config.VJOY_AXIS_CHOICES
#: poza "-", ktore oznacza brak przypisania).
AXIS_KEYS = ("X", "Y", "Z", "RX", "RY", "RZ", "SL0", "SL1", "WHL")


class OutputError(RuntimeError):
    """Backend nie moze dzialac - komunikat jest pokazywany uzytkownikowi."""


class OutputBackend:
    """Urzadzenie wirtualne, ktore widza gry."""

    name = "none"
    label = "Brak"
    supports_ffb = False

    def open(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        pass

    @property
    def is_open(self) -> bool:
        return False

    def available_axes(self) -> List[str]:
        return []

    def max_buttons(self) -> int:
        return 0

    def has_hat(self) -> bool:
        return False

    def update(self,
               axes: Dict[str, float],
               buttons: Iterable[int],
               hat: int = -1) -> None:
        """axes: klucz -> wartosc 0..1. buttons: numery 1-based. hat: -1..3."""
        raise NotImplementedError

    def center(self) -> None:
        """Ustawia wszystko w pozycji neutralnej."""

    def status_text(self) -> str:
        return self.label


class NullBackend(OutputBackend):
    """Tryb podgladu - nic nie wysyla, aplikacja dziala bez sterownika."""

    name = "none"
    label = "Tylko podgląd (bez urządzenia wirtualnego)"

    def __init__(self) -> None:
        self._open = False

    def open(self) -> None:
        self._open = True

    def close(self) -> None:
        self._open = False

    @property
    def is_open(self) -> bool:
        return self._open

    def available_axes(self) -> List[str]:
        return list(AXIS_KEYS)

    def max_buttons(self) -> int:
        return 128

    def has_hat(self) -> bool:
        return True

    def update(self, axes, buttons, hat: int = -1) -> None:
        pass

    def status_text(self) -> str:
        return "Podgląd - gry nie widzą kierownicy"
