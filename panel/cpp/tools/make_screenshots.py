"""Zrzuty ekranu panelu do README (docs/zrzuty/ w katalogu glownym repo).

    py -3 tools/make_screenshots.py [--exe bin/Kierownica.exe]

Uruchamia Kierownica.exe --selftest: okno nie powstaje, program udaje
podlaczona plytke (Arduino Due, firmware 3.0) i rysuje kazda zakladke do
PNG. Skrypt wybiera z tego kilka zakladek i zapisuje je pod stalymi nazwami
jako PNG bez strat, zoptymalizowane. Wymaga Pillow.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parents[1] / "docs" / "zrzuty"

# plik z --selftest -> nazwa w docs/zrzuty/
SHOTS = {
    "tab_0_Pulpit.png": "pulpit.png",
    "tab_1_Kierownica.png": "kierownica.png",
    "tab_2_Pedały.png": "pedaly.png",
    "g29_po_nauce.png": "g29.png",
    "tab_5_Pad.png": "pad.png",
    "tab_6_Force Feedback.png": "ffb.png",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, default=ROOT / "bin" / "Kierownica.exe")
    args = parser.parse_args()
    if not args.exe.is_file():
        print(f"brak {args.exe} - najpierw build.bat", file=sys.stderr)
        return 1

    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run([str(args.exe), "--selftest", tmp], check=False)
        if result.returncode != 0:
            print(f"selftest zwrocil {result.returncode} - zobacz {tmp}\\selftest.log", file=sys.stderr)
            return 1
        for src, dst in SHOTS.items():
            with Image.open(Path(tmp) / src) as img:
                img.convert("RGB").save(OUT / dst, optimize=True)
            print(f"{dst}: {(OUT / dst).stat().st_size // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
