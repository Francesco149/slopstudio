#!/usr/bin/env python3
"""choreograph.py — drive the XP guest through a scripted choreography and capture it.

Composes the proven primitives — QMP capture/input (xpvm) + the agent-less SMB launch
path (xpsmb) — into:
  • smooth, frame-captured desktop choreography (mouse paths w/ easing, clicks, typing,
    launches, timed holds) → a PNG sequence → an mp4 → optionally a slopstudio `video`
    asset the editor decodes in-process;
  • the BitBlt-vs-screenshot DEMO: launch a per-pixel-alpha LAYERED window (mascot.exe)
    and capture the SAME moment two ways — host QMP `screendump` (sees it, the ground
    truth) vs in-guest GDI `bitblt.exe` (misses it). The honest comparison for video-001.

Capture is single-threaded over the one QMP socket: frames are taken between micro-steps
and during holds, then assembled at the chosen fps (smooth enough for desktop motion).

Run inside `nix develop .#xp` (qemu + pillow); ffmpeg is fetched on demand.
  python tools/xp/choreograph.py demo --sock cache/xp/qmp.sock --port 4445 --out cache/xp/demo
  python tools/xp/choreograph.py run script.json --sock … --port … --out … [--mp4 clip.mp4]
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xpvm   # noqa: E402
import xpsmb  # noqa: E402


def _smoothstep(t):
    return t * t * (3 - 2 * t)


class Choreographer:
    def __init__(self, qmp, smb, outdir, fps=30, win_dir="tools/xp/win", stepped=True):
        self.q = qmp
        self.smb = smb
        self.outdir = os.path.abspath(outdir)
        self.fps = fps
        self.stepped = stepped     # freeze the guest for each dump → even guest-time spacing (no starvation)
        self.win_dir = win_dir
        os.makedirs(self.outdir, exist_ok=True)
        self.frames = []
        self.n = 0
        w, h = self.q.fb_size()
        self.W, self.H = w, h
        self.cur = (w // 2, h // 2)   # assumed pointer start

    # ── capture ───────────────────────────────────────────────────────────
    def frame(self):
        """Capture one frame. stepped → freeze the vCPU for the dump (deterministic);
        realtime → dump the running guest (jittery)."""
        p = os.path.join(self.outdir, f"f{self.n:05d}.png")
        if self.stepped:
            self.q.pause(); self.q.screendump(p); self.q.resume()
        else:
            self.q.screendump(p)
        self.frames.append(p)
        self.n += 1
        return p

    def _advance(self):
        """One captured frame separated by exactly 1/fps of GUEST time. stepped: run a
        1/fps wall-slice, then freeze+dump (the dump is outside guest time, so spacing is
        even regardless of dump cost). realtime: dump, then sleep the remainder."""
        if self.stepped:
            time.sleep(1.0 / self.fps)
            self.frame()
        else:
            t0 = time.time()
            self.frame()
            time.sleep(max(0.0, 1.0 / self.fps - (time.time() - t0)))

    def hold(self, seconds):
        """Hold for `seconds` of GUEST time, capturing at fps (keeps motion alive on screen)."""
        for _ in range(max(1, round(seconds * self.fps))):
            self._advance()

    # ── input (each captures frames) ──────────────────────────────────────
    def move_to(self, x, y, steps=14):
        x0, y0 = self.cur
        for i in range(1, steps + 1):
            t = _smoothstep(i / steps)
            self.q.move(round(x0 + (x - x0) * t), round(y0 + (y - y0) * t))
            self._advance()          # queue the move, run a slice (guest processes+renders it), capture
        self.cur = (x, y)

    def click(self, x=None, y=None, double=False):
        if x is not None:
            self.move_to(x, y)
        self.q.click(double=double)
        self._advance(); self.hold(0.25)

    def type(self, text):
        for ch in text:
            ks = xpvm.char_to_keys(ch)
            if ks:
                self.q.send_keys(ks)
                time.sleep(0.04)
        self.frame()

    def key(self, *qcodes):
        self.q.send_keys(list(qcodes)); self.frame()

    def launch(self, cmdline, wait=0):
        """Launch a GUI/console program on the interactive desktop via iexec."""
        self.smb.iexec(cmdline, wait=wait)

    # ── outputs ───────────────────────────────────────────────────────────
    def to_mp4(self, out_mp4, fps=None):
        return frames_to_mp4(self.frames, out_mp4, fps or self.fps)


def frames_to_mp4(frames, out_mp4, fps=12):
    """Encode a PNG sequence → mp4 (libx264, yuv420p) via ffmpeg (fetched on demand)."""
    if not frames:
        raise RuntimeError("no frames captured")
    out_mp4 = os.path.abspath(out_mp4)
    listfile = out_mp4 + ".frames.txt"
    with open(listfile, "w") as f:
        for p in frames:
            f.write(f"file '{os.path.abspath(p)}'\nduration {1.0/fps:.4f}\n")
        f.write(f"file '{os.path.abspath(frames[-1])}'\n")   # last frame twice (concat quirk)
    subprocess.run(
        ["nix", "run", "nixpkgs#ffmpeg", "--", "-y", "-f", "concat", "-safe", "0",
         "-i", listfile, "-vsync", "vfr", "-pix_fmt", "yuv420p", "-c:v", "libx264",
         "-crf", "18", out_mp4],
        check=True, capture_output=True)
    os.remove(listfile)
    print(f"choreograph: wrote {out_mp4} ({len(frames)} frames @ {fps}fps)")
    return out_mp4


def register_video_asset(project, key, mp4_path):
    """Patch a .slop.json assets map with a type:'video' entry pointing at the mp4
    (the editor decodes it in-process — see tools/video-proxy.py _register)."""
    import collections
    mp4_path = os.path.abspath(mp4_path)
    with open(project) as f:
        proj = json.load(f, object_pairs_hook=collections.OrderedDict)
    assets = proj.setdefault("assets", collections.OrderedDict())
    assets[key] = collections.OrderedDict([
        ("provider", "external"), ("type", "video"), ("status", "ready"),
        ("uri", "file://" + mp4_path),
    ])
    with open(project, "w") as f:
        json.dump(proj, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"choreograph: registered video asset '{key}' → {project}")


SPRITE_MASCOT = "C:\\probe\\sprites\\mascot.raw"
SPRITE_HOOK   = "C:\\probe\\sprites\\hook.raw"
# sprite blob sizes MUST match the launch sizes below — mascot.c/copyanim.c reject a
# size-mismatched .raw and fall back to a procedural diamond.  bake_sprites() guarantees it.
MASCOT_WH = (300, 410)      # the floating desktop mascot (sized for 1024x768)
HOOK_WH   = (96, 132)       # the copy-dialog walker (fits the SysAnimate32 band)
SPRITE_SRC = "library/images/gemma-smug.png"


def bake_sprites(src=SPRITE_SRC, outdir="cache/xp/sprites"):
    """Bake the host PNG → premultiplied-BGRA blobs at the EXACT sizes the demos launch."""
    p2r = os.path.join(os.path.dirname(os.path.abspath(__file__)), "png2raw.py")
    for name, (w, h), pad in (("mascot.raw", MASCOT_WH, 8), ("hook.raw", HOOK_WH, 2)):
        subprocess.run([sys.executable, p2r, src, os.path.join(outdir, name),
                        "--w", str(w), "--h", str(h), "--anchor", "bottom", "--pad", str(pad)],
                       check=True, capture_output=True)
    print("demo: baked sprites from %s (mascot %s, hook %s)" % (src, MASCOT_WH, HOOK_WH))


def wake_guest(qmp):
    """Wiggle the mouse to dismiss any XP screensaver / wake the blanked desktop before a capture.
    The VM blanks after idle, which would otherwise leave the floating mascot on a black 'Windows XP'
    logo screen instead of the Bliss desktop."""
    try:
        w, h = qmp.fb_size()
    except Exception:
        w, h = 1024, 768
    for x in (w // 2 - 60, w // 2 + 60, w // 2):
        qmp.move(x, h // 2)
        time.sleep(0.12)
    time.sleep(1.0)   # let the desktop repaint


def kill_demo_procs(smb):
    """Clear any floating mascot / running copyanim so a capture isn't polluted.
    ONE smbexec round-trip (taskkill takes multiple /im) — calling iexec per-image was
    slow + a hang risk (each smbexec creates+starts a service)."""
    smb.iexec("taskkill /f /im mascot.exe /im copyanim.exe /im copysim.exe /im bitblt.exe",
              wait=3)


def stage_demo_tools(smb, win_dir="tools/xp/win", sprite_dir="cache/xp/sprites",
                     res=(1024, 768), sprite_src=SPRITE_SRC):
    """Bake sprites at the demo sizes, push every guest tool + the blobs, set a crisp
    resolution.  smbclient for dirs/files (netexec atexec 'exec' needs :445 which the
    user-mode forward doesn't expose; smbexec/iexec + smbclient work on :4445)."""
    if sprite_src and os.path.exists(sprite_src):
        os.makedirs(sprite_dir, exist_ok=True)
        bake_sprites(sprite_src, sprite_dir)
    smb._smb('mkdir "\\probe"; mkdir "\\probe\\out"; mkdir "\\probe\\sprites"')
    exes = ["iexec.exe", "winrect.exe", "bitblt.exe", "mascot.exe",
            "copyanim.exe", "setres.exe"]
    have = [e for e in exes if os.path.exists(os.path.join(win_dir, e))]
    smb._smb('prompt OFF; lcd "%s"; cd "\\probe"; %s'
             % (os.path.abspath(win_dir), " ".join("put %s;" % e for e in have)))
    if os.path.isdir(sprite_dir):
        raws = [f for f in os.listdir(sprite_dir) if f.endswith(".raw")]
        if raws:
            smb._smb('prompt OFF; lcd "%s"; cd "\\probe\\sprites"; %s'
                     % (os.path.abspath(sprite_dir), " ".join("put %s;" % r for r in raws)))
    if res:
        smb.iexec(r"C:\probe\setres.exe %d %d 16" % (res[0], res[1]), wait=6)
        time.sleep(4)   # let the mode switch settle to the Bliss desktop
    kill_demo_procs(smb)
    print("demo: staged %d tools + sprites; set %dx%d" % (len(have), res[0] if res else 0, res[1] if res else 0))


def _capture_window(qmp, outdir, prefix, seconds, fps, stepped=True):
    """Capture `seconds` of GUEST time at `fps` → frame paths.

    stepped (default, recommended): per frame, run the guest exactly 1/fps of wall-time,
    then FREEZE it (QMP stop) for the dump and thaw. The dump happens outside guest time,
    so it can't starve the guest and the frames are evenly spaced in guest-time — the clip
    plays at true speed and high fps is smooth. Wall-time is longer (dump cost is paid out
    of band). realtime (stepped=False): the old free-running grab — fast but jittery; a
    rapid loop steals vCPU time and the guest clock advances unevenly (the documented
    starvation). Kept as a fallback."""
    frames, dt = [], 1.0 / fps
    nframes = max(1, round(seconds * fps))
    for n in range(nframes):
        p = os.path.join(outdir, "%s%04d.png" % (prefix, n))
        if stepped:
            time.sleep(dt)                              # guest runs exactly one frame of guest-time
            qmp.pause(); qmp.screendump(p); qmp.resume()
        else:
            t0 = time.time()
            qmp.screendump(p)
            time.sleep(max(0.0, dt - (time.time() - t0)))
        frames.append(p)
    return frames


def _dialog_region(im):
    """Compare only the UPPER screen, where the copy dialog appears — dropping the bottom
    taskbar/tray strip. XP pops tray balloons there ('Your computer might be at risk', display
    nags…) that are big enough to trip a whole-frame motion check; cropping them out stops a
    balloon from being mistaken for the dialog (the false positive that fired capture on nothing)."""
    w, h = im.size
    return im.crop((0, 0, w, int(h * 0.76)))


def _wait_for_motion(qmp, baseline_png, timeout=60.0, thresh=30000, interval=0.25):
    """Gently poll screendumps until the frame differs from the clean baseline by >thresh px²
    (the dialog is actually up), or timeout. Returns True if motion appeared. Robust to copy
    speed — the un-starved plain copy (no hijack loop pacing it) is much faster than the hijack
    copy, so a FIXED prewait can't catch both: too long and the fast plain copy finishes first;
    too short and the stepped capture freezes the guest mid-launch (the SMB-started copyanim never
    gets going). Polling is gentle (interval ~0.25s) so it barely perturbs the guest, and the tight
    stepped capture takes over the instant the dialog is up."""
    from PIL import Image, ImageChops
    base = _dialog_region(Image.open(baseline_png).convert("RGB"))
    probe = baseline_png + ".probe.png"
    end = time.time() + timeout
    while time.time() < end:
        qmp.pause(); qmp.screendump(probe); qmp.resume()   # FROZEN dump → the wait itself doesn't starve the guest
        bb = ImageChops.difference(_dialog_region(Image.open(probe).convert("RGB")), base).getbbox()
        if bb and (bb[2] - bb[0]) * (bb[3] - bb[1]) > thresh:
            return True
        time.sleep(interval)                                # guest runs this slice at full speed
    return False


def _frames_with_motion(frames, ref=None, thresh=2500):
    """Subset of frames that differ (by a bbox-area threshold) from a reference image —
    i.e. the dialog/animation is up.  `ref` is a clean-baseline PNG path; without it the
    first frame is the baseline (wrong if the subject is already up in frame 0)."""
    from PIL import Image, ImageChops
    refimg = _dialog_region(Image.open(ref if ref else frames[0]).convert("RGB"))
    keep = []
    for f in frames:
        d = ImageChops.difference(_dialog_region(Image.open(f).convert("RGB")), refimg)
        bb = d.getbbox()
        if bb and (bb[2] - bb[0]) * (bb[3] - bb[1]) > thresh:
            keep.append(f)
    return keep


def copy_demo(qmp, smb, outdir, sprite=SPRITE_HOOK, hook_wh=HOOK_WH, count=12000, kb=2,
              prewait=0.5, burst=10.0, fps=30, stepped=True, win_dir="tools/xp/win"):
    """Capture XP's file-copy dialog two ways via copyanim.exe (same-process copy+hijack):
       plain  → the stock flying-papers dialog ("normal");
       hijack → our mascot walking across the bar (the MinkIt copy-animation trick).
    Returns {mode: (mp4, still)}.

    STEPPED capture (default) freezes the guest for each dump, so the copy dialog runs at
    its natural ~8s guest-time (no starvation, no stretch) and is sampled EVENLY at `fps`.
    That retires the hard-won workaround this used to need (long PRE-WAIT + ~3fps gentle
    grab over a 34s window + a motion-subset filter to find where the slowed dialog landed).
    Now: a short pre-wait for the dialog to appear, a tight stepped burst at full fps, and a
    light motion-trim only to crop the clip to the frames where the dialog is actually up."""
    import threading, shutil
    os.makedirs(outdir, exist_ok=True)
    kill_demo_procs(smb)
    wake_guest(qmp)   # desktop up, not the idle screensaver
    out = {}
    for mode in ("plain", "hijack"):
        sub = os.path.join(outdir, "copy-" + mode); os.makedirs(sub, exist_ok=True)
        flag = " --plain" if mode == "plain" else ""
        spr = "" if mode == "plain" else (" %s %d %d" % (sprite, hook_wh[0], hook_wh[1]))
        cmd = r"C:\probe\copyanim.exe %d %d%s%s" % (count, kb, spr, flag)
        print("demo: copy %s — %s" % (mode, cmd))
        base = os.path.join(sub, "_baseline.png"); qmp.screendump(base)   # clean desktop ref
        threading.Thread(target=lambda c=cmd: smb.iexec(c, timeout=180), daemon=True).start()
        time.sleep(prewait)                      # brief settle: let the SMB-launched copyanim get going
        if not _wait_for_motion(qmp, base):      # poll until the dialog is actually up, THEN stepped-capture
            print("demo: copy %s — WARNING: dialog never appeared within the wait window" % mode)
        frames = _capture_window(qmp, sub, "f", burst, fps, stepped=stepped)
        smb.iexec("taskkill /f /im copyanim.exe", wait=2)
        up = _frames_with_motion(frames, ref=base, thresh=30000)   # dialog ~64k px ≫ balloon ~18k
        print("demo: copy %s — %d/%d frames have the dialog up" % (mode, len(up), len(frames)))
        seq = up if len(up) >= 3 else frames
        mp4 = os.path.join(outdir, "copy-%s.mp4" % mode); frames_to_mp4(seq, mp4, fps)
        still = os.path.join(outdir, "copy-%s.png" % mode); shutil.copyfile(seq[len(seq) // 2], still)
        out[mode] = (mp4, still)
    return out


def mascot_clip(qmp, smb, outdir, sprite=SPRITE_MASCOT, x=360, y=190, wh=MASCOT_WH,
                seconds=6, fps=30, stepped=True):
    """Launch the self-animating layered mascot and capture a short MOVING clip of it
    floating over the desktop (host QMP screendump → it's fully visible). Returns the mp4."""
    os.makedirs(outdir, exist_ok=True)
    kill_demo_procs(smb); time.sleep(1)
    wake_guest(qmp)   # dismiss the screensaver so the mascot floats on the desktop, not a black logo screen
    smb.iexec(r"C:\probe\mascot.exe %d %d %d %d %s" % (x, y, wh[0], wh[1], sprite)); time.sleep(2)
    frames = _capture_window(qmp, outdir, "m", seconds, fps, stepped=stepped)
    smb.iexec("taskkill /f /im mascot.exe", wait=1)   # don't let it bleed into later demos
    mp4 = os.path.join(os.path.dirname(outdir.rstrip("/")), "mascot-move.mp4")
    frames_to_mp4(frames, mp4, fps)
    return mp4


def halo_demo(qmp, smb, outdir, x=360, y=190, wh=MASCOT_WH, sprite_src=SPRITE_SRC):
    """The premultiplied-alpha BUG demo: the same mascot launched twice — once from a
    STRAIGHT-alpha blob (UpdateLayeredWindow expects premultiplied → bright halo/fringe
    on every soft edge) and once premultiplied (clean). Returns (halo_png, clean_png,
    comparison_png) — tight 2x crops around the mascot so the fringe reads on screen."""
    from PIL import Image, ImageDraw
    os.makedirs(outdir, exist_ok=True)
    p2r = os.path.join(os.path.dirname(os.path.abspath(__file__)), "png2raw.py")
    halo_raw = os.path.join("cache/xp/sprites", "halo.raw")
    subprocess.run([sys.executable, p2r, sprite_src, halo_raw, "--w", str(wh[0]), "--h", str(wh[1]),
                    "--anchor", "bottom", "--pad", "8", "--straight"], check=True, capture_output=True)
    smb._smb('prompt OFF; lcd "%s"; cd "\\probe\\sprites"; put halo.raw'
             % os.path.abspath(os.path.dirname(halo_raw)))
    wake_guest(qmp)
    crop = (x - 24, y - 24, x + wh[0] + 24, y + wh[1] + 24)
    shots = {}
    for tag, blob in (("halo", "C:\\probe\\sprites\\halo.raw"), ("clean", SPRITE_MASCOT)):
        kill_demo_procs(smb); time.sleep(1)
        smb.iexec(r"C:\probe\mascot.exe %d %d %d %d %s --static" % (x, y, wh[0], wh[1], blob))
        time.sleep(2.5)
        full = os.path.join(outdir, "halo-%s-full.png" % tag)
        qmp.screendump(full)
        im = Image.open(full).convert("RGB").crop(crop)
        im = im.resize((im.width * 2, im.height * 2), Image.LANCZOS)   # 2x so the fringe is legible
        p = os.path.join(outdir, "mascot-%s.png" % tag)
        im.save(p); shots[tag] = p
        print("halo demo: %s → %s" % (tag, p))
    kill_demo_procs(smb)
    a, b = Image.open(shots["halo"]), Image.open(shots["clean"])
    gap, lab = 12, 30
    comp = Image.new("RGB", (a.width + b.width + gap, max(a.height, b.height) + lab), (24, 24, 28))
    comp.paste(a, (0, lab)); comp.paste(b, (a.width + gap, lab))
    d = ImageDraw.Draw(comp)
    d.text((6, 7), "straight alpha (the bug): bright halo", fill=(255, 160, 150))
    d.text((a.width + gap + 6, 7), "premultiplied: clean", fill=(150, 235, 170))
    cp = os.path.join(outdir, "halo-comparison.png")
    comp.save(cp)
    return shots["halo"], shots["clean"], cp


def bitblt_demo(qmp, smb, outdir, win_dir="tools/xp/win",
                sprite=SPRITE_MASCOT, x=360, y=190, wh=MASCOT_WH):
    """The BitBlt-vs-screenshot demo: a layered window seen by the host capture but
    NOT by an in-guest GDI BitBlt. Returns (truth_png, bitblt_png, comparison_png)."""
    from PIL import Image, ImageDraw
    os.makedirs(outdir, exist_ok=True)
    kill_demo_procs(smb); time.sleep(1)   # clean A/B (no prior mascot)
    wake_guest(qmp)                        # ensure the desktop is up (not the idle screensaver)

    print("demo: launching the layered-window mascot (static, for a crisp A/B)…")
    # --static so the truth and BitBlt grabs frame the SAME pose (no motion between them)
    smb.iexec(r"C:\probe\mascot.exe %d %d %d %d %s --static" % (x, y, wh[0], wh[1], sprite))
    time.sleep(2.5)

    truth = os.path.join(outdir, "demo-truth.png")
    qmp.screendump(truth)
    print(f"demo: host QMP screendump → {truth} (mascot PRESENT)")

    print("demo: in-guest GDI BitBlt…")
    smb.iexec("C:\\probe\\bitblt.exe C:\\probe\\out\\bitblt.bmp", wait=6)
    bmp = os.path.join(outdir, "bitblt.bmp")
    smb.get("\\probe\\out\\bitblt.bmp", bmp)   # smbclient path is relative to the C$ share root
    bitblt_png = os.path.join(outdir, "demo-bitblt.png")
    Image.open(bmp).save(bitblt_png)
    print(f"demo: in-guest BitBlt → {bitblt_png} (mascot ABSENT)")

    # side-by-side comparison with labels
    a = Image.open(truth).convert("RGB")
    b = Image.open(bitblt_png).convert("RGB")
    gap, lab = 12, 28
    W = a.width + b.width + gap
    H = max(a.height, b.height) + lab
    comp = Image.new("RGB", (W, H), (24, 24, 28))
    comp.paste(a, (0, lab)); comp.paste(b, (a.width + gap, lab))
    d = ImageDraw.Draw(comp)
    d.text((6, 6), "host QMP screendump (sees the layered window)", fill=(120, 230, 140))
    d.text((a.width + gap + 6, 6), "in-guest GDI BitBlt (cannot)", fill=(240, 140, 140))
    comparison = os.path.join(outdir, "demo-comparison.png")
    comp.save(comparison)
    print(f"demo: comparison → {comparison}")
    return truth, bitblt_png, comparison


def run_script(chor, steps):
    """Interpret a list of step dicts: {op: move|click|type|key|launch|hold|frame, …}."""
    for s in steps:
        op = s["op"]
        if op == "move":
            chor.move_to(s["x"], s["y"], s.get("steps", 14))
        elif op == "click":
            chor.click(s.get("x"), s.get("y"), double=s.get("double", False))
        elif op == "type":
            chor.type(s["text"])
        elif op == "key":
            chor.key(*s["keys"])
        elif op == "launch":
            chor.launch(s["cmd"], wait=s.get("wait", 0))
        elif op == "hold":
            chor.hold(s.get("seconds", 1.0))
        elif op == "frame":
            chor.frame()
        else:
            raise ValueError(f"unknown op: {op}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="drive + capture an XP choreography")
    ap.add_argument("--sock", default=os.path.join(xpvm.DEFAULT_VMDIR, "qmp.sock"))
    ap.add_argument("--port", type=int, default=4445, help="forwarded guest SMB port")
    ap.add_argument("--out", default=os.path.join(xpvm.DEFAULT_VMDIR, "capture"))
    ap.add_argument("--fps", type=int, default=30, help="capture fps (deterministic stepped capture makes high fps smooth)")
    ap.add_argument("--realtime", action="store_true",
                    help="free-running capture (fast but jittery); default freezes the guest per frame (deterministic, no starvation)")
    ap.add_argument("--win-dir", default="tools/xp/win")
    ap.add_argument("--no-stage", action="store_true", help="skip staging tools + setres")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("stage", help="push all guest tools + sprites + set 1024x768")
    sub.add_parser("demo", help="BitBlt-vs-screenshot comparison (still A/B)")
    sub.add_parser("halo", help="premultiplied-alpha bug demo: straight vs premultiplied mascot stills")
    sub.add_parser("mascot", help="capture a moving layered-mascot clip → mascot-move.mp4")
    sub.add_parser("copy", help="capture the file-copy dialog: normal + hijacked → mp4s + stills")
    sub.add_parser("all", help="stage + mascot clip + BitBlt A/B + copy normal/hijacked")
    rp = sub.add_parser("run", help="run a choreography JSON → mp4")
    rp.add_argument("script")
    rp.add_argument("--mp4", default=None)
    rp.add_argument("--into", default=None, help="register the mp4 into this .slop.json")
    rp.add_argument("--key", default="xp_capture")
    args = ap.parse_args(argv)

    q = xpvm.Qmp(args.sock).connect()
    smb = xpsmb.XpSmb(port=args.port)
    try:
        if args.cmd in ("stage", "all") or (args.cmd in ("demo", "halo", "mascot", "copy") and not args.no_stage):
            stage_demo_tools(smb, win_dir=args.win_dir)
        if args.cmd == "stage":
            pass
        elif args.cmd == "demo":
            bitblt_demo(q, smb, args.out, win_dir=args.win_dir)
        elif args.cmd == "halo":
            print("halo:", halo_demo(q, smb, args.out))
        elif args.cmd == "mascot":
            print("clip:", mascot_clip(q, smb, os.path.join(args.out, "mascot"), fps=args.fps, stepped=not args.realtime))
        elif args.cmd == "copy":
            print("copy:", copy_demo(q, smb, args.out, fps=args.fps, stepped=not args.realtime, win_dir=args.win_dir))
        elif args.cmd == "all":
            print("clip:", mascot_clip(q, smb, os.path.join(args.out, "mascot"), fps=args.fps, stepped=not args.realtime))
            bitblt_demo(q, smb, args.out, win_dir=args.win_dir)
            print("copy:", copy_demo(q, smb, args.out, fps=args.fps, stepped=not args.realtime, win_dir=args.win_dir))
        elif args.cmd == "run":
            chor = Choreographer(q, smb, args.out, fps=args.fps, win_dir=args.win_dir, stepped=not args.realtime)
            run_script(chor, json.load(open(args.script)))
            mp4 = args.mp4 or os.path.join(args.out, "capture.mp4")
            chor.to_mp4(mp4)
            if args.into:
                register_video_asset(args.into, args.key, mp4)
    finally:
        q.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
