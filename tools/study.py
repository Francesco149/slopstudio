#!/usr/bin/env python
"""study.py — reference-video study pipeline (digest a YouTube video for LLM analysis).

Turns a video URL into a study directory an LLM can actually read:
  meta.json        pruned metadata (title/channel/views/chapters/description)
  video.mp4        <=480p download (gitignored — local analysis only)
  transcript.txt   timestamped transcript from YT subs (auto or manual)
  analysis.json    scene cuts, shot lengths, per-minute pacing (cuts/visual-events/words)
  sheets/*.jpg     timestamped contact sheets (1 frame / 5 s) — what's ON SCREEN over time
  DIGEST.md        the single doc to read: meta + structure + pacing table + transcript

Usage (from repo root, inside `nix develop`):
  python tools/study.py run  <url-or-id> [...]      # fetch + analyze + digest each
  python tools/study.py fetch <url-or-id>
  python tools/study.py analyze <id>
  python tools/study.py digest  <id>
  python tools/study.py list
Options:
  --dir DIR    study root (default ../slopstudio-projects/research/video-study)
  --no-feed    don't push contact sheets to the llm-feed

The point of the metrics: good explainers keep a visual going at all times.
`visual events/min` (soft scene changes) + `longest static span` expose dead air;
`cuts/min` + shot-length stats expose edit cadence; per-minute words expose
narration pacing. Read DIGEST.md + the sheets, then write STUDY.md by hand.
"""

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys

ROOT = os.environ.get("SLOPSTUDIO_ROOT") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DIR = os.path.normpath(os.path.join(ROOT, "..", "slopstudio-projects", "research", "video-study"))
FEED_CLI = "/opt/src/llm-feed/feed.py"

HARD_CUT = 0.35   # scene score ≥ this = a hard cut
SOFT_CUT = 0.08   # scene score ≥ this = "something visibly changed" (visual event)
SHEET_EVERY = 5   # seconds per contact-sheet frame
SHEET_COLS, SHEET_ROWS = 8, 6


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def fmt_ts(sec):
    sec = int(sec)
    return f"{sec // 60}:{sec % 60:02d}"


def vid_id(url_or_id):
    m = re.search(r"(?:v=|youtu\.be/|shorts/)([\w-]{11})", url_or_id)
    return m.group(1) if m else url_or_id


def find_study(root, ident):
    ident = vid_id(ident)
    for d in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        if d.startswith(ident) or d.split("-", 1)[0] == ident:
            return os.path.join(root, d)
    p = os.path.join(root, ident)
    return p if os.path.isdir(p) else None


def feed_push(img, title):
    if not os.path.exists(FEED_CLI):
        return
    try:
        subprocess.run([sys.executable, FEED_CLI, "image", os.path.abspath(img), "--title", title],
                       check=True, capture_output=True, timeout=30)
    except Exception as e:
        print(f"  (feed push failed: {e})", file=sys.stderr)


# ── fetch ────────────────────────────────────────────────────────────────────

def slugify(s, n=40):
    s = re.sub(r"[^\w]+", "-", s.lower()).strip("-")
    return s[:n].rstrip("-")


def cmd_fetch(url, root):
    vid = vid_id(url)
    url = f"https://www.youtube.com/watch?v={vid}"
    os.makedirs(root, exist_ok=True)
    gi = os.path.join(root, ".gitignore")
    if not os.path.exists(gi):
        with open(gi, "w") as f:
            f.write("# heavy downloads — analysis artifacts (transcript/analysis/DIGEST/sheets) are committed\n"
                    "*/video.*\n*/subs.*\n*/*.info.json\n")

    info = json.loads(subprocess.run(
        ["yt-dlp", "--skip-download", "--no-warnings", "-J", url],
        check=True, capture_output=True, text=True).stdout)
    d = os.path.join(root, f"{vid}-{slugify(info['title'])}")
    os.makedirs(d, exist_ok=True)

    meta = {k: info.get(k) for k in (
        "id", "title", "channel", "channel_follower_count", "duration", "view_count",
        "like_count", "comment_count", "upload_date", "chapters", "tags", "categories")}
    meta["description"] = (info.get("description") or "")[:4000]
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)

    print(f"fetch: {info['title']} ({fmt_ts(info['duration'])}) → {os.path.relpath(d, ROOT)}")
    if not os.path.exists(os.path.join(d, "video.mp4")):
        sh(["yt-dlp", "--no-warnings", "-q", "--no-progress",
            "-f", "bv*[height<=480][ext=mp4]+ba[ext=m4a]/b[height<=480]/bv*[height<=480]+ba/b",
            "--merge-output-format", "mp4",
            "-o", os.path.join(d, "video.%(ext)s"), url])
    if not os.path.exists(os.path.join(d, "subs.json3")):
        # separate + non-fatal: 'en.*' would pull dozens of auto-TRANSLATED variants
        # and 429; en-orig = the untranslated auto track, en = manual if present
        subprocess.run(["yt-dlp", "--no-warnings", "-q", "--no-progress", "--skip-download",
                        "--write-subs", "--write-auto-subs", "--sub-langs", "en,en-orig",
                        "--sub-format", "json3", "-o", os.path.join(d, "video.%(ext)s"), url])
        for f_ in os.listdir(d):  # normalize whichever en variant landed
            if f_.endswith(".json3"):
                shutil.move(os.path.join(d, f_), os.path.join(d, "subs.json3"))
    write_transcript(d)
    return d


# ── transcript ───────────────────────────────────────────────────────────────

def load_words(d):
    """[(t_sec, word)] from YT json3 subs (word-timed for auto subs)."""
    p = os.path.join(d, "subs.json3")
    if not os.path.exists(p):
        return []
    with open(p) as f:
        j = json.load(f)
    words = []
    for ev in j.get("events", []):
        t0 = ev.get("tStartMs", 0)
        for seg in ev.get("segs") or []:
            txt = (seg.get("utf8") or "").strip()
            if not txt:
                continue
            t = (t0 + seg.get("tOffsetMs", 0)) / 1000.0
            # manual subs pack whole lines into one seg — tokenize so word
            # counts/WPM stay honest (auto subs are already one word per seg)
            for w in txt.split():
                words.append((t, w))
    return words


def write_transcript(d):
    words = load_words(d)
    if not words:
        print("  (no subs found — transcript skipped)", file=sys.stderr)
        return
    lines, cur, cur_t0, last_t = [], [], words[0][0], words[0][0]
    for t, w in words:
        if cur and (t - last_t > 2.5 or len(cur) >= 45):
            lines.append(f"[{fmt_ts(cur_t0)}] {' '.join(cur)}")
            cur, cur_t0 = [], t
        cur.append(w)
        last_t = t
    if cur:
        lines.append(f"[{fmt_ts(cur_t0)}] {' '.join(cur)}")
    with open(os.path.join(d, "transcript.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  transcript: {len(words)} words, {len(lines)} lines")


# ── analyze ──────────────────────────────────────────────────────────────────

def scene_scores(d):
    """[(t, score)] for every frame whose scene score ≥ SOFT_CUT (one ffmpeg pass)."""
    vf = f"scale=320:-2,select='gte(scene\\,{SOFT_CUT})',metadata=print"
    r = subprocess.run(["ffmpeg", "-nostats", "-i", os.path.join(d, "video.mp4"),
                        "-vf", vf, "-f", "null", "-"],
                       check=True, capture_output=True, text=True)
    out, t = [], None
    for line in r.stderr.splitlines():
        m = re.search(r"pts_time:([\d.]+)", line)
        if m:
            t = float(m.group(1))
        m = re.search(r"lavfi\.scene_score=([\d.]+)", line)
        if m and t is not None:
            out.append((t, float(m.group(1))))
    return out


def cmd_analyze(ident, root, feed=True):
    d = find_study(root, ident)
    assert d, f"no study dir for {ident} — run fetch first"
    meta = json.load(open(os.path.join(d, "meta.json")))
    dur = meta["duration"]
    print(f"analyze: {meta['title']}")

    ev = scene_scores(d)
    cuts = [t for t, s in ev if s >= HARD_CUT]
    shots = [b - a for a, b in zip([0.0] + cuts, cuts + [dur])]
    # longest span with NO visual change at all (dead air on screen)
    vt = [0.0] + [t for t, _ in ev] + [dur]
    static = max((b - a, a) for a, b in zip(vt, vt[1:]))

    words = load_words(d)
    nmin = math.ceil(dur / 60)
    per_min = []
    for i in range(nmin):
        a, b = i * 60, (i + 1) * 60
        per_min.append({
            "min": i,
            "cuts": sum(1 for t in cuts if a <= t < b),
            "visual_events": sum(1 for t, _ in ev if a <= t < b),
            "words": sum(1 for t, _ in words if a <= t < b),
        })

    sshots = sorted(shots)
    analysis = {
        "duration": dur,
        "hard_cut_threshold": HARD_CUT, "soft_threshold": SOFT_CUT,
        "cuts": len(cuts), "cuts_per_min": round(len(cuts) / dur * 60, 2),
        "visual_events": len(ev), "visual_events_per_min": round(len(ev) / dur * 60, 2),
        "shot_median_s": round(sshots[len(sshots) // 2], 2) if sshots else None,
        "shot_p90_s": round(sshots[int(len(sshots) * 0.9)], 2) if sshots else None,
        "longest_shot_s": round(max(shots), 2) if shots else None,
        "longest_shot_at": fmt_ts(([0.0] + cuts)[shots.index(max(shots))]) if shots else None,
        "longest_static_span_s": round(static[0], 2), "longest_static_at": fmt_ts(static[1]),
        "words": len(words), "wpm": round(len(words) / dur * 60, 1),
        "first30s": {"cuts": sum(1 for t in cuts if t < 30),
                     "visual_events": sum(1 for t, _ in ev if t < 30),
                     "words": sum(1 for t, _ in words if t < 30)},
        "cut_times": [round(t, 2) for t in cuts],
        "per_minute": per_min,
    }
    with open(os.path.join(d, "analysis.json"), "w") as f:
        json.dump(analysis, f, indent=2)
    print(f"  {len(cuts)} cuts ({analysis['cuts_per_min']}/min), "
          f"{analysis['visual_events_per_min']} visual events/min, "
          f"median shot {analysis['shot_median_s']}s, wpm {analysis['wpm']}, "
          f"longest static {analysis['longest_static_span_s']}s @ {analysis['longest_static_at']}")

    sheets = os.path.join(d, "sheets")
    if not os.path.isdir(sheets) or not os.listdir(sheets):
        os.makedirs(sheets, exist_ok=True)
        per_sheet = SHEET_COLS * SHEET_ROWS
        vf = (f"fps=1/{SHEET_EVERY},scale=288:-2,"
              "drawtext=text='%{pts\\:hms}':x=4:y=h-18:fontsize=14:fontcolor=white:box=1:boxcolor=black@0.6,"
              f"tile={SHEET_COLS}x{SHEET_ROWS}")
        sh(["ffmpeg", "-nostats", "-loglevel", "error", "-i", os.path.join(d, "video.mp4"),
            "-vf", vf, "-vsync", "vfr", "-q:v", "5", os.path.join(sheets, "sheet%02d.jpg")])
        n = len(os.listdir(sheets))
        print(f"  {n} contact sheets ({SHEET_EVERY}s/frame, {per_sheet}/sheet)")
        if feed:
            for s in sorted(os.listdir(sheets)):
                feed_push(os.path.join(sheets, s), f"study {meta['title'][:50]} — {s}")
    return d


# ── digest ───────────────────────────────────────────────────────────────────

def cmd_digest(ident, root):
    d = find_study(root, ident)
    assert d, f"no study dir for {ident}"
    meta = json.load(open(os.path.join(d, "meta.json")))
    an = json.load(open(os.path.join(d, "analysis.json")))
    tr = open(os.path.join(d, "transcript.txt")).read() if os.path.exists(os.path.join(d, "transcript.txt")) else "(none)"

    L = [f"# DIGEST — {meta['title']}", "",
         f"**{meta['channel']}** ({meta.get('channel_follower_count') or '?'} subs) · "
         f"{fmt_ts(meta['duration'])} · {meta.get('view_count'):,} views · "
         f"{meta.get('like_count') or '?'} likes · uploaded {meta.get('upload_date')}", "",
         f"https://www.youtube.com/watch?v={meta['id']}", "",
         "## Pacing / edit metrics", "",
         f"- **{an['cuts']} hard cuts** = {an['cuts_per_min']}/min; median shot {an['shot_median_s']}s, "
         f"p90 {an['shot_p90_s']}s, longest {an['longest_shot_s']}s @ {an['longest_shot_at']}",
         f"- **{an['visual_events_per_min']} visual events/min** (any on-screen change ≥{an['soft_threshold']}); "
         f"longest fully-static span **{an['longest_static_span_s']}s** @ {an['longest_static_at']}",
         f"- **{an['wpm']} wpm** ({an['words']} words)",
         f"- first 30s: {an['first30s']['cuts']} cuts, {an['first30s']['visual_events']} visual events, "
         f"{an['first30s']['words']} words", ""]

    if meta.get("chapters"):
        L += ["## Chapters", ""]
        L += [f"- [{fmt_ts(c['start_time'])}] {c['title']}" for c in meta["chapters"]]
        L += [""]

    L += ["## Per-minute pacing (cuts / visual events / words)", "",
          "| min | cuts | vis | words | | min | cuts | vis | words |",
          "|---|---|---|---|---|---|---|---|---|"]
    pm = an["per_minute"]
    half = math.ceil(len(pm) / 2)
    for i in range(half):
        row = f"| {pm[i]['min']} | {pm[i]['cuts']} | {pm[i]['visual_events']} | {pm[i]['words']} |"
        j = i + half
        row += (f" | {pm[j]['min']} | {pm[j]['cuts']} | {pm[j]['visual_events']} | {pm[j]['words']} |"
                if j < len(pm) else " | | | | |")
        L.append(row)

    desc = (meta.get("description") or "").strip()
    if desc:
        L += ["", "## Description (as packaged)", "", "```", desc[:1500], "```"]

    sheets_dir = os.path.join(d, "sheets")
    sheet_lines = ([f"- sheets/{s}" for s in sorted(os.listdir(sheets_dir))]
                   if os.path.isdir(sheets_dir) else ["(none)"])
    L += ["", f"## Contact sheets ({SHEET_EVERY}s/frame)", "",
          *sheet_lines, "", "## Transcript", "", tr]

    with open(os.path.join(d, "DIGEST.md"), "w") as f:
        f.write("\n".join(L) + "\n")
    print(f"digest → {os.path.relpath(os.path.join(d, 'DIGEST.md'), ROOT)}")
    return d


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["run", "fetch", "analyze", "digest", "list"])
    ap.add_argument("targets", nargs="*", help="video URLs or ids")
    ap.add_argument("--dir", default=DEFAULT_DIR)
    ap.add_argument("--no-feed", action="store_true")
    a = ap.parse_args()

    if a.cmd == "list":
        for d in sorted(os.listdir(a.dir)) if os.path.isdir(a.dir) else []:
            if os.path.isdir(os.path.join(a.dir, d)):
                done = "DIGEST" if os.path.exists(os.path.join(a.dir, d, "DIGEST.md")) else "…"
                print(f"  {d}  [{done}]")
        return
    if not a.targets:
        ap.error("need at least one url/id")
    for t in a.targets:
        if a.cmd in ("run", "fetch"):
            cmd_fetch(t, a.dir)
        if a.cmd in ("run", "analyze"):
            cmd_analyze(t, a.dir, feed=not a.no_feed)
        if a.cmd in ("run", "digest"):
            cmd_digest(t, a.dir)


if __name__ == "__main__":
    main()
