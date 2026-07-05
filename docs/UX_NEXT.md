# UX NEXT — Video-Essay Pipeline

Read after `CLAUDE.md` and `docs/STATUS.md` when working on editor UX. Current target:
technical video essays / deep dives with Gemma as a cute pngtuber host, chuuni
succubus-cosplayer persona, reaction images, memes, code excerpts, and sourced B-roll.

## ★ SHORTS-POLISH HANDOFF — DONE 2026-07-03 (all three queued tasks landed)
The 2 shorts live in `../slopstudio-projects/luckymas/luckymas-short{1-copybar,2-ghostgirl}.slop.json`.
All three queued tasks are done + verified (renders on the llm-feed). Details in `docs/STATUS.md` (top).

**1. ROOM-SHOT FRAMING — FIXED via a per-sprite FACE-BOX gizmo + auto-seed.** DIAGNOSIS (as instructed):
it was a face-DETECTION bug, not the art. `framing:bust` is face-anchored (`avatar_fit`, `scl ∝ 1/faceW`),
but the pale-skin detector (`face_from_pixels`) measures skin spread over the upper **58%** of the figure —
so poses baring more shoulder/chest/arm skin (smug, neutral) report a WIDER "face" (smug 559 vs explaining
422 px) → render **0.75×** at the same scale. FIX (owner's steer — "a gizmo to fine-tune face detection per
sprite"): a per-sprite sidecar `face:{cx,eyeY,w}` (SOURCE px) OVERRIDES the detector (`avatar_face`);
authored by the **inspector "Tune face box" gizmo** (drag box = move, edges = width, yellow = eye-line;
`draw_face_box_gizmo`); **`tools/gen-face-boxes.py`** seeds a rig with a head-band TRUE-face-width (fixes
size) + a width-tied eye-line (fixes vertical). Standing room-shot poses now match to ±2.5% at scale 1.0.
Owner's per-clip scale hacks reset to 1.0 (short1 b01/b03/b09). Face sidecars are STOCK (packed by
`pack-stock-assets.py`; regenerable via the tool) — the rig art is gitignored.

**2. FOOT CONTACT-SHADOW — DONE (short2 b08).** Root cause: the auto shadow is gated `!spriteFloating &&
!faceShot`; b08 is `confused`→`confused_float` (floating) AND bust (feet off-frame). New explicit per-clip
**`foot_shadow`** (bool/intensity) FORCES it on for any pose/framing (`editor/src/main.cpp`, inspector "foot
contact-shadow" checkbox + strength). b08 reframed to a small **floating** figure (scale 0.58) casting a
ground shadow on the floor — a floating "ghost" grounded (thematic for the beat).

**2b. SHADOW REALISM — the shadow stays on the GROUND now (2026-07-03 follow-up, owner ask).** It used to ride
UP with the sprite because `feetY` was derived from `a.y`, which carries the breathing/talk **bob** (`yoff`). Now
the draw captures the bob (`bobOff`) and the shadow (a) subtracts it so it sits at the fixed **rest ground line**
(the sprite lifts, the shadow does NOT), and (b) as she lifts, the shadow spreads a touch **BIGGER** (`liftScl =
1+0.14·nl`) and fades **DIMMER** (`liftDim = 1−0.34·nl`), `nl` = upward displacement normalized over the bob range
— the realistic penumbra-grows-as-you-rise read. Verified via anti-phase render (feet lift off a stationary,
larger, dimmer shadow). Subtle at the shipped `bob:1`; obvious under an exaggerated bob.

**3. RETIME BLACK-GAP — FIXED (`slop.py cmd_retime`).** A `--generate` that resized the outro VO in place
(dur already == new long audio) left the warp with no span change → the visuals/bed behind the outro stayed
short → black. New guard: after retime, extend the last beat's BACKING layers (host, cover backdrop, filler,
bed) to the VO end; timed content (captions/transcript/code) untouched; 50ms threshold ignores sub-frame
rounding; idempotent. Verified by simulation.

**⚠ owner to sanity-check (subjective, took UX liberty):** (a) b08 is now a small floating figure (needed to
show feet+shadow) rather than a matching bust — revert framing if you wanted it prominent + just the toggle;
(b) short1 b01/b03 were reset 1.40→1.0 (they're CONTENT beats, not in the named room-shot list — the 1.40
double-counted once the width bug was fixed). **Shorts RE-EXPORTED 2026-07-03** with the shadow-realism fix +
short1's swapped-in seed-1 b05 take (retimed) — `exports/luckymas-short{1-copybar,2-ghostgirl}.mp4`.

## Product Direction

The editor should collapse most authoring into a few primitives that are pleasant by
default and still JSON-addressable by an LLM:

- **Line**: VO text + optional emotion/persona note. Generates speech, drives host talk
  motion, and anchors pacing.
- **Host**: one row with a swappable static-pose rig. Clip emotion may be explicit or
  `auto`; missing rig poses fall back through the rig fallback pose, then procedural chibi.
  Audio drives bob/light-up only; mouth/blink animation is reserved for future Inochi2D.
- **Media**: image/video with stock Ken Burns motion, optional auto grade, framed inset
  treatment, low default source audio, and a filler/background layer when not fullscreen.
- **Native overlay**: code, caption/text panel, shape/callout, diagram, gradient, blur.
  These must stay compositor-native, keyframeable, and fast.
- **Scene transition**: use only two default scene changes unless a beat truly needs
  custom motion: fade-to-black/fade-back, or blur-swap-under-blur. Host transitions should
  slide off/on from the closest screen edge.
- **Asset variant**: generated/imported assets stay non-destructive, with prior takes
  visually browseable from sidecars/history.

## What Already Exists

- Stock Ken Burns motion for image/video (`motion`, default `zoom_in`) unless manual
  transform keyframes exist.
- `filler` clips: cover-scaled heavily blurred backdrop from a source/auto media asset.
- `blur` clips: full-frame blur transition layer.
- Image/video/avatar glow and auto-grade; avatar contact shadow and scene vignette.
- Gated RMS normalization for generated audio rows and low-volume video source audio.
- Library avatar rigs with prefix + per-emotion overrides, including custom emotions such
  as `teaching`.
- Crop/mask/remove-bg sidecars for non-destructive cleanup.

## Current Pass

- Library/Viewer are no longer floating over the workspace. The Library content is a docked
  left Media pane, and the old Viewer is a right-side Asset detail tab beside the clip
  Inspector. Generation recipe/history, image paint/crop/remove-bg, audio audition, and rig
  editing still use the same controls, but they no longer cover the preview or timeline.
- Avatar sprites ALWAYS composite whole — framing never pixel-crops. The compositor detects the
  face (pale-skin heuristic, robust across front/3-4/float poses) and anchors framing on it:
  `bust` = half-bust (feet run off the bottom frame edge), `closeup` = tight face, both positioning
  the FULL sprite (no crop). Default is `full` (authored transform); `bust`/`closeup` opt-in per clip
  with pos/scale tweaks layering on top. The emotion-pose panel zooms each thumbnail to the detected
  face. (The face anchor is the single source of truth for all framings, per the user's request.)
- Main `luckymas` host row uses `gemma-gpt-static`, built from
  `/mnt/f/Pictures/oc/gemma-san/video-sprites` as 3x2 static pose sheets. Swapping back to
  `gemma-host` or `gemma-pngtuber` remains a row-param edit.
- Loud meme clip `c_meme_lp` is quieter (`video_volume: 0.08`, meme row `gain_db: -3`).
- New imported media defaults to stock motion plus framed/glow inset styling.
- Scale sliders accept negative values so flipped clips remain adjustable.

## Queued — luckymas review (2026-07-01, do first next session)

Render/code bugs (several fall out of the recent face-anchored framing work):
- **Host SNAPS mid-clip (~25s) — DONE.** The per-frame content-side eval flipped partway through the
  host clip (the CRT `c_vid_crt` appears 0.3s into `c_av4`, so she started centered then jumped left).
  Fixed in `composite_frame`: a new `content_centroid_span(p, c.start, c.start+c.dur, …)` decides each
  host clip's presenter alignment ONCE over its WHOLE span (content overlapping at ANY point during the
  clip → stable, no mid-clip snap); the user splits the clip to change pose/position. Verified headless:
  `c_av4` holds LEFT both before + after the CRT appears; solo `c_av3` stays centered (no content
  overlaps it). (Optional future nicety: animate the offset for a smooth move instead of a hard hold.)
- **Horns cut off on float poses — DONE.** It was a sprite-PROCESSING clip, not the anchor (the
  user confirmed: horns cut even when resizing). The float-row figures' thin horns fell below the
  band-coverage threshold, so the cell's top edge beheaded them. Fixed in `tools/process-pose-sheet.py`
  (`_expand_bands`: grow each band to where the projection actually hits zero — past the threshold to
  the empty inter-figure gap), reprocessed all 13 sheets → full horns on every float pose, standing
  poses unchanged.
- **Blur transition snaps (~196s) — DONE.** The `blur` clip rendered its `source:"auto"` plate
  COVER-scaled + CENTERED (full-frame), ignoring the live clip's transform — so the Konata inset
  (`cf_v_s1`, pos[360,2]/scale1.714) showed zoomed + centered during the blur, then jumped to its
  inset position when the blur ended. Fixed in `composite_frame`: the auto path now captures the
  source CLIP and renders the blurred plate at ITS transform (pos/scale/anchor + Ken Burns + transition),
  with the opaque backing over the plate's rect (a full-frame source still covers the frame, so a real
  scene cut is unchanged). Verified: Konata holds the inset transform and deblurs IN PLACE across
  196.5→197.6 (no snap). Explicit `source:<asset>` still uses a centered cover-scale (full-frame backdrop).
- **Vignette 1px vertical gap on the LEFT edge — DONE.** Not the gradient/vignette rect itself (the
  bands draw from the frame edge symmetrically) — it was the PREVIEW's frame rect: `f0`/`fw`/`fh` were
  fractional (`(sz.x-fw)*0.5`), so the bg fill, clip-rect scissor, and vignette bands fell on sub-pixel
  boundaries and the GPU scissor rounding left one bright bg column at the left edge. Fixed by snapping
  the preview rect to whole pixels (`floorf`); the export path already passes integer `f0=(0,0)`.
  Verified before/after (left-edge column x361: 675→45; seam gone).

Cut / content edits (all luckymas edits below are in the WORKING TREE, uncommitted alongside the
user's review edits — see NOTE; the assets are committed, the .slop.json is left for the user to commit):
- **~215s — DONE.** Replaced the static `wallpaper.jpg` inset with the **CRT-video segment scrolling the
  Lucky*Mas Wallpaper-Picker page in IE** (`assets-src/luckymas-crt-wallpaper.mp4`, extracted from
  `luckymas-crt.mp4` ~94.3–99.7s; gitignored/regenerable via `ffmpeg -ss 94.3 -t 5.4 … scale=1280:720`),
  then the actual wallpaper as the tail (`c_vid_wall` video 213.8–219.2 → `cf_v_s3` 219.4–227.3).
- **~228s — DONE.** Dropped the CRT stills: the "converts BPM→ms" beat (`cf_v_s4`) now shows the clean
  **`calc-imas.png`** screenshot (the calculator + Task-Rate convert dialog) as a framed inset; the
  "touch a character in the chest" beat (`c_sy_h5`) shows **Kotori cropped from `calc-convert.png`**
  (`calc-kotori.png` — her hand-on-chest UI) + the **host CONFUSED with a "clueless" label + arrow**
  pointing at her (`c_av_h5`/`c_clueless`/`c_clueless_arr` on a new shape track above the host). Blurred
  fillers behind both (no bare black). Screenshots from `../LuckyMasterEN/docs/screenshots/`.
- **~286s — DONE.** Replaced the badly-cleaned static teacher image (`c_tch2` = `gemma-teacher.png`,
  which had a light halo/white-blob at the feet) with a clean animated rig clip (`c_tch2_av` on
  `r_avatar1`, rig `gemma-gpt-static`, emotion `teaching`, framing `raw` at the same placement —
  scale figure-matched to the old image; driven by `r_vo` so it bobs/lip-syncs).
- **BitBlt comparison — DONE.** Split the static A/B (`xp-bitblt-compare.png`) into a live pair: LEFT =
  the mascot MOVING (`c_xp_move`, `xp-mascot.mp4` looping on `r_video`), RIGHT = the cropped empty-desktop
  panel (`c_xp_empty`, `xp-bitblt-empty.png` = the compare's right half; gitignored/regenerable). Motion
  sells "she vanishes in the naive capture." (Framings aren't pixel-matched — per the user, crop existing.)

Defaults to distill:
- **Content-image default size/placement — DONE.** Distilled from the hand-tuned beats
  (cf_v_h8/cf_v_s1 sat at pos≈[360,~0], the common inset X with the user's "down a touch"): dropped/added
  image clips (`add_image_clip_at`) now default to a **right-of-center inset at pos [360,0], fit to ~half
  the frame** (`min(0.55·W/nw, 0.72·H/nh)`, clamped 0.2–2.0) instead of full-frame-centered — so a media
  drop lands as an inset complementing the lower-left host + the inset frame. Falls back to native scale
  if dims can't be probed. (c_xp_copyhijack's rightward nudge was an outlier, not folded into the default.)

NOTE (resolved 2026-07-02): the luckymas cuts moved to the projects monorepo
(`../slopstudio-projects/luckymas/`) with all user edits committed there.

## Near-Term Task List

- Replace the plain keyframe editor with lanes/curves for common params: position, scale,
  opacity, blur, typewrite, diagram reveal, volume.
- One-click authoring — **largely DONE 2026-07-02** via the render-time `layout` param
  (inset/fullscreen/fit, inspector combo; drops default to inset), Clip ▸ Add special clip,
  and the `slop.py skeleton` beat compiler (line+host, media insets w/ auto filler, captions,
  code, sections as blur-swaps). See `docs/LLM_WORKFLOW.md`.
- Filler-under-inset is now the skeleton default (black is unauthorable there); a per-scene
  opt-out remains a manual delete.
- Implement host edge-aware slide-away/slide-back as a reusable transition preset.
- Add a scene-transition helper that alternates fade-black and blur-swap for variety.
- **Whole-composite blur/fill — DONE 2026-07-01.** The `blur` clip is a true full-frame post-process
  (blurs every layer), and the `filler` now samples the whole foreground composite (a tiny RT bilinear-
  upscaled — see `render_fill_backdrop`), so neither blurs a single selected plate any more: the fill
  tracks all on-screen motion and never collapses to a flat grey/black wash.
- **Host slide-out "bounce" between per-line clips — DONE 2026-07-01.** `clip_transition` now hard-cuts
  back-to-back same-row clips (gap <0.06s) instead of sliding out-and-in, so a host segmented per VO line
  no longer flickers out (which also fed the filler void). (Note #129's edge-aware slide preset is still
  a separate, opt-in idea for *intended* slide-aways.)
- Make code showcase robust for large snippets: max-lines viewport, scroll keyframes,
  auto font sizing, and line-highlight presets.
- TTS emotion gating: DONE in practice via per-preset mode — a clone voice (preset has a golden
  `ref`+`ref_text`) drops the per-line emotion from the job (`speech_params`) and the inspector
  shows the emotion field as "emotion / host pose" + a hint that it's script/pose metadata, not TTS
  delivery; a design voice still sends it (and supports it). A *structured* provider capability flag
  is deferred — emotion support is per-voice-mode, not per-backend, so it'd need protocol + editor
  capability-fetch infra for marginal gain over the current ref-based gating.
- Smart sprite processor: the **batch/LLM surface is DONE** — `tools/process-pose-sheet.py`
  infers the 3x2 grid from alpha gaps, mattes **each cell with rembg (isnet-anime) by default**
  (2026-07-01; `--local` falls back to the adaptive saturation-aware neutral-grey key, which also
  finds the grid), centers cells on a 512² canvas, pose-names them, face-checks each cut, and merges
  the manifest. Remaining: an **in-editor "smart auto" button** wrapping the same steps (the existing
  Tools ▸ Sprite sheet stays the manual one-off flow), and optional edge-debris/connected-component
  cleanup if a future sheet needs it.
- Audit dead/duplicated paths: legacy proxy-only video assumptions, stale persona notes,
  duplicate avatar rows, and old one-off opening scaffolds.

## Design Rules

- Prefer compositing primitives over generated/baked visuals for text, code, diagrams,
  transitions, grades, borders, and motion.
- Prefer row/clip params with good defaults over bespoke keyframes.
- Every new effect/clip type needs a narrow JSON surface, an inspector surface, and an LLM
  command/headless path when practical.
- Preserve old generated files and edited variants through sidecars/history; avoid destructive
  replacement unless explicitly requested.

## Suggested `/clear` Point

Use `/clear` after this pass is committed or saved in a handoff. Resume with:
`CLAUDE.md`, `docs/STATUS.md`, then this file. Next self-contained session should start at
the keyframe-lanes or smart sprite-processor task, not continue accumulating context.
