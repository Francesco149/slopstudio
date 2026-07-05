# slopstudio — Architecture

This is the durable design. It is meant to be read top-to-bottom once, then used as a
reference. The guiding constraint above all others: **minimal latency between changing
something and seeing the result.** Everything below serves that, plus robustness and
extensibility. This is a workhorse tool, not an MVP.

## 1. Goals / non-goals

**Goals**
- Sub-frame feedback for the *interactive* edits (timing, transform, effect params,
  transitions, layout). This is non-negotiable and shapes the whole design.
- Each timeline **row is one generation pipeline** (TTS, avatar, image, video, music,
  caption, effect, capture). Selecting a clip edits that pipeline's parameters.
- Local-first, redeployable, commercial-safe, MIT.
- Drivable interchangeably by a human (in the editor) and a frontier LLM (via a control
  API + the project file), human at the end of the loop.
- Extensible: new models, effects, transitions, and pipeline types plug in without
  touching the core.
- Non-destructive **variants** (e.g. a tight YouTube-Shorts cut from the same project).
- Record/export must be fast because most of the frame is already cached.

**Non-goals (for now)**
- Multi-user collaboration; cloud rendering; a polished/pretty UI (ImGui is fine).
- Real-time AI *generation* in the edit loop — we explicitly design around its slowness
  instead of pretending it's fast.

## 2. The two-layer model (the core idea)

Two layers with a hard boundary between them:

| | Compositing layer | Generation layer |
|---|---|---|
| Latency | sub-frame (µs–ms) | seconds–minutes |
| Work | timing, transform, effects, transitions, layout, captions, mixing | TTS, image, video, music synthesis |
| Where | editor GPU (D3D11) | providers (Python, on `lame`) |
| In the edit loop? | **yes — must stay instant** | **never — async + cached** |

The generation layer produces **content-addressed cached assets** (an image, a wav, a
short video). The compositing layer treats those assets as immutable inputs and arranges
them. Editing a *compositing* parameter recomputes only GPU work and is instant. Editing
a *generation* parameter enqueues a background job; the preview keeps showing the
previous cached asset with a small "regenerating" badge until the new asset lands, then
swaps it in. **The interactive loop never waits on a model.**

This is why the tool can feel instant despite being built on slow models.

## 3. Processes & topology

```
   workstation (wslop, Windows-native)         GPU host (lame, config-driven)
   ┌───────────────────────────────┐           ┌──────────────────────────────┐
   │ slopstudio.exe (C++/ImGui/D3D11)│  HTTP    │ provider: tts   (Qwen3-TTS)  │
   │  • timeline + render graph     │  + WS    │ provider: image (ComfyUI)    │
   │  • live preview + export/record│ ───────► │ provider: video (Wan/proc)   │
   │  • project doc (source of truth)│ ◄─────── │ provider: music (ACE/Jamendo)│
   │  • HTTP control API (port 8080)│  assets  │ provider: align (WhisperX)   │
   └───────────────────────────────┘           └──────────────────────────────┘
```

- **Editor**: one native process. Owns the project document, the render graph, the GPU
  compositor, preview, export, record, and the control API.
- **Providers**: one small process per capability, out-of-process by design so a model
  crash/OOM cannot take down the editor. Endpoints in `config.toml`. Can run anywhere
  (default: `lame`); a provider can also run locally for a self-contained deployment.
- **Asset transport**: providers return an asset reference. When the editor and provider
  share a filesystem (e.g. an SMB/NFS mount of the cache), the reference is a path; else
  the editor fetches the bytes over HTTP. Both are supported; path is preferred for
  large media.

## 4. Data model

### 4.1 Project document
A single declarative JSON file is the **source of truth** (schema:
`docs/PROJECT_FORMAT.md`). The editor owns persistence: it pretty-prints with a stable
key order so git diffs are clean, autosaves, and **watches the file** so an external
editor (a human, or an LLM via Claude Code) can change it and the editor live-reloads.
The same edits are available through the **HTTP control API**; both paths converge on the
same document. Human/LLM annotations live in first-class `notes` fields (not comments),
so round-tripping through the editor never loses them.

### 4.2 Rows, clips, assets
- A **row** is an instance of a *pipeline type* (`tts`, `avatar`, `image`, `video`,
  `music`, `caption`, `effect`, `capture`, `group`). It has row-level params (e.g. the
  voice preset for a `tts` row) and a list of clips.
- A **clip** is a time-ranged unit on a row (e.g. one spoken line, one reaction image).
  It references a generated **asset** by content hash, carries per-clip params, a stack
  of **effects**, a transform, and **keyframes** (any param can be animated).
- An **asset** is the immutable output of a generation job, keyed by
  `hash(provider, type, normalized_params, input_asset_hashes)`. The asset index records
  status (`queued|running|ready|error`), the cache path/URI, and provenance (incl. music
  attribution). Identical params ⇒ identical hash ⇒ cache hit ⇒ no regeneration.

Rows are ordered on **tracks** (for display order and compositing z-order). Transitions
live between adjacent clips on a track; effects live on clips/rows.

## 5. Render graph

A **pull-based DAG**. Nodes: clip-sources, effects, transitions, composites, and the
output. To render frame *t*, the output node pulls from its inputs recursively.

- **Param-hash GPU texture cache.** Each node output is keyed by
  `(node_id, frame, param_hash)` where `param_hash` folds in the node's params *and* its
  inputs' hashes. A cache hit is a no-op (no shader dispatch, no upload). This is what
  makes scrubbing and tweaking instant.
- **Dirty propagation.** Changing a param marks that node and its topological successors
  dirty (one forward BFS), evicting only their affected cache entries and only the
  affected frame range. Untouched frames/nodes stay cached.
- **Two render contexts, one graph.** A `RenderContext` carries
  `{resolution_scale, quality_flags, async}`:
  - *Preview*: half-res (configurable), may skip flagged-expensive nodes, runs async on a
    render thread; the UI blits the last completed frame (double-buffered) while the next
    computes.
  - *Export*: full-res, deterministic, synchronous serial walk → encoder. Same graph ⇒
    preview and export can't diverge in logic.
- **Background pre-render.** A low-priority thread walks forward from the playhead filling
  the frame cache up to a GPU-memory budget, so forward scrubbing is instant after a brief
  idle.
- **GPU memory**: textures are pool-allocated and LRU-evicted under a configurable cap.

## 6. Providers

A provider is any process implementing the **provider protocol**
(`docs/PROVIDER_PROTOCOL.md`): `GET /capabilities`, `POST /jobs`, `GET /jobs/{id}`,
`WS /jobs/{id}/events`, `GET /healthz`, asset fetch. Properties:

- **Async + cached.** Jobs are submitted by param-hash; a hash already `ready` returns
  immediately. Progress streams over WS so the editor can show a live percentage.
- **Capability discovery.** `/capabilities` returns the pipeline types served, their
  parameter JSON-schemas, and supported presets. The editor builds its parameter UI from
  this — so a new model exposes new params with **no editor code change**.
- **Fault isolation.** A provider can crash, OOM, or be offline; the editor degrades
  (shows last asset / "provider offline") and never dies.
- **Location-agnostic.** The same provider runs on `lame` or locally; only the URL in
  `config.toml` changes. This is the redeployability mechanism.

The image and video providers wrap **ComfyUI** (its HTTP/WS API) as the actual model
engine — a robust, extensible, widely-maintained graph runtime that already supports the
SOTA models. Our provider is a thin adapter: it owns the ComfyUI workflow templates,
injects params, submits, streams progress, and returns the produced asset + provenance.

## 7. Avatar subsystem

The host (Gemma-san) is rendered by a **static-pose pngtuber state machine** (Tier A —
the robust, zero-latency default):
- Inputs: the TTS audio envelope and the script's **emotion tags** per line.
- State: current expression/pose (tag→sprite), plus procedural idle/talk motion
  (breathing bob, talk bob, light-up) on the GPU.
- Output: a composited video layer, generated entirely in the compositing layer ⇒
  instant, deterministic, and chibi-safe (no AI talking-head, which fails on
  super-deformed art).

**Realized (Tier A).** The sprite set is a **rig** under `presets/avatars/<rig>/` (a
`manifest.json` mapping each emotion/pose → a static sprite PNG); the avatar row names its
rig via the `rig` param. Rigs can be generated, authored from GPT pose sheets, or library
defs with per-emotion overrides. The compositor picks the sprite from the clip tag (or the
driven VO line's tag when `emotion:"auto"`), then applies procedural audio-reactive motion.
The **procedural chibi** is the graceful fallback when a rig is absent. Same script inputs
across tiers ⇒ a row can upgrade to Inochi2D without rescripting.

Tier B (optional, later): an **Inochi2D** rig (BSD-licensed, the open Live2D alternative)
driven by the same script/emotion/audio stream for smooth mouth/blink/mesh deformation.
Tier C (per-clip,
offline only): AI talking-head (e.g. LatentSync) for special shots. Tiers share the same
driver inputs so a row can be upgraded without rescripting.

## 8. Effects, transitions, animation

- **Effect = GLSL shader + a param manifest** (name/type/default/range as JSON).
  Effects stack on a clip/row (each reads the previous output as `src`). Authored in GLSL
  (LLMs write GLSL reliably; humans iterate in any text editor), transpiled to HLSL/DXBC
  at load via glslang + SPIRV-Cross for the D3D11 backend. Hot-reloadable.
- **Transition = a two-input effect** (`srcA`, `srcB`, `t`) between adjacent clips.
- **Presets**: any effect/transition + its params can be saved as a named preset under
  `presets/` (committed "golden" presets) or in-project. The LLM is expected to save good
  transitions it builds as reusable presets.
- **Animation**: every numeric param is animatable via a per-param **cubic-Bezier
  keyframe curve** (Blender/AE-compatible handle model) plus an optional
  **critically-damped spring** evaluator for natural interactive-feeling motion. No
  heavyweight animation library.

## 9. LLM-driven control

Two equivalent control surfaces, same underlying document:
1. **Edit the project file.** An LLM (Claude Code) writes the JSON; the editor
   live-reloads and re-renders. Best for big structural layout.
2. **HTTP control API** (`127.0.0.1:8080`). Verbs to create/modify rows & clips, set
   params, add transitions, trigger generations, save presets, switch/render variants,
   and **introspect state** (what exists, render/job status, errors) so the model can
   reason about the current edit. Best for incremental nudges and reading results back.

The model lays out the video, picks/generates imagery, adds transitions, and saves good
ones as presets; the human verifies and tweaks live in the editor. Both see one truth.

## 10. Variants (non-destructive versions)

A **variant** is a named overlay on the base edit: a list of overrides (retime, reorder,
hide/show rows, swap aspect ratio/resolution, change pacing) that produce a different cut
(e.g. `shorts` = 9:16, faster pacing, subset of beats) **without mutating the base**.
Rendering a variant applies its overrides to the graph at evaluation time. Variants are
first-class in the project document and selectable in the editor and via the control API.

## 11. Export, record, upload staging

- **Export**: render a chosen variant at full quality via the deterministic export
  context → ffmpeg (NVENC where available) → file. Fast because most frames are cached.
- **Record**: the live composition (or a captured external window/monitor via DXGI
  Desktop Duplication) encoded in real time. Capture sources are just another row type,
  which is also the hook for later autonomy (driving a VM / retro machine and capturing
  it).
- **Upload staging**: an export lands in a **staging queue** with auto-assembled metadata
  — title/description scaffold, and **music/art credits generated from the tracked
  attribution** of every sourced asset, plus the on-screen "now playing" overlay data.
  The human reviews the queue and uploads manually (paste title/description, publish).

## 12. Robustness / fault model

- Out-of-process providers ⇒ model faults are isolated.
- Idempotent, content-addressed generation ⇒ safe to retry; crashes don't corrupt the
  cache; regeneration is deterministic.
- Crash-safe project autosave + a versioned project schema (`schema` field) with
  migrations.
- Deterministic export context ⇒ what you preview is what you render.
- The editor degrades gracefully when a provider is offline (last asset + status), never
  blocking edits.

## 13. Tech stack (rationale in `docs/RESEARCH.md`)

- **Editor**: C++ · Dear ImGui · D3D11 (→ Vulkan later) · mingw-w64 cross from the Nix
  flake · runs Windows-native via WSLInterop.
- **Compositing/effects**: HLSL at runtime; GLSL authoring transpiled via glslang +
  SPIRV-Cross.
- **Video I/O**: ffmpeg (d3d11va/NVDEC decode, NVENC encode, libavformat mux); DXGI
  Desktop Duplication for capture (WGC later).
- **Providers**: Python + FastAPI; ComfyUI as the image/video engine; Qwen3-TTS,
  ACE-Step, WhisperX, Rhubarb behind the uniform protocol.
- **Project doc / IPC**: JSON document + HTTP/JSON + WebSocket progress.

## 14. Decisions log (so we don't re-litigate)

- **Windows-native editor, mingw cross from Nix** — dodges the WSLg perf tax (user
  requirement) while keeping one reproducible flake; pattern proven by the sibling
  `OpenSummoners/tools/osr_view`.
- **C++/ImGui/D3D11 over Zig and Rust/wgpu** — Zig pre-1.0 churn + no video ecosystem;
  Rust/wgpu fights zero-copy NVENC interop. (`docs/RESEARCH.md` §engine.)
- **Generation is async + cached, never in the edit loop** — the whole latency strategy.
- **Providers out-of-process, config-driven** — fault isolation + redeployability.
- **Commercial-safe models only** — MIT codebase + monetized output (user decision).
- **pngtuber avatar as the default** — chibi-safe, zero-latency; Inochi2D as the upgrade.
- **Project file + control API both drive one JSON document** — clean LLM/human loop.
