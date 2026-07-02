#!/usr/bin/env python3
"""png2raw.py — PNG → premultiplied-BGRA top-down raw blob for the XP guest sprites.

The guest tools (win/mascot.c layered window, win/copyhook.c masked AlphaBlend) have
no PNG decoder, so we bake a flat blob the C can read straight into a 32-bpp top-down
DIB: BGRA byte order, alpha premultiplied into B/G/R (what UpdateLayeredWindow and
AlphaBlend with AC_SRC_ALPHA expect).  The sprite is *contained* (aspect-preserved)
inside W*H and centred on full transparency, so the C only needs W/H from its args.

Run inside `nix develop .#xp` (pillow):
  python tools/xp/png2raw.py library/images/gemma-smug.png cache/xp/sprites/gemma.raw --w 220 --h 300
"""
import argparse
import os
import sys

from PIL import Image


def convert(src, dst, w, h, anchor="center", pad=8, straight=False):
    im = Image.open(src).convert("RGBA")
    # contain into (w-2pad)x(h-2pad), preserve aspect
    bw, bh = max(1, w - 2 * pad), max(1, h - 2 * pad)
    iw, ih = im.size
    scale = min(bw / iw, bh / ih)
    nw, nh = max(1, round(iw * scale)), max(1, round(ih * scale))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ox = (w - nw) // 2
    oy = (h - nh) // 2 if anchor == "center" else (h - nh - pad)  # bottom-anchor for floaters
    canvas.paste(im, (ox, oy), im)

    px = canvas.load()
    out = bytearray(w * h * 4)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if straight:
                # DELIBERATELY WRONG: straight (un-premultiplied) colors under AC_SRC_ALPHA —
                # the classic bright-halo bug demo (soft edges blend too bright).
                out[i + 0] = b
                out[i + 1] = g
                out[i + 2] = r
            else:
                # premultiply + BGRA
                out[i + 0] = (b * a) // 255
                out[i + 1] = (g * a) // 255
                out[i + 2] = (r * a) // 255
            out[i + 3] = a
            i += 4
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(out)
    print(f"png2raw: {src} -> {dst}  ({w}x{h} BGRA {'STRAIGHT (halo bug)' if straight else 'premultiplied'}, sprite {nw}x{nh} @ {ox},{oy})")


def main(argv=None):
    ap = argparse.ArgumentParser(description="PNG -> premultiplied BGRA top-down raw")
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--w", type=int, required=True)
    ap.add_argument("--h", type=int, required=True)
    ap.add_argument("--anchor", choices=("center", "bottom"), default="center")
    ap.add_argument("--pad", type=int, default=8)
    ap.add_argument("--straight", action="store_true",
                    help="skip premultiplication (the alpha-halo BUG demo blob)")
    a = ap.parse_args(argv)
    convert(a.src, a.dst, a.w, a.h, a.anchor, a.pad, a.straight)
    return 0


if __name__ == "__main__":
    sys.exit(main())
