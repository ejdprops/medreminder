#!/usr/bin/env python3
"""Apple TV splash art (TVSplashView, CLAUDE.md 7i): the Did I? icon without its background, with a
margin so the pill's glow isn't clipped, and the "Did I?" wordmark in the icon's "?" style.

Same venv and font as make_icons.py. From apple/:
  python tools/make_splash_assets.py <path to Nunito[wght].ttf>
Writes the SplashLogo and SplashWord image sets into MedDot/Assets.xcassets.
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_icons as mi  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "MedDot/Assets.xcassets"
ART = 1024     # the icon's geometry
PAD = 128      # transparent margin around it
OUT = 1040     # SplashLogo px: drawn at 520 pt @2x


def styled(mask, grow, off, blur, glow_alpha, size, at):
    """The icon's "?" treatment for any L mask: fill-color glow, navy outline set down-right, aqua fill."""
    fat = mask.filter(ImageFilter.MaxFilter(grow))
    glow = fat.filter(ImageFilter.GaussianBlur(blur)).point(lambda v: round(v * glow_alpha))
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    for m, color, dx in [(glow, mi.DIDI_FILL, 0), (fat, mi.DIDI_OUTLINE, off), (mask, mi.DIDI_FILL, 0)]:
        full = Image.new("L", size, 0)
        full.paste(m, (at[0] + dx, at[1] + dx))
        solid = Image.new("RGBA", size, color + (255,))
        solid.putalpha(full)
        layer.alpha_composite(solid)
    return layer


def padded(mask, pad):
    out = Image.new("L", (mask.width + 2 * pad, mask.height + 2 * pad), 0)
    out.paste(mask, (pad, pad))
    return out


def save_set(name, img):
    d = ASSETS / f"{name}.imageset"
    d.mkdir(exist_ok=True)
    fn = f"{name[0].lower()}{name[1:]}.png"
    img.save(d / fn)
    (d / "Contents.json").write_text(json.dumps(
        {"images": [{"filename": fn, "idiom": "universal"}], "info": {"author": "xcode", "version": 1}},
        indent=2) + "\n")


def main(font_path):
    W = ART + 2 * PAD
    # logo: didi() geometry at 1024, shifted into the padded canvas
    (s0, s1), (e0, e1) = mi.pill_ends(ART, ART)
    p, (px, py) = mi.pill_layer((s0 + PAD, s1 + PAD), (e0 + PAD, e1 + PAD), mi.DIDI_PILL)
    m = max(p.width, p.height)
    big = Image.new("RGBA", (W + 2 * m, W + 2 * m), (0, 0, 0, 0))
    big.alpha_composite(p, (px + m, py + m))
    logo = big.crop((m, m, m + W, m + W))
    q = padded(mi.question(font_path, 600), 80)
    logo.alpha_composite(styled(q, 25, 8, 30, 0.30, (W, W), ((W - q.width) // 2, (W - q.height) // 2)))
    save_set("SplashLogo", logo.resize((OUT, OUT), Image.LANCZOS))

    # wordmark: 120 pt Nunito Black @2x (ink 176 px), navy shadow 6 pt down-right, 40 pt glow at 40%
    f = ImageFont.truetype(font_path, 400)
    f.set_variation_by_axes([1000])
    l, t, r, b = f.getbbox("Did I?")
    f = ImageFont.truetype(font_path, round(400 * 176 / (b - t)))
    f.set_variation_by_axes([1000])
    l, t, r, b = f.getbbox("Did I?")
    ink = Image.new("L", (r - l + 4, b - t + 4), 0)
    ImageDraw.Draw(ink).text((2 - l, 2 - t), "Did I?", font=f, fill=255)
    ink = padded(ink.crop(ink.getbbox()), 90)
    save_set("SplashWord", styled(ink, 3, 12, 40, 0.40, ink.size, (0, 0)))
    print("splash art written")


if __name__ == "__main__":
    main(sys.argv[1])
