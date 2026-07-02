#!/usr/bin/env python3
"""Import pre-keyed SINGLE sprites (one figure per PNG, alpha already clean — e.g. the
GPT-generated *-alpha.png drops) into an avatar rig. The pose-sheet path is
process-pose-sheet.py; this is its sibling for loose hi-res singles that need NO matte,
NO upscale — just alpha-trim + center on a uniform canvas + convention naming.

The single authored source is presets/avatars/<rig>/import-map.json:

  {
    "size": 1536,                       // square canvas (px); sprites are NEVER resampled
    "margin": 24,                       //   (only scaled DOWN if a figure can't fit)
    "fallback": "neutral",
    "map":     { "<source-file>.png": "<base>_<pose>" },   // pose: front|front34|float_front|float34
    "aliases": { "happy": "smug_front", ... }              // canonical/custom emotion -> sprite stem
  }

Run:  nix develop --command python tools/import-rig-sprites.py <srcdir> --rig <name>

- Source files not in "map" are listed and SKIPPED (add a line, rerun) — so a new drop
  in the same dir is: add mapping lines, one command.
- The manifest is REGENERATED from the map + aliases each run (deterministic): every
  sprite stem gets an expression key (variant resolution looks up "<base>_<pose>"), every
  alias points at its target sprite. Aliases live in import-map.json, not the manifest,
  so re-imports can't strand curated keys.
- Each import is face-checked (the editor's pale-skin detector) — a miss is loud but not
  fatal (non-character sprites are legal).
"""
import argparse
import json
import os
import sys
from collections import OrderedDict

import numpy as np
from PIL import Image


def alpha_bbox(alpha, thr=8):
    ys, xs = np.where(alpha > thr)
    if len(ys) == 0:
        return None
    return xs.min(), ys.min(), xs.max(), ys.max()


def center_on_canvas(rgba, size, margin):
    """Trim to the alpha bbox and center on a size x size transparent canvas (same
    normalization as process-pose-sheet.py, so rigs mix)."""
    bb = alpha_bbox(rgba[:, :, 3])
    if bb is None:
        return Image.new("RGBA", (size, size), (0, 0, 0, 0))
    x0, y0, x1, y1 = bb
    fig = Image.fromarray(rgba[y0:y1 + 1, x0:x1 + 1])
    fw, fh = fig.size
    avail = size - 2 * margin
    if fw > avail or fh > avail:                      # never upscale; shrink only if it can't fit
        s = avail / max(fw, fh)
        fig = fig.resize((max(1, int(fw * s)), max(1, int(fh * s))), Image.LANCZOS)
        fw, fh = fig.size
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(fig, ((size - fw) // 2, (size - fh) // 2))
    return canvas


def detect_face(img):
    """The editor's skin-based face detector (sanity check only here)."""
    A = np.asarray(img).astype(int)
    R, G, B, AL = A[:, :, 0], A[:, :, 1], A[:, :, 2], A[:, :, 3]
    op = AL > 40
    bb = alpha_bbox(A[:, :, 3], 8)
    if bb is None:
        return None
    x0, y0, x1, y1 = bb
    bh = y1 - y0 + 1
    mx = np.maximum(np.maximum(R, G), B)
    mn = np.minimum(np.minimum(R, G), B)
    sat = (mx - mn) / np.maximum(mx, 1)
    skin = op & (mx > 222) & (sat < 0.20) & (G <= R + 2) & (G <= B + 2)
    skin &= (np.arange(A.shape[0])[:, None] < (y0 + 0.58 * bh))
    sy, sx = np.where(skin)
    if len(sy) < 30:
        return None
    fx0, fx1 = np.percentile(sx, [8, 92])
    return (fx0 + fx1) / 2, fx1 - fx0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("srcdir", help="directory of pre-keyed RGBA singles")
    ap.add_argument("--rig", required=True, help="rig name (presets/avatars/<rig>/)")
    ap.add_argument("--dry", action="store_true", help="report the plan, write nothing")
    args = ap.parse_args()

    rig_dir = os.path.join("presets", "avatars", args.rig)
    map_path = os.path.join(rig_dir, "import-map.json")
    if not os.path.exists(map_path):
        sys.exit(f"no {map_path} — author it first (see the tool docstring)")
    conf = json.load(open(map_path), object_pairs_hook=OrderedDict)
    size = int(conf.get("size", 1536))
    margin = int(conf.get("margin", 24))
    smap = conf.get("map", {})
    aliases = conf.get("aliases", {})

    src_files = sorted(f for f in os.listdir(args.srcdir) if f.lower().endswith(".png"))
    unmapped = [f for f in src_files if f not in smap]
    missing = [f for f in smap if f not in src_files]

    imported = []
    for fn, stem in smap.items():
        if fn not in src_files:
            continue
        im = Image.open(os.path.join(args.srcdir, fn)).convert("RGBA")
        out = center_on_canvas(np.array(im), size, margin)
        face = detect_face(out)
        tag = f"face w={face[1]:.0f}px" if face else "NO FACE (?)"
        print(f"  {fn}  ->  {stem}.png   [{im.size[0]}x{im.size[1]} -> {size}²]  {tag}")
        if not args.dry:
            os.makedirs(rig_dir, exist_ok=True)
            out.save(os.path.join(rig_dir, f"{stem}.png"))
        imported.append(stem)

    # regenerate the manifest: sprite stems (variant lookup keys) + curated aliases
    exprs = OrderedDict()
    for emo, target in aliases.items():
        exprs[emo] = {"sprite": f"{target}.png"}
    for stem in imported:
        exprs.setdefault(stem, {"sprite": f"{stem}.png"})
    man = OrderedDict([
        ("schema", "slopstudio.avatar/1"), ("name", args.rig), ("character", "Gemma-san"),
        ("style", "hi-res pre-keyed singles (import-rig-sprites.py; source map in import-map.json)"),
        ("size", [size, size]), ("fallback", conf.get("fallback", "neutral")),
        ("expressions", exprs),
    ])
    if not args.dry:
        json.dump(man, open(os.path.join(rig_dir, "manifest.json"), "w"), indent=2, ensure_ascii=False)
        print(f"wrote {rig_dir}/manifest.json ({len(imported)} sprites, {len(aliases)} aliases)")

    for a, t in aliases.items():
        if t not in imported and not os.path.exists(os.path.join(rig_dir, f"{t}.png")):
            print(f"  WARN alias '{a}' -> missing sprite '{t}'")
    if unmapped:
        print(f"  UNMAPPED in {args.srcdir} (add to {map_path}):")
        for f in unmapped:
            print(f"    {f}")
    if missing:
        print(f"  mapped but not present (stale map entries?): {', '.join(missing)}")


if __name__ == "__main__":
    main()
