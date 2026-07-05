#!/usr/bin/env python
"""
process-pose-sheet.py — turn a GPT 3x2 pose sheet (or a whole dir of them) into matted,
named avatar-rig sprites + a merged manifest. This is the "smart" sprite processor.

What it does, per sheet:
  - INFERS the grid (3 cols x 2 rows by default) from gaps in the alpha projection of a quick
    local key — the figures are NOT in even cells (they drift), so a naive even split would
    clip them. (The local key is a saturation-aware neutral-grey matte: the GPT background is a
    flat light grey, and a plain RGB-distance colorkey EATS the light purple hair, so we key on
    *neutrality* — low chroma in the bg brightness band — which is plenty to FIND the figures.)
  - MATTES each cell with **rembg (isnet-anime, the provider on lame)** by DEFAULT — this is the
    same AI matte the avatar sprite pipeline uses (tools/cutout-sprites.sh) and it cuts the figure
    by SHAPE, so it kills the soft grey bg + drop shadows the local colorkey only thins (the
    "background bleed" the local pass left). One figure per cell is isnet-anime's sweet spot. The
    cell is cut from the ORIGINAL (un-keyed) sheet so rembg sees real edges. `--local` skips the
    provider and uses the neutral-grey key directly (offline / no-lame fallback — bleeds more).
    Use `--alpha-matting` for SOFT anti-aliased edges + fine hair (the default hard matte is
    binary → jagged/"pixelated" edges).
  - `--upscale`: STRICT anime super-resolution (RealESRGAN via the image provider's `upscale`
    capability — NO diffusion, so her design is unchanged) of the ORIGINAL cell BEFORE matting, so
    the matte runs at high res → sharp lines + fine hair, and the canvas defaults 512→1024 (2x res).
    (img2img "upscale" is NOT used — it drifts the costume/pose.)
  - TRIMS each cell to the alpha bbox, centers it on the `--canvas` (512 default, 1024 with
    `--upscale`; the manifest `size` is kept in sync), and names it by the rig pose convention:
        row0 = front / front34 / viewer34,  row1 = float_front / float34 / float_viewer34
  - SANITY-CHECKS each cell with the same skin-based face detector the editor uses for
    framing (flags any cell where no face is found — usually a bad cut / wrong grid).
  - MERGES presets/avatars/<rig>/manifest.json: sets <emotion> -> <emotion>_front and adds
    every pose variant, preserving any curated alias keys already in the manifest.

Usage:
  nix develop --command python tools/process-pose-sheet.py SHEET_OR_DIR --rig gemma-gpt-static
    [--name smug]           emotion name override (default: filename's first token, aliased)
    [--cols 3 --rows 2]     grid (default 3x2)
    [--local]               skip rembg; matte with the local neutral-grey key (bleeds more)
    [--model isnet-anime]   rembg segmentation model (isnet-anime = host/stylised art)
    [--alpha-matting]       rembg finer hair/edge matting → SOFT edges (fixes "pixelated")
    [--rembg-url URL]       override the provider URL (default: config.toml providers.rembg)
    [--upscale]             RealESRGAN anime super-res before matting → 2x res (canvas 512→1024)
    [--upscale-url URL]     image provider URL (default: config.toml providers.image)
    [--upscale-model NAME]  ESRGAN model (default RealESRGAN_x4plus_anime_6B.pth)
    [--sat 16 --lo-span 72 --hi-span 8 --ramp 10]  local-key tuning (grid-find + --local matte)
    [--canvas 512 --margin 12]                      output canvas size + margin (auto 1024/24 with --upscale)
    [--preview /tmp/sheet.jpg]                       write a contact sheet to eyeball
    [--dry]                 report (incl. face check) without writing sprites/manifest
"""
import argparse, base64, glob, io, json, os, sys, time
from collections import OrderedDict
import numpy as np
from PIL import Image

# Pose names per grid cell (row-major). 3x2 is the GPT-sheet convention; other sizes fall
# back to r{row}c{col}. Matches the existing gemma-gpt-static manifest exactly.
POSE_3x2 = ["front", "front34", "viewer34", "float_front", "float34", "float_viewer34"]
# filename token -> rig emotion (the curated single-word names)
NAME_ALIAS = {"point and laugh": "laugh", "teacher": "teaching"}


def emotion_from_filename(path):
    base = os.path.basename(path)
    raw = base.split(" ChatGPT")[0].strip().lower()        # "neutral ChatGPT Image …" -> "neutral"
    return NAME_ALIAS.get(raw, raw.replace(" ", "_"))


# ── rembg (the default cell matte — the provider on lame, isnet-anime) ────────
def rembg_url(override=None):
    """Provider URL: --rembg-url, else config.toml [providers.rembg].url, else the lame default."""
    if override:
        return override.rstrip("/")
    try:
        import tomllib
        with open("config.toml", "rb") as f:
            u = tomllib.load(f).get("providers", {}).get("rembg", {}).get("url")
        if u:
            return u.rstrip("/")
    except Exception:
        pass
    return "http://10.0.10.56:8015"


def rembg_cutout(png_bytes, url, model="isnet-anime", alpha_matting=False, timeout=240):
    """Submit one cell to the rembg provider → fetch the RGBA cutout (PNG bytes).
    Follows the provider job protocol: POST /jobs → poll GET /jobs/{id} → GET /assets/{hash}.png."""
    import requests

    body = {"type": "remove_bg", "params": {
        "image_b64": base64.b64encode(png_bytes).decode("ascii"),
        "model": model, "alpha_matting": bool(alpha_matting), "post_process": True}}
    r = requests.post(url + "/jobs", json=body, timeout=60)
    r.raise_for_status()
    j = r.json()
    job_id, h = j["job_id"], j["hash"]
    result = j.get("result") if j.get("cached") else None
    deadline = time.monotonic() + timeout
    while result is None:
        if time.monotonic() > deadline:
            raise TimeoutError(f"rembg job {job_id} timed out after {timeout}s")
        time.sleep(0.4)
        v = requests.get(f"{url}/jobs/{job_id}", timeout=60).json()
        st = v.get("status")
        if st == "done":
            result = v.get("result")
            break
        if st == "error":
            raise RuntimeError(f"rembg job {job_id} failed: {v.get('error') or v}")
    a = requests.get(f"{url}/assets/{h}.png", timeout=120)
    a.raise_for_status()
    return a.content


def image_url(override=None):
    """Image-provider URL (ComfyUI adapter, hosts the `upscale` capability): --upscale-url, else
    config.toml [providers.image].url, else the lame default."""
    if override:
        return override.rstrip("/")
    try:
        import tomllib
        with open("config.toml", "rb") as f:
            u = tomllib.load(f).get("providers", {}).get("image", {}).get("url")
        if u:
            return u.rstrip("/")
    except Exception:
        pass
    return "http://10.0.10.56:8011"


def upscale_cell(rgb, url, model="RealESRGAN_x4plus_anime_6B.pth", timeout=240):
    """Strict anime super-resolution of one cell via the image provider's `upscale` capability
    (RealESRGAN — NO diffusion, so the content is unchanged, just higher-res + sharper line/edge
    detail). Upscale the ORIGINAL (un-matted) cell so the matte then runs at high res → fine hair
    + soft edges. Returns an RGB uint8 array (~4x). Raises on failure (caller falls back)."""
    import requests
    buf = io.BytesIO()
    Image.fromarray(rgb[:, :, :3]).save(buf, format="PNG")
    body = {"type": "upscale", "params": {
        "init_b64": base64.b64encode(buf.getvalue()).decode("ascii"), "upscale_model": model}}
    r = requests.post(url + "/jobs", json=body, timeout=60)
    r.raise_for_status()
    j = r.json()
    job_id, h = j["job_id"], j["hash"]
    result = j.get("result") if j.get("cached") else None
    deadline = time.monotonic() + timeout
    while result is None:
        if time.monotonic() > deadline:
            raise TimeoutError(f"upscale job {job_id} timed out after {timeout}s")
        time.sleep(0.4)
        v = requests.get(f"{url}/jobs/{job_id}", timeout=60).json()
        if v.get("status") == "done":
            result = v.get("result"); break
        if v.get("status") == "error":
            raise RuntimeError(f"upscale job {job_id} failed: {v.get('error') or v}")
    a = requests.get(f"{url}/assets/{h}.png", timeout=180)
    a.raise_for_status()
    return np.array(Image.open(io.BytesIO(a.content)).convert("RGB"))


def detect_bg(rgb):
    """Median colour of the four corner patches (the flat GPT background)."""
    h, w = rgb.shape[:2]
    k = max(8, min(h, w) // 40)
    cor = np.concatenate([rgb[:k, :k], rgb[:k, -k:], rgb[-k:, :k], rgb[-k:, -k:]]).reshape(-1, 3)
    return np.median(cor, axis=0)


def neutral_matte(A, bg, sat=16.0, lo_span=72.0, hi_span=8.0, ramp=10.0):
    """Saturation-aware key: bg = low-chroma pixels in the brightness band around the DETECTED
    bg (so a brighter/darker sheet still keys — a fixed band missed brighter sheets). The band
    reaches below the bg to catch its soft shadows; the dark bodysuit sits far below it. Soft
    edge via a chroma ramp. Returns a new RGBA uint8 array."""
    rgb = A[:, :, :3].astype(np.int32)
    chroma = rgb.max(2) - rgb.min(2)
    bright = rgb.mean(2)
    bgB = float(np.mean(bg))
    in_band = (bright > bgB - lo_span) & (bright < bgB + hi_span)
    alpha = np.where(in_band & (chroma < sat), 0.0, 1.0)
    near = in_band & (chroma >= sat) & (chroma < sat + ramp)          # ramp soft edges
    alpha = np.where(near, (chroma - sat) / ramp, alpha)
    out = A.copy()
    out[:, :, 3] = (np.clip(alpha, 0, 1) * (A[:, :, 3].astype(float) / 255.0) * 255.0).astype(np.uint8)
    return out


def _bands(proj, n, axislen, thr_frac=0.03, merge_frac=0.012):
    """Runs of the projection above threshold, bridging only tiny (noise) gaps — the
    inter-figure gaps must stay split. Returns the n widest bands, sorted by position."""
    thr = proj.max() * thr_frac
    on = proj > thr
    runs, s = [], None
    for i, v in enumerate(on):
        if v and s is None:
            s = i
        elif not v and s is not None:
            runs.append([s, i]); s = None
    if s is not None:
        runs.append([s, len(on)])
    merge_gap = merge_frac * axislen                                  # join figure parts split by a thin gap
    merged = []
    for r in runs:
        if merged and r[0] - merged[-1][1] < merge_gap:
            merged[-1][1] = r[1]
        else:
            merged.append(r)
    merged.sort(key=lambda r: r[1] - r[0], reverse=True)              # keep the n biggest bands
    merged = sorted(merged[:n], key=lambda r: r[0])
    return merged


def _expand_bands(bands, proj, n):
    """Grow each band outward to the figure's FULL extent — past the coverage threshold to where the
    projection actually hits zero (thin horns/feet/tail), bounded by the midpoint to the neighbor band.
    Without this the threshold clips thin horns (the float-row figures were beheaded at the cell edge)."""
    out = []
    for i, b in enumerate(bands):
        b0, b1 = b[0], b[1]
        lo = 0 if i == 0 else (bands[i - 1][1] + b0) // 2
        hi = n if i == len(bands) - 1 else (b1 + bands[i + 1][0]) // 2
        while b0 - 1 >= lo and proj[b0 - 1] > 0: b0 -= 1     # grab horns above
        while b1 < hi and b1 < n and proj[b1] > 0: b1 += 1   # grab feet/tail below
        out.append([b0, b1])
    return out


def infer_grid(alpha, cols, rows):
    """Gap-based cell rects (row-major). Bands are expanded to each figure's full extent (so thin
    horns aren't clipped). Falls back to an even split if the projection doesn't yield cols x rows."""
    h, w = alpha.shape
    a = alpha > 32
    colp, rowp = a.sum(0), a.sum(1)
    cb = _bands(colp, cols, w)
    rb = _bands(rowp, rows, h)
    if len(cb) != cols or len(rb) != rows:
        print(f"  ! grid: found {len(cb)}x{len(rb)} bands, expected {cols}x{rows} — even split", file=sys.stderr)
        cb = [[int(c * w / cols), int((c + 1) * w / cols)] for c in range(cols)]
        rb = [[int(r * h / rows), int((r + 1) * h / rows)] for r in range(rows)]
    else:
        cb = _expand_bands(cb, colp, w)
        rb = _expand_bands(rb, rowp, h)
    cells = []
    for (y0, y1) in rb:
        for (x0, x1) in cb:
            cells.append((x0, y0, x1, y1))
    return cells


def alpha_bbox(alpha, thr=8):
    ys, xs = np.where(alpha > thr)
    if len(ys) == 0:
        return None
    return xs.min(), ys.min(), xs.max(), ys.max()


def center_on_canvas(cell, size, margin):
    """Trim cell (RGBA array) to its alpha bbox and center it on a size x size transparent
    canvas (scaling down to fit size-2*margin if the figure is too big)."""
    bb = alpha_bbox(cell[:, :, 3])
    if bb is None:
        return Image.new("RGBA", (size, size), (0, 0, 0, 0))
    x0, y0, x1, y1 = bb
    fig = Image.fromarray(cell[y0:y1 + 1, x0:x1 + 1])
    fw, fh = fig.size
    avail = size - 2 * margin
    if fw > avail or fh > avail:
        s = avail / max(fw, fh)
        fig = fig.resize((max(1, int(fw * s)), max(1, int(fh * s))))
        fw, fh = fig.size
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(fig, ((size - fw) // 2, (size - fh) // 2))
    return canvas


def detect_face(cell):
    """The editor's skin-based face detector (sanity check). Returns (cx, eyeY, w) in px or
    None. Pale-pink skin = bright + low-saturation + green-is-min, in the upper body."""
    A = np.asarray(cell).astype(int)
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
    fy0, fy1 = np.percentile(sy, [8, 92])
    return (fx0 + fx1) / 2, fy0 + 0.62 * (fy1 - fy0), fx1 - fx0


def pose_names(cols, rows):
    if cols == 3 and rows == 2:
        return POSE_3x2
    return [f"r{r}c{c}" for r in range(rows) for c in range(cols)]


def merge_manifest(rig_dir, rig, emotion, files, canvas=512):
    path = os.path.join(rig_dir, "manifest.json")
    if os.path.exists(path):
        man = json.load(open(path), object_pairs_hook=OrderedDict)
    else:
        man = OrderedDict([("schema", "slopstudio.avatar/1"), ("name", rig),
                           ("character", "Gemma-san"), ("size", [512, 512]),
                           ("fallback", "neutral"), ("expressions", OrderedDict())])
    man["size"] = [canvas, canvas]                  # keep in sync with the output canvas (res bump)
    exprs = man.setdefault("expressions", OrderedDict())
    # canonical emotion -> the front pose; every variant under <emotion>_<pose>
    front = next((fn for pose, fn in files if pose == "front"), files[0][1])
    exprs[emotion] = {"sprite": front}
    for pose, fn in files:
        exprs[f"{emotion}_{pose}"] = {"sprite": fn}
    json.dump(man, open(path, "w"), indent=2, ensure_ascii=False)
    return path


def cell_matte_rembg(orig_cell, rembg):
    """Matte one cell with the rembg provider (cut from the ORIGINAL, un-keyed sheet so rembg
    sees real edges). Returns an RGBA uint8 array. Raises on provider failure (caller falls back)."""
    buf = io.BytesIO()
    Image.fromarray(orig_cell).save(buf, format="PNG")
    out = rembg_cutout(buf.getvalue(), rembg["url"], rembg["model"], rembg["alpha_matting"])
    return np.array(Image.open(io.BytesIO(out)).convert("RGBA"))


def process_sheet(path, rig_dir, rig, name, cols, rows, mat, canvas, margin, dry, preview, rembg=None, upscale=None):
    emotion = name or emotion_from_filename(path)
    A = np.array(Image.open(path).convert("RGBA"))
    bg = detect_bg(A[:, :, :3].astype(int))
    matted = neutral_matte(A, bg, **mat)            # local key: finds the grid (and is the --local matte)
    cells = infer_grid(matted[:, :, 3], cols, rows)
    poses = pose_names(cols, rows)
    how = f"rembg:{rembg['model']}" if rembg else "local-key"
    print(f"[{emotion}] {os.path.basename(path)}  bg={bg.astype(int).tolist()}  {len(cells)} cells  matte={how}"
          f"{'  +upscale' if upscale else ''}")
    out_files, previews = [], []
    for (x0, y0, x1, y1), pose in zip(cells, poses):
        warn = ""
        orig = A[y0:y1 + 1, x0:x1 + 1]              # the ORIGINAL (un-keyed) cell
        if upscale:                                 # strict SR the cell BEFORE matting → hi-res edges + hair
            try:
                t0 = time.monotonic()
                orig = upscale_cell(orig, upscale["url"], upscale["model"])
                warn += f"  up{time.monotonic() - t0:.0f}s"
            except Exception as e:
                warn += f"  ⚠ UPSCALE FAILED ({e})"
        if rembg:
            t0 = time.monotonic()
            try:
                cell = cell_matte_rembg(orig, rembg)
                warn += f"  matte{time.monotonic() - t0:.1f}s"
            except Exception as e:                  # transient provider hiccup → local fallback, loud
                cell = matted[y0:y1 + 1, x0:x1 + 1]  # (original-res local key; can't local-key an upscaled cell)
                warn += f"  ⚠ REMBG FAILED ({e}) — used local key"
        else:
            cell = matted[y0:y1 + 1, x0:x1 + 1]
        sprite = center_on_canvas(cell, canvas, margin)
        face = detect_face(sprite)
        if not face:
            warn += "  ⚠ NO FACE (check cut)"
        fn = f"{emotion}_{pose}.png"
        print(f"    {pose:14} -> {fn}{warn}")
        if not dry:
            sprite.save(os.path.join(rig_dir, fn))
        out_files.append((pose, fn))
        previews.append(sprite)
    if not dry:
        merge_manifest(rig_dir, rig, emotion, out_files, canvas)
    if preview:
        cw = canvas // 3
        sheet = Image.new("RGB", (cw * cols, cw * rows), (40, 40, 50))
        for i, s in enumerate(previews):
            bgc = Image.new("RGBA", s.size, (60, 60, 72, 255)); bgc.alpha_composite(s)
            sheet.paste(bgc.convert("RGB").resize((cw, cw)), ((i % cols) * cw, (i // cols) * cw))
        sheet.save(preview)
        print(f"    preview -> {preview}")
    return emotion, out_files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help="a sheet PNG or a directory of sheets")
    ap.add_argument("--rig", required=True)
    ap.add_argument("--out", help="rig dir (default presets/avatars/<rig>)")
    ap.add_argument("--name", help="emotion name override (single-sheet only)")
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--rows", type=int, default=2)
    ap.add_argument("--local", action="store_true",
                    help="skip rembg; matte with the local neutral-grey key (offline; bleeds more)")
    ap.add_argument("--model", default="isnet-anime", help="rembg segmentation model")
    ap.add_argument("--alpha-matting", action="store_true", help="rembg finer hair/edge matting (slower)")
    ap.add_argument("--rembg-url", help="override the rembg provider URL (default: config.toml)")
    ap.add_argument("--upscale", action="store_true",
                    help="strict anime super-resolution (RealESRGAN via the image provider) BEFORE matting → "
                         "higher-res sprites (default canvas 512→1024). NO diffusion, so her design is unchanged.")
    ap.add_argument("--upscale-url", help="image provider URL (default: config.toml providers.image)")
    ap.add_argument("--upscale-model", default="RealESRGAN_x4plus_anime_6B.pth", help="ESRGAN model in ComfyUI upscale_models")
    ap.add_argument("--sat", type=float, default=16.0)
    ap.add_argument("--lo-span", type=float, default=72.0, help="band reaches this far BELOW the bg brightness (catches shadows)")
    ap.add_argument("--hi-span", type=float, default=8.0, help="band reaches this far above the bg brightness")
    ap.add_argument("--ramp", type=float, default=10.0)
    ap.add_argument("--canvas", type=int, default=512)
    ap.add_argument("--margin", type=int, default=12)
    ap.add_argument("--preview")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()

    rig_dir = a.out or os.path.join("presets/avatars", a.rig)
    if not a.dry:
        os.makedirs(rig_dir, exist_ok=True)
    sheets = ([a.input] if a.input.lower().endswith(".png")
              else sorted(glob.glob(os.path.join(a.input, "*.png"))))
    if not sheets:
        sys.exit(f"no sheets found at {a.input}")
    if a.name and len(sheets) > 1:
        sys.exit("--name only makes sense for a single sheet")
    mat = dict(sat=a.sat, lo_span=a.lo_span, hi_span=a.hi_span, ramp=a.ramp)
    rembg = None if a.local else dict(url=rembg_url(a.rembg_url), model=a.model,
                                      alpha_matting=a.alpha_matting)
    if rembg:
        print(f"matte: rembg {rembg['model']} @ {rembg['url']}"
              f"{' +alpha-matting' if rembg['alpha_matting'] else ''}")
    else:
        print("matte: LOCAL neutral-grey key (--local) — expect more background bleed")
    upscale = dict(url=image_url(a.upscale_url), model=a.upscale_model) if a.upscale else None
    canvas, margin = a.canvas, a.margin
    if upscale:
        if canvas == 512:
            canvas = 1024                          # default → 2x the rig res (safe: luckymas framings are face-relative)
        if margin == 12:
            margin = 24
        print(f"upscale: {upscale['model']} @ {upscale['url']}  → canvas {canvas}")
    done = []
    for sh in sheets:
        emo, files = process_sheet(sh, rig_dir, a.rig, a.name, a.cols, a.rows, mat,
                                   canvas, margin, a.dry, a.preview if len(sheets) == 1 else None,
                                   rembg=rembg, upscale=upscale)
        done.append(emo)
    print(f"\n{'(dry) ' if a.dry else ''}processed {len(done)} sheet(s) -> {rig_dir}  "
          f"[{', '.join(done)}]")


if __name__ == "__main__":
    main()
