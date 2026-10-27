"""Backendy wyjsciowe - urzadzenia wirtualne widziane przez gry."""

from .base import OutputBackend, NullBackend, OutputError, AXIS_KEYS
from .vjoy import VJoyBackend, VJoyNotAvailable, vjoy_diagnostics
from .xinput import (PAD_BUTTONS, PadState, ViGEmNotAvailable, XInputBackend,
                     vigem_diagnostics)

__all__ = [
    "OutputBackend", "NullBackend", "OutputError", "AXIS_KEYS",
    "VJoyBackend", "VJoyNotAvailable", "vjoy_diagnostics",
    "XInputBackend", "ViGEmNotAvailable", "vigem_diagnostics",
    "PadState", "PAD_BUTTONS",
]
