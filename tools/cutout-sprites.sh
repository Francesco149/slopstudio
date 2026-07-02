#!/usr/bin/env bash
# Background-cut the raw avatar gens → alpha sprites for the pngtuber rig.
#
# The gens come on a plain (uncuttable-by-colour) background; rembg's isnet-anime model
# mattes the chibi by shape — clean even for purple-hair-on-purple, which colour-keying
# can't do. We keep every sprite at the SAME full frame size (no per-sprite crop) so they
# share one coordinate frame ⇒ swapping mouth/emotion never shifts the head (the
# registration the 2-frame flap needs). A final pass zeroes faint edge alpha (halo).
#
#   tools/cutout-sprites.sh                 # matte presets/avatars/gemma-pngtuber/raw/*.png
#   RIG=other tools/cutout-sprites.sh       # a different rig dir under presets/avatars/
set -euo pipefail
cd "$(dirname "$0")/.."
RIG="${RIG:-gemma-pngtuber}"
DIR="presets/avatars/$RIG"
RAW="$DIR/raw"
[ -d "$RAW" ] || { echo "no raw dir: $RAW (generate sprites first)"; exit 1; }

echo "rembg isnet-anime: $RAW/*.png → $DIR/*.png"
# rembg's batch `p` SKIPS outputs that already exist — so clear stale mattes first, or a
# regen at a new resolution/prompt silently keeps the old sprites. (This bit us once.)
find "$DIR" -maxdepth 1 -name '*.png' -delete
nix run nixpkgs#rembg -- p -m isnet-anime "$RAW" "$DIR"

# Kill faint halo (alpha < 12 → 0) then ALIGN every frame to a common feet-baseline +
# horizontal center (ALIGN=0 to skip). All frames keep the same canvas size, so the editor
# composites them in one coordinate frame — but a rig's frames can still sit at different y/x
# within that canvas (authored sheets offset the eyes-open vs eyes-closed rows; the SD rig
# frames each emotion a touch differently). Re-centering by alpha bbox (bottom = feet, mid =
# x-center) on a transparent canvas removes the jump when the compositor swaps mouth/eye/emotion.
ALIGN="${ALIGN:-1}"
nix develop --command python - "$DIR" "$ALIGN" <<'PY'
import sys, glob, os, statistics
from PIL import Image
d, align = sys.argv[1], sys.argv[2] != "0"
paths = sorted(glob.glob(os.path.join(d, "*.png")))
for p in paths:
    im = Image.open(p).convert("RGBA")
    r, g, b, a = im.split()
    a = a.point(lambda v: 0 if v < 12 else v)
    Image.merge("RGBA", (r, g, b, a)).save(p)
    print(f"  cleaned {os.path.basename(p)}  bbox={Image.merge('RGBA',(r,g,b,a)).getbbox()}")
if align and len(paths) > 1:
    boxes = {p: Image.open(p).getbbox() for p in paths}
    boxes = {p: bb for p, bb in boxes.items() if bb}
    ref_cx = statistics.median((bb[0] + bb[2]) / 2 for bb in boxes.values())
    ref_bot = statistics.median(bb[3] for bb in boxes.values())
    for p, bb in boxes.items():
        dx = int(round(ref_cx - (bb[0] + bb[2]) / 2)); dy = int(round(ref_bot - bb[3]))
        if dx or dy:
            im = Image.open(p).convert("RGBA")
            canvas = Image.new("RGBA", im.size, (0, 0, 0, 0))
            canvas.paste(im, (dx, dy), im)
            canvas.save(p)
            print(f"  aligned {os.path.basename(p)}  d=({dx},{dy})")
PY
echo "done → $DIR"
