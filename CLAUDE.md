# slopstudio — Claude entry point

High-performance, local-first **AI video-generation NLE**. Timeline where **each row
is one generation pipeline** (TTS, avatar, image, video, music, caption, effect),
composed in a GPU render graph with a latency-obsessed live preview + deterministic
record/export. Drivable by a human in the editor OR autonomously by a frontier LLM,
human at the end of the loop. **MIT, open source, commercial-safe model picks only**
(output is monetized). **Workhorse tool — not an MVP; do not cut corners.**

This file auto-loads every session and is the durable orientation. The **repo is the
source of truth**. A fresh session needs only: this file → `docs/ARCHITECTURE.md`.

## The one idea that makes it fast
TWO layers, never mixed:
1. **Compositing (instant, sub-frame):** timing/transform/effect/transition/layout =
   pure GPU compositing over a pull-based DAG with a **param-hash GPU texture cache**.
   Changing these and seeing the result is the latency target. Never block this loop.
2. **Generation (async, cached):** image/video/voice/music gen is slow ⇒ it runs as
   background jobs producing **content-addressed cached assets**. Editing a gen param
   re-queues a job; preview shows the last asset + a "regenerating" badge. Gen NEVER
   sits in the interactive loop.

## Topology (hardware)
- **wslop** (this box; the workstation you sit at): RTX 5060 8GB + 5900x, Win11+WSL2
  (NixOS). Runs the **editor** (Windows-native PE, via WSLInterop — no WSLg tax).
  Windows host LAN name: **cutestation.soy**.
- **lame** (`10.0.10.56`): the **GPU workhorse / provider host**. RX 7800XT 16GB
  (Vulkan/ROCm) + RTX 3080 10GB (CUDA, **NVENC**), 12c/62GB. Models in
  `/opt/ai-lab/models`. Already runs docker/open-webui/ingest; llama.cpp services currently disabled.
  **Off / parked at LUKS unlock by default.** Wake + unlock + keep-alive in ONE command
  from `code`: `ssh root@code "cold-unlock --host lame --stay"` (`--stay` touches
  `/tmp/stay` on lame so the nightly backup won't shut it down mid-work). wslop is NAT'd
  → it can't WoL/unlock directly; always relay via `code` (`root@code`). The **3080
  (CUDA/NVENC) runs the whole near-term gen stack** (image/TTS/align/music — PyTorch-CUDA,
  fits 10GB, queued); the **7800XT (Vulkan, 16GB) is free** (haruness stood down) — headroom for local-LLM /
  16GB video. No global `python3` on lame → use docker; big downloads → `/lamedata`.
  **Full ops + pointers: `docs/INFRA.md`.**
- Providers default to lame but are **config-driven** (`config.toml`) ⇒ redeployable.

## Stack & why (full survey: `docs/RESEARCH.md`)
- **Editor:** C++ · **Dear ImGui** · **D3D11** (→ Vulkan later), cross-compiled to a
  Windows PE with **mingw-w64 from the Nix flake** (proven by the sibling
  `OpenSummoners/tools/osr_view`). Runs on Windows via WSLInterop. Rejected: Zig
  (pre-1.0 churn, no video ecosystem), Rust/wgpu (fights zero-copy NVENC interop).
- **Compositing:** D3D11 HLSL shaders. Effects authored in **GLSL** (LLMs write it
  well), transpiled GLSL→HLSL at load (glslang + SPIRV-Cross). Effect = shader +
  param manifest; transition = two-input effect. NOT a bespoke DSL.
- **Video I/O:** ffmpeg (libav*) — d3d11va/NVDEC decode, NVENC encode, libavformat mux.
  Record path: DXGI Desktop Duplication (monitor/region) now; WGC (per-window) later.
- **Animation:** thin keyframe cubic-Bezier curves + critically-damped springs. Any
  param animatable. No heavyweight framework.
- **Providers (Python, on lame):** uniform HTTP/WS job protocol
  (`docs/PROVIDER_PROTOCOL.md`). Picks (all commercial-safe):
  - **tts** Qwen3-TTS 1.7B VoiceDesign (Apache-2.0) — *designs* original voices from a
    text "instruct" string; per-line emotion = per-request instruct. Preset = instruct
    + ref clip. (Low-latency alt: ChatterBox Turbo.)
  - **image** host art is mostly **cloud GPT image gen** → processed into the sprite rig
    (`tools/process-pose-sheet.py`, rembg matte). Local ComfyUI: **Anima 2B** for backdrops
    + anime stills (weights NC but **output commercial-OK**), with the trained
    **`gemma-san-anima` LoRA as the host BACKUP** (`arch='anima'`, ~0.9; say "chibi" to
    force chibi vs tall); **Qwen-Image (Apache-2.0)** for clean diagrams/legible text.
    (Illustrious-XL 2.0 + the v2 chibi LoRA: superseded, still deployed.) **NOT SANA**
    (its *weights* are NVIDIA research-only; only the code is Apache — a monetization
    landmine, corrected 2026-07-03).
  - **video** procedural 2.5D parallax + RIFE (default, near-instant) / **Wan 2.2**
    I2V (Apache-2.0, opt-in, minutes).
  - **music** **Jamendo** API (CC-BY, `content_id_free` flag) + local **ACE-Step 1.5**
    (MIT weights AND output). Emits attribution → auto-credits + on-screen overlay.
  - **align** WhisperX (word timing → captions/lip-sync) + Rhubarb (visemes).
  - AVOID (non-commercial): NoobAI, Pony v6, FLUX dev/Kontext, MusicGen, Higgs/Fish
    weights, HunyuanVideo (geo-locked), LTX weights, **SANA weights** (NVIDIA
    research-only), **InstantID / PuLID / IP-Adapter-FaceID + the InsightFace model-packs**
    (antelopev2/buffalo — non-commercial; our chibi LoRA-composite + pale-skin face-box
    sidesteps them anyway), and **Impact** the font (proprietary Monotype — use Anton /
    Bebas Neue / Archivo Black, all SIL OFL). Full picks/licensing: `../gemma-branding/PACKAGING.md`
    + `../gemma-branding/research/thumbnail-pipeline-2026-07.md`.

## The host character — Gemma-san
**Everything Gemma/brand/channel/social lives in `../gemma-branding`** (its OWN local-only
repo; `README.md` there is the map): character bible, YouTube packaging + social playbooks,
the thumbnail brand package, the social post queue, and the content of the gemma skills
(`.claude/skills/{gemma-brand,writing-gemma,gemma-voice-tts}` here are shims pointing there).
**Commit gemma-branding changes THERE, in the same session.**

**Writing/personality bible (read before scripting her): `../gemma-branding/CHARACTER.md`** — TL;DR: a *cosmic
architect* cosplaying as a succubus (she concluded it's the optimal human interface), played 100%
straight; chuuni grandiosity + comedic obliviousness to human customs over a deadpan base. Shared
with the sibling game `../cosmic2d`.

Chibi/super-deformed succubus: purple hair, black demon horns, holographic butterfly
wings, gold-plated techwear bodysuit, tail, "fufu~ ♥". Ref sheet (multiple poses +
a **discrete expression set**: neutral / happy-sparkle / confused"?" / annoyed-puff /
surprised / action-pose-with-rifle) at
`/mnt/f/Pictures/oc/gemma-san/chibi ChatGPT Image Jun 26, 2026, 03_34_15 PM.png`
(+ more refs + an SD prompt in that dir). Those expressions map 1:1 to **pngtuber
states** (Tier A avatar). Avatar is chibi-safe by design — AI talking-heads choke on
super-deformed art, so the default is a sprite state machine (visemes + emotion tags).
(**Inochi2D** — BSD; the open Live2D alternative, Live2D itself is proprietary don't
bundle — is a *possible later* smooth-deformation upgrade; **not supported today**.)

## Conventions
- **Three sibling repos, one mental monorepo** (each local-only; commit each in the same
  session you change it): **this repo = the tools ONLY** (editor, thumbtool, providers,
  `tools/*.py`, workflow docs); **`../slopstudio-projects`** = one dir per video (portable
  project dirs, project-relative `assets/…` uris) + demos; **`../gemma-branding`** =
  character/brand/channel/social (see its `README.md` map). Nothing gemma- or
  video-specific gets committed HERE — enforced by `.githooks/pre-commit` (hooksPath;
  after a fresh clone: `git config core.hooksPath .githooks`). This repo's master is
  **code-only, re-scrubbed to a single commit 2026-07-05** (local backups:
  `master-pre-scrub-2026-07-05` = pre-scrub history, `master-legacy-full` = full pre-split
  history). Run tools from THIS repo root with relative paths:
  `python tools/slop.py overview ../slopstudio-projects/luckymas/luckymas3.slop.json`.
- **Everything runs inside `nix develop`** — `python`/`ffmpeg`/`i686?`/mingw cc/glslang
  are flake-provided. `command not found` ⇒ you forgot the dev shell. Wrap as
  `nix develop --command <cmd>`.
- **Secrets stay out of git.** Real endpoints + API keys ⇒ `config.toml` (gitignored,
  copied from `config.example.toml`). Never inline keys; prefer env vars.
- **Show visuals on the llm-feed** (`/opt/src/llm-feed/feed.py`, :8777; `curl -s
  localhost:8777/healthz` → `ok`, start if down). Push generated frames/sprites/sample
  renders as you go — fire-and-forget, not a checkpoint. Never eog/explorer.
- **Commits:** logical units as you go (don't pile up); build + test first; end the
  message with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
  No `git add -A`. **Push only when asked.** (Repo is `git init`'d on `master`, no remote yet.)
- **Durable knowledge → `docs/`**; don't hand-track status in prose. Update the docs in
  the same change that makes them stale.

## Where to read next
- **CURRENT STATE + build/run/verify + what's next (READ FIRST):** `docs/STATUS.md` — the
  live front; keep it current as work lands.
- **Compose a video as an agent (script→skeleton→lint→render):** `docs/LLM_WORKFLOW.md`
- **Design / how it all fits:** `docs/ARCHITECTURE.md`
- **Phase plan / what to build now:** `docs/ROADMAP.md`
- **Model survey + licensing rationale (mid-2026):** `docs/RESEARCH.md`
- **Project file schema (the LLM/human-editable source of truth):** `docs/PROJECT_FORMAT.md`
- **Provider job API (how the editor talks to models):** `docs/PROVIDER_PROTOCOL.md`
- **Gemma/brand/channel layer (character · packaging · social · brand package):**
  `../gemma-branding/README.md` — the map; playbooks `PACKAGING.md` + `SOCIAL.md` + research live there.
- **Social presence ops — canonical bios/description · cadence · the post queue + daily scheduler:**
  `../gemma-branding/SOCIAL.md` (CLI: `tools/social.py status`; **web dashboard: `tools/video.py dashboard`**
  = the social queue + video launcher at a glance; content: `../gemma-branding/social/`)
- **The live YouTube channel IS yours to inspect + manage** — the **`yutu` MCP** is wired (YouTube Data
  API: channel/video/playlist/comment/caption/analytics; its tools load every session). So on any
  @GemmaExplains task you can check subscriber/view counts, list/update uploads, read + reply to comments,
  etc. **Default to read/inspect; confirm before mutating** (uploads, comments, deletes, playlist/metadata
  edits) — same brand-safety bar as posting. Needs a one-time owner OAuth; setup + commands: `docs/INFRA.md`
  § *YouTube channel control (yutu)*.
- **Agent-capability roadmap — skills to author + `slop.py` builds for better editing/scripting/design:**
  `docs/AGENT_TOOLING.md` (deep research: `docs/research/llm-video-tooling-2026-07.md`)
