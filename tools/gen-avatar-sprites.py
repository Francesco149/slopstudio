#!/usr/bin/env python3
"""Generate the Gemma-san SD avatar rig — ONE static frame per pose/emotion.

Why single-frame: Stable Diffusion (Illustrious-XL) is not stable enough to ANIMATE a
character — even from a fixed base, redrawing the mouth/eyes (txt2img tag-swap, img2img, or
masked inpaint) either drifts the body or bleeds artifacts (open eyes through a blink mask),
and it can't reliably distinguish more than ~2 mouth-openness levels. So the SD rig is just a
library of clean SINGLE poses (one per emotion here; add more — action/rifle/etc. — the same
way): cheap to generate any pose on demand for videos. Lip-sync/blink ANIMATION comes from a
separate rig — a GPT-authored expression set and/or a better-fit local model (see
docs/RESEARCH.md) — dropped in via the same manifest (an animated rig uses per-expression
`mouths`/`blink`; this static rig uses `sprite`). The editor compositor supports both.

  nix develop --command python tools/gen-avatar-sprites.py            # all 7 poses
  nix develop --command python tools/gen-avatar-sprites.py --only neutral,smug

Writes raw PNGs to presets/avatars/gemma-pngtuber/raw/<emotion>.png and pushes each to the
llm-feed. Cut out to alpha with tools/cutout-sprites.sh. Pure HTTP to the image provider's
/jobs API (docs/PROVIDER_PROTOCOL.md); no torch here.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "presets" / "avatars" / "gemma-pngtuber" / "raw"
FEED = ROOT.parent / "llm-feed" / "feed.py"

LORA = "gemma-san-chibi-v2"
LORA_STRENGTH = 0.9
SEED = 1234
WIDTH, HEIGHT = 768, 1344   # tall SDXL bucket — forces the standing full-body figure (a bust
                            # can't fill a tall-narrow frame), the framing the chibi rig needs.

# A clean super-deformed full-body chibi resting pose (the ref-sheet style). "full body" + the
# strong SD tags keep it chibi; "upper body" pushes Illustrious to a semi-realistic bust. Mouth
# closed = a calm rest state (the host isn't mid-speech — there's no flap). {expr} swaps the
# expression; the plain "simple background" is matted to alpha by rembg isnet-anime (cutout).
TEMPLATE = ("gemma-san, chibi, super deformed, 1girl, solo, (full body:1.3), full figure, "
            "head to toe, standing, feet visible, wide shot, {expr}, closed mouth, simple background, "
            "soft even lighting, masterpiece, best quality, very aesthetic")
NEGATIVE = ("(upper body:1.5), (portrait:1.5), (close-up:1.4), bust, face focus, head shot, cropped legs, "
            "realistic, normal proportions, dutch angle, dramatic lighting, rim light, glowing, energy, "
            "from side, profile, multiple views, reference sheet, turnaround, character sheet, "
            "worst quality, low quality, bad anatomy, bad hands, extra limbs, "
            "jpeg artifacts, watermark, signature, text")

# emotion → expression tags. Keys match the editor's canon_emotion / expr_for set.
EXPR = {
    "neutral":   "neutral expression, calm",
    "happy":     "happy, big smile, blush, sparkle, ^_^",
    "smug":      "smug, smirk, half-closed eyes, confident",
    "confused":  "confused, head tilt, question mark, raised eyebrow",
    "annoyed":   "annoyed, pout, puffed cheeks, frown, cross-popping vein",
    "surprised": "surprised, wide eyes, raised eyebrows, shocked",
    "sad":       "sad, teary eyes, frown, drooping ears",
}


def post(url: str, payload: dict | None = None, timeout: float = 120.0) -> dict:
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(
        url, data=data, method="POST" if data else "GET",
        headers={"content-type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def gen_one(base: str, emotion: str) -> Path | None:
    params = {"prompt": TEMPLATE.format(expr=EXPR[emotion]), "negative": NEGATIVE, "seed": SEED,
              "width": WIDTH, "height": HEIGHT, "steps": 28, "cfg": 5.0,
              "lora": LORA, "lora_strength": LORA_STRENGTH}
    print(f"  [{emotion}] submitting…", flush=True)
    sub = post(f"{base}/jobs", {"type": "text2image", "params": params})
    jid, h = sub["job_id"], sub["hash"]
    t0 = time.monotonic()
    while True:
        job = post(f"{base}/jobs/{jid}")
        st = job.get("status")
        if st in ("done", "ready"):  # base.py uses "done" as the terminal success status
            break
        if st == "error":
            print(f"  [{emotion}] ERROR: {job.get('error')}", file=sys.stderr)
            return None
        if time.monotonic() - t0 > 300:
            print(f"  [{emotion}] TIMEOUT", file=sys.stderr)
            return None
        time.sleep(0.8)
    OUT.mkdir(parents=True, exist_ok=True)
    dest = OUT / f"{emotion}.png"
    with urllib.request.urlopen(f"{base}/assets/{h}.png", timeout=60) as r:
        dest.write_bytes(r.read())
    dt = time.monotonic() - t0
    print(f"  [{emotion}] → {dest.relative_to(ROOT)}  {dest.stat().st_size}B  {dt:.1f}s"
          f"{' (cached)' if dt < 1.5 else ''}")
    return dest


def feed(path: Path, emotion: str):
    if not FEED.exists():
        return
    subprocess.run([sys.executable, str(FEED), "image", str(path),
                    "--title", f"gemma pose: {emotion}",
                    "--note", f"single-frame SD rig · v2 LoRA, seed {SEED}, {WIDTH}x{HEIGHT}"],
                   check=False, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://lame:8011", help="image provider base URL")
    ap.add_argument("--only", default="", help="comma list of emotions (default: all)")
    a = ap.parse_args()

    emotions = [e.strip() for e in a.only.split(",") if e.strip()] or list(EXPR)
    bad = [e for e in emotions if e not in EXPR]
    if bad:
        sys.exit(f"unknown: {bad}; emotions={list(EXPR)}")

    print(f"image provider {a.url} · {len(emotions)} poses · lora={LORA}@{LORA_STRENGTH} seed={SEED}")
    ok = 0
    for emotion in emotions:
        p = gen_one(a.url, emotion)
        if p:
            feed(p, emotion)
            ok += 1
    print(f"done: {ok}/{len(emotions)} poses → {OUT.relative_to(ROOT)}  (now run tools/cutout-sprites.sh)")


if __name__ == "__main__":
    main()
