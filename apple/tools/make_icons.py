#!/usr/bin/env python3
"""App icons and the in-app logo from the Averak Labs cyber logo.

Needs Pillow + numpy (throwaway venv). From apple/:
  python tools/make_icons.py ../firmware/main/images/src/averak_labs_cyber.png
Writes into the asset catalogs: iOS/watchOS AppIcon (1024, opaque), tvOS brand assets (layered
icons, App Store icon, top shelf images), the AverakLogo image set (About), the AverakBadge (top of the home
page) and firmware/main/images/src/averak_labs_pill.png (the devices' splash and About art). The app icons get a dark two-tone pill behind the
logo; About and the top shelf keep the plain logo.
The logo sits on its own background color (#0b0d14), so the icons are that color edge to edge.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

BG = (11, 13, 20)
ROOT = Path(__file__).resolve().parent.parent


def fit(logo, w, h, height_frac=None, box=None, pill=False):
    """Logo scaled to fit (box_w, box_h) (default: height_frac of h), centered on a BG canvas; with
    pill, a capsule corner to corner behind the logo."""
    bw, bh = box or (w, int(h * height_frac))
    s = min(bw / logo.width, bh / logo.height)
    l = logo.resize((round(logo.width * s), round(logo.height * s)), Image.LANCZOS)
    c = Image.new("RGB", (w, h), BG)
    ox, oy = (w - l.width) // 2, (h - l.height) // 2
    if pill:
        # the pill first, then the logo over it (its background made transparent, glows kept)
        # corner to corner, ends inset 9% so iOS's rounded mask doesn't cut them
        p, (px, py) = pill_layer((round(w * 0.09), round(h * 0.91)), (round(w * 0.91), round(h * 0.09)))
        c.paste(p, (px, py), p)
        lt = unblend(l)
        c.paste(lt, (ox, oy), lt)
    else:
        c.paste(l, (ox, oy))
    return c


def unblend(img):
    """BG -> transparent, keeping glows as partial alpha (for tvOS front layers)."""
    a = np.asarray(img.convert("RGB")).astype(np.float32)
    bg = np.array(BG, np.float32)
    up = np.where(a > bg, (a - bg) / (255 - bg), 0)
    down = np.where(a < bg, (bg - a) / np.maximum(bg, 1), 0)
    alpha = np.clip(np.max(np.maximum(up, down), axis=2), 0, 1)
    safe = np.maximum(alpha, 1e-4)[..., None]
    fg = np.clip(bg + (a - bg) / safe, 0, 255)
    out = np.dstack([fg, alpha[..., None] * 255]).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


# Pill halves: dark and muted so the logo's neon lines (cyan, magenta, yellow), drawn on top, stand out
PILL_LEFT = (98, 38, 186)    # violet
PILL_RIGHT = (18, 96, 148)   # navy-teal


def pill_layer(start, end, ratio=2.55):
    """A two-tone capsule (violet / navy-teal, behind the logo) with a soft glow, a light rim, a dark
    seam and a faint highlight, in canvas pixels from start (violet end) to end (navy end); width =
    length / ratio, a real capsule's proportions. Returns the RGBA image and its top-left corner."""
    import math
    dx, dy = end[0] - start[0], end[1] - start[1]
    length = round(math.hypot(dx, dy))
    width = round(length / ratio)
    angle = math.degrees(math.atan2(dy, dx))   # negative: up to the right
    center = ((start[0] + end[0]) // 2, (start[1] + end[1]) // 2)
    pad = round(width * 0.4)
    W, H = length + 2 * pad, width + 2 * pad
    r = width // 2
    box = (pad, pad, pad + length, pad + width)
    shape = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shape).rounded_rectangle(box, radius=r, fill=255)
    half = Image.new("L", (W, H), 0)
    ImageDraw.Draw(half).rectangle((0, 0, W // 2, H), fill=255)

    fill = Image.new("RGB", (W, H), PILL_RIGHT)
    fill.paste(Image.new("RGB", (W, H), PILL_LEFT), mask=half)
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    glow.paste(fill, mask=shape.filter(ImageFilter.GaussianBlur(width * 0.03)).point(lambda v: min(255, v * 2)))
    glow = glow.filter(ImageFilter.GaussianBlur(width * 0.12))
    pill = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    pill.alpha_composite(glow)
    body = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    body.paste(fill, mask=shape)
    pill.alpha_composite(body)
    d = ImageDraw.Draw(pill)
    d.line((W // 2, pad, W // 2, pad + width), fill=(11, 13, 20, 255), width=max(2, round(width * 0.035)))
    d.rounded_rectangle(box, radius=r, outline=(200, 210, 255, 150), width=max(1, round(width * 0.022)))
    hl = Image.new("L", (W, H), 0)
    ImageDraw.Draw(hl).rounded_rectangle((pad + r // 2, pad + width * 0.14, pad + length - r // 2, pad + width * 0.32),
                                         radius=int(width * 0.09), fill=70)
    hl = hl.filter(ImageFilter.GaussianBlur(width * 0.025))
    white = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    white.putalpha(hl)
    pill.alpha_composite(white)

    pill = pill.rotate(-angle, resample=Image.BICUBIC, expand=True)
    return pill, (center[0] - pill.width // 2, center[1] - pill.height // 2)


def write_contents(folder, images):
    (folder / "Contents.json").write_text(json.dumps(
        {"images": images, "info": {"author": "xcode", "version": 1}}, indent=2) + "\n")


def main(src):
    plain = Image.open(src).convert("RGB")
    logo = plain
    ios = ROOT / "MedDot/Assets.xcassets"
    watch = ROOT / "MedDotWatch/Assets.xcassets"

    # iOS: the whole logo, 86% of the height (iOS rounds the corners)
    d = ios / "AppIcon.appiconset"
    fit(logo, 1024, 1024, 0.86, pill=True).save(d / "icon-1024.png")
    write_contents(d, [{"filename": "icon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"}])

    # watchOS: round mask, so the logo's corners must stay inside the circle
    d = watch / "AppIcon.appiconset"
    diag = (logo.width ** 2 + logo.height ** 2) ** 0.5
    s = 1024 * 0.9 / diag
    fit(logo, 1024, 1024, box=(round(logo.width * s), round(logo.height * s)), pill=True).save(d / "icon-1024.png")
    write_contents(d, [{"filename": "icon-1024.png", "idiom": "universal", "platform": "watchos", "size": "1024x1024"}])

    # tvOS: layered icons (Back = background, Front = logo with transparency, Middle = empty)
    brand = ios / "App Icon & Top Shelf Image.brandassets"
    for stack, sizes in [("App Icon.imagestack", [(400, 240, "1x"), (800, 480, "2x")]),
                         ("App Icon - App Store.imagestack", [(1280, 768, "1x")])]:
        for layer in ["Back", "Middle", "Front"]:
            d = brand / stack / f"{layer}.imagestacklayer" / "Content.imageset"
            imgs = []
            for w, h, scale in sizes:
                name = f"{layer.lower()}-{w}x{h}.png"
                if layer == "Back":
                    Image.new("RGB", (w, h), BG).save(d / name)
                elif layer == "Front":
                    unblend(fit(logo, w, h, 0.86, pill=True)).save(d / name)
                else:
                    Image.new("RGBA", (w, h), (0, 0, 0, 0)).save(d / name)
                imgs.append({"filename": name, "idiom": "tv", "scale": scale})
            write_contents(d, imgs)
    for name, (w, h) in [("Top Shelf Image.imageset", (1920, 720)), ("Top Shelf Image Wide.imageset", (2320, 720))]:
        d = brand / name
        imgs = []
        for scale, k in [("1x", 1), ("2x", 2)]:
            f = f"topshelf-{w}x{h}@{scale}.png"
            fit(plain, w * k, h * k, 0.8).save(d / f)
            imgs.append({"filename": f, "idiom": "tv", "scale": scale})
        write_contents(d, imgs)

    # The brand mark: logo + pill, square, no rounded corners. Master PNG for the devices' splash and
    # About screens (firmware/tools/png_image.py converts it), and the apps' About logo (660 px: 220 pt @3x)
    mark = fit(logo, 1024, 1024, 0.86, pill=True)
    mark.save(ROOT.parent / "firmware/main/images/src/averak_labs_pill.png")
    d = ios / "AverakLogo.imageset"
    d.mkdir(exist_ok=True)
    mark.resize((660, 660), Image.LANCZOS).save(d / "averak-logo.png")
    write_contents(d, [{"filename": "averak-logo.png", "idiom": "universal"}])
    # Badge (top of the home page): the iOS app icon at 90 pt @3x
    d = ios / "AverakBadge.imageset"
    d.mkdir(exist_ok=True)
    fit(logo, 1024, 1024, 0.86, pill=True).resize((270, 270), Image.LANCZOS).save(d / "averak-badge.png")
    write_contents(d, [{"filename": "averak-badge.png", "idiom": "universal"}])
    print("icons written")


if __name__ == "__main__":
    main(sys.argv[1])
