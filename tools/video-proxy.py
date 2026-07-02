#!/usr/bin/env python3
"""video-proxy.py — turn a source video into an editor-loadable PROXY: a directory of
decimated JPEG frames + an index.json the editor samples at the playhead.

This is the "decode" half of the video-clip path. The editor (a Windows PE) has no libav
linked and can't run the Nix ffmpeg, so — in the spirit of the project's two layers —
decode runs ONCE here (host ffmpeg, the slow cached layer) producing content-addressed
frames, and the interactive loop only does texture loads (the instant compositing layer).
Scrubbing a video clip is then just "pick frame at local time → upload" — no per-frame
decode in the hot path, identical in preview + export. (A future in-process libav/NVDEC
decoder can replace the frame SOURCE without changing the editor's clip+time→SRV contract.)

  # extract the whole clip at 15fps, longest side ≤1280, into cache/video/<name>/
  python tools/video-proxy.py assets-src/luckymas-crt.mp4

  # a 12s segment, register it into a project's assets map under key `vid_crt`
  python tools/video-proxy.py assets-src/luckymas-crt.mp4 --ss 4 --t 12 \
      --into examples/video-broll.slop.json --key vid_crt

Frames are written to <cache>/video/<name>/f00001.jpg … + index.json. The proxy uri the
editor stores is `cache://video/<name>` (resolved against --cache, default `cache`)."""
import argparse, json, os, subprocess, sys, collections, shutil
OD = collections.OrderedDict


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def probe(src):
    """source width,height,duration via ffprobe (inside the dev shell)."""
    r = run(["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height", "-show_entries", "format=duration",
             "-of", "default=noprint_wrappers=1:nokey=0", src])
    if r.returncode != 0:
        sys.exit(f"ffprobe failed on {src}:\n{r.stderr}")
    w = h = 0
    dur = 0.0
    for line in r.stdout.splitlines():
        if line.startswith("width="):    w = int(line.split("=", 1)[1])
        elif line.startswith("height="): h = int(line.split("=", 1)[1])
        elif line.startswith("duration="):
            try: dur = float(line.split("=", 1)[1])
            except ValueError: dur = 0.0
    if not (w and h):
        sys.exit(f"ffprobe gave no video dims for {src}")
    return w, h, dur


def even(n):
    n = int(round(n))
    return n - (n & 1)            # JPEG is fine with odd dims, but keep ×2 for any later yuv path


def main():
    ap = argparse.ArgumentParser(description="extract a video proxy (JPEG frames + index) for the editor")
    ap.add_argument("source", help="source video (mp4/mov/webm/…)")
    ap.add_argument("--name", default=None, help="proxy dir name (default: source basename)")
    ap.add_argument("--cache", default="cache", help="cache root (default: cache)")
    ap.add_argument("--fps", type=float, default=15.0, help="proxy frame rate (default 15)")
    ap.add_argument("--max-dim", type=int, default=1280, help="clamp longest side (default 1280; no upscale)")
    ap.add_argument("--ss", type=float, default=0.0, help="source start offset seconds (default 0)")
    ap.add_argument("--t", type=float, default=None, help="duration seconds (default: to end)")
    ap.add_argument("--q", type=int, default=3, help="JPEG quality 2..31, lower=better (default 3)")
    ap.add_argument("--force", action="store_true", help="re-extract even if the proxy dir exists")
    ap.add_argument("--into", default=None, help="project .slop.json to register the asset into")
    ap.add_argument("--key", default=None, help="asset key to write (with --into; default: vid_<name>)")
    args = ap.parse_args()

    src = args.source
    if not os.path.isfile(src):
        sys.exit(f"no such file: {src}")
    name = args.name or os.path.splitext(os.path.basename(src))[0]
    name = "".join(c if (c.isalnum() or c in "-_") else "-" for c in name)
    outdir = os.path.join(args.cache, "video", name)

    sw, sh, sdur = probe(src)
    scale = min(1.0, args.max_dim / float(max(sw, sh)))
    tw, th = even(sw * scale), even(sh * scale)
    seg_dur = (sdur - args.ss) if args.t is None else args.t
    if seg_dur <= 0:
        sys.exit(f"empty segment (src dur {sdur:.2f}s, ss {args.ss}, t {args.t})")

    if os.path.isdir(outdir) and not args.force:
        existing = [f for f in os.listdir(outdir) if f.startswith("f") and f.endswith(".jpg")]
        if existing and os.path.isfile(os.path.join(outdir, "index.json")):
            print(f"proxy exists ({len(existing)} frames) at {outdir} — use --force to rebuild")
            if args.into:
                _register(args, name, outdir)
            return
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir, exist_ok=True)

    # fps decimate + scale → f00001.jpg … . -ss before -i = fast keyframe seek; we re-clock
    # the proxy from frame 0 so the editor's `in` is relative to the proxy start.
    vf = f"fps={args.fps},scale={tw}:{th}:flags=bicubic"
    cmd = ["ffmpeg", "-y", "-v", "error", "-ss", f"{args.ss}", "-i", src]
    if args.t is not None:
        cmd += ["-t", f"{args.t}"]
    cmd += ["-vf", vf, "-q:v", str(args.q), os.path.join(outdir, "f%05d.jpg")]
    print(f"extracting {name}: {tw}x{th} @ {args.fps}fps  (src {sw}x{sh}, {sdur:.1f}s, seg {seg_dur:.1f}s)")
    r = run(cmd)
    if r.returncode != 0:
        sys.exit(f"ffmpeg failed:\n{r.stderr}")

    frames = sorted(f for f in os.listdir(outdir) if f.startswith("f") and f.endswith(".jpg"))
    if not frames:
        sys.exit("ffmpeg produced no frames")
    index = OD([
        ("source", src), ("fps", args.fps), ("frames", len(frames)),
        ("w", tw), ("h", th), ("src_w", sw), ("src_h", sh),
        ("dur", round(len(frames) / args.fps, 3)), ("ss", args.ss),
        ("pattern", "f%05d.jpg"),
    ])
    json.dump(index, open(os.path.join(outdir, "index.json"), "w"), indent=2)
    print(f"  → {len(frames)} frames, {len(frames)/args.fps:.1f}s proxy at {outdir}")

    if args.into:
        _register(args, name, outdir)


def _register(args, name, outdir):
    """patch the project's assets map with a type:'video' entry pointing at the proxy."""
    idx = json.load(open(os.path.join(outdir, "index.json")), object_pairs_hook=OD)
    proj_path = args.into
    p = json.load(open(proj_path), object_pairs_hook=OD)
    key = args.key or f"vid_{name}".replace("-", "_")
    p.setdefault("assets", OD())
    p["assets"][key] = OD([
        ("provider", "external"), ("type", "video"), ("status", "ready"),
        ("uri", idx["source"]),
        ("proxy", f"cache://video/{name}"),
        ("fps", idx["fps"]), ("frames", idx["frames"]),
        ("w", idx["w"]), ("h", idx["h"]),
        ("src_w", idx["src_w"]), ("src_h", idx["src_h"]),
        ("dur", idx["dur"]), ("pattern", idx["pattern"]),
        ("meta", OD()),
    ])
    json.dump(p, open(proj_path, "w"), indent=2, ensure_ascii=False)
    print(f"  registered asset '{key}' → {proj_path}  (point a video clip's `asset` at it)")


if __name__ == "__main__":
    main()
