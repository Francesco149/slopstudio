# slopstudio — Roadmap

Phased so each phase lands a robust, usable slice on a solid core — not an MVP we throw
away. Acceptance criteria are concrete and demoable. Mirrors the session task list.

## Phase 0 — Foundation ✅ (in progress)
Repo scaffold, MIT license, Nix flake (validated: mingw-w64 + ImGui + glslang + ffmpeg +
python provider env all resolve), and the durable design: `ARCHITECTURE.md`,
`RESEARCH.md`, this file, plus the two contracts `PROJECT_FORMAT.md` and
`PROVIDER_PROTOCOL.md`, and the worked example project.
**Done when:** `nix develop` works; the docs + contracts + example exist; repo in git.

## Phase 1 — Core host spine
The irreducible vertical: *Gemma-san reads a script aloud, lip-synced, and you can scrub,
tweak, and record it — instantly.*
1. **Editor skeleton**: Windows PE (mingw), Dear ImGui + D3D11 window, loads/saves a
   `.slop.json`, file-watch live-reload, the ImGui timeline (tracks/rows/clips, playhead,
   scrub).
2. **Render graph v1**: pull-based DAG + param-hash GPU texture cache + dirty propagation;
   composite image/sprite/text layers; the two `RenderContext`s (async half-res preview /
   deterministic full-res export). Audio playback + A/V sync.
3. **TTS provider**: Qwen3-TTS VoiceDesign behind the protocol (`speech`, `voice_design`),
   content-hash cache, WS progress; `align` provider (WhisperX word timings + Rhubarb
   visemes).
4. **Avatar (pngtuber)**: sprite state machine driven by visemes + emotion tags using
   Gemma-san's expression sprites; pure compositing-layer ⇒ instant.
5. **Record/export**: deterministic full-res walk → ffmpeg/NVENC → mp4.

**Done when:** load the example, the host speaks with lip-sync, editing timing/transform/
expression is sub-frame, regenerating a line is async (preview never blocks), and export
produces a correct mp4. **This proves the latency thesis.**

## Phase 1b (parallel) — Gemma-san assets on `lame`
Stand up ComfyUI (now: Anima 2B; was Illustrious-XL 2.0) and Qwen3-TTS on `lame`; build a LoRA
dataset from `/mnt/f/Pictures/oc/gemma-san/`, train the **Gemma-san character LoRA**;
**generate** a **pngtuber expression + viseme sprite set** from that LoRA (consistent busts →
rembg-matted to alpha — see `tools/gen-avatar-sprites.py`/`cutout-sprites.sh`; cleaner + more
consistent than slicing a ref sheet); design + iterate + save her **Qwen3-TTS voice preset**
(succubus-playful "fufu~"). Optional later: Inochi2D rig.
**Done when:** the editor's Phase 1 demo uses the *real* voice + sprites, not placeholders.
✅ **done** — host speaks lip-synced with real LoRA sprites (rig `gemma-pngtuber`).

## Phase 2 — Generation providers
- **image**: ComfyUI-backed `text2image`/`image2image`/`controlnet` for reaction pics
  (now: GPT art primary, Anima + Gemma LoRA backup) and clean diagrams (Qwen-Image — NOT SANA, weights are research-only).
- **video**: procedural 2.5D parallax/RIFE (default) + Wan 2.2 I2V (opt-in).
- **music**: Jamendo CC-BY search (`content_id_free`) + ACE-Step local gen, emitting the
  attribution provenance that drives auto-credits + the on-screen overlay.
- Async job UX everywhere: editing a gen param re-queues; preview shows last + a
  "regenerating" badge.

**Done when:** a row can generate on-model reaction pics, add subtle motion to a still,
and drop in a credited background track — all without ever blocking the edit loop.

## Phase 3 — Authoring power + autonomy
- **LLM control API** + file-watch loop: a frontier model lays out the video, generates
  imagery, adds transitions, saves good ones as presets; human verifies/tweaks live.
- **Scriptable effects/transitions**: GLSL+manifest pipeline (glslang→SPIRV-Cross→HLSL),
  hot-reload, a **golden preset library** under `presets/`.
- **Animation**: full cubic-Bezier keyframe curves + springs in the editor UI.
- **Variants**: non-destructive YouTube/Shorts/TikTok cuts.
- **Export → upload staging queue** with auto-assembled title/description/credits +
  overlays for manual upload.
- Later: capture-source rows for autonomy (driving a VM / retro machine and capturing it).

**Done when:** "make me a 90-second explainer of X, then a Shorts cut" is one LLM-driven
pass landing two staged, credited exports for review.

## Cross-cutting (every phase)
Robustness first (out-of-process isolation, idempotent content-addressed cache,
crash-safe autosave, deterministic export), commercial-safe models only, secrets out of
git, visuals pushed to llm-feed, docs updated in the same change that makes them stale.
