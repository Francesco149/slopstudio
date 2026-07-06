#!/usr/bin/env python
"""ab-slider.py — a smooth eased before/after wipe slider, rendered to an mp4.

A vertical divider eases left↔right (pingpong, cubic ease-in-out); the BEFORE image
shows on one side of the line, AFTER on the other, each with a big obvious label that
rides with the split. For A/B comparison beats (e.g. MODULATE2X off→on) where a hard
cut or a static split doesn't read — the motion of the reveal IS the explanation.

Brand tokens (fonts/colours) from gemma-branding/brand-package/brand.json. Encodes with
the flake ffmpeg (NVENC not required — this is short).

Usage (from slopstudio repo root, inside nix develop):
    python tools/ab-slider.py --before A.png --after B.png --out demo.mp4 \
        --label-before "OFF" --label-after "×2" --dur 6

Defaults: 1920x1080, 30fps, 6s, two full pingpong sweeps, holds briefly at each end.
"""
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

BRAND = json.load(open("/opt/src/gemma-branding/brand-package/brand.json"))
PAL = BRAND["palette"]
FONTDIR = Path("/opt/src/gemma-branding/brand-package/fonts")


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def ease_in_out(t):
    return 0.5 - 0.5 * math.cos(math.pi * t)


def load_fit(path, w, h):
    im = Image.open(path).convert("RGB")
    # cover-fit
    s = max(w / im.width, h / im.height)
    im = im.resize((round(im.width * s), round(im.height * s)))
    x = (im.width - w) // 2
    y = (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def label_chip(draw, text, cx, cy, font, fill_hex, text_hex):
    b = draw.textbbox((0, 0), text, font=font)
    tw, th = b[2] - b[0], b[3] - b[1]
    pad_x, pad_y = 34, 18
    draw.rounded_rectangle(
        [cx - tw / 2 - pad_x, cy - th / 2 - pad_y, cx + tw / 2 + pad_x, cy + th / 2 + pad_y],
        radius=18, fill=hexrgb(fill_hex))
    draw.text((cx, cy), text, font=font, fill=hexrgb(text_hex), anchor="mm")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", required=True)
    ap.add_argument("--after", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label-before", default="BEFORE")
    ap.add_argument("--label-after", default="AFTER")
    ap.add_argument("--dur", type=float, default=6.0)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("-W", "--width", type=int, default=1920)
    ap.add_argument("-H", "--height", type=int, default=1080)
    ap.add_argument("--sweeps", type=int, default=2, help="full left↔right pingpong cycles")
    ap.add_argument("--margin", type=float, default=0.16, help="line travel margin (keeps both labels always visible)")
    a = ap.parse_args()

    W, H = a.width, a.height
    before = load_fit(a.before, W, H)
    after = load_fit(a.after, W, H)
    lo, hi = a.margin * W, (1 - a.margin) * W

    fbig = ImageFont.truetype(str(FONTDIR / "Anton-Regular.ttf"), 96)
    n = max(1, round(a.dur * a.fps))
    hold = 0.10  # fraction of each half-sweep spent held at the extreme

    ff = subprocess.Popen(
        ["ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
         "-r", str(a.fps), "-i", "-",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", a.out],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    for i in range(n):
        # global phase across all sweeps, triangle wave with eased ramps + holds
        ph = (i / n) * a.sweeps * 2.0        # 0..2*sweeps ; each unit = one half sweep
        seg = ph % 2.0                        # 0..2 ; 0..1 = L→R, 1..2 = R→L
        if seg <= 1.0:
            u = seg
        else:
            u = 2.0 - seg
        # apply hold plateaus at the ends then ease
        if u < hold:
            e = 0.0
        elif u > 1 - hold:
            e = 1.0
        else:
            e = ease_in_out((u - hold) / (1 - 2 * hold))
        x = round(lo + e * (hi - lo))

        frame = before.copy()
        frame.paste(after.crop((x, 0, W, H)), (x, 0))   # AFTER on the right of the line
        d = ImageDraw.Draw(frame)
        # divider handle
        d.rectangle([x - 3, 0, x + 3, H], fill=hexrgb(PAL["white"]))
        d.rectangle([x - 1, 0, x + 1, H], fill=hexrgb(PAL["amber"]))
        r = 34
        d.ellipse([x - r, H // 2 - r, x + r, H // 2 + r], fill=hexrgb(PAL["white"]))
        d.polygon([(x - 14, H // 2), (x - 4, H // 2 - 12), (x - 4, H // 2 + 12)], fill=hexrgb(PAL["violet"]))
        d.polygon([(x + 14, H // 2), (x + 4, H // 2 - 12), (x + 4, H // 2 + 12)], fill=hexrgb(PAL["violet"]))
        # labels ride their own side
        label_chip(d, a.label_before, x / 2, 90, fbig, PAL["violet"], PAL["white"])
        label_chip(d, a.label_after, (x + W) / 2, 90, fbig, PAL["gold"], PAL["black"])

        ff.stdin.write(frame.tobytes())
    ff.stdin.close()
    ff.wait()
    print(a.out)


if __name__ == "__main__":
    sys.exit(main())
