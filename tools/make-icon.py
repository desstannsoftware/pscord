#!/usr/bin/env python3
# ps5-discord - Draws the PSCord app icon (sce_sys/icon0.png).
# Copyright (C) 2026 Desstann
# SPDX-License-Identifier: MIT
#
# An original mark: no Discord logo and no PlayStation button shapes, both of
# which are their owners' trademarks. Drawn at 4x and downscaled for clean edges.

import math
import pathlib

from PIL import Image, ImageDraw, ImageFilter, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
font_path = root / "third_party/fonts/Inter-SemiBold.ttf"
out_path = root / "sce_sys/icon0.png"

S = 4
W = 512 * S
top, bottom = (139, 92, 246), (67, 56, 202)

image = Image.new("RGB", (W, W))
pixels = image.load()
for y in range(W):
    for x in range(W):
        t = min(1.0, max(0.0, (x * 0.35 + y) / (W * 1.35)))
        pixels[x, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))

glow = Image.new("L", (W, W), 0)
ImageDraw.Draw(glow).ellipse((-W * 0.2, -W * 0.3, W * 0.8, W * 0.6), fill=70)
glow = glow.filter(ImageFilter.GaussianBlur(W * 0.12))
image = Image.composite(Image.new("RGB", (W, W), (255, 255, 255)), image, glow)

layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
draw = ImageDraw.Draw(layer)
white = (255, 255, 255, 255)

# Speech bubble with a tail at its lower left.
bubble = (88 * S, 96 * S, 360 * S, 300 * S)
draw.rounded_rectangle(bubble, radius=64 * S, fill=white)
draw.polygon([(128 * S, 270 * S), (104 * S, 352 * S), (206 * S, 290 * S)], fill=white)

# "PS" monogram inside the bubble.
monogram = ImageFont.truetype(str(font_path), 132 * S)
text = "PS"
box = draw.textbbox((0, 0), text, font=monogram)
cx = (bubble[0] + bubble[2]) / 2
cy = (bubble[1] + bubble[3]) / 2
draw.text((cx - (box[0] + box[2]) / 2, cy - (box[1] + box[3]) / 2), text, font=monogram,
          fill=(84, 66, 220, 255))

# Voice waves to the right of the bubble.
wave_center = (360 * S, 198 * S)
for radius, alpha in ((52, 255), (96, 190)):
    r = radius * S
    draw.arc((wave_center[0] - r, wave_center[1] - r, wave_center[0] + r, wave_center[1] + r),
             start=-48, end=48, fill=(255, 255, 255, alpha), width=18 * S)

# Wordmark.
wordmark = ImageFont.truetype(str(font_path), 64 * S)
text = "PSCord"
box = draw.textbbox((0, 0), text, font=wordmark)
draw.text((W / 2 - (box[0] + box[2]) / 2, 392 * S), text, font=wordmark, fill=white)

image = Image.alpha_composite(image.convert("RGBA"), layer).convert("RGB")
image.resize((512, 512), Image.LANCZOS).save(out_path, optimize=True)
print(f"wrote {out_path}")
