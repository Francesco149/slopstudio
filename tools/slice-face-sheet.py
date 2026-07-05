#!/usr/bin/env python3
"""Slice an authored Gemma face sheet into the animated pngtuber rig.

The good rig (vs the unstable SD flap): externally-authored sheets — one per pose+expression
combo — laid out as a grid of perfectly-registered frames. Default layout (1536x1024):
  row 0 = eyes OPEN,  row 1 = eyes CLOSED
  cols  = 6 mouth-openness levels, closed → wide
Filenames are `face_<expr> pose_<pose> ...png`. This slices the grid into equal cells (no
per-cell crop, so every frame shares one coordinate frame ⇒ no jitter), writes them to the
rig's raw/, and updates manifest.json with an ANIMATED expression entry:
  "<expr>": { "mouths": [o0..o5], "mouths_blink": [c0..c5] }
The editor quantizes viseme openness over `mouths` (6 levels) and, during a blink, swaps to
`mouths_blink` at the same mouth level (eyes close without the mouth dropping).

  nix develop --command python tools/slice-face-sheet.py "/path/face_neutral pose_neutral ....png"
  # then matte + shrink:
  RIG=gemma-chibi tools/cutout-sprites.sh
  nix run nixpkgs#pngquant -- --quality 82-98 --force --ext .png presets/avatars/gemma-chibi/*.png

Run once per sheet as you author expressions; the manifest accretes (fallback = neutral).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
FEED = ROOT.parent / "llm-feed" / "feed.py"


def parse_name(path: Path, face: str, pose: str):
    """Pull face_<expr> / pose_<pose> from the filename unless overridden."""
    stem = path.stem
    if not face:
        m = re.search(r"face[_ ]([A-Za-z0-9]+)", stem)
        face = m.group(1).lower() if m else ""
    if not pose:
        m = re.search(r"pose[_ ]([A-Za-z0-9]+)", stem)
        pose = m.group(1).lower() if m else "neutral"
    if not face:
        sys.exit(f"could not parse face_<expr> from '{path.name}'; pass --face")
    return face, pose


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", help="path to the face sheet PNG")
    ap.add_argument("--rig", default="gemma-chibi", help="rig dir under presets/avatars/")
    ap.add_argument("--face", default="", help="expression key (default: parsed from filename)")
    ap.add_argument("--pose", default="", help="pose name (default: parsed; informational for now)")
    ap.add_argument("--rows", type=int, default=2, help="grid rows (eyes open, eyes closed)")
    ap.add_argument("--cols", type=int, default=6, help="grid cols (mouth-openness levels)")
    a = ap.parse_args()

    sheet = Path(a.sheet)
    if not sheet.exists():
        sys.exit(f"no such sheet: {sheet}")
    face, pose = parse_name(sheet, a.face, a.pose)
    rigdir = ROOT / "presets" / "avatars" / a.rig
    raw = rigdir / "raw"
    raw.mkdir(parents=True, exist_ok=True)

    img = Image.open(sheet).convert("RGBA")
    W, H = img.size
    cw, ch = W // a.cols, H // a.rows
    if a.rows != 2:
        sys.exit("only the 2-row (eyes open / eyes closed) layout is wired up")
    print(f"sheet {sheet.name}  {W}x{H} → {a.rows}x{a.cols} cells of {cw}x{ch}  "
          f"face={face} pose={pose} rig={a.rig}")

    open_frames, closed_frames = [], []
    for col in range(a.cols):
        for row in range(a.rows):
            cell = img.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
            tag = "o" if row == 0 else "c"               # row0 eyes-open, row1 eyes-closed
            name = f"{face}-{tag}{col}.png"
            cell.save(raw / name)
            (open_frames if row == 0 else closed_frames).append(name)
    print(f"  wrote {a.rows * a.cols} cells → {raw.relative_to(ROOT)}/{face}-[o|c][0..{a.cols-1}].png")

    # update (or create) the manifest with this expression as an ANIMATED entry
    mpath = rigdir / "manifest.json"
    if mpath.exists():
        man = json.loads(mpath.read_text())
    else:
        man = {
            "schema": "slopstudio.avatar/1", "name": a.rig, "character": "Gemma-san",
            "style": "authored super-deformed chibi animation sheets (registered grid)",
            "size": [cw, ch], "fallback": "neutral",
            "mouth_open_threshold": 0.45, "blink": {"period": 3.6, "dur": 0.13},
            "notes": ("Animated pngtuber rig from authored face sheets (tools/slice-face-sheet.py). "
                      "Each expression: `mouths` = 6 eyes-OPEN mouth-openness frames (closed→wide) the "
                      "editor quantizes viseme openness over, and `mouths_blink` = the eyes-CLOSED row at "
                      "the same 6 levels (a blink closes the eyes without the mouth dropping). One static "
                      "pose per sheet for now; the SD single-frame rig (gemma-pngtuber) is the cheap "
                      "on-demand pose library."),
            "expressions": {},
        }
    man.setdefault("expressions", {})[face] = {"mouths": open_frames, "mouths_blink": closed_frames}
    mpath.write_text(json.dumps(man, indent=2) + "\n")
    print(f"  manifest: expressions.{face} = mouths[{len(open_frames)}] + mouths_blink[{len(closed_frames)}]"
          f"  → {mpath.relative_to(ROOT)}")

    if FEED.exists():
        prev = Path("/tmp") / f"_{a.rig}_{face}_preview.png"  # /tmp, not raw/ (cutout mattes raw/*)
        Image.open(sheet).convert("RGB").save(prev)
        subprocess.run([sys.executable, str(FEED), "image", str(prev),
                        "--title", f"face sheet sliced: {face} (pose {pose})",
                        "--note", f"{a.cols} mouth × 2 eye → animated rig {a.rig}"],
                       check=False, capture_output=True)
    print(f"done. next: RIG={a.rig} tools/cutout-sprites.sh  &&  pngquant the {a.rig}/*.png")


if __name__ == "__main__":
    main()
