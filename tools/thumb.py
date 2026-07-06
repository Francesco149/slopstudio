#!/usr/bin/env python3
"""thumb.py — the agent-facing CLI for slopthumb (.thumb.json thumbnail docs).

The LLM authoring loop: edit the doc here (or write the JSON directly), a GUI
instance watching the file hot-reloads it, `render` produces the exact pixels
(it shells out to build/slopthumb.exe so CLI and GUI are byte-identical) and
pushes them to the llm-feed. `lint` runs the research-backed hard gates
(≤N words, no title-word repeat, bottom-right duration corner clear) before a
human ever sees a candidate.

  python tools/thumb.py new t.thumb.json [--brand DIR] [--portrait] [--template NAME] [--blank]
      # auto-finds the gemma brand (walks up for gemma-branding/brand-package) and defaults to
      # its marks layout (a2/b2/thirst) — just fill the text + 2 image src's; --blank = bare bg
  python tools/thumb.py overview t.thumb.json
  python tools/thumb.py set t.thumb.json host x=940 scale=1.1 src="/mnt/f/.../sprite.png"
  python tools/thumb.py add t.thumb.json text id=h1 text="BIG HOOK" style=headline x=360 y=200
  python tools/thumb.py rm t.thumb.json h1     |  order t.thumb.json h1 --to 3
  python tools/thumb.py render t.thumb.json [--no-feed] [--proof]
  python tools/thumb.py lint t.thumb.json --title "the paired video title"
  python tools/thumb.py snapshot t.thumb.json      # save into history/ (A/B bank)
  python tools/thumb.py variants t.thumb.json      # list sibling docs + history

Values in set/add are parsed as JSON when possible ("12"→12, "true"→True,
'{"px":30}'→object), else kept as strings. Keys with dots descend ("shadow.dx=4").
"""
import argparse, json, os, re, shutil, subprocess, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "slopthumb.exe")
FEED = "http://localhost:8777"


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save(path, doc):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")


def jval(s):
    try:
        return json.loads(s)
    except Exception:
        return s.replace("\\n", "\n")   # shell-friendly newlines in text values


def find_layer(doc, ident):
    for i, l in enumerate(doc.get("layers", [])):
        if l.get("id") == ident:
            return i, l
    try:
        i = int(ident)
        return i, doc["layers"][i]
    except (ValueError, IndexError):
        pass
    sys.exit(f"no layer '{ident}' (have: {[l.get('id') for l in doc.get('layers', [])]})")


def set_kv(obj, key, val):
    parts = key.split(".")
    for p in parts[:-1]:
        obj = obj.setdefault(p, {})
    if val is None or val == "":
        obj.pop(parts[-1], None)
    else:
        obj[parts[-1]] = val


FEED_CLI = "/opt/src/llm-feed/feed.py"


def feed_push(png, title):
    if not os.path.exists(FEED_CLI):
        return False
    try:  # fire-and-forget, never block the authoring loop
        subprocess.run([sys.executable, FEED_CLI, "image", os.path.abspath(png), "--title", title],
                       capture_output=True, timeout=15)
        return True
    except Exception as e:
        print(f"  (feed push failed: {e})", file=sys.stderr)
        return False


def run_exe(args):
    r = subprocess.run([EXE] + args, cwd=ROOT, capture_output=True, text=True)
    if r.stdout:
        print(r.stdout.strip())
    if r.returncode != 0:
        sys.exit(r.stderr.strip() or f"slopthumb exited {r.returncode}")
    return r


# ───────────────────────────── commands ─────────────────────────────────────

def find_brand(start):
    """Walk up from `start` for a gemma brand package (gemma-branding/brand-package, or a
    sibling brand-package) so `new` auto-loads branding without a --brand flag."""
    d = os.path.abspath(start)
    for _ in range(8):
        for cand in (os.path.join(d, "gemma-branding", "brand-package"), os.path.join(d, "brand-package")):
            if os.path.exists(os.path.join(cand, "brand.json")):
                return cand
        nd = os.path.dirname(d)
        if nd == d:
            break
        d = nd
    return ""


def cmd_new(a):
    if os.path.exists(a.doc) and not a.force:
        sys.exit(f"{a.doc} exists (use --force)")
    docdir = os.path.dirname(os.path.abspath(a.doc))
    brand = a.brand or find_brand(docdir)                       # auto-load the gemma brand if present
    brand_rel = os.path.relpath(brand, docdir).replace(os.sep, "/") if brand else ""
    doc = {"format": "thumb-1",
           "canvas": [1080, 1920] if a.portrait else [1280, 720],
           "brand": brand_rel,
           "title": a.title or "",
           "layers": [{"id": "bg", "type": "bg", "fill": "$bg", "vignette": 0.4}]}
    # template: explicit --template wins; else the brand's default layout (the a2/b2/thirst
    # marks layout) unless --blank. So a new branded thumb is "fill text, pick 2 images, place marks".
    bj = json.load(open(os.path.join(brand, "brand.json"), encoding="utf-8")) if brand else {}
    tpl_name = a.template or ("" if a.blank else bj.get("default_template_portrait" if a.portrait else "default_template", ""))
    if tpl_name:
        tpl = next((t for t in bj.get("templates", []) if t["name"] == tpl_name), None)
        if not tpl:
            sys.exit(f"no template '{tpl_name}' (have: {[t['name'] for t in bj.get('templates', [])]})")
        doc["layers"] = json.loads(json.dumps(tpl["layers"]))   # deep copy (don't alias the brand file)
    save(a.doc, doc)
    print(f"created {a.doc} ({doc['canvas'][0]}x{doc['canvas'][1]}, brand={doc['brand'] or '(none)'}, template={tpl_name or '(blank)'})")


def cmd_overview(a):
    doc = load(a.doc)
    cw, ch = doc.get("canvas", [1280, 720])
    print(f"{a.doc}  {cw}x{ch}  brand={doc.get('brand', '')}  title={doc.get('title', '')!r}")
    for i, l in enumerate(doc.get("layers", [])):
        t = l.get("type", "?")
        bits = [f"[{i}] {l.get('id', '?'):<12} {t:<9}"]
        if l.get("hidden"):
            bits.append("HIDDEN")
        if t == "text":
            bits.append(f'"{l.get("text", "")}" style={l.get("style", "-")} @({l.get("x", "?")},{l.get("y", "?")})')
        elif t == "image":
            bits.append(f'{os.path.basename(str(l.get("src", "")))} @({l.get("x", "?")},{l.get("y", "?")}) x{l.get("scale", 1)}')
        elif t == "shape":
            bits.append(l.get("shape", "?"))
        elif t == "bg":
            bits.append(f'fill={l.get("fill", "")} image={os.path.basename(str(l.get("image", "") or ""))}')
        print("  " + " ".join(bits))


def cmd_set(a):
    doc = load(a.doc)
    _, layer = find_layer(doc, a.layer)
    for kv in a.kv:
        k, _, v = kv.partition("=")
        set_kv(layer, k, jval(v))
    save(a.doc, doc)
    print(f"set {a.layer}: {' '.join(a.kv)}")


def cmd_docset(a):
    doc = load(a.doc)
    for kv in a.kv:
        k, _, v = kv.partition("=")
        set_kv(doc, k, jval(v))
    save(a.doc, doc)
    print(f"doc set: {' '.join(a.kv)}")


def cmd_add(a):
    doc = load(a.doc)
    layer = {"type": a.type}
    for kv in a.kv:
        k, _, v = kv.partition("=")
        set_kv(layer, k, jval(v))
    layer.setdefault("id", f"{a.type}{len(doc.get('layers', []))}")
    doc.setdefault("layers", []).append(layer)
    save(a.doc, doc)
    print(f"added {layer['id']} ({a.type})")


def cmd_rm(a):
    doc = load(a.doc)
    i, l = find_layer(doc, a.layer)
    doc["layers"].pop(i)
    save(a.doc, doc)
    print(f"removed {l.get('id')}")


def cmd_order(a):
    doc = load(a.doc)
    i, l = find_layer(doc, a.layer)
    doc["layers"].pop(i)
    doc["layers"].insert(max(0, min(len(doc["layers"]), a.to)), l)
    save(a.doc, doc)
    print(f"{l.get('id')} → index {a.to}")


def out_paths(docpath):
    base = re.sub(r"\.thumb\.json$", "", docpath)
    return base + ".png", base + "-proof.png", base + ".info.json"


# ── duration-badge simulation (how the thumb reads at small feed sizes) ─────────
# YouTube stamps a ~fixed-pixel duration pill bottom-right; at small display sizes
# (channel list, watch sidebar) it eats a big fraction and can cover the subject's
# face. This renders the thumb at real feed widths with the pill drawn so collisions
# are visible, and `lint` warns when the subject fills that corner.
# real desktop display widths (measured off youtube.com, 2026-07): the duration pill is a
# ~fixed 38x20px on-screen, so its FRACTION shrinks as the thumb grows — rendering the thumb
# too small makes the pill look oversized. home/search ~400px, channel grid ~340px (both give
# a ~9-10% pill), watch sidebar is the genuinely small one.
FEED_WIDTHS = [(400, "home / search"), (340, "channel page"), (210, "watch sidebar")]
PILL_PX = 20          # measured pill height (px); width ≈ text + pad, margin 8px, radius 4, bg rgba(0,0,0,.6)
PROGRESS_PX = 4       # red "resume" progress bar (shown on partially-watched videos)


def _font(size):
    import glob
    from PIL import ImageFont
    for p in glob.glob("/nix/store/*dejavu*/**/DejaVuSans.ttf", recursive=True) + \
             [os.path.join(ROOT, "assets-src", "fonts", "ArchivoBlack-Regular.ttf")]:
        try:
            return ImageFont.truetype(p, size)
        except Exception:
            pass
    return ImageFont.load_default()


def _draw_pill(img, dur, ph=PILL_PX):
    """Draw YouTube's duration pill bottom-right at its measured size (ph≈20px on-screen,
    12px text, 4px pad, 8px margin, radius 4, bg rgba(0,0,0,0.6))."""
    from PIL import ImageDraw
    W, H = img.size
    d = ImageDraw.Draw(img, "RGBA")
    font = _font(round(ph * 0.6))                     # ~12px at ph=20
    tb = d.textbbox((0, 0), dur, font=font); tw = tb[2] - tb[0]
    pw = tw + 8                                        # ~4px padding each side
    m = 8                                             # 8px from the thumb edges
    x1, y1 = W - m, H - m; x0, y0 = x1 - pw, y1 - ph
    d.rounded_rectangle([x0, y0, x1, y1], radius=4, fill=(0, 0, 0, 153))
    d.text((x0 + (pw - tw) // 2 - tb[0], y0 + (ph - (tb[3] - tb[1])) // 2 - tb[1]),
           dur, font=font, fill=(255, 255, 255, 235))


def _draw_progress(img, frac=0.45, ph=PROGRESS_PX):
    """Draw YouTube's red resume-progress bar along the very bottom (gray track + red fill)."""
    from PIL import ImageDraw
    W, H = img.size
    d = ImageDraw.Draw(img, "RGBA")
    d.rectangle([0, H - ph, W, H], fill=(255, 255, 255, 70))          # track
    d.rectangle([0, H - ph, int(W * frac), H], fill=(255, 0, 0, 235))  # watched


def duration_montage(png_path, out_path, dur="12:00"):
    """Thumb at each real feed width with the pill drawn; small cells zoomed to a common
    height so the pill's true proportion (and any face collision) is obvious."""
    from PIL import Image, ImageDraw
    base = Image.open(png_path).convert("RGBA")
    cellH, pad, lbl = 300, 14, 22
    cells = []
    for w, name in FEED_WIDTHS:
        h = round(w * base.height / base.width)
        t = base.resize((w, h), Image.LANCZOS)
        _draw_pill(t, dur)          # 20px absolute → its real fraction at this display size
        _draw_progress(t)           # red resume bar
        t = t.resize((round(w * cellH / h), cellH), Image.NEAREST)   # zoom for visibility, proportion kept
        cells.append((t, f"{name} ~{w}px · pill {round(100*PILL_PX/h)}% tall"))
    canvas = Image.new("RGBA", (sum(c[0].width for c in cells) + pad * (len(cells) + 1),
                                cellH + pad * 2 + lbl), (18, 18, 22, 255))
    d = ImageDraw.Draw(canvas); f = _font(14); x = pad
    for img, label in cells:
        canvas.paste(img, (x, pad + lbl), img)
        d.text((x + 2, pad), label, font=f, fill=(205, 205, 215, 255))
        x += img.width + pad
    canvas.convert("RGB").save(out_path)
    return out_path


def cmd_render(a):
    png, proof, info = out_paths(a.doc)
    args = [os.path.relpath(a.doc, ROOT), "--export", os.path.relpath(png, ROOT), "--info", os.path.relpath(info, ROOT)]
    if a.proof:
        args += ["--proof", os.path.relpath(proof, ROOT)]
    run_exe(args)
    sim = None
    if a.badge:
        sim = re.sub(r"\.thumb\.json$", "-badge.png", a.doc)
        duration_montage(png, sim, a.dur)
        print(f"badge sim → {os.path.relpath(sim, ROOT)}  (pill at {', '.join(str(w) for w, _ in FEED_WIDTHS)}px feed sizes)")
    if not a.no_feed:
        doc = load(a.doc)
        feed_push(png, f"thumb: {os.path.basename(a.doc)} — {doc.get('title', '')}")
        if a.proof:
            feed_push(proof, f"thumb 168px proof: {os.path.basename(a.doc)}")
        if sim:
            feed_push(sim, f"thumb duration-pill sim (feed sizes): {os.path.basename(a.doc)}")


STOPWORDS = {"the", "a", "an", "of", "to", "in", "and", "or", "is", "it", "that", "this", "for", "on", "with", "how", "why", "what"}


def cmd_lint(a):
    doc = load(a.doc)
    title = a.title or doc.get("title", "")
    png, proof, info = out_paths(a.doc)
    run_exe([os.path.relpath(a.doc, ROOT), "--info", os.path.relpath(info, ROOT)])
    inf = load(info)
    cw, ch = inf["canvas"]
    problems, warns = [], []

    # brand lint config
    brand_dir = doc.get("brand", "")
    lint_cfg = {}
    if brand_dir:
        bp = os.path.join(os.path.dirname(os.path.abspath(a.doc)), brand_dir, "brand.json")
        if os.path.exists(bp):
            lint_cfg = json.load(open(bp, encoding="utf-8")).get("lint", {})
    lint_cfg.update(doc.get("lint", {}))   # doc-level override for deliberate exceptions
    max_words = int(lint_cfg.get("max_words", 3))

    title_words = {w.lower().strip(".,!?:;'\"") for w in title.split()} - STOPWORDS
    text_layers = [l for l in inf["layers"] if l["type"] == "text" and not l.get("hidden")]
    all_words = 0
    for l in text_layers:
        # punctuation-only tokens ("?!", "→") are marks, not words — don't count them
        words = [w for w in re.split(r"[\s\n]+", l.get("text", "")) if re.search(r"[A-Za-z0-9]", w)]
        all_words += len(words)
        overlap = {w.lower().strip(".,!?") for w in words} & title_words
        if overlap:
            problems.append(f"text '{l['id']}' repeats title words: {sorted(overlap)} (thumb+title must form a curiosity gap, not an echo)")
    if all_words > max_words:
        problems.append(f"{all_words} words on the thumbnail (max {max_words}) — cut until it reads in <1s")

    if lint_cfg.get("keep_clear_br", True):
        brx, bry = cw - 0.20 * cw, ch - 0.16 * ch   # YouTube duration stamp zone
        for l in inf["layers"]:
            if l.get("hidden") or l["type"] in ("bg",) or "bbox" not in l:
                continue
            x0, y0, x1, y1 = l["bbox"]
            if l["type"] == "text" and x1 > brx and y1 > bry:
                problems.append(f"text '{l['id']}' enters the bottom-right duration-stamp zone")
            elif l["type"] == "shape" and x1 > brx and y1 > bry and (x1 - x0) * (y1 - y0) < 0.25 * cw * ch:
                # big rects are panel backdrops, not content — only warn on real marks
                warns.append(f"shape '{l['id']}' touches the duration-stamp corner")

    imgs = [l for l in inf["layers"] if l["type"] == "image" and not l.get("hidden") and "bbox" in l]
    if imgs:
        biggest = max(imgs, key=lambda l: (l["bbox"][2] - l["bbox"][0]) * (l["bbox"][3] - l["bbox"][1]))
        area = (biggest["bbox"][2] - biggest["bbox"][0]) * (biggest["bbox"][3] - biggest["bbox"][1]) / (cw * ch)
        if area < 0.12:
            warns.append(f"largest subject '{biggest['id']}' covers only {area:.0%} of frame — likely too small at feed size")
        # subject in the small-size duration-pill footprint (covered on channel list / watch sidebar)
        zw, zh = lint_cfg.get("br_subject_zone", [0.28, 0.24])   # worst-case pill footprint @ ~168px
        zx, zy, za = cw * (1 - zw), ch * (1 - zh), (cw * zw) * (ch * zh)
        for l in imgs:
            x0, y0, x1, y1 = l["bbox"]
            cover = max(0, min(x1, cw) - max(x0, zx)) * max(0, min(y1, ch) - max(y0, zy))
            if cover > 0.45 * za:
                warns.append(f"subject '{l['id']}' fills the bottom-right duration-pill zone — it's covered "
                             f"at channel-list/sidebar sizes; pull it out of the bottom-right ~{int(zw*100)}%×{int(zh*100)}% "
                             f"(run: thumb.py render {os.path.basename(a.doc)} --badge  to see)")
    else:
        warns.append("no image layer — face/subject is the #1 CTR lever")

    if not title:
        warns.append("no paired title on the doc (docset title=\"...\") — title/thumb synergy can't be linted")

    for p in problems:
        print(f"FAIL  {p}")
    for w in warns:
        print(f"warn  {w}")
    if not problems and not warns:
        print("lint clean")
    sys.exit(1 if problems else 0)


def cmd_snapshot(a):
    doc = load(a.doc)
    ddir = os.path.dirname(os.path.abspath(a.doc))
    hdir = os.path.join(ddir, "history")
    os.makedirs(hdir, exist_ok=True)
    stem = re.sub(r"\.thumb\.json$", "", os.path.basename(a.doc))
    n = 1
    while os.path.exists(os.path.join(hdir, f"{stem}-{n:03d}.thumb.json")):
        n += 1
    base = os.path.join(hdir, f"{stem}-{n:03d}")
    save(base + ".thumb.json", doc)
    run_exe([os.path.relpath(a.doc, ROOT), "--export", os.path.relpath(base + ".png", ROOT)])
    print(f"snapshot {os.path.basename(base)} (.thumb.json + .png)")


def cmd_variants(a):
    ddir = os.path.dirname(os.path.abspath(a.doc)) or "."
    print("docs (A/B variants):")
    for f in sorted(os.listdir(ddir)):
        if f.endswith(".thumb.json"):
            mark = "→" if os.path.abspath(os.path.join(ddir, f)) == os.path.abspath(a.doc) else " "
            print(f"  {mark} {f}")
    hdir = os.path.join(ddir, "history")
    if os.path.isdir(hdir):
        snaps = sorted(f for f in os.listdir(hdir) if f.endswith(".thumb.json"))
        print(f"history: {len(snaps)} snapshots" + (f" (latest {snaps[-1]})" if snaps else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("new"); s.add_argument("doc"); s.add_argument("--brand", default="")
    s.add_argument("--portrait", action="store_true"); s.add_argument("--template", default="")
    s.add_argument("--blank", action="store_true", help="bare bg only (skip the brand's default template)")
    s.add_argument("--title", default=""); s.add_argument("--force", action="store_true")
    s = sub.add_parser("overview"); s.add_argument("doc")
    s = sub.add_parser("set"); s.add_argument("doc"); s.add_argument("layer"); s.add_argument("kv", nargs="+")
    s = sub.add_parser("docset"); s.add_argument("doc"); s.add_argument("kv", nargs="+")
    s = sub.add_parser("add"); s.add_argument("doc"); s.add_argument("type", choices=["text", "image", "shape", "bg", "watermark", "mosaic"]); s.add_argument("kv", nargs="*")
    s = sub.add_parser("rm"); s.add_argument("doc"); s.add_argument("layer")
    s = sub.add_parser("order"); s.add_argument("doc"); s.add_argument("layer"); s.add_argument("--to", type=int, required=True)
    s = sub.add_parser("render"); s.add_argument("doc"); s.add_argument("--proof", action="store_true"); s.add_argument("--no-feed", action="store_true")
    s.add_argument("--badge", action="store_true", help="also emit a duration-pill sim across feed sizes")
    s.add_argument("--dur", default="12:00", help="duration text for the badge sim (e.g. 14:55)")
    s = sub.add_parser("lint"); s.add_argument("doc"); s.add_argument("--title", default="")
    s = sub.add_parser("snapshot"); s.add_argument("doc")
    s = sub.add_parser("variants"); s.add_argument("doc")

    a = ap.parse_args()
    {"new": cmd_new, "overview": cmd_overview, "set": cmd_set, "docset": cmd_docset, "add": cmd_add,
     "rm": cmd_rm, "order": cmd_order, "render": cmd_render, "lint": cmd_lint,
     "snapshot": cmd_snapshot, "variants": cmd_variants}[a.cmd](a)


if __name__ == "__main__":
    main()
