#!/usr/bin/env python3
"""Render the rotated row labels ("1d", "7d", "30d") used by the stats screen.

The Pebble SDK cannot rotate text at draw time, so the sideways labels that sit
inside each bar's coloured block are pre-rendered here as white-on-transparent
PNGs and shipped as bitmap resources.

Usage:  python3 tools/make_labels.py
"""

import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, os.pardir, "resources", "images")

# Candidate font files, in order of preference.
FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
]

LABELS = [("1d", "label_1d.png"), ("7d", "label_7d.png"), ("30d", "label_30d.png")]

FONT_SIZE = 20
# Label block is 26px wide, so the rotated glyphs must stay under that.
MAX_THICKNESS = 24


def pick_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, FONT_SIZE)
    raise SystemExit("no usable TrueType font found; install fonts-dejavu-core")


def render(text, font):
    # Draw horizontally on a generous canvas, crop to ink, then rotate 90° CCW
    # so the label reads bottom-to-top.
    canvas = Image.new("RGBA", (160, 64), (255, 255, 255, 0))
    draw = ImageDraw.Draw(canvas)
    draw.text((8, 8), text, font=font, fill=(255, 255, 255, 255))
    canvas = canvas.crop(canvas.getbbox())
    rotated = canvas.rotate(90, expand=True)
    if rotated.width > MAX_THICKNESS:
        scale = MAX_THICKNESS / rotated.width
        rotated = rotated.resize(
            (MAX_THICKNESS, max(1, round(rotated.height * scale))), Image.LANCZOS
        )
    # The watch composites with GCompOpSet, which keys on full transparency, so
    # flatten the antialiased edge into a hard alpha mask.
    alpha = rotated.getchannel("A").point(lambda v: 255 if v >= 110 else 0)
    out = Image.new("RGBA", rotated.size, (255, 255, 255, 0))
    out.putalpha(alpha)
    out.paste((255, 255, 255, 255), mask=alpha)
    return out


def main():
    font = pick_font()
    os.makedirs(OUT_DIR, exist_ok=True)
    for text, filename in LABELS:
        image = render(text, font)
        path = os.path.join(OUT_DIR, filename)
        image.save(path, "PNG", optimize=True)
        print("wrote {} ({}x{})".format(path, image.width, image.height))


if __name__ == "__main__":
    main()
