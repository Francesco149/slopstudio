#!/usr/bin/env python3
"""Bake a GOLDEN REFERENCE clip for a voice preset → stable cloned synthesis.

VoiceDesign re-derives the voice from the instruct on every call, so the host's timbre drifts
line-to-line. The fix (Qwen's recommended design-once→clone, docs/RESEARCH.md): render ONE
reference clip from the preset's instruct, then CLONE from that clip for all lines. This tool
does the bake — renders the ref via the provider's `voice_design`, saves it next to the preset,
and records `ref`/`ref_text` in the preset JSON. After this, the editor sends the ref with every
line and the provider clones it (identical timbre across lines).

  nix develop --command python tools/bake-voice-ref.py gemma-san-deep
  nix develop --command python tools/bake-voice-ref.py gemma-san-deep --ref-text "..." --test

A longer ref (~8-15s, varied phonemes) clones better; the default sample is in-character and
covers a good spread. Re-run to re-bake (e.g. after tweaking the instruct).
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VOICES = ROOT / "presets" / "voices"

DEFAULT_REF_TEXT = ("Fufu~ welcome back, darling. Today we are breaking a game older than some "
                    "of you, and I intend to enjoy every single second of it.")


def post(url, payload=None, timeout=300):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method="POST" if data else "GET",
                                 headers={"content-type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def run_job(base, body, label):
    sub = post(f"{base}/jobs", body)
    jid = sub["job_id"]
    t0 = time.monotonic()
    while True:
        job = post(f"{base}/jobs/{jid}")
        st = job.get("status")
        if st in ("done", "ready"):
            return job
        if st == "error":
            sys.exit(f"{label}: ERROR {job.get('error')}")
        if time.monotonic() - t0 > 600:
            sys.exit(f"{label}: TIMEOUT")
        time.sleep(1.0)


def asset_url(base, result, kind):
    for a in result.get("assets", []):
        if a.get("kind") == kind:
            # uri like cache://tts/<hash>.wav → fetch by hash
            uri = a["uri"]
            name = uri.rsplit("/", 1)[-1]
            return f"{base}/assets/{name}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("preset", help="preset name under presets/voices/ (no .json)")
    ap.add_argument("--url", default="http://lame:8010", help="tts provider base URL")
    ap.add_argument("--ref-text", default=DEFAULT_REF_TEXT, help="transcript rendered as the ref clip")
    ap.add_argument("--test", action="store_true", help="after baking, clone two lines to sanity-check")
    a = ap.parse_args()

    ppath = VOICES / f"{a.preset}.json"
    if not ppath.exists():
        sys.exit(f"no preset: {ppath}")
    preset = json.loads(ppath.read_text())
    instruct = preset.get("voice", "")
    if not instruct:
        sys.exit(f"preset {a.preset} has no 'voice' instruct")
    seed = preset.get("seed", 0)
    language = preset.get("language", "English")

    print(f"baking ref for '{a.preset}' (seed={seed}) — designing {len(a.ref_text)} chars…", flush=True)
    res = run_job(a.url, {"type": "voice_design", "params": {
        "instruct": instruct, "name": a.preset, "sample_text": a.ref_text,
        "language": language, "seed": seed}}, "design")["result"]
    au = asset_url(a.url, res, "audio")
    if not au:
        sys.exit("design returned no audio asset")
    refwav = VOICES / f"{a.preset}.ref.wav"
    refwav.write_bytes(urllib.request.urlopen(au, timeout=120).read())
    print(f"  ref clip → {refwav.relative_to(ROOT)}  ({refwav.stat().st_size}B)")

    # record ref in the preset (preserve key order; drop the now-unused single-call seed note)
    preset["ref"] = refwav.name
    preset["ref_text"] = a.ref_text
    ppath.write_text(json.dumps(preset, indent=2) + "\n")
    print(f"  preset updated: {ppath.relative_to(ROOT)}  (ref + ref_text)")

    if a.test:
        ref_b64 = base64.b64encode(refwav.read_bytes()).decode()
        for line in ("Capitalism, ho!", "And that, my dear, is how the whole thing falls apart."):
            r = run_job(a.url, {"type": "speech", "params": {
                "text": line, "ref_b64": ref_b64, "ref_text": a.ref_text,
                "language": language, "seed": seed}}, "clone")["result"]
            dur = r["assets"][0]["meta"].get("duration")
            print(f"  clone OK: {dur}s  «{line}»")
    print("done — the editor will clone this preset's ref for every line (stable timbre).")


if __name__ == "__main__":
    main()
