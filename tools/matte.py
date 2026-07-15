#!/usr/bin/env python3
"""matte.py — cut a subject out of an image to a transparent RGBA PNG.

Modes (in priority order):
  rembg (default)  — POST to the rembg provider on lame (job protocol), any model. Best for
                     photographic / product shots (u2net / isnet-general-use / birefnet-general)
                     and stylised art (isnet-anime). Follows tools/process-pose-sheet.py's client.
  --chroma R,G,B   — local color key: pixels within --tol of the color go transparent (a clean
                     solid background — the "chroma key the bg out" case). Feathers the edge.
  --defringe N     — erode the alpha edge by N px and un-multiply light halo (kills white-pixel
                     bleed on an already-transparent PNG). Composable with the above.
  --crop x,y,w,h   — crop (fractions 0..1) BEFORE matting (split a composite into separate objects).

Examples:
  matte.py in.png out.png --model isnet-general-use
  matte.py glove.png glove-cut.png --chroma 255,255,255 --tol 26
  matte.py ttt2.png ttt2-clean.png --defringe 1
  matte.py cart-vs-wiimote.png cart.png --crop 0,0,0.55,1 --model isnet-general-use
"""
import argparse, base64, sys, time, io
from pathlib import Path

REMBG_URL = "http://10.0.10.56:8015"


def rembg_cutout(png_bytes, url, model, alpha_matting, timeout=240):
    import requests
    body = {"type": "remove_bg", "params": {
        "image_b64": base64.b64encode(png_bytes).decode("ascii"),
        "model": model, "alpha_matting": bool(alpha_matting), "post_process": True}}
    r = requests.post(url + "/jobs", json=body, timeout=60); r.raise_for_status()
    j = r.json(); job_id, h = j["job_id"], j["hash"]
    result = j.get("result") if j.get("cached") else None
    deadline = time.monotonic() + timeout
    while result is None:
        if time.monotonic() > deadline:
            raise TimeoutError(f"rembg job {job_id} timed out after {timeout}s")
        time.sleep(0.4)
        v = requests.get(f"{url}/jobs/{job_id}", timeout=60).json()
        if v.get("status") == "done": result = v.get("result"); break
        if v.get("status") == "error": raise RuntimeError(f"rembg failed: {v.get('error') or v}")
    a = requests.get(f"{url}/assets/{h}.png", timeout=120); a.raise_for_status()
    return a.content


def chroma_key(im, rgb, tol, feather):
    from PIL import Image
    import math
    im = im.convert("RGBA"); px = im.load(); W, H = im.size
    kr, kg, kb = rgb
    lo, hi = tol, tol + max(1, feather)
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            d = math.sqrt((r-kr)**2 + (g-kg)**2 + (b-kb)**2)
            if d <= lo: px[x, y] = (r, g, b, 0)
            elif d < hi: px[x, y] = (r, g, b, int(a * (d-lo)/(hi-lo)))
    return im


def remove_color(im, rgb, tol, dilate=3):
    """Remove a DRAWN overlay colour (e.g. a red X baked over an object). Pixels of that colour that
    sit ON the subject (within `dilate` px of the real body) are filled with the subject's MEDIAN
    colour — for a uniform object (a grey remote) this is a near-invisible flat patch; pixels off the
    subject (isolated marker arms over the former background) go transparent. (A diffusion fill was
    tried but pulled dark edge/shadow colour into the strokes — worse; flat median wins here.)"""
    import numpy as np
    from PIL import Image
    im = im.convert("RGBA"); arr = np.asarray(im).astype(np.int16)
    r, g, b, a = arr[..., 0], arr[..., 1], arr[..., 2], arr[..., 3]
    kr, kg, kb = rgb
    red = (np.abs(r-kr) < tol) & (np.abs(g-kg) < tol) & (np.abs(b-kb) < tol) & (a > 0)
    body = (a > 40) & (~red)
    reach = body.copy()
    for _ in range(dilate):
        reach = reach | np.roll(reach, 1, 0) | np.roll(reach, -1, 0) | np.roll(reach, 1, 1) | np.roll(reach, -1, 1)
    on, off = red & reach, red & (~reach)
    med = [int(np.median(arr[..., i][body])) for i in range(3)] if body.any() else [185, 185, 190]
    for i in range(3):
        arr[..., i][on] = med[i]
    arr[..., 3][off] = 0
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA")


def defringe(im, n):
    """Erode the alpha edge by n px (kills 1px halos) then un-premultiply light fringe."""
    from PIL import Image, ImageFilter
    im = im.convert("RGBA")
    r, g, b, a = im.split()
    for _ in range(n):
        a = a.filter(ImageFilter.MinFilter(3))
    return Image.merge("RGBA", (r, g, b, a))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp"); ap.add_argument("outp")
    ap.add_argument("--model", default="isnet-general-use")
    ap.add_argument("--alpha-matting", action="store_true")
    ap.add_argument("--chroma", help="R,G,B local color key instead of rembg")
    ap.add_argument("--tol", type=float, default=24.0)
    ap.add_argument("--feather", type=float, default=18.0)
    ap.add_argument("--defringe", type=int, default=0)
    ap.add_argument("--rmcolor", help="R,G,B of a drawn overlay to inpaint away (e.g. a red X)")
    ap.add_argument("--rmtol", type=float, default=90.0)
    ap.add_argument("--crop", help="x,y,w,h fractions 0..1 before matting")
    ap.add_argument("--url", default=REMBG_URL)
    a = ap.parse_args()
    from PIL import Image
    im = Image.open(a.inp).convert("RGBA")
    if a.crop:
        x, y, w, h = (float(v) for v in a.crop.split(","))
        W, H = im.size
        im = im.crop((int(x*W), int(y*H), int((x+w)*W), int((y+h)*H)))
    if a.chroma:
        rgb = tuple(int(v) for v in a.chroma.split(","))
        im = chroma_key(im, rgb, a.tol, a.feather)
    else:
        buf = io.BytesIO(); im.save(buf, "PNG")
        out = rembg_cutout(buf.getvalue(), a.url.rstrip("/"), a.model, a.alpha_matting)
        im = Image.open(io.BytesIO(out)).convert("RGBA")
    if a.rmcolor:
        im = remove_color(im, tuple(int(v) for v in a.rmcolor.split(",")), a.rmtol)
    if a.defringe:
        im = defringe(im, a.defringe)
    # tight-crop to the alpha bbox so the object fills the PNG (better for scene placement)
    bbox = im.getbbox()
    if bbox: im = im.crop(bbox)
    Path(a.outp).parent.mkdir(parents=True, exist_ok=True)
    im.save(a.outp)
    print(f"matte: {a.inp} -> {a.outp}  {im.size}")


if __name__ == "__main__":
    main()
