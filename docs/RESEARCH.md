# slopstudio — Model & Tooling Research (mid-2026)

Durable record of the "latest and greatest" survey done at project start (June 2026),
filtered for our hardware and for **commercial-safe licensing** (MIT codebase + monetized
output). Picks are bolded. Each section ends with what's bolded into the build and what to
re-verify. Re-run this survey when the landscape shifts.

Hardware targets: **lame** = RX 7800XT 16GB (Vulkan/ROCm) + RTX 3080 10GB (CUDA, NVENC);
**workstation** = RTX 5060 8GB (editor host, not a gen node).

## TTS — voice design + per-line emotion

**Pick: Qwen3-TTS 1.7B (VoiceDesign variant), Apache-2.0.** Designs an *original* voice
from a natural-language `instruct` string (no real-person cloning needed); per-line
emotion/intonation = vary the instruct per request, which maps cleanly onto interleaving
direction into the script. ~4–5 GB on the 3080. A saved **voice preset** = the instruct
string (+ optionally a generated reference clip). Offline inference is stable; streaming
in vLLM-Omni was still maturing at survey time.

| Model | Voice design | Emotion control | VRAM | License | Note |
|---|---|---|---|---|---|
| **Qwen3-TTS 1.7B VoiceDesign** | yes (text→voice) | per-request NL instruct | ~5 GB | **Apache-2.0** | the pick |
| ChatterBox Turbo | no (ref audio) | scalar dial | ~4–6 GB | **MIT** | <200ms streaming, ROCm — low-latency live alt |
| MiMo-V2.5-TTS VoiceDesign | yes | tags + style | ~? | **MIT** | Xiaomi, Apr 2026; verify VRAM |
| Fish S2 / Higgs v3 | — | best inline tags | 8–16 GB | **non-commercial** | excluded |

- **Per-line emotion**: per-request instruct string is the practical answer (works with
  the pick). Inline mid-sentence tags only exist on non-commercial models.
- **Word timing** (captions + lip-sync): **WhisperX** (fast, GPU, one dep). Upgrade to
  Montreal Forced Aligner only if frame-perfect sync is ever needed.

### Voice STABILITY across lines — finding (2026-06-27)
The host voice drifts in timbre line-to-line because **VoiceDesign re-derives the voice from
the instruct on every call** (the realization depends on the text too; a fixed `seed` pins the
noise, not the text-dependent derivation). Qwen's own recommended fix for a *consistent character
voice* is **design-once → clone**: render ONE golden reference clip with **VoiceDesign**, then
use the **Qwen3-TTS-12Hz-1.7B-Base** model's `create_voice_clone_prompt(ref_wav, ref_text)` once
+ `generate_voice_clone(text, prompt, language)` for every line → stable timbre (the clip defines
the voice, not a re-run instruct). This matches CLAUDE.md's intended preset format ("instruct +
ref clip"). **Plan:** (1) deploy the Base variant alongside VoiceDesign on lame's 3080 (~2×1.7B
bf16 fits 10 GB, or load on demand); (2) provider gains a clone path (`speech` uses the preset's
ref clip when present, else falls back to VoiceDesign); (3) a one-time "bake golden ref" step per
preset (design clip + transcript → `presets/voices/<name>/ref.wav` + `ref_text`); (4) editor
unchanged (still picks `voice_preset`). Trade-off: clone weakens per-line *instruct* emotion —
acceptable for a host; revisit CustomVoice's `instruct` if needed. Variants:
Qwen3-TTS-12Hz-1.7B-{VoiceDesign (design), Base (clone), CustomVoice (9 timbres + instruct)}.

**Accents — no English knob (2026-06-27).** Qwen3-TTS has NO `dialect`/`accent` parameter; its
"dialect" support is **Chinese only** (the Beijing/Sichuan preset speakers Dylan/Eric). Accent is
*not* one of VoiceDesign's controllable dimensions, so a free-text "Scottish/British accent"
instruct is weak/inconsistent (the model drifts to standard American). **The reliable way to give
the host a regional accent is to CLONE from real accented reference audio** (10–15 s) — which our
clone pipeline already does: point a preset's `ref`/`ref_text` at an accented clip. (TODO: an
"import ref clip" button so an external accented sample can seed a preset without the CLI.)
- **Verify**: Qwen3-TTS streaming status in vLLM-Omni; MiMo VRAM; ChatterBox ROCm path; whether
  Base clone accepts an emotion/instruct (for per-line direction) or only ref+text.

### TTS replacement survey — deep research (2026-07-01, web-verified)
Qwen3-TTS take-to-take instability is **upstream-confirmed and unfixed** (non-deterministic
output w/ identical inputs: QwenLM/Qwen3-TTS#298 no maintainer response; rate drift on >100-char
text closed "not planned" #239). Sampling hygiene (temp 0.8 / top_p 0.9 / rep-pen 1.05 /
max_new_tokens ≤2048) + seed pinning + the frozen clone prompt are necessary but NOT sufficient —
a bad seed is deterministically bad. Ranked replacements (all licenses verified on live HF tags):
1. **Chatterbox (Resemble AI) — MIT code+weights, the stability winner.** Turbo (350M, EN-only,
   ~2-3GB, `[laugh]` tags) + **Multilingual V3** (500M, 2026-06, 23 langs **incl. JA**, release
   notes explicitly target repetition/off-prompt fixes). Best mitigation ecosystem:
   Chatterbox-TTS-Extended = reference best-of-N + faster-whisper WER auto-select pipeline;
   known failure modes mapped (gibberish on very short "Hi!" segments, >350-char hallucination →
   we already chunk per sentence). devnen/Chatterbox-TTS-Server has an explicit ROCm path.
2. **CosyVoice 3 0.5B-2512 (Apache-2.0)** — cross-lingual clone across 9 langs incl. EN+JA (the
   exact design-in-JP-speak-EN workflow), instruct emotion, ~2GB, official docker. Weakness:
   prosody variance on hard text.
3. **VoxCPM2 (OpenBMB, Apache-2.0)** — 2B, 48kHz output (everything else is 22-24k), 30 langs,
   ~8GB; caveat: documented ref-tail chirp artifact at segment start (VoxCPM#272, trim ~100ms).
Watch: ZONOS2 (Apache, EN+JA, but 8B MoE ≈16GB bf16 — doesn't fit the 3080, CUDA-only so the
7800XT can't host it either; wait for quants); Step-Audio-EditX (Apache 3B) = token-level
emotion/style EDITING of already-generated audio — maps onto per-clip intonation tuning.
**License landmines (extend AVOID):** F5-TTS checkpoints CC-BY-NC (code MIT — trap); IndexTTS-2
bilibili license (commercial = contact); Higgs v2 relicensed off Apache, TTS3 research-only;
Fish/OpenAudio more restrictive since 2026-03; XTTS CPML unfixable (Coqui defunct); LLaSA/
Spark-TTS/MaskGCT/sarashina2.2-tts non-commercial; VibeVoice MIT-tagged but research-worded+slow.
**Plan:** (a) `providers/chatterbox` on lame's 3080 (docker; Turbo EN + Multilingual JA),
re-clone the golden gemma ref through it + CosyVoice3, ear-test vs Qwen3; (b) **best-of-N +
WER rescoring in the provider layer** (engine-agnostic — transcript is always known; scores via
faster-whisper or **Qwen3-ForcedAligner-0.6B** (Apache, single-pass word timestamps — could also
replace WhisperX in align)); (c) keep Qwen3 VoiceDesign as the voice-DESIGN tool only.

**VoiceDesign instruct cheat-sheet (how to write a `voice`).** Free-form natural language,
**2–4 sentences (~40–80 words)** — describe HOW it *sounds* (acoustic), never the persona's job.
Layer the dimensions in this order: **identity** (gender / age / archetype) → **pitch/register**
(low/mid/high) → **texture/timbre** (breathy, smooth, raspy, velvety) → **emotion/personality**
(warm, confident, teasing, shy) → **pacing/cadence** (slow, unhurried, pauses; quick, clipped) →
**accent** (weak — say "… accent *speaking English*"; clone for real ones) → a **distinguishing
detail** (one vivid image). Don't combine contradictions ("high-pitched deep bass"). Instruct
language = English or Chinese (others weaker). Design mode appends per-line emotion to the instruct;
**clone mode ignores it** (timbre only). Example (Gemma-deep): *"A seductive low, breathy young
woman's voice — a deeper chest register, smooth and velvety with a mischievous succubus purr. Warm
and confident, unhurried and knowing, a slow smile in the tone."*

## Image — anime host + reaction pics + diagrams

**Picks: Illustrious-XL 2.0 (CreativeML Open RAIL-M, commercial OK)** for anime/comedy,
with a **trained Gemma-san character LoRA (Kohya)** for the recurring host + ControlNet
for posing; **SANA (Apache-2.0)** for fast clean technical diagrams. Engine: **ComfyUI**
(HTTP/WS API) — the de-facto extensible, LLM-driveable backend.

| Model | 8/10/16 GB | Anime | License | Note |
|---|---|---|---|---|
| **Illustrious-XL 2.0** | ✓/✓/✓ | ★★★★★ | RAIL-M (commercial OK) | deepest LoRA+ControlNet ecosystem |
| Animagine XL 4.0 | ✓/✓/✓ | ★★★★ | RAIL-M | safe alt |
| **SANA 0.6B** | ✓/✓/✓ | ★★ | **Apache-2.0** | sub-second; diagrams/illustration |
| FLUX.2 Klein 4B | –/–/✓ | ★★★ | **Apache-2.0** | general, commercial-safe; 13 GB |
| NoobAI XL / FLUX Kontext | — | ★★★★★ | **non-commercial** | **excluded** |

- **Character consistency**: train a LoRA from the ref sheet (20–50 imgs of varied
  pose/expression) + ControlNet pose control. Most robust path for one on-model OC. We
  have a ref sheet + extra refs in `/mnt/f/Pictures/oc/gemma-san/` to bootstrap the set.
- **AMD note**: ROCm trails CUDA for brand-new models; GGUF/FP8 fallbacks. SDXL-class
  (Illustrious) is well-supported on the 7800XT.
- **Verify**: Illustrious-XL 2.0 exact license text; ComfyUI auth wrapper (add API key,
  bind localhost).

## Video — subtle motion + short clips

**Default: cheap procedural motion** (depth-based 2.5D parallax via MiDaS/ZoeDepth + RIFE
interpolation, or ffmpeg `zoompan` for pan/zoom) — near-instant, deterministic, perfect
for "add subtle motion to a reaction image." **Opt-in AI I2V: Wan 2.2 (Apache-2.0)** with
the Lightning 4-step LoRA when real character motion is needed (minutes per clip).

| Model | T2V/I2V | VRAM | ~5s clip | License | Note |
|---|---|---|---|---|---|
| **Wan 2.2 5B FP8** | both | 8–10 GB | ~4–6 min | **Apache-2.0** | I2V for the 10 GB 3080 |
| **Wan 2.2 14B GGUF** | both | 12–16 GB | ~3–6 min (Lightning) | **Apache-2.0** | quality leader on the 7800XT |
| LTX-Video | both | 12–16 GB | fastest | custom (rev-gated) | flag; not bundled |
| HunyuanVideo | T2V | 8–16 GB | ~5 min | custom (**geo-locked**) | excluded |

- Real-time video gen is not feasible on these cards — hence procedural-first.
- **Verify**: Wan 2.2 ComfyUI low-VRAM node wiring (T5 on CPU, GGUF/FP8) to avoid OOM.
- **GPU backend (lab-specific):** the 7800XT runs **Vulkan**, but PyTorch diffusion/video
  is ROCm-only — so AMD video means accepting ROCm for the 16 GB headroom, or running
  **Wan 2.2 5B-FP8 on the 3080/CUDA** (10 GB). Image/TTS/align/music all run on the
  3080/CUDA regardless (see `docs/INFRA.md` §GPU strategy).

## Music — background beds + royalty-free sourcing

**Sourcing pick: Jamendo API v3** (filter CC-BY, `content_id_free=true`); rich per-track
metadata drives **automatic credits** + the on-screen overlay. **Local gen: ACE-Step 1.5
(MIT weights AND output)** for loopable non-distracting beds; BPM/key control; runs on all
cards. Excluded: MusicGen (CC-BY-NC weights).

Attribution schema (store per track, sourced or generated): `title, artist, source_platform,
source_url, license_name, license_url, attribution_required, attribution_text,
content_id_risk, ai_generated, ai_model`. Render `attribution_text` into the description;
`title + artist + license` into the overlay.

- **Verify**: Jamendo `content_id_free` reliability; ACE-Step training-data provenance is
  self-reported (MIT weights are clear).

## Avatar — animating the chibi host

**Tier A (default): pngtuber sprite state machine** driven by **Rhubarb Lip Sync**
(phoneme→viseme, MIT, CPU) + script emotion tags. Zero VRAM, instant, deterministic, and
**chibi-safe** — AI talking-heads (MuseTalk/LivePortrait/etc.) blur or fail on
super-deformed art. Gemma-san's ref-sheet expression set maps 1:1 to states.

**Tier B (optional): Inochi2D** (BSD-2 — the open Live2D alternative; **Live2D's own SDK
is proprietary, do not bundle**), `inox2d` runtime, driven by the same viseme/emotion
stream for smooth deformation. Requires a one-time manual rig (no reliable single-image
auto-rig exists yet in 2026).

**Tier C (offline only): LatentSync 1.5** (OpenRAIL++, anime-capable) for special
per-clip talking-head shots — not for the real-time preview.

| Tool | Anime? | VRAM | License | Verdict |
|---|---|---|---|---|
| **Rhubarb (sprites)** | yes | none | MIT | the default driver |
| **Inochi2D / inox2d** | yes | tiny | BSD-2 | optional smooth rig |
| LatentSync 1.5 | semi | 8 GB | OpenRAIL++ | offline special shots |
| MuseTalk/LivePortrait/THA4/Sonic | no/NC | — | mixed/NC | excluded for default |

- **Verify**: Rhubarb license file; LatentSync behaviour on *chibi* (not just semi-real).

### Generating STABLE rig frames — local image-model survey (2026-06-27)

**Finding, the hard way (this session): Stable Diffusion cannot animate a character.** With
Illustrious-XL + the Gemma LoRA, fixed-seed txt2img tag-swap drifts the whole body every frame;
low-denoise img2img locks the body but won't open a small mouth; masked inpaint opens it but
bleeds artifacts (open eyes through a blink mask) and can't resolve >2 mouth-openness levels. So
SD is demoted to a **single-frame pose library** (`gemma-pngtuber`). We then tried
**externally-authored registered grid sheets** (GPT-image: 2 eyes × 6 mouth, perfectly aligned —
`gemma-chibi`) — the registration is near-perfect, but **flapping those frames still doesn't feel
good** (the call: image-gen, even frontier, isn't the right tool to ANIMATE a recurring host).
**Decision: until an Inochi2D rig, the host is a STATIC pose + an audio-reactive bob/light-up; no
per-frame generation drives animation.** That reframes the question from "animate frames" to
"generate good STATIC poses cheaply + on-model" — and *eventually* mesh-rig one drawing. So the
local-model question is really: **what's the best LOCAL, commercial-safe model for consistent
on-model character POSES** (and as a stretch, identity-preserving edits to seed an Inochi2D base)?
Survey of the 2026 options:

| Model | License | Fits lame? | Fit for the rig task |
|---|---|---|---|
| **Qwen-Image-Edit (2511)** | **Apache-2.0** | ~20B → GGUF/FP8 on 16GB 7800XT | **Best commercial-safe local EDIT model.** Identity-preserving instruction edits ("open mouth"/"close eyes") on a fixed reference; 2509→2511 markedly improved person consistency. Preserves *identity*, not pixel-exact body ⇒ still needs the baseline-align pass. |
| **OmniGen2** | **Apache-2.0** | ~7B, fits | Subject-driven / in-context: re-renders the character in new poses from a reference. Great for a *pose library*, weaker for pixel-locked flap frames. |
| **FLUX.2 [klein] 4B** | **Apache-2.0** | ~13GB on the 7800XT | General gen, commercial-safe, Qwen3 text encoder; consumer-grade. Already noted in the Image table. No special rig trick. |
| **HiDream-I1** | **MIT** | large; quantize | Multi-reference control (up to 10 imgs) + JSON/HEX control for repeatable scenes — promising for consistency, untested here. |
| FLUX.1/2 **Kontext** [dev] (incl. the 3×3 expression-sheet LoRA) | **non-commercial** | — | The slickest expression-sheet tool, but **excluded** (monetized output). It's what GPT-image is doing for us, license-free. |

**Recommendation.** (1) **Animation = Inochi2D** (Tier B above), not generation — one authored
drawing → mesh-deformed mouth/eye/head, infinite states, zero inter-frame drift, tiny VRAM. That's
the real path for a recurring host; everything below is just for the STATIC poses until then.
(2) For static poses, the current SD rig is fine; for *better* on-model poses on a LOCAL,
commercial-safe model, trial **Qwen-Image-Edit-2511** (Apache) — strongest identity-preserving
editor, good to vary pose/expression off one canonical Gemma drawing (and to seed an Inochi2D
base). **OmniGen2** (Apache) is the alternative for subject-driven pose variety. (3) **HiDream-I1**
(MIT, multi-ref) is worth a look for tight consistency. Avoid FLUX Kontext (non-commercial) despite
its slick expression-sheet LoRA. Net: stop trying to *generate animation*; generate good stills,
animate with a mesh.
- **Verify**: Qwen-Image-Edit GGUF quality + VRAM on the 7800XT; whether it holds the chibi LoRA
  style; HiDream-I1 multi-ref consistency on chibi.

## Engine — the NLE itself

**Pick: C++ · Dear ImGui · D3D11** (→ Vulkan later), mingw-w64 cross from Nix, Windows
PE via WSLInterop. First-class ffmpeg/NVENC/NVDEC interop, non-fragile, the reference
stack for an ImGui NLE. **Rejected**: Zig (pre-1.0 churn, no video ecosystem — revisit
after 1.0), Rust/egui/wgpu (wgpu fights zero-copy NVENC; an egui perf regression noted).

Core architecture adopted (see `ARCHITECTURE.md`): pull-based DAG + param-hash GPU
texture cache; dirty-subtree invalidation; one graph, two `RenderContext`s
(async half-res preview / deterministic full-res export); background pre-render; proxy
media. Effects = GLSL shader + param manifest (transpiled to HLSL), not a bespoke DSL.
Animation = cubic-Bezier keyframes + critically-damped springs. Capture = DXGI Desktop
Duplication (WGC later). Pin ffmpeg to a release; pool NVENC sessions (consumer cards cap
concurrent encode sessions).

## Excluded-for-commercial summary (do not bundle as default)
NoobAI XL, FLUX.1/Kontext [dev], MusicGen, Fish S2 weights, Higgs Audio weights,
HunyuanVideo (geo-locked), LTX-Video weights (rev-gated), THA4 models, Live2D SDK,
InsightFace (LivePortrait dep). Some are fine for *personal/non-monetized* experiments if
the user opts in later, but they are not the shipped defaults.
