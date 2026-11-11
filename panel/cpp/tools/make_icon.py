"""Rysuje logo i ikony programu do src/res/.

    py -3 tools/make_icon.py [--preview plik.png]

  kierownica.ico      ikona programu: kafelek (zaokraglony kwadrat w stylu
                      Windows 11) z kierownica wyscigowa w ksztalcie litery D
  kierownica_256.png  to samo logo w PNG (instalator)
  ../python/wheelbridge/ui/res/logo_*.png  logo okna wersji Python
  tray_on.ico         zasobnik, plytka polaczona: sama kierownica, bez kafelka
  tray_off.ico        zasobnik, plytka rozlaczona: szara kierownica

Kazdy rozmiar jest rysowany osobno z nadproborkowaniem, a male rozmiary
(do 24 px) maja grubsze linie i mniej detali - 16 px nie jest
pomniejszonym 256 px. Kolory: akcent interfejsu z ui/gfx.h (ACCENT 4cc2ff)
i pomaranczowy znacznik srodka jak na kierownicach wyscigowych.
Wymaga Pillow.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
SS = 4          # nadproborkowanie
BASE = 1024     # siatka projektu - wszystkie wymiary nizej sa w tych jednostkach

RGB = tuple[int, int, int]

TILE_TOP: RGB = (0x1A, 0x2B, 0x44)
TILE_BOTTOM: RGB = (0x08, 0x0D, 0x16)
GLOW: RGB = (0x4C, 0xC2, 0xFF)
RIM_LIGHT: RGB = (0xF2, 0xFA, 0xFF)
RIM_MID: RGB = (0x8F, 0xD9, 0xFF)
RIM_DARK: RGB = (0x2E, 0x9C, 0xE6)
MARK: RGB = (0xFF, 0x8A, 0x3D)
HUB_DARK: RGB = (0x0B, 0x14, 0x22)

# zasobnik: pasek zadan bywa jasny i ciemny - mocniejsze kolory niz na kafelku
TRAY_LIGHT: RGB = (0x7F, 0xD6, 0xFF)
TRAY_MID: RGB = (0x3A, 0xB0, 0xF5)
TRAY_DARK: RGB = (0x1A, 0x78, 0xC8)
OFF_LIGHT: RGB = (0xA8, 0xB0, 0xBD)
OFF_MID: RGB = (0x88, 0x92, 0xA2)
OFF_DARK: RGB = (0x5A, 0x64, 0x74)
OFF_MARK: RGB = (0xA8, 0xB0, 0xBD)


# ------------------------------------------------------------------ narzedzia

def vgrad(n: int, top: RGB, bottom: RGB, stops: tuple[float, ...] = ()) -> Image.Image:
    """Pionowy gradient n x n (RGBA)."""
    img = Image.new("RGBA", (n, n))
    d = ImageDraw.Draw(img)
    for y in range(n):
        t = y / max(1, n - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
        d.line([(0, y), (n, y)], fill=c + (255,))
    return img


def three_stop(n: int, a: RGB, b: RGB, c: RGB, mid: float = 0.45) -> Image.Image:
    img = Image.new("RGBA", (n, n))
    d = ImageDraw.Draw(img)
    for y in range(n):
        t = y / max(1, n - 1)
        if t < mid:
            u, p, q = t / mid, a, b
        else:
            u, p, q = (t - mid) / (1 - mid), b, c
        col = tuple(round(x + (z - x) * u) for x, z in zip(p, q))
        d.line([(0, y), (n, y)], fill=col + (255,))
    return img


def mask(n: int) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    m = Image.new("L", (n, n), 0)
    return m, ImageDraw.Draw(m)


def paint(dst: Image.Image, src: Image.Image, m: Image.Image) -> None:
    dst.alpha_composite(Image.composite(src, Image.new("RGBA", src.size, (0, 0, 0, 0)), m))


# --------------------------------------------------------------- kierownica

def wheel_masks(n: int, small: bool) -> dict[str, Image.Image]:
    """Maski czesci kierownicy na kanwie n x n (wspolrzedne projektu * n/BASE)."""
    k = n / BASE
    cx, cy = 512 * k, 528 * k
    R = 330 * k                         # promien zewnetrzny wienca
    rim = (96 if small else 74) * k     # grubosc wienca
    flat = cy + R * 0.80                # sciety dol (ksztalt D)

    # wieniec = (kolo R ∩ nad linia) minus (kolo R-rim ∩ nad linia-rim)
    outer, od = mask(n)
    od.ellipse((cx - R, cy - R, cx + R, cy + R), fill=255)
    od.rectangle((0, flat, n, n), fill=0)
    inner, idr = mask(n)
    r2 = R - rim
    idr.ellipse((cx - r2, cy - r2, cx + r2, cy + r2), fill=255)
    idr.rectangle((0, flat - rim, n, n), fill=0)
    ring = ImageChops.subtract(outer, inner)

    # szprychy: dwie boczne lekko w dol i dolna, rozszerzajace sie do piasty
    spokes, sd = mask(n)
    hub_r = (64 if small else 96) * k
    w_out = (58 if small else 50) * k   # szerokosc przy wiencu
    w_in = (66 if small else 104) * k  # szerokosc przy piascie
    for ang in (188.0, -8.0):
        a = math.radians(ang)
        dx, dy = math.cos(a), -math.sin(a)
        px, py = -dy, dx
        ex, ey = cx + dx * (R - rim * 0.5), cy + dy * (R - rim * 0.5)
        sd.polygon([
            (cx + px * w_in / 2, cy + py * w_in / 2),
            (ex + px * w_out / 2, ey + py * w_out / 2),
            (ex - px * w_out / 2, ey - py * w_out / 2),
            (cx - px * w_in / 2, cy - py * w_in / 2),
        ], fill=255)
    sd.polygon([(cx - w_in / 2, cy), (cx + w_in / 2, cy),
                (cx + w_out / 2, flat - rim * 0.5), (cx - w_out / 2, flat - rim * 0.5)], fill=255)

    hub, hd = mask(n)
    hd.ellipse((cx - hub_r, cy - hub_r, cx + hub_r, cy + hub_r), fill=255)

    hub_in, hid = mask(n)
    hr2 = hub_r * (0.62 if small else 0.66)
    hid.ellipse((cx - hr2, cy - hr2, cx + hr2, cy + hr2), fill=255)

    dot, dd = mask(n)
    hr3 = hub_r * 0.30
    dd.ellipse((cx - hr3, cy - hr3, cx + hr3, cy + hr3), fill=255)

    # znacznik srodka na gorze wienca
    mark, md = mask(n)
    mw = (84 if small else 58) * k
    md.rectangle((cx - mw / 2, cy - R - 2 * k, cx + mw / 2, cy - R + rim + 2 * k), fill=255)
    mark = ImageChops.multiply(mark, outer)

    body = ImageChops.lighter(ImageChops.lighter(ring, spokes), hub)
    return {"ring": ring, "body": body, "hub": hub, "hub_in": hub_in, "dot": dot, "mark": mark}


def draw_wheel(n: int, small: bool, colors: str, shadow: bool) -> Image.Image:
    """colors: "tile" (na kafelku), "on" / "off" (zasobnik)."""
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    m = wheel_masks(n, small)
    on = colors != "off"
    light, mid, dark = {"tile": (RIM_LIGHT, RIM_MID, RIM_DARK), "on": (TRAY_LIGHT, TRAY_MID, TRAY_DARK),
                        "off": (OFF_LIGHT, OFF_MID, OFF_DARK)}[colors]

    if shadow:
        sh = m["body"].filter(ImageFilter.GaussianBlur(n * 0.025))
        sh = ImageChops.offset(sh, 0, round(n * 0.022))
        shade = Image.new("RGBA", (n, n), (0, 0, 0, 255))
        paint(img, shade, sh.point(lambda v: v * 0.55))

    paint(img, three_stop(n, light, mid, dark, 0.42), m["body"])
    # piasta: ciemny srodek z jasnym ringiem i kropka akcentu
    # w malych ikonach ciemny srodek zlewa sie ze szczelinami - piasta pelna
    if not small:
        paint(img, Image.new("RGBA", (n, n), HUB_DARK + (255,)), m["hub_in"])
        paint(img, Image.new("RGBA", (n, n), (GLOW if on else OFF_MID) + (255,)), m["dot"])
    paint(img, Image.new("RGBA", (n, n), (MARK if on else OFF_MARK) + (255,)), m["mark"])
    return img


# ------------------------------------------------------------------ kafelek

def draw_tile(n: int) -> Image.Image:
    k = n / BASE
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    inset = 56 * k
    radius = 226 * k
    tile, td = mask(n)
    td.rounded_rectangle((inset, inset, n - inset, n - inset), radius=radius, fill=255)
    paint(img, vgrad(n, TILE_TOP, TILE_BOTTOM), tile)

    # poswiata akcentu za kierownica
    glow, gd = mask(n)
    gr = 380 * k
    gd.ellipse((512 * k - gr, 560 * k - gr, 512 * k + gr, 560 * k + gr), fill=150)
    glow = ImageChops.multiply(glow.filter(ImageFilter.GaussianBlur(120 * k)), tile)
    paint(img, Image.new("RGBA", (n, n), GLOW + (255,)), glow.point(lambda v: v * 0.55))

    # cienka jasna krawedz u gory - kafelek lekko wypukly
    edge, ed = mask(n)
    ed.rounded_rectangle((inset, inset, n - inset, n - inset), radius=radius, outline=255, width=max(1, round(6 * k)))
    top_fade = vgrad(n, (255, 255, 255), (0, 0, 0)).convert("L").point(lambda v: v * 0.35)
    paint(img, Image.new("RGBA", (n, n), (255, 255, 255, 255)), ImageChops.multiply(edge, top_fade))
    return img


# ------------------------------------------------------------------- ikony

def fit(img: Image.Image, fill: float, dy: float = 0.0) -> Image.Image:
    """Przycina do zawartosci i skaluje tak, zeby dluzszy bok zajal `fill` kanwy."""
    n = img.width
    box = img.getchannel("A").getbbox()
    if not box:
        return img
    part = img.crop(box)
    scale = fill * n / max(part.width, part.height)
    part = part.resize((max(1, round(part.width * scale)), max(1, round(part.height * scale))), Image.LANCZOS)
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.alpha_composite(part, ((n - part.width) // 2, (n - part.height) // 2 + round(dy * n)))
    return out


def render(size: int, kind: str) -> Image.Image:
    n = size * SS
    small = size <= 24
    if kind == "app":
        img = draw_tile(n)
        # w malych ikonach kierownica wypelnia wiecej kafelka - inaczej ginie
        img.alpha_composite(fit(draw_wheel(n, small, "tile", shadow=not small), 0.78 if small else 0.70, 0.01))
    else:
        img = fit(draw_wheel(n, small, kind, shadow=False), 1.0 if size <= 24 else 0.96)
    return img.resize((size, size), Image.LANCZOS)


def save_ico(path: Path, kind: str) -> None:
    frames = [render(s, kind) for s in ICO_SIZES]
    frames[-1].save(path, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=frames[:-1])
    print(f"zapisano {path}")


def preview_small(path: Path) -> None:
    """Male rozmiary powiekszone x8 (piksel w piksel)."""
    items = [(s, k) for k in ("app", "on", "off") for s in (16, 24, 32)]
    sheet = Image.new("RGBA", (len(items) * 290 + 20, 560), (0, 0, 0, 255))
    d = ImageDraw.Draw(sheet)
    d.rectangle((0, 0, sheet.width, 280), fill=(10, 12, 17))
    d.rectangle((0, 280, sheet.width, 560), fill=(238, 241, 245))
    for i, (s, k) in enumerate(items):
        im = render(s, k).resize((s * 8, s * 8), Image.NEAREST)
        for y0 in (12, 292):
            sheet.alpha_composite(im, (20 + i * 290, y0 + (256 - s * 8) // 2))
    sheet.convert("RGB").save(path)
    print(f"podglad {path}")


def preview(path: Path) -> None:
    """Arkusz: logo na ciemnym i jasnym tle oraz ikony zasobnika w 100%."""
    sheet = Image.new("RGBA", (1180, 620), (0, 0, 0, 255))
    d = ImageDraw.Draw(sheet)
    d.rectangle((0, 0, 1180, 310), fill=(10, 12, 17))
    d.rectangle((0, 310, 1180, 620), fill=(238, 241, 245))
    for row, y0 in enumerate((20, 330)):
        x = 20
        for s in (256, 128, 64, 48, 32, 24, 16):
            im = render(s, "app")
            sheet.alpha_composite(im, (x, y0 + (256 - s) // 2))
            x += s + 24
        for kind in ("on", "off"):
            for s in (32, 24, 16):
                im = render(s, kind)
                sheet.alpha_composite(im, (x, y0 + 120 - s // 2))
                x += s + 14
            x += 10
    sheet.convert("RGB").save(path)
    print(f"podglad {path}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", type=Path, help="tylko arkusz podgladu, bez zapisu ikon")
    args = ap.parse_args()
    if args.preview:
        preview(args.preview)
        preview_small(args.preview.with_name(args.preview.stem + "_male.png"))
        return
    res = Path(__file__).resolve().parent.parent / "src" / "res"
    res.mkdir(parents=True, exist_ok=True)
    save_ico(res / "kierownica.ico", "app")
    save_ico(res / "tray_on.ico", "on")
    save_ico(res / "tray_off.ico", "off")
    render(256, "app").save(res / "kierownica_256.png")
    # to samo logo dla okna wersji Python (Tk nie czyta .ico z wieloma rozmiarami)
    py_res = Path(__file__).resolve().parents[2] / "python" / "wheelbridge" / "ui" / "res"
    py_res.mkdir(parents=True, exist_ok=True)
    for s in (16, 32, 48, 64, 256):
        render(s, "app").save(py_res / f"logo_{s}.png")
    print(f"zapisano {py_res}")


if __name__ == "__main__":
    main()
