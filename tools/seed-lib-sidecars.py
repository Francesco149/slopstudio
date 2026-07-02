#!/usr/bin/env python3
"""Seed gen-recipe sidecars (`<file>.meta.json`) for the committed golden library sprites +
backgrounds, so the editor's **Library Viewer** self-demonstrates L4 (gen recipe + Regenerate +
history) and L5 (pan/zoom) on load — select a `gemma-*` / `bg-*` item → its recipe + a Regenerate
button appear, no setup.

The recipes are the CURRENT golden host engine (see docs/STATUS.md, [[anima-lora-preferred-host-engine]]):
Anima 2B (`anima-base-v1.0`) + the `gemma-san-anima` LoRA for the host; Anima base (no LoRA) for the
backgrounds (their canon prompts/seeds come from presets/backgrounds/backgrounds.json). No `hash` is
recorded — these committed PNGs weren't generated through the editor's library path, so there are no
cached past-gen bytes; `history` starts empty and fills as you Regenerate in the editor.

Idempotent — rewrites the sidecars. Run: `python tools/seed-lib-sidecars.py`.
"""
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMG = ROOT / "library" / "images"

# host expressions → Anima prompt tail (mirrors tools/gen-avatar-sprites.py EXPR, the canon set)
HOST_EXPR = {
    "neutral":   "neutral expression, calm",
    "happy":     "happy, big smile, blush, sparkle",
    "smug":      "smug, smirk, half-closed eyes, confident",
    "confused":  "confused, head tilt, raised eyebrow",
    "annoyed":   "annoyed, pout, puffed cheeks, frown",
    "surprised": "surprised, wide eyes, shocked",
    "sad":       "sad, teary eyes, frown",
}
HOST_TMPL = ("gemma-san, chibi, super deformed, 1girl, solo, {expr}, upper body, simple background, "
             "masterpiece, best quality, very aesthetic")


def write_sidecar(png: pathlib.Path, params: dict) -> None:
    side = {"kind": "gen", "provider": "image", "cap": "text2image",
            "params": params, "ext": "png", "history": []}
    out = png.with_name(png.name + ".meta.json")
    out.write_text(json.dumps(side, indent=2) + "\n")
    print("seeded", out.relative_to(ROOT))


def main() -> None:
    # host sprites — Anima + gemma-san-anima LoRA @ 0.9
    for emo, expr in HOST_EXPR.items():
        f = IMG / f"gemma-{emo}.png"
        if f.exists():
            write_sidecar(f, {"prompt": HOST_TMPL.format(expr=expr), "seed": 1234,
                              "width": 1024, "height": 1024, "arch": "anima",
                              "lora": "gemma-san-anima", "lora_strength": 0.9})

    # backgrounds — Anima base, no LoRA, canon prompts/seeds from backgrounds.json
    bgj = json.loads((ROOT / "presets/backgrounds/backgrounds.json").read_text())
    neg = bgj.get("neg", "")
    for key, b in bgj.get("backgrounds", {}).items():
        f = IMG / f"bg-{key}.png"
        if f.exists():
            w, h = b.get("size", [1280, 768])
            write_sidecar(f, {"prompt": b["prompt"], "negative": neg, "seed": b.get("seed", 0),
                              "width": w, "height": h, "arch": "anima"})


if __name__ == "__main__":
    main()
