#!/usr/bin/env python3
"""Seed per-sprite face-box sidecars for an avatar rig so face-anchored framing (bust/closeup/full)
renders every pose at a CONSISTENT apparent size.

WHY: the editor's live face detector (face_from_pixels) measures the horizontal spread of pale-skin
pixels over the upper ~58% of the figure. Poses that bare more shoulder/chest/arm skin (smug, neutral)
report a WIDER "face" than a hands-in pose (explaining). Because the fit scales so faceW -> a fixed
fraction of the frame (scl proportional to 1/faceW), an inflated faceW shrinks the WHOLE figure -> the
room shots don't match at the same clip scale.

FIX: write a sidecar "face":{cx,eyeY,w} (source px) next to each sprite. We PRESERVE the detector's
cx/eyeY (vertical/horizontal placement is unchanged, so the golden reference shot doesn't move) and
replace only the WIDTH with a head-band measurement of the TRUE face width (the skin spread in the
head region, ABOVE where the shoulders widen it). The editor's avatar_face() reads this override.
The in-editor "Tune face box" gizmo fine-tunes from here.

Usage:
  tools/gen-face-boxes.py presets/avatars/gemma-big            # write sidecars for the rig
  tools/gen-face-boxes.py presets/avatars/gemma-big --dry-run  # report only, write nothing
"""
import argparse, glob, json, os, sys
import numpy as np
from PIL import Image


def alpha_bbox(a, thr=8):
    ys, xs = np.where(a > thr)
    if len(xs) == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def skin_mask(px):
    r = px[:, :, 0].astype(int); g = px[:, :, 1].astype(int)
    b = px[:, :, 2].astype(int); a = px[:, :, 3].astype(int)
    mx = np.maximum(np.maximum(r, g), b); mn = np.minimum(np.minimum(r, g), b)
    # EXACT mirror of face_from_pixels' pixel test (bright, low-sat, pink-leaning)
    return (a > 40) & (mx > 222) & ((mx - mn) * 100 < 20 * mx) & (g <= r + 2) & (g <= b + 2)


def detect_old(px):
    """Faithful port of editor face_from_pixels -> (cx, eyeY, w) in source px, or None."""
    h, w, _ = px.shape
    a = px[:, :, 3].astype(int)
    bb = alpha_bbox(a, 8)
    if bb is None:
        return None
    x0, y0, x1, y1 = bb
    bw, bh = x1 - x0 + 1, y1 - y0 + 1
    headBot = y0 + int(bh * 0.58)
    ymax = min(y1, headBot)
    skin = skin_mask(px)
    region = np.zeros_like(skin); region[y0:ymax + 1, x0:x1 + 1] = True
    sy, sx = np.where(skin & region)
    if len(sx) >= 30:
        sxv = np.sort(sx); syv = np.sort(sy); n = len(sxv)
        lo, hi = int(n * 0.08), min(n - 1, int(n * 0.92))
        fx0, fx1 = float(sxv[lo]), float(sxv[hi]); fy0, fy1 = float(syv[lo]), float(syv[hi])
        return dict(cx=(fx0 + fx1) * 0.5, eyeY=fy0 + 0.62 * (fy1 - fy0), w=max(8.0, fx1 - fx0),
                    bbox=bb, fallback=False)
    return dict(cx=x0 + bw * 0.5, eyeY=y0 + bh * 0.20, w=bw * 0.34, bbox=bb, fallback=True)


def true_face_width(px):
    """Head-band TRUE face width: the skin spread in the head region, stopping where the shoulders
    widen it. Robust to poses baring chest/arm skin. Returns (w, headTop, shoulderY, cx_head)."""
    h, w, _ = px.shape
    a = px[:, :, 3].astype(int)
    bb = alpha_bbox(a, 8)
    x0, y0, x1, y1 = bb
    bh = y1 - y0 + 1
    skin = skin_mask(px)
    # per-row skin spread s(y) (95/5 pct) + count, over the figure
    spread = np.zeros(y1 + 1); count = np.zeros(y1 + 1)
    for y in range(y0, y1 + 1):
        xs = np.where(skin[y])[0]
        count[y] = len(xs)
        if len(xs) >= 8:
            xsv = np.sort(xs)
            spread[y] = xsv[min(len(xsv) - 1, int(len(xsv) * 0.95))] - xsv[int(len(xsv) * 0.05)]
    sub = np.where(count >= 20)[0]
    sub = sub[sub >= y0]
    if len(sub) == 0:
        return max(8.0, (x1 - x0 + 1) * 0.34), y0, y0 + int(bh * 0.2), x0 + (x1 - x0) * 0.5
    headTop = int(sub[0])
    # smooth spread with a small window
    k = max(3, int(bh * 0.01))
    sm = np.convolve(spread, np.ones(k) / k, mode="same")
    refRows = [y for y in range(headTop, min(y1, headTop + int(bh * 0.10)) + 1) if count[y] >= 20]
    faceRef = float(np.median([sm[y] for y in refRows])) if refRows else float(sm[headTop])
    # shoulders: first row past a minimum head height where the (smoothed) spread jumps past 1.35x ref
    minHead = headTop + int(bh * 0.05)
    shoulderY = None
    for y in range(minHead, y1 + 1):
        if count[y] >= 20 and sm[y] > 1.35 * faceRef:
            shoulderY = y; break
    if shoulderY is None:
        shoulderY = min(y1, headTop + int(bh * 0.22))
    band = [y for y in range(headTop, shoulderY) if count[y] >= 20]
    if not band:
        band = [headTop]
    # true face width = 85th pct of per-row spreads in the head band (cheek/jaw width, noise-robust)
    wtrue = float(np.percentile([spread[y] for y in band], 85))
    # head center-x = median center of skin in the band
    cxs = []
    for y in band:
        xs = np.where(skin[y])[0]
        if len(xs) >= 8:
            xsv = np.sort(xs); cxs.append(0.5 * (xsv[int(len(xsv) * 0.05)] + xsv[min(len(xsv) - 1, int(len(xsv) * 0.95))]))
    cxh = float(np.median(cxs)) if cxs else (x0 + (x1 - x0) * 0.5)
    # eye-line: ~0.55 x face-width below the forehead-skin top. Tied to the ROBUST width (not the
    # fragile shoulder row) — heads are ~1.25x taller than wide and the eyes sit ~0.42 down, so
    # 0.42*1.25 ~= 0.55 of the width. A CONSISTENT facial feature across poses ⇒ every framing pins
    # the same point at eyeFrameY (fixes the vertical mismatch the old full-body skin scan caused).
    eyeY = headTop + 0.55 * wtrue
    return max(8.0, wtrue), headTop, shoulderY, cxh, eyeY


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rigdir", help="rig dir (globs *.png) or a single PNG")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.rigdir.lower().endswith(".png"):
        files = [args.rigdir]
    else:
        files = sorted(glob.glob(os.path.join(args.rigdir, "*.png")))
        files = [f for f in files if not f.endswith(".mask.png")]
    if not files:
        print("no sprites found", file=sys.stderr); sys.exit(1)

    rows = []
    for f in files:
        px = np.array(Image.open(f).convert("RGBA"))
        old = detect_old(px)
        if old is None:
            print(f"  {os.path.basename(f):26s} EMPTY — skipped"); continue
        wtrue, headTop, shoulderY, cxh, eyeYh = true_face_width(px)
        # WIDTH (true face width) fixes apparent SIZE; EYE-LINE (head-band, a consistent facial feature)
        # fixes vertical FRAMING. cx keeps the detector value (robust 8/92-pct center; head-band cx is
        # pulled off-face by an outstretched arm's skin).
        bh = old["bbox"][3] - old["bbox"][1] + 1
        face = {"cx": int(round(old["cx"])), "eyeY": int(round(eyeYh)), "w": int(round(wtrue))}
        eye_frac = (eyeYh - old["bbox"][1]) / bh
        rows.append((os.path.basename(f), old["w"], wtrue))
        note = " (fallback detect)" if old.get("fallback") else ""
        print(f"  {os.path.basename(f):26s} old_w={old['w']:6.1f} -> true_w={wtrue:6.1f}  "
              f"cx={face['cx']:4d} eye={face['eyeY']:4d} (eye {eye_frac*100:4.1f}% down){note}")
        if not args.dry_run:
            side_path = f + ".meta.json"
            side = {}
            if os.path.exists(side_path):
                try:
                    side = json.load(open(side_path))
                except Exception:
                    side = {}
            side["face"] = face
            json.dump(side, open(side_path, "w"), indent=2)

    # consistency report: apparent size at the same clip scale is proportional to 1/faceW (fit anchors the face)
    if rows:
        base_old = next((ow for n, ow, tw in rows if "explaining" in n), rows[0][1])
        base_new = next((tw for n, ow, tw in rows if "explaining" in n), rows[0][2])
        print("\n  apparent host size vs explaining (bust fit anchors the face; smaller = shrunk):")
        for n, ow, tw in rows:
            print(f"    {n:26s} old {base_old/ow:0.3f}x   ->   tuned {base_new/tw:0.3f}x")
    print(f"\n{'DRY-RUN (nothing written)' if args.dry_run else 'wrote face sidecars for %d sprites' % len(rows)}")


if __name__ == "__main__":
    main()
