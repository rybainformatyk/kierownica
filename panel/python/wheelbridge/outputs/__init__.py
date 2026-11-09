"""Backendy wyjsciowe - urzadzenia wirtualne widziane przez gry."""

from .base import OutputBackend, NullBackend, OutputError, AXIS_KEYS
from .vjoy import (VJoyBackend, VJoyNotAvailable, set_vjoy_driver,
                   vjoy_diagnostics, vjoy_driver_enabled)
from .xinput import (PAD_BUTTONS, PadState, ViGEmNotAvailable, XInputBackend,
                     vigem_diagnostics)

__all__ = [
    "OutputBackend", "NullBackend", "OutputError", "AXIS_KEYS",
    "VJoyBackend", "VJoyNotAvailable", "vjoy_diagnostics",
    "set_vjoy_driver", "vjoy_driver_enabled",
    "XInputBackend", "ViGEmNotAvailable", "vigem_diagnostics",
    "PadState", "PAD_BUTTONS",
]
