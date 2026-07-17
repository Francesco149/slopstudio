#!/usr/bin/env python3
"""video.py — the human front-door for building a @GemmaExplains video end to end.

A friendly wrapper over the real tools (slop.py / export.sh / the editor) so you never
have to remember the command sequence or type long project paths. Everything runs inside
the dev shell:  nix develop --command python tools/video.py <cmd>

  video.py doctor                    is my machine ready? (dev shell · lame · feed · builds)
  video.py wake                      wake + unlock the GPU box (lame) — needed for VO/gen
  video.py new  <name> [--portrait]  scaffold a new video (skeleton + research + packaging stubs)
  video.py status [<name>]           where is this video in the pipeline? (no name → list all)
  video.py build <name>              (re)compile the skeleton → .slop.json
  video.py voice <name>              generate VO + visemes + retime            (needs lame)
  video.py lint  <name>              gaps / repeats / stale VO / missing assets / overlaps
  video.py scene-check <name>        validate every Lua scene animation headlessly
  video.py animations                list reusable scene widgets and authoring syntax
  video.py look  <name> [--n 6]      render frames across the cut → montage → llm-feed
  video.py export <name> [--final]   render to mp4 (--final = 1080p60)
  video.py show  <name>              compact timeline (slop.py overview)
  video.py dashboard [--port N]      open the web control panel (social queue + launcher)

<name> = a folder under ../slopstudio-projects (e.g. `luckymas`), or a direct path to a
.slop.json / .skeleton.json. Full playbook + what to decide at each step: docs/VIDEO_RUNBOOK.md
"""
import argparse, glob, json, os, re, shutil, socket, subprocess, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECTS = os.path.normpath(os.path.join(ROOT, "..", "slopstudio-projects"))
LAME_HOST = "10.0.10.56"
FEED_HEALTH = "http://localhost:8777/healthz"
FEED_CLI = "/opt/src/llm-feed/feed.py"


def col(s, code):
    return f"\033[{code}m{s}\033[0m" if sys.stdout.isatty() else s
def ok(s):   return col(s, "32")
def bad(s):  return col(s, "31")
def warn(s): return col(s, "33")
def dim(s):  return col(s, "90")


def run(cmd, **kw):
    print(dim("$ " + " ".join(cmd)))
    return subprocess.run(cmd, **kw)


def slop(*a, **kw):
    return run([sys.executable, "tools/slop.py", *a], cwd=ROOT, **kw)


def rel(p):
    return os.path.relpath(p, ROOT)


# ── environment probes ───────────────────────────────────────────────────────
def tcp_ok(host, port, t=1.5):
    try:
        with socket.create_connection((host, port), timeout=t):
            return True
    except OSError:
        return False


def http_ok(url, t=1.5):
    try:
        with urllib.request.urlopen(url, timeout=t) as r:
            return r.status == 200
    except Exception:
        return False


# ── project resolution ───────────────────────────────────────────────────────
def list_projects():
    return sorted(os.path.basename(p.rstrip("/")) for p in glob.glob(os.path.join(PROJECTS, "*/")))


def proj_dir(name):
    if os.path.isdir(name):
        return os.path.abspath(name)
    if name.endswith((".slop.json", ".skeleton.json")) and os.path.exists(name):
        return os.path.dirname(os.path.abspath(name))
    d = os.path.join(PROJECTS, name)
    if os.path.isdir(d):
        return d
    sys.exit(bad(f"no project '{name}' in {PROJECTS}") + f"\n  projects: {', '.join(list_projects()) or '(none)'}")


def find_one(name, ext):
    """Resolve <name> to a single *<ext> file (direct path, or the one cut in the folder)."""
    if name.endswith(ext) and os.path.exists(name):
        return os.path.abspath(name)
    d = proj_dir(name)
    base = os.path.basename(d)
    cands = [x for x in sorted(glob.glob(os.path.join(d, "*" + ext))) if os.sep + "history" + os.sep not in x]
    if not cands:
        sys.exit(bad(f"no {ext} in {base}/") + (f"  → `video.py new {base}`" if ext == ".skeleton.json" else f"  → `video.py build {base}`"))
    exact = os.path.join(d, base + ext)
    if exact in cands:
        return exact
    if len(cands) == 1:
        return cands[0]
    print(warn(f"{base}/ has several {ext} — pass the exact one:"))
    for x in cands:
        print("   " + rel(x))
    sys.exit(1)


# ── commands ─────────────────────────────────────────────────────────────────
def cmd_doctor(a):
    print(col("environment check", "1"))
    rows = [
        ("dev shell (ffmpeg)",     bool(shutil.which("ffmpeg")),                          "run everything inside:  nix develop"),
        ("dev shell (glslang)",    bool(shutil.which("glslangValidator")),                "run inside:  nix develop"),
        ("config.toml",            os.path.exists(os.path.join(ROOT, "config.toml")),     "cp config.example.toml config.toml  (then edit)"),
        ("editor built",           os.path.exists(os.path.join(ROOT, "build", "slopstudio.exe")), "nix develop --command make -C editor"),
        ("thumbtool built",        os.path.exists(os.path.join(ROOT, "build", "slopthumb.exe")),  "nix develop --command make -C thumbtool"),
        (f"lame GPU box ({LAME_HOST})", tcp_ok(LAME_HOST, 22),                            "video.py wake      (needed for `voice` / gen)"),
        ("llm-feed (:8777)",       http_ok(FEED_HEALTH),                                  "python /opt/src/llm-feed/feed.py   (needed for `look`)"),
    ]
    for label, good, hint in rows:
        print(f"  {ok('✓') if good else bad('✗')} {label}" + ("" if good else "   " + warn("→ " + hint)))
    print(dim("\n  (✗ on lame/feed is fine until you need `voice`/`look`.)"))


def cmd_wake(a):
    print("waking + unlocking lame via code (LUKS unlock can take ~1 min)…")
    r = run(["ssh", "root@code", "cold-unlock --host lame --stay"])
    print(ok("lame is up.") if r.returncode == 0 else bad("wake failed — check `ssh root@code` works."))


def cmd_new(a):
    d = os.path.join(PROJECTS, a.name)
    if os.path.exists(d):
        sys.exit(bad(f"{rel(d)} already exists"))
    os.makedirs(os.path.join(d, "docs"))
    os.makedirs(os.path.join(d, "assets"), exist_ok=True)
    skel = {
        "title": f"{a.name} — working title",
        "format": "portrait" if a.portrait else "landscape",
        "fps": 60, "voice": "gemma-san-deep-clone", "rig": "gemma-big",
        "bg": "presets/backgrounds/room-day.png", "bg_dark": "presets/backgrounds/room-dark.png",
        "music": "library/music/space-jazz.mp3",
        "beats": [
            {"line": "Cold-open hook — the single most 'wait, what?' sentence.", "emotion": "smug",
             "visual": {"image": "assets/HERO.png", "layout": "fit"}},
            {"line": "One line of setup: what is this thing.", "emotion": "explaining", "visual": {"host": True}},
            {"line": "The reveal / payoff that the hook promised.", "emotion": "pointing",
             "visual": {"code": "// the beat that carries it"}},
        ],
    }
    with open(os.path.join(d, f"{a.name}.skeleton.json"), "w", encoding="utf-8") as f:
        json.dump(skel, f, indent=2, ensure_ascii=False); f.write("\n")
    with open(os.path.join(d, "docs", f"research-{a.name}.md"), "w", encoding="utf-8") as f:
        f.write(f"# {a.name} — research\n\nEvery on-screen claim must trace to a source you actually read.\n\n"
                "| claim | source | verified |\n|---|---|---|\n|  |  |  |\n")
    with open(os.path.join(d, "docs", f"{a.name}-packaging.md"), "w", encoding="utf-8") as f:
        f.write(f"# {a.name} — packaging\n\n**Angle:** \n\n## Titles (8–12 across ≥4 formulas — /packaging-first)\n1. \n\n## Description\n```\n\n```\n")
    print(ok(f"created {rel(d)}/") + "  (skeleton + research + packaging stubs)")
    print("  next:")
    print(f"    1. research  → {rel(d)}/docs/research-{a.name}.md   (claim→evidence first)")
    print(f"    2. beats     → {rel(d)}/{a.name}.skeleton.json      (one sentence per beat)")
    print(f"    3. compile   → video.py build {a.name}   then  voice · lint · look · export")


def _duration(cut):
    r = slop("overview", rel(cut), capture_output=True, text=True)
    m = re.search(r"total\s+([\d.]+)s", r.stdout or "")
    return float(m.group(1)) if m else 0.0


def cmd_build(a):
    sk = find_one(a.name, ".skeleton.json")
    out = sk.replace(".skeleton.json", ".slop.json")
    if slop("skeleton", rel(sk), "--out", rel(out)).returncode == 0:
        print(ok(f"built {rel(out)}") + f"  → next: video.py voice {os.path.basename(proj_dir(a.name))}")


def cmd_voice(a):
    cut = find_one(a.name, ".slop.json")
    if not tcp_ok(LAME_HOST, 22):
        print(bad(f"lame ({LAME_HOST}) is unreachable.") + warn("  VO generation runs on the GPU box — `video.py wake` first."))
        if not a.force:
            sys.exit(1)
    slop("genvo", rel(cut))


def cmd_lint(a):
    cut=rel(find_one(a.name, ".slop.json"))
    r1=slop("lint", cut)
    r2=slop("critique", cut)
    if r1.returncode or r2.returncode: sys.exit(1)


def cmd_scene_check(a):
    slop("scene-check", rel(find_one(a.name, ".slop.json")), check=True)


def cmd_animations(a):
    slop("scene-widgets")
    print(dim("\nRecipes and data fields: docs/SCENE_COOKBOOK.md"))


def cmd_show(a):
    slop("overview", rel(find_one(a.name, ".slop.json")))


def cmd_look(a):
    cut = find_one(a.name, ".slop.json")
    exe = os.path.join(ROOT, "build", "slopstudio.exe")
    if not os.path.exists(exe):
        sys.exit(bad("editor not built:") + " nix develop --command make -C editor")
    dur = _duration(cut)
    if dur <= 0:
        sys.exit(bad("couldn't read the cut duration (is it compiled? `video.py build`)"))
    n = max(2, a.n)
    times = [round(dur * (i + 1) / (n + 1), 2) for i in range(n)]   # interior, evenly spaced
    scratch = os.environ.get("TMPDIR", "/tmp")
    shots = []
    for i, t in enumerate(times):
        p = os.path.join(scratch, f"look_{i:02d}.png")
        r = run([exe, rel(cut), "--shot-frame", p, "--time", str(t), "--cache", "cache"], cwd=ROOT,
                capture_output=True, text=True)
        if os.path.exists(p):
            shots.append((t, p))
        else:
            print(warn(f"  frame @ {t}s failed: {r.stderr.strip()[:120]}"))
    if not shots:
        sys.exit(bad("no frames rendered."))
    montage = os.path.join(scratch, "look_montage.png")
    _montage([p for _, p in shots], montage, cols=2)
    print(ok(f"rendered {len(shots)} frames @ {', '.join(str(t) for t, _ in shots)}s"))
    if os.path.exists(FEED_CLI):
        run([sys.executable, FEED_CLI, "image", montage, "--title", f"look: {os.path.basename(cut)}"])
        print(dim("  → pushed montage to the llm-feed"))
    else:
        print(f"  montage: {montage}")


def _montage(paths, out, cols=2):
    from PIL import Image
    ims = [Image.open(p).convert("RGB") for p in paths]
    tw = 640
    ims = [im.resize((tw, round(tw * im.height / im.width))) for im in ims]
    rows = (len(ims) + cols - 1) // cols
    cw, ch = tw, max(im.height for im in ims)
    canvas = Image.new("RGB", (cols * cw + (cols + 1) * 8, rows * ch + (rows + 1) * 8), (18, 18, 22))
    for i, im in enumerate(ims):
        r, c = divmod(i, cols)
        canvas.paste(im, (8 + c * (cw + 8), 8 + r * (ch + 8)))
    canvas.save(out)


def cmd_export(a):
    cut = find_one(a.name, ".slop.json")
    args = ["bash", "tools/export.sh", rel(cut), "--cache", "cache"]
    if a.final:
        args += ["--fps", "60"]
        # PORTRAIT shorts render at their NATIVE 1080x1920 — `--scale 1080` scales HEIGHT to 1080,
        # which squashes a portrait cut to 608x1080 (the render-modal fix, ported to the CLI).
        try:
            res = json.load(open(cut)).get("meta", {}).get("resolution", [1920, 1080])
        except Exception:
            res = [1920, 1080]
        if res[1] <= res[0]:                 # landscape only → downscale to 1080p
            args += ["--scale", "1080"]
    run(args, cwd=ROOT)


def _to_clipboard(text, srcfile=None):
    """WSL → Windows clipboard, Unicode-safe (the transcript has JP like らき☆マス). Prefer
    PowerShell reading the UTF-8 sidecar (survives non-ASCII); fall back to clip.exe / linux tools."""
    if srcfile and shutil.which("powershell.exe") and shutil.which("wslpath"):
        win = subprocess.run(["wslpath", "-w", srcfile], capture_output=True, text=True).stdout.strip()
        if win and subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                                   f"Set-Clipboard -Value (Get-Content -Raw -Encoding UTF8 -LiteralPath '{win}')"]).returncode == 0:
            return "powershell Set-Clipboard"
    for tool in (["clip.exe"], ["wl-copy"], ["xclip", "-selection", "clipboard"], ["pbcopy"]):
        if shutil.which(tool[0]):
            subprocess.run(tool, input=text.encode("utf-8"))
            return tool[0]
    return None


def cmd_transcript(a):
    cut = find_one(a.name, ".slop.json")
    fmt = "srt" if a.srt else "txt"
    r = slop("captions", rel(cut), "--fmt", fmt, "--sidecars", capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(bad((r.stderr or "captions failed").strip()))
    text, base = r.stdout, re.sub(r"\.slop\.json$", "", cut)
    tool = _to_clipboard(text, base + (".srt" if a.srt else ".captions.txt"))
    n = len([l for l in text.splitlines() if l.strip()]) if fmt == "txt" else text.count("-->")
    print((ok(f"transcript ({n} lines, {fmt}) → clipboard") + dim(f"  [{tool}]")) if tool
          else warn("no clipboard tool found — use the files below"))
    print(dim(f"  files: {rel(base)}.{{captions.txt,srt,vtt}}"))
    print("  YouTube Studio → Subtitles → Add language → " +
          ("Upload file → pick the .srt (exact timing)" if a.srt else "Paste transcript + auto-sync (clipboard is ready)"))


def cmd_dashboard(a):
    args = [sys.executable, "tools/dashboard.py", "--port", str(a.port)]
    if not a.no_open:
        args.append("--open")
    print(ok(f"dashboard → http://localhost:{a.port}") + dim("  (Ctrl-C to stop)"))
    run(args, cwd=ROOT)


def cmd_status(a):
    if not a.name:
        print(col("projects", "1"))
        for p in list_projects():
            cuts = len([x for x in glob.glob(os.path.join(PROJECTS, p, "*.slop.json"))])
            print(f"  {p}" + dim(f"   ({cuts} cut{'s' if cuts != 1 else ''})"))
        print(dim("\n  video.py status <name>   for pipeline detail"))
        return
    d = proj_dir(a.name)
    base = os.path.basename(d)
    print(col(f"{base}", "1"))
    def mark(label, good, extra=""):
        print(f"  {ok('✓') if good else warn('○')} {label}" + (dim("  " + extra) if extra else ""))
    sk = glob.glob(os.path.join(d, "*.skeleton.json"))
    cuts = [x for x in glob.glob(os.path.join(d, "*.slop.json")) if os.sep + "history" + os.sep not in x]
    thumbs = glob.glob(os.path.join(d, "thumbs", "*.thumb.json"))
    pkg = glob.glob(os.path.join(d, "docs", "*packaging*.md"))
    exports = glob.glob(os.path.join(ROOT, "exports", base + "*.mp4"))
    mark("skeleton", bool(sk), os.path.basename(sk[0]) if sk else "video.py new / author one")
    mark("compiled cut", bool(cuts), ", ".join(os.path.basename(c) for c in cuts) if cuts else "video.py build")
    mark("thumbnails", bool(thumbs), f"{len(thumbs)} candidate(s)" if thumbs else "tools/thumb.py")
    mark("packaging (title+desc)", bool(pkg), os.path.basename(pkg[0]) if pkg else "/packaging-first")
    mark("exported mp4", bool(exports), ", ".join(os.path.basename(e) for e in exports) if exports else "video.py export --final")
    if cuts:
        print(dim("\n  lint:"))
        slop("lint", rel(cuts[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("doctor")
    sub.add_parser("wake")
    s = sub.add_parser("new"); s.add_argument("name"); s.add_argument("--portrait", action="store_true")
    s = sub.add_parser("status"); s.add_argument("name", nargs="?")
    s = sub.add_parser("build"); s.add_argument("name")
    s = sub.add_parser("voice"); s.add_argument("name"); s.add_argument("--force", action="store_true")
    s = sub.add_parser("lint"); s.add_argument("name")
    s = sub.add_parser("scene-check"); s.add_argument("name")
    sub.add_parser("animations")
    s = sub.add_parser("look"); s.add_argument("name"); s.add_argument("--n", type=int, default=6)
    s = sub.add_parser("export"); s.add_argument("name"); s.add_argument("--final", action="store_true")
    s = sub.add_parser("transcript"); s.add_argument("name"); s.add_argument("--srt", action="store_true", help="copy SRT (timed) instead of plain text")
    s = sub.add_parser("show"); s.add_argument("name")
    s = sub.add_parser("dashboard"); s.add_argument("--port", type=int, default=8080)
    s.add_argument("--no-open", action="store_true", help="don't auto-open the browser")
    a = ap.parse_args()
    {"doctor": cmd_doctor, "wake": cmd_wake, "new": cmd_new, "status": cmd_status, "build": cmd_build,
     "voice": cmd_voice, "lint": cmd_lint, "scene-check": cmd_scene_check, "animations": cmd_animations,
     "look": cmd_look, "export": cmd_export, "transcript": cmd_transcript,
     "show": cmd_show, "dashboard": cmd_dashboard}[a.cmd](a)


if __name__ == "__main__":
    main()
