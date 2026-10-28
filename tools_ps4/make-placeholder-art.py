#!/usr/bin/env python3
"""Draw the placeholder package art (original, no game or Sony artwork):
tools_ps4/sce_sys/icon0.png (512x512) and pic1.png (1920x1080).
Needs Pillow and the DejaVu fonts.  python3 tools_ps4/make-placeholder-art.py"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sce_sys")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_LIGHT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
GOLD = (214, 172, 74)
GOLD_DIM = (120, 96, 42)


def background(w, h):
    """Dark radial gradient, slightly warm in the centre."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    cx, cy, r = w / 2, h / 2, math.hypot(w, h) / 2
    for y in range(h):
        for x in range(w):
            t = min(1.0, math.hypot(x - cx, y - cy) / r)
            px[x, y] = (int(34 - 26 * t), int(30 - 23 * t), int(26 - 20 * t))
    return img


def reticle(d, cx, cy, r, width):
    """A plain sight: ring, gapped cross hairs, centre dot."""
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=GOLD_DIM, width=width)
    gap, ext = r * 0.28, r * 1.18
    for sx, sy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        d.line((cx + sx * gap, cy + sy * gap, cx + sx * ext, cy + sy * ext), fill=GOLD_DIM, width=width)
    dot = width * 1.6
    d.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=GOLD_DIM)


def centred(d, cx, y, text, font, fill):
    w = d.textlength(text, font=font)
    d.text((cx - w / 2, y), text, font=font, fill=fill)


def icon():
    img = background(512, 512)
    d = ImageDraw.Draw(img)
    reticle(d, 256, 256, 170, 6)
    centred(d, 256, 196, "GE64", ImageFont.truetype(FONT, 104), GOLD)
    centred(d, 256, 330, "PS4 PORT", ImageFont.truetype(FONT_LIGHT, 34), GOLD)
    d.rectangle((6, 6, 505, 505), outline=GOLD_DIM, width=4)
    img.save(os.path.join(OUT, "icon0.png"))


def pic1():
    img = background(1920, 1080)
    d = ImageDraw.Draw(img)
    reticle(d, 1440, 540, 300, 8)
    d.text((180, 400), "GE64", font=ImageFont.truetype(FONT, 200), fill=GOLD)
    d.text((190, 640), "PS4 PORT  ·  placeholder art", font=ImageFont.truetype(FONT_LIGHT, 48), fill=GOLD_DIM)
    img.save(os.path.join(OUT, "pic1.png"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    icon()
    pic1()
