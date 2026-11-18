"""Recolor the Averak Labs neon logo to a cyberpunk palette (main/images/src/averak_labs_cyber.png).

Usage (Pillow + numpy venv, as for png_image.py):
    python tools/cyber_logo.py main/images/src/averak_labs_neon.png main/images/src/averak_labs_cyber.png

Neutral (unsaturated) pixels are recolored by brightness: the gray AVERAK fill (a large solid
mid-gray area) -> neon yellow; the white strokes (geodesic, LABS outline) -> magenta-purple with a
glow. Saturated pixels (cyan circuit, magenta nodes, glitch edges) are kept.
"""
import sys

import numpy as np
from PIL import Image, ImageFilter

src, out = sys.argv[1], sys.argv[2]
img = np.asarray(Image.open(src).convert("RGB")).astype(np.float32)
bg = np.array([0x0B, 0x0D, 0x14], np.float32)

mx, mn = img.max(axis=2), img.min(axis=2)
sat = (mx - mn) / np.maximum(mx, 1)
lum = img.mean(axis=2)
neutral = (sat < 0.18) & (lum > 30)

# Gray text fill: mid-gray, and solid (survives an opening that removes thin stroke edges)
gray = neutral & (lum > 105) & (lum < 175)
m = Image.fromarray((gray * 255).astype(np.uint8))
m = m.filter(ImageFilter.MinFilter(7)).filter(ImageFilter.MaxFilter(9))
gray_fill = (np.asarray(m) > 0) & neutral & (lum < 200)

YELLOW = np.array([0xFC, 0xEE, 0x0A], np.float32)
MAGENTA = np.array([0xE0, 0x2C, 0xFF], np.float32)

res = img.copy()
# Yellow fill keeps its shading: intensity relative to the fill's gray level
t = np.clip((lum - bg.mean()) / (138 - bg.mean()), 0, 1)[..., None]
res = np.where(gray_fill[..., None], bg + (YELLOW - bg) * t, res)
# White strokes -> magenta-purple by intensity
strokes = neutral & ~gray_fill
strokes[-12:, :] = False  # the source's light bottom border row
res[-12:, :] = np.where(neutral[-12:, :, None], bg, res[-12:, :])
t = np.clip((lum - bg.mean()) / (255 - bg.mean()), 0, 1)[..., None]
res = np.where(strokes[..., None], bg + (MAGENTA - bg) * t, res)

# Glow around the strokes, screened onto the result (like the cyan circuit's)
layer = np.where(strokes[..., None], MAGENTA * t, 0).astype(np.uint8)
glow = np.asarray(Image.fromarray(layer).filter(ImageFilter.GaussianBlur(14))).astype(np.float32) * 0.9
res = 255 - (255 - res) * (255 - glow) / 255

Image.fromarray(np.clip(res, 0, 255).astype(np.uint8)).save(out)
print("wrote", out)
