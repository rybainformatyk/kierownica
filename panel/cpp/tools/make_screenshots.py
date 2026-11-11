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
    "cala_1400_Pulpit.png": "pulpit.png",
    "popover_tryb.png": "menu.png",
    "tab_1_Kierownica.png": "kierownica.png",
    "tab_2_Pedały.png": "pedaly.png",
    "g29_po_nauce.png": "g29.png",
    "tab_5_Pad.png": "pad.png",
    "tab_6_Force Feedback.png": "ffb.png",
}
# Zrzuty "cala_*" to cala wysokosc strony (2300 px) z pustym tlem pod
# trescia - obcinane do ostatniego wiersza tresci plus margines.
MARGIN = 24
BACKGROUND_TOLERANCE = 30


def crop_to_content(img: Image.Image) -> Image.Image:
    # Szuka od dolu wiersza, ktory w obszarze stron (na prawo od paska
    # bocznego) rozni sie od tla. Pasek boczny ma na dole kafelek z katem,
    # wiec nie jest brany pod uwage.
    width, height = img.size
    px = img.load()
    bg = px[width - 100, height - 100]
    for y in range(height - 1, -1, -1):
        for x in range(260, width - 10, 4):
            if sum(abs(a - b) for a, b in zip(px[x, y], bg)) > BACKGROUND_TOLERANCE:
                return img.crop((0, 0, width, min(height, y + 1 + MARGIN)))
    return img


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
                shot = img.convert("RGB")
            if src.startswith("cala_"):
                shot = crop_to_content(shot)
            shot.save(OUT / dst, optimize=True)
            print(f"{dst}: {shot.width}x{shot.height}, {(OUT / dst).stat().st_size // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
