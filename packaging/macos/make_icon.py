#!/usr/bin/env python3
"""Build packaging/macos/Polyphorm.icns from a Polyphorm render.

    .venv/bin/python packaging/macos/make_icon.py

Crops a square from docs/mode_trace.png (the SDSS trace render), places it
in the macOS app-icon shape (824 px rounded square on a 1024 canvas, Big
Sur template, with a soft drop shadow) and packs every required size with
iconutil. The .icns is committed; rerun only to change the artwork.
"""

import os
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SOURCE = os.path.join(ROOT, 'docs', 'mode_trace.png')
OUT_ICNS = os.path.join(HERE, 'Polyphorm.icns')

CANVAS = 1024
TILE = 824            # Apple's icon grid: 824x824 body centred on 1024
RADIUS = 185          # corner radius of that body
CROP = (183, -100, 1183, 900)  # square around the web (x0, y0, x1, y1); out-of-image area is black


def rounded_mask(size, radius, scale=4):
    """Anti-aliased rounded-rect mask (drawn large, downsampled)."""
    big = Image.new('L', (size * scale, size * scale), 0)
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, size * scale - 1, size * scale - 1), radius * scale, fill=255)
    return big.resize((size, size), Image.LANCZOS)


def build_icon():
    art = Image.open(SOURCE).convert('RGB').crop(CROP)
    art = art.resize((TILE, TILE), Image.LANCZOS)

    # Radial vignette times a smooth fade toward the tile edges, so the
    # filaments (the web is wider than the square) dissolve into the dark
    # tile instead of being cut off hard at the sides.
    r = np.linspace(-1.0, 1.0, TILE)
    xx, yy = np.meshgrid(r, r)
    radial = 1.0 - np.clip((np.hypot(xx, yy) - 0.55) / 0.5, 0.0, 1.0)
    edge = np.clip((1.0 - np.maximum(np.abs(xx), np.abs(yy))) / 0.22, 0.0, 1.0)
    fade = radial * edge * edge * (3.0 - 2.0 * edge)   # smoothstep on the edge term
    vignette = Image.fromarray((fade * 255).astype(np.uint8), 'L')
    dark = Image.new('RGB', (TILE, TILE), (4, 10, 22))
    art = Image.composite(art, dark, vignette)

    mask = rounded_mask(TILE, RADIUS)
    body = Image.new('RGBA', (TILE, TILE))
    body.paste(art, (0, 0))
    body.putalpha(mask)

    # Subtle top highlight edge, like system icons.
    rim = Image.new('L', (TILE, TILE), 0)
    ImageDraw.Draw(rim).rounded_rectangle((1, 1, TILE - 2, TILE - 2), RADIUS, outline=70, width=3)
    rim = ImageChops.multiply(rim, Image.linear_gradient('L').rotate(180).resize((TILE, TILE)))
    body = Image.alpha_composite(body, Image.merge('RGBA', (rim, rim, rim, rim)))

    canvas = Image.new('RGBA', (CANVAS, CANVAS), (0, 0, 0, 0))
    off = (CANVAS - TILE) // 2
    shadow = Image.new('RGBA', (CANVAS, CANVAS), (0, 0, 0, 0))
    shadow_mask = Image.new('L', (CANVAS, CANVAS), 0)
    shadow_mask.paste(mask, (off, off + 12))
    shadow.putalpha(shadow_mask.point(lambda v: v * 0.45).filter(ImageFilter.GaussianBlur(14)))
    canvas = Image.alpha_composite(canvas, shadow)
    canvas.alpha_composite(body, (off, off))
    return canvas


def main():
    icon = build_icon()
    tmp = tempfile.mkdtemp()
    iconset = os.path.join(tmp, 'Polyphorm.iconset')
    os.makedirs(iconset)
    for size in (16, 32, 128, 256, 512):
        icon.resize((size, size), Image.LANCZOS).save(os.path.join(iconset, f'icon_{size}x{size}.png'))
        icon.resize((size * 2, size * 2), Image.LANCZOS).save(os.path.join(iconset, f'icon_{size}x{size}@2x.png'))
    subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', OUT_ICNS], check=True)
    shutil.rmtree(tmp)
    print(f'wrote {OUT_ICNS}')


if __name__ == '__main__':
    main()
