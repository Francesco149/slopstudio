# slopstudio — STATUS (the live front)

Hand-maintained "what's true right now." **Read this first after `CLAUDE.md`** and update it
in the same change that lands work, so a fresh session reorients in ~60s. Last updated:
**2026-07-05**. For the next editor UX/product pass, also read `docs/UX_NEXT.md`; for
composing a video as an agent, **`docs/LLM_WORKFLOW.md`**.

> **★ NEWEST (2026-07-05): THREE-REPO SPLIT + social scheduler + thumbtool gesture perf.**
> Gemma/brand/channel/social content moved to **`../gemma-branding`** (own local-only repo; its
> `README.md` is the map): CHARACTER/PACKAGING/SOCIAL playbooks + the 3 research reports +
> `brand-package/` (ex `../slopstudio-projects/branding/gemma`; thumb docs re-pointed) + the social
> post queue. The gemma skills under `.claude/skills/` are now shims pointing there. New
> `tools/social.py` = the owner's daily posting check-in (`status`) over that queue — ops doc
> `../gemma-branding/SOCIAL.md`. thumbtool: live-gesture quarter-res path + cache bypass killed the
> resize/border-drag chug (`08ef49a`); released/settled output byte-identical. Docs de-staled
> (models: GPT art primary for host, Anima backup + backdrops; Inochi2D = planned-not-supported;
> HANDOFF.md deleted). Master re-scrubbed to a single clean commit (backups: `master-pre-scrub-2026-07-05`,
> `master-legacy-full`); `.githooks/pre-commit` (core.hooksPath) blocks non-tool content from this repo.

> **★ NEXT SESSION:** shorts-polish is DONE + **both shorts RE-EXPORTED 2026-07-03** (`exports/luckymas-short{1-copybar,2-ghostgirl}.mp4`).
> Latest pass: **contact-shadow REALISM** — the ground shadow no longer rides up with her bob; it stays on the
> floor and grows slightly bigger + dimmer as she lifts (see the top "Built + working" entry). short1 also got the
> owner's swapped-in **seed-1 b05 take** (retimed → clean, 56.0s). Two earlier owner sanity-checks still stand
> (subjective, UX liberty): (a) short2 b08 is a small floating figure w/ a ground shadow (was a bust) — revert
> framing if you wanted it prominent; (b) short1 b01/b03 reset 1.40→1.0. Owner may fine-tune face boxes via the
> inspector "Tune face box" gizmo (then re-run `pack-stock-assets.py` to bake sidecars into the stock pack).
> **Also this session (owner ask): 3 deep-research reports → durable docs.** `../gemma-branding/PACKAGING.md` (YouTube
> titles·thumbnails·branding·engagement playbook, VTuber-audience-tuned) + `docs/AGENT_TOOLING.md` (skills +
> `slop.py` builds for better editing/scripting/design), backed by `docs/research/{youtube-packaging,thumbnail-pipeline,llm-video-tooling}-2026-07.md`.
> **Locked YT brand kit** (avatar + banner) lives at `/mnt/f/Pictures/oc/gemma-san/branding/` — palette/wordmark/pillars
> extracted into PACKAGING §4. **Fixed a monetization landmine:** SANA *weights* are NVIDIA research-only → use
> **Qwen-Image (Apache)**; blacklisted the InstantID/InsightFace face-ID stack + Impact font (CLAUDE.md/RESEARCH.md).
> ~~full-video (luckymas3) music-choice + timing tweaks~~ → **DONE (owner, 2026-07-05): luckymas3 is READY
> FOR UPLOAD.** ~~build the thumbnail pipeline~~ → **DONE 2026-07-05 as slopthumb** (top "Built + working"
> entry + `docs/THUMBNAIL_TOOL.md`); 3 A/B thumbnail variants for luckymas are cut + lint-clean in
> `../slopstudio-projects/luckymas/thumbs/`. **NEXT: lock the real title (packaging-first checklist — the
> thumb docs carry a placeholder title pairing; update via `thumb.py docset title=…` + re-lint), pick 2–3
> finalists → upload → YouTube Test & Compare.**
> **★ NEWER (2026-07-04): SONG CREDITS — auto "now playing" chip + ID3 auto-metadata on music import + editable
> song meta in the Inspector + `slop.py musicmeta` backfill + `export.sh` writes a paste-ready `<video>.credits.txt`.
> luckymas3 backfilled (Twisting/Hard Boiled + all beds now have title/artist). See the top "Built + working" entry.**
> **★ NEWEST (2026-07-04, owner feedback on the chip): it now measures the ACTUAL text bounding boxes on screen
> (fixes a hand-moved plate slipping past the old style-heuristic), stays in its start corner and shifts VERTICALLY
> to clear them, and EASES to the new spot with a motion-blur trail (spring, deterministic in export); the pill's
> broken accent stripe is gone (the ♪ is the accent). Plus TIMELINE EDGE-SNAPPING (drag/trim snaps to nearby clip
> edges + playhead; hold Alt to bypass) so host clips drop flush and their pose-swap slide fires.**

## Built + working (verified)
- **★ THUMBNAIL TOOL (slopthumb) + BRANDING SYSTEM — NEW 2026-07-05 (owner ask: branding session).**
  A **separate app** from the editor: `thumbtool/` → `build/slopthumb.exe` (mingw PE, ImGui/D3D11 shell,
  **CPU stb compositor** ⇒ headless export needs no GPU, GUI/CLI renders byte-identical). Full spec:
  **`docs/THUMBNAIL_TOOL.md`**. The tool is **brand-agnostic**: palette/fonts/styles/sticker/sprites/
  watermark/templates/lint live in a **brand package** dir the `.thumb.json` doc points at — the
  @GemmaExplains package is `../gemma-branding/brand-package/` (**palette hexes LOCKED, sampled from
  the rig art**), a generic demo package + doc is committed at `examples/thumb-demo/` (self-demonstrating).
  Layers: bg (gradient/image+blur/vignette) · image (sticker outline via distance transform, glow, shadow,
  flip) · text (brand styles, stroke/gradient/plate/auto-fit, Anton/Bebas/Archivo OFL) · shapes
  (arrow/circle/rect SDF) · watermark. GUI: layer list/inspector/palette swatches/sprite browser/brand
  templates, drag-move + ctrl-wheel scale, **live 168px squint inset**, **history panel** (sibling docs =
  A/B variants + `history/` snapshots with PNG previews), **undo persisted to `<doc>.undo.jsonl`**
  (survives sessions; external edits land as undo steps), **hot-reload on doc mtime** = the LLM-authoring
  loop (verified live: sed the doc while the GUI ran → canvas updated + "hot-reloaded (external edit)").
  Agent CLI: **`tools/thumb.py`** (new/overview/set/add/rm/order/render/lint/snapshot/variants; render
  pushes PNG + 168px proof to the llm-feed; **lint gates**: ≤3 words, no title-word repeat, duration-corner
  clear, subject-size warn). **3 A/B variants for luckymas authored + committed**
  (`../slopstudio-projects/luckymas/thumbs/a-reaction|b-artifact|c-deadpan`, all lint-clean, on the feed).
  New research: **`../gemma-branding/research/vtuber-branding-2026-07.md`** (VTuber thumbnail meta, ずんだもん解説 genre
  proof, growth flywheel) — conventions folded into the gemma-brand + packaging-first skills + PACKAGING §3.
  NEXT here: ComfyUI expression-render stage (new exaggerated-expression gens), VLM-judge (frame-critic on
  proofs), oshi mark + tag kit + clipping-guideline (research §8 Tier 2).
  **Same-day follow-ups (owner feedback):** `mosaic` censor layer + shape gradient fills + doc-level lint
  overrides + punctuation-only marks don't count as words; **6 A/B variants for luckymas** (a/a2-marks/
  b/b2-marks/c-Joi-meme/d-censor-gag — owner: marks meta = most eye-catching; luckymas3 is READY FOR
  UPLOAD, title TBD). **GPU-ACCELERATED PREVIEW** (owner: dragging lagged): per-layer **block cache**
  (param-hash excludes position → drags never re-rasterize) + **D3D11 compositor** (premult rotated quads,
  mosaic quantize shader, offscreen RT the preview shows directly; toolbar shows build/gpu ms). CPU compose
  stays the deterministic export path — **verified byte-identical pre/post refactor** (cmp on c-deadpan).
- **★ PROJECT ANCHORS: per-project tunable category base positions — NEW 2026-07-05 (owner idea).**
  `meta.anchors` (category → base [x,y]) + `params.anchor` on clips: the editor renders **anchor +
  transform.pos**, so a clip's pos is an OFFSET and ONE Project-panel knob nudges the whole category in
  THAT project only; no param → absolute pos (full back-compat — luckymas3 untouched). Editor: anchor
  applied at the central cx/cy transform read (preview = export), Project panel "anchors" DragFloat2
  knobs, inspector shows "offset from anchor …". `slop.py`: skeleton seeds per-format defaults + tags
  host clips (`bust`, `code_host`); transcript tags chunks (`tr_room/tr_scene/tr_content/tr_code`).
  Defaults distilled from the owner's luckymas tuning: **landscape bust [0,-104]** (median of 53
  hand-nudges — busts sat too low), **code_host corner [660,194]** (was 320), **portrait tr_scene
  [0,-448]** (the b02 ghost-girl band: room beat WITH a beat-authored scene backdrop, detected via a
  non-`c_*` r_bg clip; solo-room stays tr_room -673). Both shorts regenerated onto anchors (positions
  byte-identical to the owner's tweaks) + re-exported. Docs: PROJECT_FORMAT §meta + §clips.
- **★ TRANSCRIPT TIMING v3 (same day): global pause↔sentence matching + wav-fallback visibility.**
  v2's nearest-gap snap (0.8s window) crossed assignments on staccato runs — short2 b02 "No grey box /
  No title bar" grabbed the wrong pause (owner report). `_sent_cuts` now chooses cuts GLOBALLY:
  prefer the most pause-cuts, then min Σ|sentence speech dur − spoken-char share|, pause-cut admissible
  only within a 0.3×–3× duration band (rejects mid-sentence comma pauses posing as boundaries); a
  boundary with no plausible pause stays char-proportional ("free"). Verified b02 all 4 boundaries on
  their true pauses (6.184/7.292/8.446/12.492 play) with the comma pause skipped; b08 unchanged-correct
  incl. the skipped dramatic pause; b10 "Go" exact. Also: `transcript` now prints a per-run
  **"timing: N wav / N viseme / N linear"** line — the wav→viseme fallback is a silent quality cliff
  (scratch-dir copies resolve `cache://` but not project-relative `assets/…` uris → visemes → Rhubarb's
  B-shaped pauses invisible → drift; run transcript against the real project dir).
- **★ TRANSCRIPT TIMING v2: wav-RMS pauses + spoken-char weights + sentence snap — NEW 2026-07-05 (owner:
  short2 "Half your capture" popped at 2.76s instead of ~3.46s).** `tools/slop.py` `transcript_apply`;
  both shorts regenerated + re-exported. Three compounding causes, all fixed:
  - **Chunk weights now use the SPOKEN text** (`_tts_norm`): "2007" is 4 display chars but "two thousand
    seven" out loud, so every chunk after a year started early.
  - **Speech/pause segments now come from the take's WAV** (20ms RMS gate, `_speech_segs_audio`; visemes
    kept as fallback): Rhubarb renders a soft inter-sentence pause as viseme **B (near-closed), not X
    (silence)** — b01's real 0.46s pause was invisible to the old viseme-only path.
  - **Sentence boundaries SNAP to speech-resume after a real pause** (`_snap_bounds`): 1:1 in-order when
    boundary count == big-pause count (the common case), else a tiny order-preserving DP (max matches,
    min distance, 0.8s tolerance) — nearest-gap-per-boundary crossed assignments on tiny sentences
    (short1 "Go." grabbed the wrong pause). Chunks inside a sentence distribute by spoken-char fraction
    over THAT sentence's speech window, so drift can't cross a boundary.
  - Verified: short2 "Half your capture" pops at **3.446** (speech resume 4.48s source ÷1.3); b08's 3
    sentence starts land on their pauses with the mid-sentence dramatic pause correctly skipped; short1
    "Go" pops at its true onset 54.09 (old r_transcript overlap note gone; lint 0/0 + 0/1 pre-existing
    b09 info). Frames at 2.9s/3.7s on the feed.
- **★ RENDER MODAL: format-aware defaults (Shorts no longer downscaled + oversized) — NEW 2026-07-04 (owner).**
  `editor` File ▸ Render video…. The modal defaulted **"Scale to 1080p" ON + target 300 MB** for every project —
  fine for the landscape full video, WRONG for a Short: `--scale 1080` scales HEIGHT to 1080, so a 1080x1920
  portrait rendered as a **squashed 608x1080** AND at ~300 MB (verified on the owner's test renders). Now, on
  open (`IsWindowAppearing`): **portrait → no scale (native 1080x1920) + target `≈dur×0.7 MB`** (a ~60s Short ≈
  40–48 MB, ~5.5 Mbps @ 1080x1920), landscape → the owner's 300 MB pick; the "Scale to 1080p" checkbox is
  landscape-only (portrait shows "native 1080 x 1920"); output-fps clamps to the project's native. (The two
  stale `*-1080p60.mp4` 300 MB test files in `exports/` are the old squashed renders — safe to delete.)
- **★ TRANSCRIPT TIMING: reject stale paired visemes + hold the last chunk to the audio end — NEW 2026-07-04
  (owner: "captions too fast; last one ends before the audio, e.g. the soda-cans line").** `tools/slop.py`
  `transcript_apply`; both shorts regenerated + re-exported.
  - **Root cause:** `b09_vo`'s PAIRED viseme track was STALE — its `dialog` was an OLD take ("Heh~ There's so
    much more…") while the clip's audio is the regenerated "You can find more … soda cans" (9.52s). The char-
    fraction timer trusted it blindly → it spread the new (longer) text over the old (shorter) take's speech
    [0.18–7.81s] → captions rushed and the LAST one ended at 68.02 vs the audio's 69.27 (**1.25s gap**). A VO regen
    left the wrong viseme paired.
  - **Fix 1:** only trust the paired viseme if its `dialog` matches THIS line (normalized; empty dialog still
    trusted for back-compat) — else fall through to a text-matched track, else **linear** timing over the clip.
    Surgical: across both shorts, 18/19 paired visemes still match (all keep speech-aligned timing); only the one
    genuinely-stale b09 falls back to linear (even spread).
  - **Fix 2:** the last chunk now **holds to ~the audio end** (`max(last-viseme+0.35, dur−0.05)`), so it never
    vanishes during a trailing word/pause. Verified: b09's last caption "ALL of your soda cans" now ends at 69.27
    = the audio end (**0.00s gap**), chunks evenly paced across the whole line.
  - **NB (data, not fixed here):** b09's stale viseme also mis-drives the outro LIP-SYNC (avatar mouth follows the
    wrong take). Fixing that needs a re-align on lame (WhisperX) — offered to the owner; the caption path no
    longer depends on it.
- **★ TRANSCRIPT drops trailing periods · space-jazz chip meta · WSL "copy command" everywhere — NEW 2026-07-04
  (owner feedback).** `tools/slop.py` + editor; both shorts backfilled + transcripts regenerated (projects repo).
  - **On-screen transcript strips a trailing full-stop period** (`_tr_display`; a period reads unnatural at the
    end of a big shorts caption) — but keeps `? ! … ,` and all mid-text punctuation (tone/context for a muted
    viewer), and chunk BOUNDARIES still use the original punctuation. Regenerated both shorts (short1 53 chunks,
    short2 76). Verified: no chunk ends in a lone period; "tools cannot see her" renders clean.
  - **space-jazz now-playing chip showed the FILENAME** ("space-jazz") in short2 — its asset carried only
    `attribution`, no `title`/`artist`, so the chip fell back to the stem. `slop.py musicmeta --sidecar` on both
    shorts backfilled title/artist from ID3 → the chip reads **"Space Jazz — Kevin MacLeod"** (verified @t=4).
  - **The Project-panel "Regenerate transcript" + the stock-asset "Fetch" buttons got a Copy-command option**
    (`wsl_command` → `ImGui::SetClipboardText`), same as the render modal — so any nix/bash step is runnable in
    the user's own WSL shell when the nested `wsl.exe` launch fails.
- **★ SONG-CHIP MOTION + BBOX AVOIDANCE · TIMELINE EDGE-SNAP · accent fix — NEW 2026-07-04 (owner feedback on the
  chip).** Editor only (`editor/src/main.cpp`); verified headless (feed has the shots + a motion contact sheet).
  - **★ The chip dodges the ACTUAL on-screen text boxes now (was a style/place guess).** `draw_caption_clip`
    records each drawn plate/caption/transcript's screen rect into `g_frameTextBoxes` (cleared at the top of
    `composite_frame`, read by `draw_song_credit` at the end). Root-cause of the reported miss: a `lower_third`
    the owner had **moved to the top** (`pos:[0,-881]`) — the old heuristic assumed lower-thirds sit at the
    BOTTOM, so it never blocked the top-left corner. Now the chip **stays on its start corner's side and shifts
    VERTICALLY** (a small sweep) until its box clears every text rect; if the whole column is blocked it shifts
    UP above the topmost box. (owner's spec: "stay in the corner it started on, shift vertically… if all
    occupied, shift up".) Sweep-break bug found + fixed (the far-edge test tripped on frame 0 at a top corner).
  - **★ It EASES to the new position with a motion-blur trail, like the host slide.** A critically-damped spring
    integrated over PLAYHEAD time (fixed 1/fps steps in export ⇒ deterministic; snaps on a new song / scrub),
    position stored as a frame-FRACTION (preview-size vs export-size safe). Velocity drives 5 trailing ghost
    copies (same technique as the avatar pose-swap `vx/vy` ghosts). Verified with a new **`--seq-n N`** headless
    flag (render N consecutive frames in ONE process — the only way to see frame-to-frame motion, since each
    `--shot-frame` is a fresh snap): the chill-wave chip eases down + trails as the "per-pixel transparency"
    strap fades in at 365.3s. Skipped in the filler-backdrop pre-pass (`!g_compositeSkipFillers`) so it never
    blurs into the backdrop.
  - **★ Accent fix:** the pill's left accent stripe overshot the rounded corner (a line poking outside the
    edge) — removed; the purple-pink **♪ note IS the accent** now (clean rounded pill).
  - **★ TIMELINE EDGE-SNAPPING (`snap_edge_time`).** Dragging a clip body or trimming either edge snaps the moved
    edge to the nearest clip edge (start/end), the playhead, or t=0, within ~8px — so a host clip drops
    FLUSH against its neighbour and the pose-swap slide fires (it only triggers within ~0.35s; before, a hand
    drag left a sub-px gap that silently demoted it). **Hold Alt to bypass** (place two things deliberately
    touching). A cyan guide line marks the edge it snapped to. Body-move snaps whichever of the clip's two edges
    is closer; trims snap the dragged edge. (Not applied to the Shift-ripple-block move, where Alt already means
    ripple-before.) **Snap targets are the clip's OWN lane + the two ADJACENT lanes only** (owner: was the whole
    timeline = too grabby; `snapRowsFor`).
  - **★ RENDER-FROM-EDITOR → COPY-TO-CLIPBOARD.** Launching the render pipeline via `wsl.exe` from this
    Windows-PE-under-WSLInterop is fundamentally flaky: **nested WSL can't start the systemd user session**, which
    then breaks `chdir` (`CreateProcessCommon: chdir(/opt/src/slopstudio) failed 2`) AND leaves a bare PATH
    (`nix: command not found`) — the `--cd`/PATH patches weren't enough. So **File ▸ Render video… now shows the
    exact paste-ready command in a read-only field with a "Copy command" button** (`render_command` →
    `ImGui::SetClipboardText`); the user runs it in their OWN WSL shell (correct env, watches ffmpeg there). The
    command = `cd '<repo>' && nix develop --command bash tools/export.sh '<proj>' --target-mb N --fps N [--scale
    1080] --out '<out>'` (UNC paths mapped to Linux via `win_path_to_wsl`). **Verified end-to-end**: the emitted
    command wrote the export plan (53485 frames, 4 credits) and ffmpeg began encoding. A small "try launching via
    WSL" fallback remains for setups where nested `wsl.exe` works. (Same nested-WSL limit hits the Project panel's
    "Regenerate transcript" + the stock-asset fetch buttons — they still spawn; copy is only wired for render.)
- **★ SONG CREDITS: auto "now-playing" chip · ID3 auto-metadata on import · editable song meta · `slop.py musicmeta`
  · export `credits.txt` — NEW 2026-07-04 (owner ask).** Editor (`editor/src/main.cpp`) + `tools/slop.py` +
  `tools/export.sh`; luckymas3 backfilled (projects repo). One source of truth — a music asset's
  `meta.title`/`meta.artist`/`meta.attribution` — feeds BOTH an on-screen credit AND the description credits.
  - **★ Auto "now playing" CHIP (`draw_song_credit`, drawn in `composite_frame` → preview AND export).** At each
    song's START a `♪ Title — Artist` pill fades into a corner for ~10s (fade in 0.4s / out 0.7s), then fades out.
    A song INSTANCE = a contiguous run of same-asset music clips (gap >0.35s or asset change = new instance), so a
    **looped bed triggers ONCE** and the **same song reused later triggers again** (matches the per-(file,start)
    bed ids). Gated by **`meta.song_credits`** (default on); **`meta.song_credit_corner`** (`tl` default) +
    **`meta.song_credit_secs`** (10) tune it; a start-clip's **`params.credit:false`** opts one instance out.
    **Dodges title plates:** it marks the corners active caption plates occupy (auto/`term` → top, `lower_third`/
    strap → bottom, explicit `place` honoured) and takes the first free corner from the configured one → vertical
    opposite → horizontal → diagonal. Verified headless: Deadly Roulette @t=6 sits top-left; Hard Boiled @t=200
    auto-moved to bottom-left to clear the "22 character packs" plate (both on the feed).
  - **★ Auto-metadata from ID3 on import (`audio_meta_from_tags`, wired into `add_audio_clip_at`).** A dragged-in
    SONG reads its embedded tags via libav → `meta.title/artist/album/year` + a computed `attribution_text` (a
    `<file>.meta.json` sidecar still WINS). **Kevin MacLeod → the canonical incompetech CC BY 4.0 line**; any other
    artist → a plain `"Title" by Artist` the owner completes with a license (never asserts a license we can't infer).
  - **★ Editable song metadata in the Inspector** (music clips): `title`/`artist`/`credit (description)` InputText
    (writes the ASSET meta — a looped bed edits once), a **Re-detect from file tags** button, and a per-instance
    **show chip** checkbox. Project panel gained a **song-credits** section (toggle + corner + hold-s).
  - **★ `slop.py musicmeta <proj> [--sidecar]`** backfills title/artist/credit on every music asset from its ID3
    tags (gaps only — a hand-tuned credit is kept; `--sidecar` writes the `.meta.json` too). `bed`/`skeleton` now
    seed music meta from ID3 as well as the sidecar. **Description credits are now USED-only + deduped** (an
    imported-but-unplaced song like Twisting isn't credited; a bed reused 3× is listed once) and **`export.sh`
    writes a paste-ready `<video>.credits.txt`**. luckymas3 backfilled: 5 beds get title/artist; export plan = 4
    correct credits (Deadly Roulette · Chill Wave · EDM Detection Mode · Hard Boiled).
- **★ CONTACT-SHADOW REALISM · short1 seed-1 b05 retime · BOTH SHORTS RE-EXPORTED — NEW 2026-07-03 (owner feedback).**
  Editor (`editor/src/main.cpp`) + short1 data (projects repo); both shorts re-rendered.
  - **★ The foot contact-shadow stays on the GROUND now (was riding up with her bob).** Root cause: `feetY` was
    derived from the sprite-quad top `a.y`, which carries the breathing/talk **bob** (`yoff`), so the shadow
    translated up/down with the figure. Fix (single avatar draw path, preview + export): capture the bob into an
    outer `bobOff` (set in BOTH the fit-ok and fallback branches), then in the shadow block (1) **anchor to the
    rest ground line** — `feetY = a.y - bobOff + feetOffset` (the sprite lifts, the shadow does NOT), and (2)
    modulate by the UPWARD lift `nl` (= `lift / (fH·0.045)`, clamped 0..1): the shadow spreads **bigger**
    (`sw2 ×= 1 + 0.14·nl`) and fades **dimmer** (`salpha ×= 1 − 0.34·nl`) as she rises — the realistic
    penumbra-grows-with-height read the owner asked for. Subtle at the shipped `bob:1`; **verified** via an
    exaggerated-bob anti-phase render (feet clearly lift off a stationary, larger, dimmer shadow; compare on the
    feed). Applies to the auto shadow AND the forced `foot_shadow` (e.g. short2 b08's floating "ghost").
  - **short1 b05 = the owner's swapped-in seed-1 take** ("Here's how. It installs one single, system-wide hook.",
    `a_zuz2ttzp7s7vpdud`, 4.32s raw) + a code-card tweak (the `SysAnimate32` compare wrapped in an `if`). The
    owner set `dur` to the RAW 4.32s (not ÷1.3) → desync + 0.12s r_vo overlap; **`slop.py retime`** snapped it
    (b05 −0.68s, clean uniform ripple of the back half, outro boom relative-preserved) → **lint-clean, 56.0s**.
  - **Both shorts RE-EXPORTED** → `exports/luckymas-short{1-copybar,2-ghostgirl}.mp4` (short2 also carries the
    owner's fresh ghost-girl `rot`/`opacity` wobble keyframes, committed as-is — no retime; its only lint note is
    a pre-existing sub-frame b09 desync, unchanged from the already-shipped cut).
- **★ FACE-BOX GIZMO · foot-shadow toggle · retime black-gap guard — NEW 2026-07-03 (shorts-polish handoff,
  the 3 queued tasks).** Editor + slop.py + a new tool; code committed, shorts data committed (projects repo),
  **shorts NOT re-exported yet** (deferred pending owner sanity-check — see UX_NEXT ⚠). Feed has the shots.
  - **★ ROOM-SHOT FRAMING FIXED — per-sprite FACE-BOX override + gizmo.** DIAGNOSIS (as the handoff asked,
    step 0): it's a face-DETECTION bug, NOT the sprite art. `framing:bust` is face-anchored (`avatar_fit`,
    `scl ∝ 1/faceW`), but `face_from_pixels` measures pale-skin spread over the upper **58%** of the figure,
    so poses baring more shoulder/chest/arm skin report a WIDER "face" (smug **559** vs explaining **422** px)
    → the fit shrinks them (smug rendered **0.755×** at the same scale; owner had hacked scale→1.38 to fight
    it). FIX (owner's steer mid-session — "a gizmo to fine-tune face detection per sprite"): a per-sprite
    sidecar `"face":{cx,eyeY,w}` (SOURCE px) OVERRIDES the detector — `avatar_face` reads it first (hoisted
    `g_faceCache` + `invalidate_face_cache`; `sidecar_face_override`). Authored by the **inspector "Tune face
    box" gizmo** (`draw_face_box_gizmo`, behind a per-session checkbox in the Emotion-poses panel): the current
    emotion's sprite on a checkerboard with a draggable box — **body=move (cx/eye)**, **edges=width**, yellow
    line = eye-line; non-overlapping hit regions; writes the sidecar + invalidates the cache live so the
    preview updates as you drag; "Reset to auto-detect" clears it. **`tools/gen-face-boxes.py`** seeds a whole
    rig: it faithfully ports the C++ detector for cx, and measures a **head-band TRUE face width** (skin spread
    ABOVE where the shoulders widen it — fixes SIZE) + a **width-tied eye-line** (`headTop + 0.55·w` — fixes
    vertical FRAMING, was the old full-body scan's inconsistent fy-range). Ran on gemma-big → 13 `.png.meta.json`
    sidecars; standing room-shot poses (smug/explaining/teaching) now match to **±2.5%** at scale 1.0
    (verified: short1 b06/b09/b10 + short2 b02/b09 render identical host size + eye-line). Owner's per-clip
    scale hacks reset to 1.0 (short1 b01/b03/b09). Sidecars are STOCK (auto-packed by `pack-stock-assets.py`,
    which walks the rig dir; regenerable via the tool) — consistent with the rig art being gitignored.
  - **★ FOOT CONTACT-SHADOW toggle (short2 b08).** The auto contact shadow is gated `!spriteFloating &&
    !faceShot`; b08 is `confused`→`confused_float` (floating) AND bust (feet off-frame) → doubly gated off.
    New explicit per-clip **`foot_shadow`** (bool or intensity) FORCES the shadow on for ANY pose/framing
    (inspector "foot contact-shadow" checkbox + strength slider, next to auto-grade). b08 reframed to a small
    **floating** figure (framing:floating, scale 0.58) casting a ground shadow on the floor — a floating
    "ghost" grounded, thematic for the "clueless narrator" beat.
  - **★ RETIME BLACK-GAP GUARD (`slop.py cmd_retime`).** When a `--generate` resizes the outro VO in place
    (dur already == the new longer audio), the warp sees no span change for the last beat → the visuals/bed
    behind the outro stay short → the outro line + boom play over BLACK (hand-fixed 3× before). New guard:
    after the warp + VO-dur snap, extend the last beat's BACKING layers (host `r_av`, cover backdrop, filler,
    music bed) to the VO end; timed content (captions/transcript/code/shapes) untouched; a **50ms threshold**
    ignores sub-frame rounding; idempotent (2nd pass = no-op). Verified by simulation (stranded backing at
    66.66 → all extended to 68.98 to cover the re-snapped VO; transcript chunk unchanged).
- **★ SHORTS TOOLING PASS: SFX-cue visibility · delete-track · transcript regen · anchor fix · voice/bgm
  defaults — NEW 2026-07-03 (6th feedback pass).** Editor + slop.py; committed (code repo + short1):
  - **SFX cues are now VISIBLE + editable** (were params-only, invisible on the timeline): an orange flag
    with the cue name draws on the clip at `clip.start+sfx_at`, DRAGGABLE to retime (handle submitted before
    the body button); inspector "SFX cue" section (type dropdown / at(s)+abs-time readout / gain / duck).
  - **Delete a track** (rows + all their clips): a red `x` in each track header next to the reorder `^`/`v`
    (guarded ≥1 track); `delete_track()` mirrors delete_clip, applied deferred at frame end.
  - **Transcript:** a "Regenerate transcript" button (Project panel, portrait) saves + runs `slop.py
    transcript`; the file-watch live-reloads. `transcript_apply` now lands r_transcript in its OWN track
    floated to the TOP (was under r_cap).
  - **Avatar anchor bug FIXED (took two tries):** the presenter side-offset (`autoOffX`) is a LANDSCAPE idea
    (host beside content); in PORTRAIT the host stacks BELOW the content, so a horizontal offset just shoves
    a `[0,0]` host off-center with no way back (user: "pos says 0,0 but she's on the left" @18.77s). Now gated
    to landscape only (+ the earlier full-default gate). Portrait `[0,0]` = centered.
  - **★ LAYOUT PASS on both shorts (7th feedback) — room/host safe-margins + transcript-to-top.** The
    solo/ROOM host no longer fills edge-to-edge — sized + eye-lined to the middle band (horns below the top
    status strip, body above the bottom controls strip). **Room-scene transcripts ride the TOP**
    (`transcript_apply` default flipped from `[0,650]` to `[0,-673]`; both shorts regenerated). **short2
    content beats pushed down +337** for the top margin (mirroring short1's b01-b03). short1's owner outro
    (boom on "ideas" @4.32, music fade to −49 dB before the dark-room gag) preserved through the regen.
    Content-beat top-margin stays DATA (per-clip nudge) not a render default — a default would double-shift
    the owner's existing nudges.
  - **★ CAPTION `pos` = OFFSET from the auto-default (the anchoring bug, generalized) — NEW.** A caption/plate
    with `place:auto`/`strap`/a corner used to IGNORE `pos` at the corner but jump to `center+pos` the moment
    you nudged it off `[0,0]` (the owner's "it snaps to the middle"). Now `pos` is an OFFSET from the
    auto-placed corner (`draw_caption_clip`): `[0,0]` = the clean default corner, nudging tweaks FROM there.
    `lower_third` straps in ALL cases now. Portrait strap moved to 16% from the bottom (was 4.5%) to clear the
    Shorts title/controls strip. Both shorts' lower_thirds + luckymas3 b36 reset to `[0,0]` (they carried
    absolute coords fighting the old bug). Avatars got the same treatment earlier (landscape-gated offset);
    content already layered pos on the layout. **Principle: every element has a smart default; `pos` offsets it.**
  - **★ SHORTS FINISHING PASS — short2 outro gag + layout, short1 exported.** slop.py: transcript defaults —
    a bg-desk scene with a reaction OVERLAY now reads as a ROOM beat (transcript to the TOP), CODE beats drop
    the transcript to CENTER (y=0, clear of the top-docked card); gag callout labels ("clueless") get their
    OWN track (r_gag) at the arrow tail (was on r_cap, colliding with the beat's plate). short2 data: cat meme
    hugs the TOP (was a bottom overlay), room/bg-desk transcripts to top, code transcripts y=0, clueless →
    r_gag + arrow tail. **short2 OUTRO GAG:** genvo'd "…Check it out, or I will shake all your soda cans.",
    boom on "soda cans", music tapers −18→−46 dB into it, host + dimmed room extended to cover the lengthened
    VO (retime lengthens the VO but NOT the avatar/backdrop → a black gap; extend them + reset the fillers).
    Mirrors short1's ominous outro. short1: b09 outro host scale 1.17→1.0 = the b06/b10 room framing (owner:
    outro room shots match WH_CALLWNDPROC on all shorts). **`exports/luckymas-short1-copybar.mp4` re-rendered.**
  - **BOOM placement fix (short1):** `sfx_at` 3.1→3.9. It was landing on "Go" (played 3.08s), not the
    punchline — placed from a viseme track that didn't match b10_vo's audio. NO code bug (the cue plays
    exactly at `clip.start+sfx_at`); the value was wrong. Now on "Before I get ideas" (played 3.85-4.65).
  - **Defaults:** global speech boost +8→**+12 dB** (owner-tuned; both shorts). Portrait **code-shot host
    scales with card size** — a short card (≤8 code lines, "< half a portrait screen") → host BIG+centered
    ([4,120]@1.17), a long card → small corner ([250,120]@0.55); distilled from short1 b07 (7 lines). A BGM
    track can carry its own default gain in `catalogue.json` (`gain_db`); **edm-detection-mode = −24** (it's
    hot); `slop.py bed`/skeleton use it. NB — the shorts' backdrop top-margin + caption-over-code tweaks were
    SHORT1-SPECIFIC (inset dialogs); short2's beats are cover backdrops / non-code captions, so nothing
    analogous to re-apply (a portrait-inset margin DEFAULT is a render change deferred — would double-shift
    short1's manual nudges).
- **★ VOLUME-ENVELOPE EDITOR · RATE-AWARE LIP-SYNC BOB · RECORDED POP SFX — NEW 2026-07-03 (5th feedback
  pass).** Two reported fine-tuning blockers + a SFX swap; editor rebuilt (`editor/src/main.cpp`), all in
  the CODE repo:
  - **Volume automation is now EDITABLE in two places** (was authorable only via `slop.py bed --ramp`).
    The backend already read keyframed `params.gain_db` (mixer + export piecewise-linear volume) — only the
    editor surface was missing. (1) **Inline rubber-band on the timeline**: any audio/music clip carrying a
    ramp draws its dB envelope over the waveform (yellow line + breakpoints); selecting the clip makes the
    points DRAGGABLE (y=dB, interior x=time). (2) **Precise Inspector graph** (`draw_gain_envelope`): a
    dB-gridded canvas (+6…−60) with the waveform behind, the playhead line, drag/click-to-add/right-click-
    delete breakpoints, and fade-in/out/flat presets. The whole **audio section moved ABOVE the generation
    panel** in the Inspector so a bed's volume is a scroll-free glance away. Undo auto-settles (drags are
    active items; add/delete flag `g_undoDirty`). Verified on luckymas3's 7-key chill-wave swell.
    - **Drag hit-testing (crucial):** in BOTH surfaces the point handles are submitted BEFORE the catch-all
      button (timeline: before the edge-trim + clip-body buttons; inspector: before the `##genv` canvas), so
      an overlapping press GRABS the point instead of moving the clip / adding a new point — ImGui 1.91 gives
      an overlapping press to the FIRST-submitted item (same rule the timeline edge-trim handles rely on). The
      timeline point handles are small (10px on each dot), so an ENDPOINT dot is draggable while the full-height
      trim handle stays grabbable above/below it. In the inspector, interaction runs first and the dots are
      drawn in a later pass so they still render on top.
    - **Range:** the static clip/lane volume sliders now floor at **−48 dB** (was −24) — a loud bed can be too
      hot even at −24 in a quiet context (owner ask); the envelope already spans +6…−60.
    - **Global speech-volume knob (`meta.speech_gain_db`, +12 default):** a fixed dB added to EVERY tts
      clip on top of its normalize + clip/lane gain, in the mixer AND export plan — the relative dynamics
      between lines are untouched (constant delta), the whole speech track just sits louder over music.
      Music/video/SFX untouched. Project panel slider next to `final gain`. Verified in the export plan:
      +8.00 exactly on every speech clip vs `speech_gain_db=0`, beds unchanged. **Default +12 re-renders
      existing cuts louder** — intended (speech was quiet vs the beds).
  - **Lip-sync bob is RATE-AWARE.** The pngtuber bob/mouth sampled visemes/audio at raw play-time
    (`playhead − visOrigin`), but a sped-up clip (shorts run ~1.3×) plays a TIME-STRETCHED buffer — so the
    mouth drifted behind the sound (and never animated the final ~23% of a 1.3× line). Now samples the source
    at `in + tau*rate`, mirroring the mixer's played→source map EXACTLY (so the mouth tracks whatever the
    mixer actually sounds). One-line semantic fix in the single `composite_frame` avatar path (preview +
    export). Verified with an old-vs-new mouth-drive plot against the played audio (NEW overlaps, OLD lags).
    **Consistency follow-up (same pass):** the timeline WAVEFORM draw is now rate-aware too (played px →
    source ×`rate`), so a sped-up clip's drawn waveform matches the sound + the bob; the animated transcript
    timing (`slop.py transcript_apply`) was already rate-aware (`(frac_to_t−in)/rate`) — verified, unchanged.
  - **`pop.wav` transition SFX = the owner's RECORDING** (was synthesized). A raw take is quiet + fronted by
    ~0.14 s of silence → it would play inaudible AND land late, so `tools/gen-sfx.py` gained `recorded_pop()`:
    reads the committed source `tools/sfx-src/pop.wav`, trims to the transient (robust ≥4%-of-peak gate, above
    a quiet take's −70 dB floor), de-clicks the ends, and `write_wav` re-normalizes to the calibrated 0.7 peak
    (the pop event plays at −8 dB) → a punchy 0.068 s pop landing on the cut. **Synthesized `gen_pop()` stays
    the fallback** when the source is absent. The 24 KB source is committed via a narrow `.gitignore`
    exception (`!tools/sfx-src/*.wav`; `library/sfx/` stays gitignored/regenerable) so a regen / fresh checkout
    keeps the recorded pop. NB [[sfx-gag-sound-language]]'s "synthesize, never sample" is now overridden FOR
    THE POP by explicit owner request.
- **★ BOTTOM-OVERLAY MEMES · SOLO-BIG BACKDROP HOST · 8-ACT MUSIC ARC — NEW 2026-07-03 (4th feedback
  pass).** Shorts committed; luckymas3 music laid (rendering):
  - **Cat meme = a bottom reaction OVERLAY** (not fullscreen) over the host's lower body, face
    uncovered — new skeleton `place:"bottom"` (r_overlay row above the host, `width_frac`, `vid_dims`
    scale) + editor `span_has_content` ignores `params.overlay` so the **backdrop beat renders the host
    solo-BIG** like a room shot. Solo room host bumped bigger (feet just off). Copy/mascot crops
    extended vertically (region stays put, desktop fills below). short2 outro reworded to LEAD with the
    Heh (`"Heh~ There's so much more…"` — the TTS delivers a sentence-initial Heh like the intro's
    golden take); regenerated. Both shorts re-exported.
  - **luckymas3 8-act music arc** (matched to pacing): **deadly-roulette = her THEME** (cold-open,
    ACT 2, ACT 4 — scheming/host acts), **chill-wave = the swell song** (ACT 1 montage, ACT 5 ghost-
    vanish, ACT 8 triumph — each entering quiet + swelling), **edm-detection-mode = intense** (ACT 3,
    ACT 6), **30 s of SILENCE over the apology** (ACT 7) for impact, each bed fading to silence and the
    next starting on a voice line. **Bed ids are now per-(file,start)** (a song reused N times can't
    clobber itself). `export.sh --remux-from` still available for audio-only re-mux.
  - **The `CACHE_WIN`/Windows-drive cache relocation from pass 3 was REVERTED** (it silently broke
    exports — ffmpeg in WSL can't read the `C:\…` VO paths the editor baked into the plan). cache +
    exports back on WSL ext4; `docs/INFRA.md` keeps the gotcha.
- **★ PORTRAIT LAYOUT REDESIGN + MONTAGE-2x + LOUDER RAMP + AUDIO-REMUX — NEW 2026-07-03 (3rd feedback
  pass).** Committed:
  - **Portrait content beats now stack vertically:** content (image/video/crop) sits in the **TOP band**
    (portrait `fit`/`inset` lift content `-0.24*fh`), the transcript tucks just under it, and the **host is
    BIG at the bottom** (feet off-frame) — the content-beat `avatar_fit` bust grew 0.235→0.40. Was a small
    dead-center shot; now uses the vertical space. Code-shot host scale trimmed 0.9→0.55 to compensate.
  - **Portrait clueless gag** rides high (label above the horns, arrow tip stops ABOVE the head — not over
    her face) now that the solo host is big. **Cat-laugh meme now plays audio** (video_volume 0.02, matching
    the full video). **Lonely-loser beat** got a **bg-desk backdrop** (new skeleton `backdrop` key — a
    fullscreen cover behind the beat so the host reads big) instead of a black fill.
  - **luckymas3 montage = HALF speed / 2× longer** (6 flashes over 47.5→55.3s, ~1.3s each, slow zoom-out
    punch; re-rippled +3.96, Act-2 blur re-moved to the new boundary); **chill-wave ramp starts LOUDER**
    (−20 dB vs −32) but still below the −7 climax. **BUG FIXED:** `slop.py bed` clip ids (`c_bedNN`)
    collided across two `bed` calls → the 2nd (chill-wave) clobbered the deadly-roulette cold-open →
    silence; ids now per-file (`c_bed_<name>NN`).
  - **`export.sh --remux-from FILE`** — re-mux NEW audio onto an existing render's video (`-c:v copy`),
    skipping the ~20-min video render for audio-only edits (music tweaks, gain).
- **★ CROP + VOLUME-RAMP + MONTAGE + CACHE→C: — NEW 2026-07-03 (2nd shorts/video feedback pass).**
  Editor + slop.py + a music arrangement; ⚠ **not yet committed** (working tree):
  - **Per-clip `crop` `[x,y,w,h]`** (source fractions) on image+video — zoom into part of a grab with no
    derived asset; composes with `layout`/transform/mirror (`clip_crop` + cropped `clip_native_size`).
    Used for the shorts: copy-dialog close-up (short1), mascot-area close-up (short2 b01/b06/b07).
  - **Keyframeable `gain_db`** = a music-lane **volume ramp** (preview: per-sample envelope; export: a
    piecewise-linear ffmpeg `volume` expr composed with the duck). **`slop.py bed` reworked** →
    `--ramp "t:dB,…"` breakpoints (replaced the stepped intro-gain). **BUG FIXED:** the export expr
    builder used a `char[220]` snprintf buffer → the nested-if TAIL truncated → ffmpeg evaluated it to
    0 in spots = **music dropouts** at the ramp keyframes; rebuilt with `std::string` (preview was always
    fine — it uses eval_kf directly).
  - **Portrait room-shot host** sized up so **feet sit just off the bottom** (wings may clip) — the solo
    `avatar_fit` path; **room-scene transcript drops LOW** (just above the plate band) vs mid-band on
    content beats. Portrait code-shot host bigger (0.9). Short1 copy beats + short2 mascot beats cropped;
    **cat-laugh meme** (`meme-laughpoint.mp4`, cover-cropped) added to short2's "lonely loser" beat.
  - **luckymas3 music arrangement** (via `slop.py bed --ramp` + a speechless insert montage): a
    **deadly-roulette cold-open bed** (after "welcome back", fading to silence over the last pre-Act-1
    sentence) → **chill-wave** entering quiet at Act 1, **swelling to a peak over a 6-shot screenshot
    montage** inserted after "…power level" (`ripple --at 47.5 --by 3.6`, `layout:cover` hard-cut
    flashes; the Act-2 section blur moved to the montage→act2 boundary), then settling to the -18 bed.
    Re-rendered to `exports/luckymas3-fixed.mp4`. Lint clean.
  - **(reverted)** A cache/exports→Windows-C: relocation was tried + reverted — a native `C:\…` cache
    path baked into the export plan's audio paths made ffmpeg (WSL) unable to read the VO wavs → silent
    exports. `cache/` + `exports/` stay on WSL ext4. (`docs/INFRA.md` keeps the gotcha.)
- **★ SHORTS POLISH: animated TRANSCRIPT · portrait sizing · music palette · Heh~ — NEW 2026-07-03
  (user feedback on the 2 shorts).** All committed (code repo + projects repo):
  - **Animated on-screen transcript** (the shorts convention): `slop.py transcript` /
    portrait compile+retime auto-generate big pop-in caption chunks on `r_transcript`,
    loosely word-timed by char-fraction across each take's SPEECH time (viseme non-`X`
    segments — pauses don't advance chunks; linear fallback). `params.transcript` on a tts
    clip (skeleton beat key `"transcript"`) = the DISPLAY text when the line is written
    weird for the TTS. Chunks pop through each other (caption added to `popThrough`),
    `sfx:false` keeps them out of the transition-SFX pass. PROJECT_FORMAT/LLM_WORKFLOW
    documented.
  - **Portrait sizing:** room shots with NOTHING else on screen render the host SOLO-BIG
    (`avatar_fit` solo — new `span_has_content`; bust 0.38 face-frac, eye 0.44), like the
    full video's presence; code-shot host bigger (compiler: scale 0.9 tucked lower-right);
    portrait clueless-gag geometry recomputed for the big host (label above the horns,
    arrow at the forehead — verified on short2). Portrait code cards: wrap code NARROW,
    comments on their own line ABOVE calls (BitBlt beat), short card titles (long ones clip).
  - **Music palette (owner-assigned, in `catalogue.json` `core`+`role` + LLM_WORKFLOW):**
    space-jazz = MAIN bgm · edm-detection-mode = intense alternate · chill-wave =
    title-drop (loud → taper → keeps going) · voxel-revolution = speech-free montages ONLY ·
    deadly-roulette = gags · bossa-antigua = beach-relax gag intros · pixelland = credits/
    "to be continued". Bed gain default −16→**−18 dB**. New **`slop.py bed`** lays a looping
    bed (repeated clips w/ continuous `in` — the mixer doesn't loop) + stepped intro taper;
    **luckymas3 got chill-wave** (loud at the ACT-1 drop 26s → −18 bed to the end; export
    plan verified; NOT re-rendered yet). slop.py now copies the `.meta.json` sidecar into
    music assets (skeleton/bed) — **CC-BY auto-credits were silently dropped before** (all
    3 projects patched; shorts re-exported with credits).
  - **Content:** short1 boom cue moved to the punchline (sfx_at 2.6→3.1 = "Before I get
    ideas", placed from the take's viseme silences; −2.4 dB peak verified in the export);
    short2 outro reworded "cursed artifact"→"weird artifact" + "Fufu~"→**"Heh~"** (TTS
    stumbles on "cursed"; can't say "fufu" — CHARACTER.md now: Heh~ is the SPOKEN giggle,
    fufu stays written-only) + regenerated; short2 bed blippy-trance→space-jazz; both
    re-exported (`exports/luckymas-short{1,2}-*.mp4`). NB: luckymas3 still says "cursed" in
    b04 + the outro (seed-shootout-approved takes — ask the user before churning).
- **★ AUDIO ARC: BGM library · built-in SFX + gag cues + music duck · Project panel · SHORTS mode +
  2 shorts cut — NEW 2026-07-03.** One session, all committed:
  - **Hygiene:** dead `assets/` deleted (+ its lfs rules); `examples/` keeps ONLY signature-opener
    (editor default + parser fixture); fufu-lab/opening/video-broll moved to
    `../slopstudio-projects/demos/` (committed there; docs/tools repointed).
  - **Stock BGM:** `library/music/catalogue.json` populated — 12 Kevin MacLeod CC-BY beds (4 core:
    neon-laser-horizon/voxel-revolution/bit-quest/bossa-antigua + 8 variety; `core/role/tags` are
    agent-facing pick hints), all fetch-verified; sidecars carry attribution → auto-credits. The
    Media pane now scans `library/music/`; **mp3 (any libav audio) decodes IN-EDITOR** (get_pcm/
    get_wave fall back to the extracted `decode_audio_libav`) so beds preview + waveform + normalize.
  - **Built-in transition SFX** (`tools/gen-sfx.py` → `library/sfx/`, synthesized = license-free,
    in the stock pack; tuned against the user's meme-SFX reference pack by spectral analysis):
    pop.wav on default pop entrances (incl. pop-through), whoosh.wav on slide_*/rise + the avatar
    pose-swap seam. The transition DECISION logic is shared (`clip_trans_info`) so sounds always
    match the compositor; the same event list feeds preview + the export plan. Gated by **meta.sfx**.
  - **Gag cues + music duck:** `params.sfx_cue` ("awkward" = cough+crickets, "boom" = slowed vine
    boom) plays UNGATED (full videos keep punchlines) and the music DUCKS around it (duck_factor
    per-sample in preview; an editor-built ffmpeg volume expression per music entry on export —
    verified −36 vs −48 dB in a rendered file). Skeleton: gag:"clueless" auto-cues awkward;
    `{"sound":"boom","sound_at":s}` for authored stings. **luckymas3 got cues on both clueless
    gags + boom on the outro threat** (projects repo; also CRLF→LF normalized + .gitattributes).
  - **Docked Project panel** (right tab bar): format radio (1080p = locked full-video defaults,
    SFX off / portrait = shorts), built-in-SFX toggle, final gain (post-normalize master, preview
    `audio_fill` + export post-amix `volume=`), default speech rate. Persisted in meta
    (`format/sfx/gain_db/speech_rate` — PROJECT_FORMAT §meta); the format switch only re-derives
    keys the user hasn't pinned. luckymas3/signature-opener plans verified unchanged.
  - **SHORTS (portrait) mode:** skeleton `"format":"portrait"` → 1080x1920, speech ~1.3x
    (meta.speech_rate; mixer/export/inspector/retime/lint are all RATE-AWARE — played dur =
    audio/rate, split windows count dur*rate source seconds), gap 0.35→0.2, short act cards,
    geometry squeeze-mapped (landscape recompiles BYTE-IDENTICAL — diffed). **Portrait render
    defaults:** host = bottom-band presenter (smaller face frac, eye-line 0.66), insets = top band
    (~86% W / 42% H, −0.20H), new **`layout:"cover"`** (never degrades — for backdrops; the
    portrait compiler emits it for bg spans; fullscreen still degrades extreme aspects to contain).
  - **Two shorts CUT from luckymas3** (`../slopstudio-projects/luckymas/luckymas-short{1-copybar,
    2-ghostgirl}.slop.json` + skeletons, committed there): the copy-bar hijack (56.6s) + the
    layered-window ghost girl (65.9s). VERBATIM lines `adopt`ed the tuned lm3 takes + visemes,
    only the new hook/CTA lines genvo'd on lame; both lint-clean; each carries its own music bed
    (edm-detection-mode / blippy-trance), a gag cue, and a boom outro (short 1). Rendered to
    `exports/luckymas-short{1,2}-*.mp4`.
  - **Workflow doc:** the shorts recipe (separate project BESIDE the source cut so `assets/…`
    resolve; adopt-verbatim, rewrite-terse; hook in ~2s; one reveal + CTA) is in
    `docs/LLM_WORKFLOW.md`.
- **★ REPO SPLIT EXECUTED + luckymas3 FINAL FIXES + FORMAT LOCKED — NEW 2026-07-02.** The destructive
  half of the public-repo plan is DONE:
  - **Projects monorepo:** `../slopstudio-projects` (own LOCAL-ONLY git) holds the luckymas family
    (v1/v2/v3 cuts + per-cut `assets/` + the video-001 research/script docs) as **portable project
    dirs** — uris are project-relative (`assets/…`); the editor (`resolve_asset`) and `slop.py`
    resolve plain uris CWD-first then project-dir-relative. Run tools from THIS repo root with
    relative paths (`../slopstudio-projects/luckymas/luckymas3.slop.json`); WSL-absolute paths don't
    survive WSLInterop. Verified: lint clean + identical `--shot-frame` from the moved location.
  - **Code-only master:** fresh orphan commit (no heavy-asset history), force-pushed to origin; the
    full pre-split history is the local branch **`master-legacy-full`** (backup only, never push).
  - **luckymas3 export review fixed:** the chiyo-key beat's user-authored VO SPLIT restored (a retime
    had reset both halves to full-length takes → repeat + 8.9s silent bobbing; timeline re-rippled,
    903→887s) — and **retime/lint now RESPECT splits** (`params.in` / shared speech asset) so it can't
    recur; '14 wallpapers' CRT footage hugs the left edge; 1st clueless gag re-aimed (label between
    horns, arrow at forehead) + arrow got a real pointed tip (shaft stops at head base); term plates
    got real inner margin (pill radius-aware pad); 4 r_av overlaps trimmed (rb4→b85 flashed 2 sprites);
    outro 'cursed' take regenerated via a **seed shootout** (params.seed busts the TTS cache; WhisperX
    word-duration scoring; seed 3 won, seeds 2/8 cached on lame as alternates).
  - **Format LOCKED (user-approved):** `docs/LLM_WORKFLOW.md` now carries the reference-cut pointer +
    the distilled format conventions (acts/plates/clueless-gag/typewriter/recreation-section/outro,
    split-takes for pauses, seed shootouts for bad takes). luckymas3 is THE reference cut. Cleared the video half of a
  ~22-item list + built the release scaffolding. **All committed.**
  - **Editor fixes:** explicit plate corner pins (`place: tl/tr/bl/br`); diagram default pop-in (+ the pop
    scale now reaches `draw_diagram_clip`); sticky-ruler scrub submits FIRST so clips scrolled under it can't
    steal the click; the inspector px readout folds in the render-time `layout` auto-scale (the "both say
    800x600 but render different sizes" was a lying readout, not a bug); pose-swap slide survives seam
    gaps/overlaps (avatar adjacency window 0.06→**0.35s**) + a same-sprite reposition GLIDE; Media pane lists
    ALL the project's asset dirs (`g_projAssetDirs`, filled at parse) and a timeline OS-drop now imports into
    the project library (managed copy, no F:\ uris); `tokenize_code` treats a UTF-8 sequence as ONE token
    (was `?`-per-byte) + JP face merged into the mono font + Win-1252 glyphs → code cards can show Shift-JIS /
    mojibake.
  - **Content:** wallpaper-scroll video on the "14 wallpapers" beat; localized the 7 dragged-in screenshots
    into `examples/assets/luckymas/`; 2 VO overlaps cleared; locale-section visuals (a Shift-JIS-vs-cp1252
    code card + a Control-Panel switch diagram, NO VO change); a new **pre-outro "minimal recreation" section**
    (rb1–rb4: mascot.exe + copyanim.exe code cards from `tools/xp/win/*.c` + a github CTA, VO gen'd on lame +
    retimed). luckymas2 is now **1080p60, ~903s, lint-clean**.
  - **Render:** `tools/export.sh` gained `--target-mb` (1-pass x264 ABR sized to a target — the user's
    "~300MB first"), `--fps`/`--scale`/`--abitrate`, and an **ffprobe guard that skips silent video inputs**
    (a no-audio screen-capture mp4 in the mix broke the whole filtergraph). Editor **File ▸ Render video…**
    modal spawns it via `wsl.exe --cd` in a console. `exports/luckymas2-1080p60.mp4` rendered (~300MB target).
  - **Public-repo scaffold (NON-destructive parts done):** `master-legacy-full` LOCAL backup branch of the
    full history; **luckymas3** = trimmed self-contained copy (69 dead TTS takes swept) + `tools/localize-project.py`
    (copies cache/assets-src refs into `examples/assets/<stem>/{audio,video,viseme}/`, gitignored so master stays
    code-only); `tools/pack-stock-assets.py` → `dist/stock-assets.zip` (gemma-big rig + backgrounds, 19.9MB) +
    `tools/fetch-stock-assets.py` + an editor first-run "stock assets missing" prompt; nightly CI
    (`.github/workflows/nightly.yml`, rolling `nightly` pre-release like ../openrecet); README reframed around
    @GemmaExplains + hero shots (`docs/hero/`); `tools/fetch-stock-music.py` + `library/music/catalogue.json`
    scaffold (empty — needs the owner's CC0/CC-BY picks).
  - **STILL OPEN:** stock-asset **copy-on-use** into a project library (#19, depends on the pack
    layout); populating the music catalogue. (The destructive remote rewrite + the projects-repo
    move were EXECUTED 2026-07-02 — see the top bullet.)
- **★ luckymas2 REVIEW PASS 2 (user feedback on the rebuild) — 2026-07-02.** Cleared a ~30-item
  list; defaults distilled into the editor + `slop.py`, content applied to `examples/luckymas2.slop.json`
  (lint-clean, 319 clips / 834s). **Editor (`editor/src/main.cpp`):** (1) `lower_third` captions default to
  the bottom **strap** on the side away from the host/content, `jp_lesson` to **big centered-low** with the
  host stepping hard aside, `term` to the least-busy **corner** (auto-corner gained bottom slots) — kills
  "plate over her face"; (2) image/video **pop THROUGH a contiguous edge** (a back-to-back media swap reads
  as a pop, only the fade is suppressed — fixes the ~151s hard cut); (3) the **pose-swap slide keys on the
  RESOLVED sprite** (rig alias/canon) so adjacent beats that land on the same sprite don't slide into an
  identical pose; (4) default rig fallback `gemma-pngtuber`→**`gemma-big`**. **`slop.py`:** host draws
  **ABOVE code/diagram** now (only captions + the gag arrow sit above her); the `stack` visual gained a
  left-anchored **cascade unveil** (n>2 / `side:left` — staggered pops, even left margin, each on its own
  overflow image row `r_img2…` = deterministic z, no same-row overlaps) and a **banner+main** auto-layout
  (wide banner top-right on top, main shot big); **room bg spans extend through a section blur** with a slow
  fade so the swap under the blur is content-only (no sudden backdrop colour change); clueless-gag geometry
  moved off her face. **Content:** year TTS regen (2007→"two thousand seven"); calendar VO reworded (ALL
  mascots share one engine) + regen; b81/b83 reworded + regen + retimed; vignettes on act1/calc/box-art;
  calc + BitBlt-empty share fit framing (host in a corner or removed); **password beat = the settings
  dialog with Gemma shh-covering the Pass field**; copy-anim 2x-zoom into the top-left dialog; wallpaper
  section = 4-image cascade (+ imas saver + the wallpaper picker, captured via Playwright); ruputer site big
  + banner on its own top row; screensaver clip re-cut to the chibis/imas-3D section (`ffmpeg -ss 102 -t
  11.5 -i assets-src/luckymas-crt.mp4 … assets-src/luckymas-crt-saver.mp4` — regenerable, gitignored); outro
  on `bg-desk.png`; installer screenshot beside the savers. New committed assets:
  `examples/assets/luckymas/{wallpaper-picker,settings-password,xp-bitblt-empty-crop}.png`.
- **★ OUT-OF-THE-BOX PASS (rig · layout · library · skeleton/lint · timeline) — NEW 2026-07-02.** One
  arc: make the one video format compose well with zero hand-tuning (user directive). All committed:
  - **`gemma-big` hi-res rig** (`presets/avatars/gemma-big`): 13 pre-keyed ~1536² GPT singles from
    `F:\Pictures\oc\gemma-san\video-sprites-big` — deadpan set (neutral/smug/teaching(+¾)/explaining/
    presenting/pointing/thinking standing + smug/annoyed/pointing/thinking/confused floats), script
    vocabulary aliased in. NO matte/upscale (already clean) — new `tools/import-rig-sprites.py` +
    `import-map.json` is the repeatable path for future drops (unmapped files listed loudly).
  - **`layout` param on image/video** (render-time): `fullscreen`(cover) / `fit` / `inset(-left)` —
    the distilled content-inset default computed LIVE from native dims, so any aspect lands framed;
    transform nudges on top; presenter side-logic sees it; drops default to `layout:inset`.
  - **Emotion resolution: exact rig key WINS** over canon_emotion collapsing (a rig with a real
    `explaining` sprite gets it; strict-teacher used to hijack those beats).
  - **Per-project library + auto-appear**: `<projdir>/assets/<stem>/` scanned recursively into the
    Media pane (amber, grouped first, `in: both/project/common` scope row); library rescans every
    ~2.5s (agent-dropped files just appear), overwritten files drop stale textures, edited rig
    manifests hot-reload; imports/gens land in the project library by default (regen stays in place).
  - **`slop.py skeleton/lint/retime/genvo`**: beats `{line, emotion, visual}` compile to a full
    project (visuals HOLD until changed → gaps unauthorable; insets get filler backdrops; sections =
    blur-swaps); `genvo` bulk-generates VO+visemes headlessly then `retime` time-warps the timeline
    (incl. keyframes) onto real audio durations; `lint` flags black spans, no-visual narration,
    asset repeats, missing files, stale/ungenerated VO, overlaps, negative starts, unknown emotions.
    Lint on luckymas correctly exposes the unfinished tail (501s+ VO-only) + 7× img_desktop reuse.
  - **REVIEW PASS 2 (user feedback on the rebuild) — defaults distilled:** TTS speaks years as words
    (job-text-only normalization; slop.py mirrors it for adopt/lint); `fullscreen` auto-degrades to
    contain on extreme aspect (banners); code clips typewrite BY DEFAULT (reveal → highlight sweep,
    no keyframes) with the strict-teacher pose on code beats; captions default to the bottom
    lower-third strap (plates top-right — never over her face); the room bg backs HOST beats only,
    content beats get the whole-frame fill gradient; beat visuals support LISTS (in-beat swaps:
    BitBlt mascot→empty, the lonely-loser meme burst) + `zoom` (credits closeup). NEW
    **`choreograph.py halo`** demo (+ `png2raw --straight`): the premultiplied-alpha bug captured on
    real XP (straight vs premultiplied mascot, 2x crops → `assets-src/xp-mascot-{halo,clean}.png`),
    wired into the pre-multiply beat. luckymas2 recompiled lint-clean (277 clips).
  - **THE OUT-OF-BOX TEST: luckymas REBUILT from a skeleton** (`examples/luckymas2.skeleton.json` →
    `examples/luckymas2.slop.json`, 87 beats / 241 clips / 827s, **lint-clean**). Script + visual
    mapping extracted from the hand-tuned cut (dropping dup takes/cruft), compiled, the user's tuned
    VO takes `adopt`ed (86/87; 1 re-worded line + all 87 viseme tracks regenerated via `genvo` on
    lame). Zero hand-tuned transforms — everything places via layout/framing defaults. 9-beat render
    montage on the feed. Honest gaps found + partially fixed: native code/diagram cards now count as
    content (host dodges; compiler places them right-of-center), but the **diagram primitive is still
    the weakest** (wide node graphs brush the host; needs auto-size + backdrop + side-aware layout),
    and compound hand-polish beats (arrows/labels/A-B pairs) aren't skeleton-expressible yet.
  - **Timeline**: sticky ruler now covers the padding sliver (opaque from the window edge + shadow);
    zoom-adaptive tick ladder (m:ss majors ≥72px apart, minors when roomy) replaces the every-second
    grid that smeared at fit zoom; `--tl-vscroll` pins lane scroll for headless shots.
  - **Gotcha (memory'd):** a still-running Windows slopstudio.exe serves a STALE image to new runs of
    the same path — `tasklist.exe | grep -i slop` + `taskkill.exe /IM slopstudio.exe /F` before
    rebuild-verify cycles.
  - **TTS research (deep, 2026-07-01):** Chatterbox (MIT — Turbo EN / Multilingual V3 JA, ~2-6GB,
    proven ROCm serving) is the recommended stable clone engine + engine-agnostic best-of-N with
    Whisper-WER rescoring (the industry mitigation; fixes "regenerate until it sounds right" even on
    Qwen3). CosyVoice 3 (Apache) second. F5-TTS/IndexTTS-2/Higgs/Fish/XTTS = license landmines.
    Qwen3 instability is upstream-confirmed (issue #298), sampling+seed pinning necessary-not-
    sufficient. Bonus: Qwen3-ForcedAligner-0.6B (Apache) could replace WhisperX + score best-of-N.
    Full findings appended to `docs/RESEARCH.md`; next concrete step = chatterbox provider on lame's 3080.
- **★ FIVE USER-REPORTED FIXES — stale-fade · whole-frame FILLER · sticky ruler · video loop · credits image — NEW
  2026-07-01 (user feedback).** All in `editor/src/main.cpp` + `examples/luckymas.slop.json` (⚠ UNCOMMITTED —
  left in the working tree; luckymas also carries the user's prior review edits, so review before committing):
  - **Stale fade / translucent square on clip EXTEND is gone.** Two coupled fixes: (1) `draw_clip_glow` now
    honours the clip's animated opacity (like shadow/frame did) — it used to draw at full strength even when
    the clip faded to 0, so a baked opacity fade left a translucent glow *square* (the user's 187s idolmaster:
    the image was EXTENDED but its baked fade-out stranded at the OLD end 186.92, so it faded early + the glow
    persisted). (2) Trimming a clip edge now **re-anchors baked edge fades** (`reanchor_trailing_fade`/
    `reanchor_leading_fade` — the last strictly-descending opacity run follows the moved end; symmetric for the
    start), so extending never strands a fade. **Data repair:** re-anchored 4 stranded fades in luckymas
    (cf_v_h1/cf_v_h8/cc_v_h8/c_cap_h5). Verified headless (187.6s: square gone, image visible to the new end).
  - **`filler` now blurs the WHOLE composited frame** (not a single guessed image plate) — **mirrors the `blur`
    clip**: the full foreground composite (every layer except fillers) is rendered at half-res, read back, and
    run through the same heavy `blur_rgba` gaussian, then zoomed ~4x (so only smooth colour shows, no scene
    'halo') → a SMOOTH gradient that fills the frame (an earlier tiny-RT + bilinear-upscale version was pixelated + left a dark content-shadow — fixed).
    Tracks every layer + its motion, never collapses to a flat grey/black wash (the user's
    111s "banner slides out → bg abruptly goes grey + no motion" bug). New `render_fill_backdrop` pre-pass
    (mirrors `render_preview_blur`, on the private `g_blurCtx`) wired into preview + both export paths; the
    pre-pass base is a soft dark tint so a genuine one-sided-content moment reads as a dim backdrop, not a hole.
    Also implemented the **transition CONTIGUITY** the header comment always promised: back-to-back same-row
    clips (gap <0.06s) get a HARD CUT instead of a slide-out-and-in "bounce" — the host was segmented per VO
    line and kept sliding fully out between lines (which emptied the frame → the filler void). Extended 2
    fillers (fl_v_h8/fl_v_h5) that ended before their content. Verified headless (111.9/112.4/187.6s all fill).
  - **Timeline ruler/playhead header is STICKY** — pinned to the viewport top (`origin.y + GetScrollY()`) so the
    ruler stays visible + the playhead stays scrubbable when the lanes are scrolled down. Verified headless
    (scrolled to the bottom lanes; ruler + red playhead marker stay at the top).
  - **Video clips LOOP by default** (`video_frame_index` loop `false`→`true`; `loop:false` opts out, honoured by
    the CRT cold-open) so extending a short animation repeats it. Verified headless (a 4s source in a 12s clip:
    t=9s shows source 1.0s with loop, frozen last frame without).
  - **Guest-artist CREDITS beat (~136s) was BLACK → now the authentic source.** Re-verified the claim's source
    (`docs/research/sygnas-history.md` → the circle's own archived `sygnas.tv/doujin/imas/about.html`, 2007-03)
    and captured the real スタッフ roll via Playwright (Wayback chrome removed, 2.6× zoom) →
    `examples/assets/luckymas/sygnas/imas-credits.png`: every illustrator printed *with their own circle in
    parentheses* (あらたとしひら（ノヘッパDo!）, 双龍（R-Type Nirvana）, 田中松太郎（FOMALHAUT）… — the "smoking gun").
    Authored the beat (credits inset + smug host + caption + filler) matching the video's two-column style.
- **★ TWO INTERACTIVE-LOOP FREEZES FIXED — blur-scrub deadlock · async pose thumbnails — NEW 2026-07-01
  (user feedback).** Both in `editor/src/main.cpp` (committed + confirmed live by the user). Both were
  the same class of bug: heavy/blocking work in the interactive loop.
  - **Dragging the playhead over a `blur` clip froze the editor indefinitely.** Root cause: the
    whole-frame blur pre-pass (`render_preview_blur`) ran a full extra `ImGui::NewFrame()`/`Render()`
    every frame on the MAIN context, right before the real UI frame. `ImGui::NewFrame()` DRAINS the
    shared input-event queue → the offscreen frame ATE the editor's own mouse drags/clicks, so the
    scrubber stopped receiving `MouseDelta`/clicks and couldn't leave the blur clip → the pre-pass ran
    forever (self-perpetuating). The export/`--shot-frame` path never hit it (no competing interactive
    frame). Fix: the pre-pass now runs on a **private ImGui context** (`g_blurCtx`, created at init,
    shares the main font atlas + D3D device, its own DX11 backend + input queue), so it can't touch the
    editor's input. Verified headless via `--shot` (30 real interactive frames parked on the blur clip →
    no crash; live preview shows the whole-frame blur; main-UI text/atlas intact = shared atlas OK).
  - **First click on an avatar clip froze while the "Emotion poses" grid loaded.** The inspector panel
    (and the preset-rig viewer) decoded ~a dozen 1024² sprites + ran a full-image face-scan on each,
    synchronously, on the click frame. Fix: an **async thumbnail loader** (`emo_thumb_worker` +
    `get_emo_thumb`) decodes/mattes/face-scans each on a worker thread into an ISOLATED store (own cache
    + lock, never races the UI-thread `g_texCache`/`g_srcCache`); device-side SRV creation is thread-safe
    (no `D3D11_CREATE_DEVICE_SINGLETHREADED`). The pickers poll it each frame and show a `...`/blank
    placeholder until each face-zoomed thumb pops in. Skin-scan is now shared (`face_from_pixels`, used
    by both `avatar_face` and the worker); `avatar_face_uv` (its only callers moved to the async path)
    was removed. Verified headless (`--select c_av1 --shot` → grid loads face-cropped thumbs).
- **★ REAL ANIME UPSCALER (RealESRGAN) + HOST RIG REPROCESSED @1024, SOFT ALPHA — NEW 2026-07-01 (user
  feedback).** The user: the gpt sprites are "pixelated at the edges" + "bleed through between hair strands" +
  "run them through upscaling." Root cause: the rembg matte was **binary alpha** (0% AA → hard jagged edges) and
  the figures were low-res (~350px in a 512 canvas). Fixes (⚠ UNCOMMITTED — 78 PNGs + manifest + tool + provider):
  - **A real strict upscaler is set up (NOT img2img).** img2img via Anima DRIFTS her design (at denoise 0.35 it
    swapped her costume to a black suit + dropped the wings; 0.2 dropped the teaching pointer) — rejected.
    Instead: **RealESRGAN_x4plus_anime_6B** (BSD-3, commercial-safe, 4x, ~2.5s/img) installed into lame's ComfyUI
    (`/lamedata/comfy/models/upscale_models`) + a new **`upscale` capability** on the image provider
    (`providers/image`: `UpscaleModelLoader → ImageUpscaleWithModel`; `init_b64`+`upscale_model`). Redeploy:
    `deploy/lame/deploy-image.sh`. Content-preserving — verified pointer/wings/gold-suit/pose all intact, just
    sharper. See [[anime-upscaler-realesrgan]].
  - **Pipeline:** `tools/process-pose-sheet.py --upscale --alpha-matting` upscales the ORIGINAL cell 4x → mattes
    at high res with alpha-matting (soft AA edges + fine hair) → 1024 canvas (manifest `size` synced). **All 13
    sheets reprocessed → gemma-gpt-static: 78 sprites @1024², soft alpha (mean 1.45%, 0 binary), 0 failures, every
    cell face-checked.** Fixes the pixelation + hair bleed AND doubles the res.
  - **Editor-safe:** all 27 luckymas host clips use face-relative framing (bust/closeup/floating → `avatar_fit`
    scales by detected face width = res-independent), so the 512→1024 bump keeps the same on-screen size/placement
    — verified by rendering the closeup/bust/teaching beats (sharper host, identical layout; fed). A `raw`-framed
    clip would render 2x (native px × scale) — none in luckymas.
- **★ EDITOR POLISH PASS 3 — blur=whole-frame · flip · add-special-clip · wheel · frame-opacity · filler — NEW
  2026-07-01 (user feedback while polishing luckymas).** All in `editor/src/main.cpp` (⚠ UNCOMMITTED — left in
  the working tree for the user to try live + commit; +7 re-matted library PNGs, below):
  - **★ `blur` clip is now a TRUE WHOLE-FRAME post-process** — it blurs the ENTIRE composite (host, captions,
    code, insets, every layer), not one image plate. Fixes the user's 261.78s bug (the old blur grabbed the
    topmost image = kotori, drew a blurred OPAQUE copy at kotori's transform → it covered the "clueless" text
    and left host/text sharp). New `frame_blur_strength()` (bell/param → strength+sigma) + `blur_rgba()` (CPU
    separable gaussian, downsample→blur→upscale) applied to the readback buffer on EXPORT + `--shot-frame`,
    and to an isolated half-res composite pre-pass on PREVIEW (`render_preview_blur` → `g_pvBlurSrv`, shown in
    place of the live frame). Identical preview/export. The blur clip's per-clip draw is now a no-op. `source`/
    `dim` params dropped (obsolete). Verified: edge-energy 3.01→0.46 start→peak; A/B shows host+kotori+text all
    blur together (fed); live-preview UI shot confirms the pre-pass path.
  - **Flip H / Flip V checkboxes** (inspector, next to scale) — negative scale used to "snap back" (aspect-lock
    coupled both axes → a 180° rotate, and the px field forced `fabs`). Now `clip_flip()` sets the axis SIGN
    (static + keyframes); image/video honor it as a **UV mirror with the rect kept in place** (|scale| for w/h
    so the frame/shadow/glow stay correct); avatar mirrors via its existing reversed-quad path. Verified: kotori
    inset mirrors cleanly, border intact (fed).
  - **"Clip" menu** (menu bar) — Duplicate (Ctrl+D) · Split (S) · Delete · **Add special clip ▸** vignette/
    gradient · blur · background-fill · caption · code · shape · diagram, each inserted at the playhead with a
    sensible default via `add_native_clip_at()` (finds/creates a row of that type; filler auto-drops to the
    BOTTOM of the stack). Closes "no simple way to add special clips like the vignette from scratch." (Duplicate
    already existed via Ctrl+D + the inspector button — now also discoverable in the menu.)
  - **Timeline wheel = zoom ONLY** — the child now has `NoScrollWithMouse`, so a plain wheel no longer zooms AND
    scrolls the lanes at once. Lanes scroll via the scrollbar or **Ctrl+wheel**; Shift+wheel still pans.
  - **Auto inset border + drop shadow follow clip OPACITY** — `draw_clip_frame`/`draw_inset_shadow` take the
    clip's animated alpha, so the frame fades in/out WITH the clip (was hardcoded alpha).
  - **bg `filler` no longer pure-black on a host-only beat** — when no image plate is up it falls back to the
    top-most host sprite (`topmost_avatar_sprite`), backed with a flat wash of her dominant (opaque-pixel)
    colour so the whole frame fills (a soft tinted backdrop, not a black void). Verified (fed).
- **★ LIBRARY REACTION SPRITES RE-MATTED (green fringe killed) — NEW 2026-07-01 (user feedback).** The seeded
  `library/images/gemma-{neutral,smug,happy,confused,annoyed,surprised,sad}.png` carried the green gen-bg
  (RGB 71,112,76) in their transparent/soft-edge pixels → a green halo over coloured backgrounds. Re-matted
  through **rembg (isnet-anime on lame:8015)** + a premultiplied-dilation **defringe** → corners now [0,0,0],
  zero visible green, character un-shifted (≤2px). ⚠ UNCOMMITTED (7 PNGs in the working tree).
  (`gemma-ominous`/`gemma-host-teacher-test` were already rembg.) NB: the gpt-static pose rig's *background* was
  already rembg-clean, but its EDGES were binary/jagged + low-res — now reprocessed @1024 with soft alpha +
  RealESRGAN (see the upscaler bullet above). Its holographic butterfly WINGS are translucent by design (art),
  not a matte halo.
- **★ luckymas REVIEW PASS — editor fixes + content edits — NEW 2026-07-01.** Cleared the queued review
  list (`docs/UX_NEXT.md`). **Editor fixes (committed, verified headless):** (1) host no longer **snaps
  mid-clip** — presenter alignment is decided ONCE per clip span (`content_centroid_span`), not per-frame;
  (2) the **blur clip matches the source clip's transform** (no zoom/snap when it blurs an off-center inset);
  (3) the **1px vignette left-edge seam** is gone — the preview frame rect is pixel-snapped (`floorf`);
  (4) **content-inset drop default** = a right-lower inset fit to ~half the frame (`add_image_clip_at`).
  **Content edits (in `examples/luckymas.slop.json`, WORKING TREE — left for the user to commit with their
  review edits; see UX_NEXT):** 215s wallpaper = CRT IE wallpaper-picker **scroll** + the actual wallpaper;
  228s = clean **doujin-calculator** screenshot + the **Kotori "touch-the-chest"** crop with a **confused
  host + "clueless" arrow**; 286s = the **teaching rig** replaces the badly-cleaned static teacher image;
  the **BitBlt A/B** is now the mascot **MOVING** (video) vs the cropped **empty** desktop. New committed
  screenshots in `examples/assets/luckymas/` (`calc-imas`/`calc-convert`/`calc-kotori`); CRT-scroll +
  BitBlt-empty derived assets are gitignored/regenerable (recipes in UX_NEXT).
- **★ rembg IS THE DEFAULT SPRITE MATTE — pose sheets reprocessed — NEW 2026-07-01 (user feedback).**
  Last session's `process-pose-sheet.py` reprocess used the LOCAL neutral-grey key, which left a grey
  halo/bleed around the silhouette (hair/wings) — a downgrade. The processor now mattes **each cell with
  the rembg provider (isnet-anime on lame:8015)** by default — cut by SHAPE, so the soft grey bg + drop
  shadows are gone (one figure per cell is isnet-anime's sweet spot). The local neutral key is kept only
  to FIND the grid (its alpha projection) + as the `--local` offline fallback; each cell is cut from the
  ORIGINAL un-keyed sheet so rembg sees real edges, and per-cell provider failures fall back to the local
  key, loud. All **13 sheets reprocessed** (78 cells, ~1m47s, every cell face-checked) →
  `presets/avatars/gemma-gpt-static`. New flags: `--local`, `--model`, `--alpha-matting`, `--rembg-url`
  (URL is config-driven from `config.toml`). Verified: before/after on dark teal (halo gone) + headless
  luckymas host beats (`/tmp/host_*.png`) composite clean over the room/content (fed).
- **★ PRESET RIGS IN THE LIBRARY · CLIP DUPLICATE — NEW 2026-06-30 (user feedback).** (1) Bundled
  **preset rigs** (`presets/avatars/<name>/manifest.json`, e.g. `gemma-gpt-static`) now show in the
  Library's **Media pane** (teal cell, face-cropped thumbnail) alongside authored library rigs —
  `scan_library` adds them with a read-only `preset` flag (double-click / drag to place via
  `add_avatar_clip_at`; the Asset-detail tab shows a read-only pose grid; delete is disabled). Fixes
  "I don't see the new gpt sprite rig in the asset library." (2) **Duplicate a clip** without
  splitting: **Ctrl+D** (or the inspector **Duplicate** button) clones the selected clip
  (params/transform/keyframes/asset) right after itself on the same row + selects it; headless
  **`--duplicate <clipId>`**. In-memory like split (Ctrl+S persists).
- **★ SMART POSE-SHEET PROCESSOR — NEW 2026-06-30 (matte → rembg 2026-07-01, above).** `tools/process-pose-sheet.py`
  turns a GPT 3x2 pose sheet (or a whole dir) into matted, named avatar-rig sprites + a merged manifest.
  The matte is **rembg per cell by default** (isnet-anime — see the 2026-07-01 bullet); the `--local`
  saturation-aware **neutral-grey key** (low chroma in a brightness band **derived from the detected bg**,
  adaptive across brighter/darker sheets — a plain RGB-distance colorkey EATS the light purple hair, so it
  keys on **neutrality**) is the offline fallback AND still drives the **gap-based
  grid inference** (the figures drift off the even-cell centers, so a naive split clips them — proven
  on all 13 sheets, 0 fallbacks), cut → alpha-trim → centered on a 512² canvas (matches the rig),
  **pose-named** by the convention (row0 front/front34/viewer34, row1 float_front/float34/
  float_viewer34), a **skin-based face-detect sanity check** per cell (same detector as the framing;
  flags bad cuts), and a **manifest merge** that preserves curated alias keys (e.g. `surprised`→
  `shocked_front`). Run: `nix develop --command python tools/process-pose-sheet.py <sheet|dir> --rig
  gemma-gpt-static` (`--name` override, `--dry`, `--preview x.jpg`). Verified: all 13 source sheets
  cut cleanly (faces found in every cell); writes only the processed emotion's files (existing
  committed sprites untouched). Remaining for full task #9: an in-editor "smart auto" button (the
  existing Tools ▸ Sprite sheet covers manual one-offs; this CLI is the batch/LLM surface).
- **★ DOCKED WORKSPACE PANES — NEW 2026-06-30.** The editor no longer opens Library/Viewer as
  floating windows over the preview/timeline. `editor/src/main.cpp` now reuses the Library body
  as a docked left **Media** pane, and reuses the Viewer body as a right-side **Asset detail**
  tab beside the clip Inspector. Existing library gen/import/drag/drop, image crop-mask/remove-bg,
  audio audition, avatar-rig editing, and gen recipe/history controls remain in place but stay
  inside the main workspace. Tools menu toggles now show/hide the Media browser pane and Asset
  detail tab. Verified with `nix develop --command make -C editor`.
- **★ FACE-ANCHORED AVATAR FRAMING + EMOTION FACE-PREVIEW PANEL — NEW 2026-06-30.** The host sprite
  **always composites WHOLE — framing never pixel-crops it** (a prior pass UV-cropped to a "bust" and
  she rendered as a torso sliced at the thigh with a hard edge — rejected). Instead the compositor
  **detects the face and anchors every framing on it** (`avatar_face` in `editor/src/main.cpp`):
  keys on this art's pale-pink **skin** (bright + low-saturation + green-is-min, vs. the purple
  hair/horns and saturated eyes) inside the head band, 8/92-percentile box → robust face center-x +
  eye-line + width across **front / 3-4-turn / floating** poses (bbox fallback for non-character
  sprites). `avatar_fit` then scales the FULL sprite by the detected face width and pins the eye-line
  down the frame so the lower body runs off the **bottom frame edge** (cut by the canvas clip-rect —
  the only "crop" is the natural boundary): **`bust`** (face ≈ 0.36·frame-w, eyes at 0.32 → half-bust,
  feet off) and **`closeup`** (face ≈ 0.50·frame-w, eyes at 0.45 → tight face) — the "doesn't show her
  feet / not floating when she bobs" look the user asked for. Default framing is **`full`** (authored
  transform verbatim — existing luckymas side beats unchanged); `bust`/`closeup` are opt-in per clip;
  clip pos nudges + clip scale multiplies for in-between framings. **Newly-created** host clips
  (`add_avatar_clip_at` — drag/drop, double-click, `--add-avatar`) default to `framing:"bust"`.
- **★ FRAMING v2 (zoom-out · float full-body · content-aware ¾ · feet shadow) — NEW 2026-06-30 (user
  feedback).** (1) bust/closeup were "way too close" → zoomed out (closeup face≈0.42·frame-w, bust 0.30).
  (2) **`full` framing is now face-anchored** (consistent size/placement, no manual tweaking) and uses the
  **floating** sprite variant — full-body shots default to floating poses (look natural in the air). Unset
  framing → **`raw`** = authored transform verbatim (existing manual beats untouched; the global default is
  no longer face-anchored). (3) **Pose variant is auto + overridable** (`pose` param): when off-center
  content (a screenshot/inset/CRT) is on screen the host turns to the **looking-aside ¾ pose facing it**
  (`front34`/`float34`, flipped via the cached face-offset; `face:"left"/"right"` overrides) so she looks at
  the thing; else front. `avatar_variant()` resolves `<base>_<pose>` (handles aliases). (4) **Contact shadow
  at her FEET** (alpha-bbox bottom) for **non-floating** sprites only (floating/bust/closeup get none);
  `shadow` adjusts. luckymas intro CRT beats (`c_av4`/`c_av_o4`/`c_av_o6`) converted to `full` with uniform
  transform → consistent big floating host facing the CRT. Verified headless (`/tmp/i_montage.jpg`, fed).
- **★ PROFESSIONAL INSET FRAME — NEW 2026-06-30 (user feedback).** Non-fullscreen media insets (e.g. the
  CRT) looked barebones with just an outer glow. The `frame` treatment is now a clean **card**: a soft
  **drop shadow** (`draw_inset_shadow`, drawn before the image — offset down/right, lifts it off the bg) +
  a crisp light **border** + a faint inner **highlight** bevel (`draw_clip_frame` reworked). It's **default-ON
  for a non-fullscreen inset that has NO glow** (`resolve_frame`) — so the bare CRT/screenshot insets get the
  pro border automatically while a glow-styled (filler-backed) inset is left alone (the user: "glow is fine
  with the bg filler"). `frame:false` opts out; `frame:{color,thickness,radius,shadow}` tunes. Verified
  headless on the CRT + the "22 character packs" inset (`/tmp/fr_montage.jpg`, `/tmp/fc_montage.jpg`, fed).
- **★ FRAMING DEFAULTS distilled from user edits + library DRAG fix — NEW 2026-06-30.** The user hand-tuned a
  sample of host beats to show the desired defaults; distilled into code: (a) **bust eye-line lower**
  (`eyeFrameY` 0.34→**0.48** — every edited beat had a ~+150px down nudge); (b) a **presenter auto-offset** —
  with content on screen + the clip's horizontal pos at default, the host auto-places in the **opposite
  corner** (`-side·0.24·frame-w`) facing the content. The edited luckymas bust beats were reset to neutral
  pos → they reproduce the lower-left-facing-content layout from the defaults alone (`/tmp/d_montage.jpg`).
  All remaining host beats (16 `ah_v_*`/`c_av_l*`/`c_av_k*`/`c_av1` explain beats still on the old far-left
  placement) were then converted to `bust` + neutral transform → the whole cut is a consistent presenter
  layout now (half-bust host lower-left, facing the framed content; `/tmp/al_montage.jpg`).
  Also: **dragging a preset rig** (gpt-static) made a dead `manifest.json` clip — the drop handler now
  treats a `…/manifest.json` payload as a rig (`add_avatar_clip_at` by dir name), like double-click did.
- (cont.) The avatar inspector's **Emotion
  poses** panel zooms each thumbnail to the detected face (same detector) + a framing selector.
  `signature-opener` self-demos: giggle = `closeup` (snaps in, scale-spring removed), "welcome back"
  line = `bust`. In `luckymas`, the **solo intro-monologue beats** (host alone over the room bg —
  `c_av2`/`c_av3`/`c_av_o3_2_2`) were converted to `bust`; the side-by-side explain beats (host beside
  a screenshot/CRT inset) stay `full` (per the user: half-bust only the solo/reaction beats). Verified
  headless across poses (`/tmp/final_verify.jpg`, `/tmp/finalsim.jpg`, fed).
- **★ UX DIRECTION + GPT STATIC RIG PASS — NEW 2026-06-30.** Current target is technical
  video essays/deep dives with chuuni succubus-cosplayer Gemma, reaction images/memes, code,
  diagrams, and sourced B-roll. Durable task list: `docs/UX_NEXT.md`; suggested `/clear`
  after this pass, then resume from `CLAUDE.md` → `docs/STATUS.md` → `docs/UX_NEXT.md`.
  `luckymas` now uses `gemma-gpt-static`, built from
  `/mnt/f/Pictures/oc/gemma-san/video-sprites` as 3x2 static pose sheets (not mouth/blink
  animation). The active pngtuber model is static pose + procedural audio bob/light-up;
  mouth/blink/deformation is reserved for a possible future Inochi2D rig. New media drops
  get stock motion + framed/glow inset defaults; negative scale remains slider-editable;
  the laugh meme clip is quieter; `.playwright-mcp/` is gitignored.
  Known follow-up: current blur clips blur a scene/background plate, not the full composited
  frame, so native/foreground elements can pop instead of blurring.
- **★ EDITOR UX PASS 2 — timeline scroll · audio volume/loudness/rate · TTS wrap · glow/auto-grade · blur clip — NEW 2026-06-30
  (overnight polish for luckymas).** All in `editor/src/main.cpp` (+ `tools/export.sh`, `tools/slop.py`):
  - **Timeline lanes you scroll to no longer render EMPTY.** `DrawTimeline` sized every vertical span
    (gridlines/clip clip-rect/playhead/drop-test/content Dummy) to the VISIBLE viewport anchored at the
    scrolled origin → scrolling down raised the clip clip-rect's bottom above the viewport and culled the
    bottom lanes' clips. Now sized to the full content height (RULER + nLanes·ROWH); the Dummy reserves it.
  - **Audio: per-clip volume + per-lane (track) volume** (row `gain_db`, summed) in the preview mixer +
    export plan; **loudness normalize reworked to a GATED RMS** (measures speech, ignoring pauses) so a
    quiet line auto-levels right. Inspector audio block (clip/lane volume · normalize + target + live "auto
    gain"). `sync_to_doc` now persists ROW params (lane volume/normalize survive Save).
  - **Audio playback `rate`** (pitch-preserving): WSOLA time-stretch for preview (cached), ffmpeg `atempo`
    for export; the speed slider resizes the clip to keep the same audio. (Lip-sync tracks original timing.)
  - **TTS text box word-wraps** (soft-wrap the edit buffer; strip soft newlines before the provider).
  - **Outer glow + auto color-grade are now common, inspector-exposed clip look props** (glow on
    image/video/avatar; per-clip `auto_grade` grades a floating inset toward the bg like the host —
    image: desat+warm/cool, video: warm/cool only).
  - **New `blur` clip type** = a full-frame blur of the scene plate below, default ease-in→out bell, so one
    clip over a scene cut blurs A out + B in (place it on a track ABOVE the scene). Reuses the cached CPU
    gaussian → identical preview/export. Verified headless (edge energy 4.86→1.20 at the bell peak). (A
    true full-composite RT/shader blur — blurring native caption/code layers too — is a documented upgrade.)
- **★ EDITOR UX PASS: clip size in px · drag clips between lanes · crop/mask paint tool — NEW 2026-06-29
  (user requests while tweaking luckymas).** Three editor features + the keyframed-resize bug, all in
  `editor/src/main.cpp`:
  - **Clip SIZE in actual pixels (⇄ scale factor, aspect-locked, synchronized) — fixes the "resizing the
    CRT does nothing" bug.** `anim_xform` ignores the static `transform.scale` whenever a clip has scale
    KEYFRAMES, so the inspector's scale field was dead for the CRT cold-open inset (a spring pop-in), the
    Fufu zoom, any animated clip. The transform inspector now shows W×H output **px** (default) AND the
    scale factor, both driving `transform.scale` via **`clip_rescale()`** which multiplies the whole
    keyframe TRACK when animated (pop/zoom shape preserved, just bigger) — so resize works on animated
    clips. Native px per type (image texture / video frame / avatar sprite via `clip_native_size`);
    native clips (code/caption/shape/gradient) show the factor only. Aspect-lock default-on. Headless
    **`--clip-size <clipId> <wpx> [hpx]`** (D3D-free: stbi/libav for native dims). Verified: CRT 653→900px
    rescales its scale kf 0.51→0.703, pop-in/hold/out preserved.
  - **Drag a clip onto another lane of the SAME type to move it between rows.** Timeline body-drag reads
    the cursor's lane; dropping on a different row of the same type (image→image, avatar→avatar, …) moves
    it (target lane highlighted green). **`move_clip_to_row()`** updates the clip's `row` back-ref AND both
    rows' `clips` membership (the compositor walks `row.clips` for z-order); **`sync_to_doc` now persists
    `clip.row` + rebuilds row membership** (it persisted neither, so a move wouldn't survive Ctrl+S).
    Headless **`--clip-move <clipId> <rowId>`**. Same-type only (cross-type rejected).
  - **Crude crop & mask PAINT tool (Viewer ▸ image ▸ "crop / mask (paint)").** Non-destructive, like
    remove-bg: edits ride the sidecar + a `<file>.mask.png`, applied on the fly in `get_texture`
    (removebg → **mask** → **crop** into one buffer) so they flow to grid/timeline/export — source PNG
    untouched. **Erase/Restore brush** (size + feather) cleans up stray pixels a colour-key/rembg matte
    leaves; **box-erase**; **crop rect**. Live checkerboard transparency; commits per stroke; middle/right
    drag pans. Shared `mask_dab/mask_box/mask_load_or_blank/mask_save` + headless **`--lib-crop`/`--lib-mask`**
    (verify + agent); `--mask-mode` opens the tool for shots; `scan_library` skips `*.mask.png`. Verified
    end-to-end: erase + crop on gemma-happy → hole (bg shows through) + tighter framing over the canon room.
  - (Ops) the **XP capture VM was shut down** (was left running in the background; `xpvm.py --connect quit`).
- **★ XP CAPTURE @ 60fps (deterministic stepped) + copy-anim fixes — NEW 2026-06-29. The choreography is
  recaptured smooth at 60fps with no manual recording — and the starvation hack is retired.** Root cause
  (from the research memo): QMP `screendump` on a RUNNING guest steals vCPU time, so a fast grab loop made
  the guest clock advance unevenly (the documented ~3fps "gentle" workaround + motion-subset filter). Fix in
  `tools/xp/`: **`Qmp.pause()/resume()`** (QMP `stop`/`cont`) + **stepped capture** — each frame runs the guest
  exactly `1/fps`, then FREEZES it for the dump and thaws, so the dump is out of band and frames are evenly
  spaced in guest-time (KVM pauses the guest clock while stopped). `_capture_window`, the `Choreographer`
  (`hold`/`move_to`/`frame`), `mascot_clip`, and `copy_demo` all gained `stepped=True` (default); `--fps`
  default **30**, **`--fps 60`** for hero shots, **`--realtime`** falls back to the old free-running grab. The
  source motion is unchanged (the "perfect" choreography is preserved — just sampled finer). **Verified on the
  live VM: copy normal + hijack at 60fps, 600/600 dialog frames each; the mascot walks R→L with no trail.**
  - **`copy_demo` = poll-then-capture (robust):** a FIXED prewait can't time both modes (the un-starved *plain*
    copy, with no hijack loop pacing it, is much faster than the *hijack* copy), so after launching copyanim it
    **`_wait_for_motion`s** until the dialog is actually up, THEN stepped-captures. Two non-obvious fixes found
    on the VM: the poll dumps are **FROZEN** (`stop`/`cont`) so the wait itself doesn't starve+delay the launch,
    and the motion check is cropped to the **upper screen** (`_dialog_region`) so an XP tray balloon ("Your
    computer might be at risk") down in the corner isn't mistaken for the dialog. `_frames_with_motion` trims
    the clip to dialog-up frames (same region).
  - **`copyanim.c` (user-reported):** (1) **trail fixed** — the hijack mascot bobbed a few px past the
    SysAnimate32 `band` we clear each frame, and those rows never got erased → a trail at the bottom edge;
    now `IntersectClipRect(hdc, band)` clips her to exactly what we clear (motion unchanged). (2) **walk
    direction flipped** right→left (`px = band.right - …`) — the pose strides left, so the old left→right
    travel read as moonwalking. (3) **fresh source dir per run** (`copysrc_<tick>`) — a cached re-copy of the
    same files is RAM-fast and flashes by (esp. the 2nd mode), so each run writes its own source → the dialog
    reliably lingers. Rebuilt the i686 PE; re-staged by the demo. (`.exe` is gitignored — `make -C tools/xp/win`.)
  - **`wake_guest()` (screensaver):** after ~2h idle the VM blanks to the black "Windows XP" logo screensaver, so
    `mascot`/`bitblt`/`copy` captures wiggle the mouse first → the demo lands on the Bliss desktop, not a black screen.
  - **RE-WIRED INTO `luckymas` @60fps (regenerable assets-src):** `choreograph.py --fps 60 all` → copy `copy-hijack`,
    `copy-normal`, `mascot-move`, `demo-comparison` over `assets-src/xp-*` (the asset URIs already point there; in-process
    decode fills fps/frames). Beats verified @106/155/204s. Cosmetic follow-up: the Security Center "at risk" tray balloon
    shows bottom-right in desktop captures (suppress via guest registry in provision).
  - **CRT cold-open un-blacked:** the in-process decoder read `luckymas-crt.mp4` from frame 0 (the file's black lead-in)
    while the proxy was offset to the 18s content → black inset. Fixed by trimming the content segment to a decoder-friendly
    mp4 (`ffmpeg -ss 18 -t 10 -an -vf scale=1280:720 -r 30 -c:v libx264 -pix_fmt yuv420p assets-src/luckymas-crt-seg.mp4`),
    frame 0 = content; `vid_crtcopy.uri` points at it (proxy/fps/frames dropped → decoder fills them). Plays in motion.
- **★ LIBRARY AVATAR-RIG ITEMS + AVATAR CLIP FROM SCRATCH — NEW 2026-06-29. Author an avatar in the library,
  edit it, drop it on the timeline; all its clips inherit the emotion frames.** Closes the user's "can't add an
  avatar clip except by splitting." A rig is a tiny def `library/avatars/<name>.avatar.json` = a **`prefix`**
  (emotion E → `library/images/<prefix>E.png`) **+ `emotions` overrides** (a manually-named emotion → a specific
  library image; overrides win). Pieces in `editor/src/main.cpp`:
  - **Resolution:** `get_rig()` now falls back from `presets/avatars/<rig>/manifest.json` to the library def —
    builds a static-pose rig (one frame per emotion, full library/images paths through the same `get_texture`, so
    `removebg` sidecars still matte). The canonical set is `AVATAR_EMOS` (neutral/happy/smug/confused/annoyed/
    surprised/sad). Edits drop the rig cache (`invalidate_rig`) → live.
  - **Library (`LIB_AVATAR`):** scanned from `library/avatars/`, shown as a **teal** grid cell (thumbnail = the
    fallback pose), with a **"rig"** type filter + a **"+ avatar"** author modal (name + prefix, live "resolves:
    …/missing: …" preview).
  - **Viewer rig editor:** prefix field + the resolved emotion frames as **live thumbnails**, each with a
    **per-emotion image-override dropdown** (`(use prefix)` or any library image) + an **Add override** (custom
    emotion name). Every change live-saves the def. (Rename disabled for rigs — delete+recreate, so `rig` refs
    can't orphan.)
  - **Place from scratch:** double-click / drag a rig onto the timeline → **`add_avatar_clip_at`** ensures an
    avatar row that references the rig (adopts a rig-less avatar row, else creates one + auto-wires `driven_by`
    to the first VO row for lip-sync/emotion auto-follow), then adds a static-pose clip (`emotion:"auto"`). All
    clips on the row share the rig. (Fixed `add_track` to RETURN the new row id — a `std::map` re-scan would
    re-pick an existing avatar row.) Headless **`--add-avatar <rig> [time]`** (automation + smoke test).
  - **Self-demo (committed):** `library/avatars/gemma-host.avatar.json` (prefix `gemma-`) resolves all 7 emotions
    from the seeded `library/images/gemma-*.png` → open the editor, Library ▸ filter **rig** ▸ select it (Viewer
    shows the editor) ▸ drag it onto the timeline. Verified end-to-end: `rig:gemma-host emotion:smug` →
    `gemma-smug.png` composited over the canon room; an override `smug→gemma-ominous.png` correctly wins.
- **★ UNDO/REDO + STRUCTURAL EDITS ARE IN-MEMORY — NEW 2026-06-29. Everything is undoable; no edit ever
  rolls back to the last save.** Two coupled changes to `editor/src/main.cpp`:
  - **Root-cause fix (the user's "rough edges"):** `split_clip`/`delete_clip`/`apply_generations` used to
    patch a STALE `p.doc` (it lacked unsaved timeline moves/trims/param tweaks, which live in `p.clips`)
    then **write+reread from disk** — so unsaved edits were lost (rollback) and split computed its halves
    against mismatched starts (the **overlap**). `load_project`/`save_project` were split into
    **`parse_project_json`** (doc→model, no disk I/O) + **`sync_to_doc`** (model→doc, no write). Structural
    ops now **`sync_to_doc` FIRST** (fold unsaved edits in) → patch the doc → **`parse_project_json`**
    (rebuild in-memory, NO reread). split/delete are pure in-memory (persist on Ctrl+S, like move/trim — no
    forced save); generate stays in-memory too (headless `--generate`/`--split`/`--delete` now save
    explicitly). Split is adjacent halves `[start,t)`+`[t,end)` — no overlap, no ripple. Verified headless:
    split `c_bg`@4s → `[0,4)`+`c_bg_2 [4,8)`, others untouched.
  - **Automatic doc-snapshot undo (the discipline the user asked for):** the doc fully captures state, so a
    snapshot is a compact `doc.dump()` string and undo = restore a past dump (`parse_project_json`).
    `undo_checkpoint()` runs every frame but only commits at **gesture boundaries** (`!ImGui::IsAnyItemActive()`
    after activity — timeline drags/trims, inspector sliders, held buttons all set an active id) AND when the
    doc actually changed → a 60-frame drag coalesces to ONE step. **A new feature needs no `push_undo()`** —
    mutate `p.clips`/`p.doc` and the next settle captures it (the only bookkeeping is `g_undoDirty`, a hint for
    async/non-widget edits: job landings, the S-key split, OS file drops; a missed flag just groups into the
    next gesture, never lost). **Ctrl+Z** / **Ctrl+Shift+Z** + **Ctrl+Y**; suppressed while typing (the input
    box keeps its own char-undo). Buffer `UNDO_CAP=300` compact dumps (~tens of MB — "plenty of RAM"). Save
    now adopts its own mtime so the live-reload watcher can't wipe history; external/manual reload re-baselines.
- **★ XP CAPTURE HARNESS (autonomous QEMU Windows-XP VM) — NEW 2026-06-29. The BitBlt-vs-screenshot demo
  is PROVEN end-to-end.** `tools/xp/` is a self-contained harness that boots a Windows-XP guest under **KVM**
  (works in WSL2 — `/dev/kvm`) and drives it host-side via **QMP** — `screendump` capture (off QEMU's live
  framebuffer, so it sees per-pixel-alpha LAYERED windows + never goes black) + absolute mouse/key/text input,
  plus the agent-less **SMB control** (netexec `--exec-method smbexec` → `iexec.exe` on the interactive console
  desktop; smbclient NT1 files) reused from `../retro-hardware/xp-remote-probe`. Pieces: **`xpvm.py`** (QEMU
  driver + `Qmp` client; boot frees QMP so `--connect` can drive a running VM), **`xpsmb.py`** (exec/gui/files
  against the forwarded SMB port), **`provision.py`** (UNATTENDED SP3 install → golden qcow2: winnt.sif floppy +
  first-logon `setup.cmd` baking firewall-off + blank-pw network logon + autologon + staged tools; monitors over
  QMP, auto-dismisses the first-logon modal), **`choreograph.py`** (scripted choreography → frame capture → mp4 →
  slopstudio `video` asset + the BitBlt demo), **`vnckey.py`** (RFB key/click when a process holds QMP), and four
  **i386 / subsystem-5.1** guest tools built from the flake's new `pkgsCross.mingw32` cross (`win/{iexec,winrect,
  bitblt,mascot}.c`) — `bitblt.c` is a naive GDI `BitBlt` (misses layered windows), `mascot.c` a `WS_EX_LAYERED`
  + `UpdateLayeredWindow` floater (the video's `Launch.exe` technique). All in a dedicated **`nix develop .#xp`**
  shell (qemu + i686 cross + pillow + mtools, isolated from the lean editor shell). **Verified end-to-end on this
  box:** fresh SP3 install → golden `cache/xp/xp.qcow2` (gitignored) → booted → SMB up → `choreograph demo` →
  **`demo-comparison.png`: the layered mascot is PRESENT in the host QMP screendump and ABSENT in the in-guest
  GDI BitBlt** (the honest video-001 reveal), pushed to the feed. Orientation: **`tools/xp/README.md`**.
  ([[xp-vm-capture-system]])
- **★ XP VIDEO-001 DEMOS — BUILT, CAPTURED + WIRED INTO `luckymas` — NEW 2026-06-29. Both tricks reproduced on real
  XP, captured, and dropped into the cut.** Three new i686 guest tools + choreograph demo modes turn the harness into a
  one-command capture rig (`choreograph.py --out … all` → stage + 3 clips/stills):
  - **Layered mascot (upgraded `mascot.c`):** loads a REAL sprite (a premultiplied-BGRA blob baked host-side by
    **`png2raw.py`** from `library/images/gemma-smug.png`) instead of the procedural diamond, **click-through**
    (`WS_EX_TRANSPARENT`) + **self-animating** (bob+drift via `WM_TIMER`; `--static` to freeze). → `mascot-move.mp4`
    (Gemma floating on the desktop) + the **BitBlt-vs-screendump A/B** (`demo-comparison.png`: Gemma PRESENT in the host
    QMP grab, ABSENT in the in-guest GDI BitBlt — the preservation reveal).
  - **Copy-hook (`copyanim.c`):** ONE process — a worker thread runs `SHFileOperation(FO_COPY)` (raising XP's `#32770`
    copy dialog) while the main thread finds ITS OWN dialog, hides the `SysAnimate32` flying-papers control, and
    AlphaBlends our mascot walking across the bar (the MinkIt trick, faithfully reproduced). `--plain` = the stock
    dialog. → `copy-hijack.mp4` + a normal-vs-hijacked compare. (A separate-process watcher `copyhook.c` couldn't see the
    dialog via EnumWindows — a console-session desktop quirk — hence same-process; kept as a diagnostic.) **`setres.c`**
    lifts the guest to **1024×768×16** (crisp captures, kills the low-res nag); **`wlist.c`** dumps window classes.
  - **Wired into `examples/luckymas.slop.json`** (self-demonstrates on scrub; verified via `--shot-frame`): copy-hook
    repro inset @106s (the "20 lines of C" beat), floating mascot @155s (the `mascot.c`/`UpdateLayeredWindow` code beat),
    the **BitBlt reveal** full-frame @204s (the "raw GDI BitBlt → bare desktop" beat). Sources in `assets-src/xp-*.{mp4,png}`
    (gitignored, regenerable via the harness — same pattern as the CRT footage). **Capture gotcha (documented):** rapid QMP
    screendumps starve the VM → the copy dialog stretches/shifts late; capture GENTLY (~3fps) over a long window.
  - **`tools/slop.py` gained `--asset`/`--asset-uri` + `--pos`/`--scale`** so an agent registers a media asset + places a
    video/image clip in ONE command (used for the wiring). Camtasia / interactive human capture is the remaining follow-up.
    ([[xp-vm-capture-system]])
- **★ IN-PROCESS VIDEO DECODE (libav linked into the editor) — NEW 2026-06-29. The direct-decode arc,
  CLOSED: B-roll plays from the mp4 with no proxy step.** The flake cross-compiles a **minimal static
  ffmpeg** for the Windows PE (`ffmpegCross` — only `libav{codec,format,util,swscale}`+swresample with
  FFmpeg's BUILT-IN decoders, every external media dep OFF; the upstream cross is flagged broken only
  because its default set pulls libvmaf, so the stripped build clears the flag). The editor links it
  (`-DSLOP_LIBAV`, Makefile `VIDEO_LIBS`) and gains a **`VideoDecoder`** (open mp4 → accurate seek →
  `avcodec` decode → `swscale`→RGBA→D3D11 texture) sitting behind the **SAME `clip+time→SRV` contract**
  the JPEG proxy used: `get_decoded_frame_tex(src,idx,decoder)` shares the LRU `FrameTex` pool, and the
  compositor's `video` branch just tries the decoder first (`vm.src`), falling back to the proxy
  (`vm.proxy`) when there's no libav / the source won't open. A `video` asset is now just `{type:"video",
  uri:"…mp4"}` — the decoder fills fps/frames/dims on first open. **Verified end-to-end:** distinct
  decoded frames at t=0.5/4/8/11.5 (boot screen → XP wizard) in preview AND a full **mp4 export** (720
  frames streamed through the same `composite_frame`); `--no-video-decode` forces the proxy fallback
  (renders the placeholder, no crash). Open decoders are capped+LRU-closed (6); resident frames capped
  (96). **`examples/video-broll.slop.json` is the self-demo** (points straight at the mp4 — no setup
  beyond the gitignored source). Eventual NVDEC/d3d11va swaps the decode SOURCE inside `VideoDecoder`
  without touching the compositor. **Drag-drop a video onto the timeline now JUST WORKS** (library DnD +
  OS file-drop + double-click → `add_video_clip_at` probes the duration via the decoder, auto-creates a
  `video` row). `tools/video-proxy.py` is kept as the fallback / higher-fps master path.
- **★ rembg PROVIDER + editor wiring — NEW 2026-06-29. Cleaner background removal (AI matte) for images.**
  New **`providers/rembg`** (`remove_bg`): an image (base64 in params) → **RGBA cutout** via **rembg
  isnet-anime** (the anime host + stylised art; model-selectable: isnet-general-use/u2net/u2net_human_seg/
  silueta) — segments soft/gradient bgs + **purple-on-purple** the colour key can't. CPU (dodges the
  3080/7800XT gen contention), **live on lame `:8015`** (`slop-rembg`, `restart=unless-stopped`), model
  ONNX persisted on `/lamedata`. Deterministic `SLOP_REMBG_FAKE` corner-key path → **5 tests** (21 total
  pass). The editor's **`removebg` sidecar block** gained `method:"rembg"` alongside `"colorkey"`:
  `item_removebg` loads the provider cutout (cached, recorded in the sidecar) in place of the source —
  **non-destructive, flows to grid/Viewer/timeline/export through the one texture path**. The **Viewer's
  Remove-bg control** has a method picker (color key / rembg AI) + a model dropdown + a **Cut out** button
  (worker thread → `run_provider_job` → caches the cutout → patches the sidecar → live re-key). Headless
  **`--lib-removebg <library-file> [model]`** for automation/verify. **Verified end-to-end on live lame:**
  `gemma-ominous.png` → clean isnet-anime matte (73% transparent) composited over the canon room with **no
  white box**. (Cutout cache is gitignored → like gen-history, the rembg self-demo applies via Cut out /
  `--lib-removebg`, not on load; `gemma-ominous` keeps its instant **colorkey** self-demo.)
- **Foundation** — flake (mingw-w64 + Dear ImGui DX11 + glslang + spirv-cross + nlohmann +
  stb + a python provider env), MIT, the design/contract docs, and `examples/signature-opener.slop.json`.
  `nix develop` works (homelab cache).
- **Provider protocol** — `providers/base.py` (async job queue, content-hash cache, WS
  progress, `/capabilities`, `/jobs`, `/assets`) + `providers/mock` + `providers/tests`
  (**5 pass**).
- **TTS provider — LIVE on lame, DESIGN + CLONE** — `providers/tts` (Qwen3-TTS-12Hz-1.7B,
  Apache-2.0) as docker `slop-tts` on lame's **3080** (`http://lame:8010`). Two modes behind
  `speech`, one model resident at a time (swaps to fit the 3080 + align):
  - **clone (stable, production):** when a preset ships a golden **ref clip**, the editor sends
    it + transcript and the **Base** model clones it (`create_voice_clone_prompt` cached +
    `generate_voice_clone`) → **identical timbre across lines**. Fixes VoiceDesign's per-call
    drift (docs/RESEARCH §Voice STABILITY). Bake a ref with `tools/bake-voice-ref.py <preset>`
    → `presets/voices/<name>.ref.wav` + `ref`/`ref_text` in the preset. (Clone drops per-line
    `instruct` emotion — fine for a host.) **`gemma-san-deep` is baked + cloning, verified.**
  - **design (exploration):** no ref → **VoiceDesign** derives the voice from the instruct
    (drifts across lines) — used to PREVIEW/author new voices and to bake refs.
  Voice variants (`presets/voices/`): `gemma-san`, `-mid`, `-deep` (the host pick, cloned). The
  editor's TTS inspector has a **voice selector**; Tools ▸ Voice preset editor authors new ones.
- **align provider — LIVE on lame, validated end-to-end** — `providers/align` (docker
  `slop-align` on the 3080, healthy at `http://lame:8014`). Two capabilities: **word_timing**
  (WhisperX, CUDA → per-word `[t0,t1]` for captions) and **visemes** (Rhubarb 1.14, MIT/CPU →
  mouth-shape cues + normalized `openness`, the pngtuber lip-sync input). Validated on the
  real Gemma VO: 12 words aligned, 25 Rhubarb cues (`engine=rhubarb`). Audio input is resolved
  **by content-hash from a local store** (sibling caches mounted read-only) — zero-copy, no
  provider↔provider HTTP. Both 3080 models coexist in ~6/10 GB.
- **image provider + ComfyUI — LIVE on lame's 7800XT (ROCm), verified end-to-end.** ComfyUI
  (`comfyui`, v0.26.0) runs Illustrious-XL-v2.0 (RAIL-M, commercial-safe) on the **RX 7800XT
  via ROCm 6.2** (16GB free; keeps the 3080 for tts/align/NVENC). `providers/image` (`slop-image`
  on `:8011`) is a thin ComfyUI adapter (`text2image`): owns an SDXL workflow template, injects
  prompt/seed/size/steps/cfg, submits to ComfyUI's HTTP API, returns the PNG. Adapter + engine
  share docker net **`slopnet`** (adapter → `comfyui:8188`). Verified: editor image-Generate →
  on-character Gemma chibi in ~20s (resident), composited live in place of the ref-sheet stand-in.
- **Gemma-san chibi LoRA — TRAINED (v2), wired, verified end-to-end.** Custom Illustrious-XL LoRA
  for a consistent host. The adapter inserts a `LoraLoader` when a clip's `lora` param is set
  (`g["10"]` in `_build_graph`); the example's `r_react` row uses `lora: "gemma-san-chibi-v2",
  lora_strength: 0.9`. **v2** (the good one): **3080/CUDA bf16 @ 1024, dim 64, +text-encoder**, on a
  **38-img reference-heavy set** (turnaround/horn/tail sheets cut to singles + back-views), with the
  **design tags pruned** from captions so the trigger owns the design. Fixed v1's wing/horn/tail
  flicker — A/B is decisive and v2 holds the full design across poses incl. back-view wings-spread.
  (**v1** = `gemma-san-chibi`, fp32@768/dim32/unet-only on the 7800XT — kept for reference; appendages
  were inconsistent. The fp32 was a forced NaN workaround, see gotcha; CUDA fixed that.)
- **★ Anima host engine + canon backgrounds + color grade — NEW 2026-06-28.** **Anima 2B**
  (`anima-base-v1.0`) is the **default/main image model** (backgrounds AND characters). The local
  **`gemma-san-anima` LoRA** (3080/kohya, dim32/10ep, loss .0605) locks the host — stable across framings,
  beats Illustrious — wired into `providers/image` as **`arch='anima'`** (`lora`/`lora_strength` ~0.9;
  `anima_text2image.json`), deployed to `slop-image` + smoke-tested end-to-end. Say `chibi` to force chibi
  vs the tall succubus. **Canon backgrounds** `presets/backgrounds/{room-day,desk}.png` (bright/less-purple
  so the purple host pops; `backgrounds.json` records prompt+seed). **Per-clip color grade** in the
  compositor — `saturation`/`contrast` (cached processed-texture, like blur) + `temperature`/`tint`
  (per-channel tint) — keyframeable + JSON/LLM-tweakable + live-reload; tames oversaturated gens and is the
  lever for **integrating the host into a plate**. The integration is SHIPPED in-editor: the avatar clip
  takes `saturation`/`contrast`/`temperature`/`tint` (same grade as image clips) + a `shadow` contact-shadow,
  and project `meta.vignette` darkens frame edges — the crude "blend the host into the bg" combo (grade +
  shadow + vignette), all live + keyframeable. (Authoring note: `tracks` are TOP-first — array[0] draws on top.)
- **★ Signature opener recipe + keyframe inspector — NEW 2026-06-28.** `examples/signature-opener.slop.json`
  (the new **default project**, replacing the retired hello-gemma) — the host giggles zoomed CLOSE (keyframed
  avatar scale, spring) then backs off for "Welcome back, mortals." on the canon day room, integrated; a
  reusable recipe (its `notes` document making variations). Voice = `gemma-san-deep-jp` (designed JP→EN),
  per-sentence clips. The clip inspector gained a **Keyframes panel** — edit any clip's curves (t / value /
  interp / spring, add @ playhead, delete) with live preview; **Save serializes keyframes** (it previously
  synced only transform/params). Avatar clips now honor keyframed scale/anchor (the zoom); `--generate` snaps
  `dur` only for speech clips (fixed an avatar disappear-gap). `tools/demo-cache.sh` regenerates any project's
  clips via `--generate`.
- **★ Auto bg→host match + full opening — NEW 2026-06-28.** Avatar clips now **auto-match the background by
  default** — sample the bottom-most active bg plate (`image_mean`, cached) → grade the host toward it (desat
  + warm/cool) + a contact shadow, no params (`auto_grade:false` opts out; explicit grade/shadow override).
  The full **host opening** is `examples/opening.slop.json` (~28s, mp4 in `exports/opening.mp4`): giggle zoom +
  "Welcome back, mortals." + the MoistCr1TiKaL intro, per-sentence `gemma-san-deep-jp` VO + per-sentence emotes
  (smug/happy/surprised). NEXT for the host: a **pose library** (gestures + smile/neutral variants) + switching
  poses every ~2-4s so it is less stiff (gen Anima+LoRA sprites + a keyframeable `pose` selector).
- **Phase-1 spine CLOSED — lip-synced avatar, verified end-to-end.** The editor's **pngtuber
  state machine** (compositor `avatar` branch): an `avatar` clip's **Generate** resolves
  its row's `driven_by` VO asset and submits `visemes` to align (`inputs:[{hash,uri}]`); the
  returned track drives **mouth openness at the playhead**, the clip's `emotion` tag drives the
  **expression**, plus procedural breathing — all pure GPU compositing, instant.
  Validated: voice→align→avatar, mouth open at viseme `D` (1.0), closed at `X` (silence),
  emotion=smug.
- **★ Avatar = a STATIC pose per expression + an audio-reactive bob/light-up (reworked
  2026-06-27) — verified.** **Decision: no per-frame animation until an Inochi2D rig.** Neither SD
  nor authored face-sheets are stable enough to ANIMATE a character frame-to-frame and feel good,
  so the host is **one PNG per expression**, sold as "talking" by a **bob** (gentle idle + a bounce
  when speaking) and a **light-up** (additive brighten ∝ viseme openness) — the classic pngtuber
  move. **Tweakable live** (inspector sliders + clip params): idle breathing `bob`/`bob_speed`;
  audio-reactive `talk_bob` (up/down) and `talk_scale` (scale) as SEPARATE knobs; `talk_attack`/
  `talk_decay` (1/s envelope on the reactive level so it eases in/out, not snaps); `lightup`
  (talk brighten) and `dim` (silent brightness, <1 dims when quiet). The talk level is a
  deterministic attack/decay envelope over the visemes (`viseme_talk_at`); light-up is an additive
  D3D11 blend. `avatar_sprite` picks the pose by the emotion tag (clip tag, or `"auto"` → the
  driven VO line's emotion), sampled relative to the DRIVING VO clip; `draw_pngtuber` is the
  procedural fallback. Sizes by native px × clip scale.
  - **`gemma-chibi` — host rig, from AUTHORED face sheets.** Sheets (`face_<expr> pose_<pose> …png`
    in `/mnt/f/Pictures/oc/gemma-san/chibi-animation/`, 1536×1024 = 2 eyes × 6 mouth) are sliced by
    `tools/slice-face-sheet.py` into a `mouths[6]`+`mouths_blink[6]` manifest; the compositor
    currently renders just the **resting frame** (the flap grid is kept dormant for when a stable
    animation path — Inochi2D — exists). **neutral** wired; more expressions drop in as authored.
  - **`gemma-pngtuber` — SD single-pose library.** Illustrious-XL + `gemma-san-chibi-v2` LoRA, one
    clean static sprite per emotion (`{"sprite":"x.png"}`); cheap to generate any pose on demand for
    video. (Why SD is single-frame: txt2img tag-swap drifts the body; img2img won't open a small
    mouth; inpaint bleeds artifacts + can't resolve >2 mouth levels — see gotchas.)
  - Both share **`tools/cutout-sprites.sh`** (rembg matte → halo-kill → **align every frame to a
    common feet-baseline + x-center**, so swapping mouth/eye/emotion never jitters), then pngquant.
    The compositor sizes sprites by native px × clip scale, so the 256×512 sheet rig uses a larger
    clip scale than the 768×1344 SD rig (the demo's `c_av1` is tuned for `gemma-chibi`). `raw/`
    gitignored. **The image provider also gained img2img + masked-inpaint** (`init_b64`/`mask_b64`/
    `denoise`) — kept as a general capability even though the avatar no longer relies on it.
- **Export — produces a real mp4 (verified).** The editor's deterministic full-res graph
  walk: `--export` renders each frame with the **same `composite_frame`** the preview uses
  (offscreen RT + background draw list) and streams raw RGBA to stdout; `--export-plan` emits
  dims + the audio recipe + auto-credits (from sourced-asset attribution). `tools/export.sh`
  pipes the frames into **ffmpeg** (libx264 1080p default, `SLOP_NVENC=1` for h264_nvenc) and
  muxes the audio (per-clip delay+gain+amix) → `exports/<name>.mp4`. Verified: demo + the
  lip-synced av-test → 1080p h264 + AAC VO, lip-sync baked into the encoded frames (open at
  3.22s, closed at 1.90s). Missing audio (e.g. unbuilt music) is skipped; credit auto-assembled.
- **Editor (Windows PE)** — `editor/` (C++ · ImGui · D3D11, mingw-cross, run via WSLInterop):
  timeline of pipeline rows; **live compositor** (real textures at the playhead, per-clip
  transform/opacity); **audio** (timeline waveform + per-clip Play/Stop audition via winmm).
  Headless `--shot` capture for llm-feed.
- **★ Per-clip tweak UX — NEW 2026-06-28 (usability pass).** Every clip type is now hand-tweakable in the
  inspector — the native compositing clips (**code / caption / shape**) were JSON-only + their keyframe
  panel was trapped inside the generable-only block. Now: a real **code editor** (multiline, `imgui_stdlib`
  `std::string`) for the typewriter clip + lang/title/line-numbers/highlight(csv)/font/typewrite/scroll;
  caption style/text/sub/gloss/align/box + **color pickers** (accent/text/sub); shape kind/size/thickness/
  round/from-to/grow + stroke+fill colors; an **anchor** transform widget; and the **keyframe editor moved
  out** so code/caption/shape get it too. `--select <clipId>` for headless inspector shots.
- **★ Timeline zoom/scroll + ripple editing — NEW 2026-06-28.** The timeline mapping (`pps`/`T2X`) is now
  zoom+scroll aware: **mouse-wheel = zoom** (anchored at the cursor; floor snaps back to auto-fit), **Shift+wheel
  = horizontal scroll**; off-screen gridlines/clips are culled + a zoom% / window readout shows when zoomed.
  **Ripple editing** (reworked 2026-06-28 after feedback): **right-edge trim** ripples the clips *after* as a
  block (**Shift = resize WITHOUT ripple**); **left-edge trim** is a plain in-point trim (no backwards ripple).
  **Shift-drag** a clip ripples a BLOCK with it — the clips *after* by default, **hold Alt** for the clips
  *before*; clamped at t=0 (no jarring rebase). Plain move carries the clip's own keyframes. (Needs a human mouse.)
- **★ ASSET LIBRARY (L1) — NEW 2026-06-28. A global, cross-project golden-media library, browsable in the studio.**
  `library/<images|audio|video>/` is a dir the editor **scans** (drop a file in on disk OR via the panel → it
  appears; **filename = display name**). **Tools ▸ Library** (default-open, docks top-right): a searchable grid with
  **image thumbnails** + type filters (all/img/aud/vid), **Add files…** (multi-pick → copied in, de-duped), **Rename**
  (modal → moves the file) + **Delete** (context menu). **Double-click** an item to drop it at the playhead;
  **drag** onto a timeline lane to place it (an ImGui drag-drop `LIB_ITEM` payload → `add_image_clip_at`). Seeded
  starter set: the Gemma host reaction sprites (`gemma-*`) + canon backgrounds (`bg-*`). Code: `scan_library`/
  `library_import`/`library_rename`/`library_delete` + `draw_library_window` (Win32 `FindFirstFile`/`CopyFile`/
  `MoveFile`; UTF-8↔W via `to_w`/`from_w`). **Library push status: ALL LANDED** — L1 ✓ · L2 sprite processor ✓ ·
  L3 audio→timeline ✓ · L4 gen items + regenerate + history ✓ · L5 viewer (pan/zoom + scrub) ✓ · L6 TTS pacing split ✓.
- **★ SPRITE-SHEET PROCESSOR (L2) — NEW 2026-06-28. Cut a GPT sheet into background-keyed, auto-trimmed library
  sprites.** **Tools ▸ Sprite sheet**: Load a sheet → **key out the flat background** (a colour swatch + an eyedropper
  *pick from image* + a **fuzz** slider; live checkerboard preview shows the keyed transparency) → mark **rectangle
  crops** (an **auto-grid** rows×cols for the common GPT grid sheet, or **freehand drag** on the image) → **Export →
  library** writes each crop **auto-cropped to its minimum (alpha) bbox** as `library/images/<prefix>-NN.png` (simple
  rects, no masking — by design for now). Processing core (`sprite_color_key`/`sprite_crop`/`sprite_alpha_bbox`/
  `sprite_cut_one`) is shared with a headless **`--sprite-cut <sheet> <RRGGBB> <fuzz> <prefix> <x,y,w,h>…`** mode
  (agent/automation + verification). Verified: a 2×3 magenta test sheet → six 200×200 cells **trimmed to 91×91**
  with **alpha-0 corners** (bg keyed). (`--sprite-load <png>` opens the panel on a sheet for dev/shots.)
- **★ Library audio (L3) + TTS pacing split (L6) — NEW 2026-06-28.** **L3:** the library accepts **sounds** (wav/mp3/
  ogg/flac/m4a) + **video** files (scan/import/show); double-click/drag an **audio** item onto the timeline →
  `add_audio_clip_at` adds a clip on a music row **at the WAV's real decoded length** (mixed by the same recipe as gen
  audio). (Video library items still need a decoded proxy — `tools/video-proxy.py` — so they're not dropped directly
  yet.) **L6 (already worked, now verified):** **Split @ playhead (S)** on a `tts`/`music` clip makes two clips sharing
  the SAME asset with the right half's `params.in` += split offset (`split_clip`) — so a **long TTS gen can be placed,
  split, and the halves moved apart to insert pauses with ZERO regeneration** (the preview mixer + waveform + export
  all honor `in`). Verified: split a 6.08s VO at t=6 → halves [3.64,6.0)+[6.0,9.72) share the asset, right `in=2.36`.
- **★ LIBRARY GEN ITEMS (L4) + VIEWER (L5) + "add a gen item" — NEW 2026-06-28. The library closes: gens are
  first-class items you can author, regenerate, and restore from prior takes — no JSON.** Three pieces:
  - **"Add a gen item"** (the entry point): Library toolbar **+ image** / **+ voice** → a modal authors a NEW
    *generatable* library entry (not just an import). Image = the **Anima host engine** (prompt + seed + "Gemma host
    LoRA" toggle + size); voice = **text + a TTS voice preset** + emotion + seed. Submits a provider job on a worker
    thread → lands as `library/<images|audio>/<name>.<ext>` **+ a `<file>.meta.json` gen sidecar** (records the recipe +
    content hash + a `history` of past gens). Gen items show **purple** in the grid (`LibItem.gen`, stat'd at scan).
  - **L4 — regenerate + history (in the Viewer):** a gen item's sidecar recipe is **editable** (prompt/seed/host-LoRA,
    or text/voice/emotion/seed) → **Regenerate** / **Regenerate (fresh)** (bumps the seed) re-runs in place, demoting the
    prior gen into the sidecar `history` (newest-first, capped 24). The **history strip** (expandable, image thumbnails)
    **restores** any past take — copies its cached bytes back over the library file + swaps the sidecar. (Past-gen bytes
    live in the content-addressed `cache/` — gitignored, so restore is best-effort, like the per-clip gen-history.)
  - **L5 — viewer panel** (**Tools ▸ Viewer**, default-open, docks below the Library): **image pan (drag) + zoom
    (wheel, anchored at the cursor; double-click = fit)**; **audio waveform + a draggable scrub marker + Play/Stop**
    (WAV, whole-file — matches the editor's no-mp3-decoder constraint); shows the gen recipe + Regenerate + history when
    the selected item is a gen.
  - **Code (`editor/src/main.cpp`):** the clip-gen submit→poll→download was **refactored into a shared `run_provider_job`**
    (progress-callback) now used by BOTH the clip path (`gen_worker`) and the library path (`lib_gen_worker`); `start_lib_gen`/
    `lib_job_body`/`speech_params` (extracted) / `lib_load_sidecar`/`lib_save_sidecar` / `draw_lib_gen_modals` /
    `draw_viewer_window` / `lib_restore_gen` / `invalidate_texture`. Headless (automation + verify, no window):
    **`--lib-gen <image|voice> <name> <prompt|text> [preset]`**, **`--lib-regen <library-file>`** (fresh seed, keeps
    history), **`--lib-select <library-file>`** (focus the Viewer for shots). **Self-demo:** `tools/seed-lib-sidecars.py`
    writes gen-recipe sidecars for the committed `gemma-*` host sprites + `bg-*` backgrounds (Anima recipes) → selecting
    one in the Viewer shows its recipe + Regenerate + pan/zoom **on load, no setup**. Verified end-to-end on live lame:
    voice gen (2.0s WAV + sidecar), image gen (1024² Anima chibi + hash), regen (history populated + restorable), clip-gen
    regression (`--generate` still OK after the refactor), + Viewer shots (image/audio/recipe) pushed to the feed.
- **★ Library/Viewer polish — NEW 2026-06-28 (user feedback).** (1) **In-flight gen placeholder:** a NEW gen has no file
  until it lands (~20s), so the grid now draws a **purple "NN% · gen…" placeholder cell** for each active lib-gen
  (`libgen_pending`), and the Viewer **doesn't `get_texture` a not-yet-landed gen** (guarded on `gs.state==1`) — kills the
  one-shot `tex load FAILED can't fopen` the user saw. (2) **Audio Viewer = zoom + play-from-marker:** the waveform now
  **zooms (wheel, cursor-anchored) + scrolls (Shift+wheel)** (`g_vAudioZoom`/`g_vAudioView0`); **Play starts from the
  scrubbed marker** (a one-shot `waveOut` over the decoded PCM from the offset — `audition_play`/`audition_pos`), the
  marker **advances with playback** (auto-follows when zoomed), drag re-seeks on release. (3) **Rename bug fixed:**
  `library_rename` now **moves the `.meta.json` sidecar with the media** (renaming a gen item was orphaning its recipe).
- **★ ON-THE-FLY BACKGROUND REMOVAL — NEW 2026-06-28 (user ask). Cut a gen's flat bg to alpha, non-destructively,
  re-tweakable, flowing everywhere.** A library item's sidecar can carry a **`removebg`** block
  (`{method:"colorkey", key:[r,g,b], fuzz, enabled}`); **`get_texture` applies it on cache-miss** (`item_removebg` →
  the shared `sprite_color_key`) so the **matte is what gets cached + composited — the source PNG is never touched**, and
  it flows to the **grid thumbnail, Viewer, timeline, AND export** through the one texture path. The **Viewer** gains a
  *Remove bg* control on image items: enable + an **eyedropper** (`pick bg` → click the canvas to sample the key colour) +
  a **fuzz** slider + a key swatch + clear, over a **checkerboard** so the cutout's transparency reads; every tweak
  **saves the sidecar + invalidates the texture** for a live re-key. `scan_library` now distinguishes gen items
  (`kind:"gen"`) from removebg-only sidecars (purple = gen only). **Self-demo:** `library/images/gemma-ominous.png` (a
  user gen on a flat off-white bg) ships a `removebg` block → select it in the Viewer to see the live cutout; verified
  end-to-end by compositing it over the canon room with **no white box** (`--shot-frame`, pushed to the feed). For hard
  (gradient) backgrounds where color-key isn't enough, a `method:"rembg"` provider pass can slot in behind the same
  `removebg` block later (isnet-anime, the avatar pipeline's matte) — the params model already allows it.
- **★ Avatar bob follows the AUDIO + animated code-highlight — NEW 2026-06-28.** The host's talk-react (bob/scale/
  light-up) is now driven by the **active VO clip's audio envelope** (`audio_talk_at`), sampled relative to that
  VO's CURRENT start — so it **follows when audio clips are moved** and goes idle (0) when no VO plays under the
  host (fixed a stale bob at the opener). The `code` clip's **line-highlight now reveals AFTER the typewriter**:
  the bar sweeps in (smoothstep) + the other lines dim, timed off the typewrite keyframes' completion.
- **★ VIDEO-CLIP DECODE PATH — NEW 2026-06-28 (the big editor gap, CLOSED). B-roll/footage now plays in
  preview + export.** Decode never runs in the editor (a Windows PE, no libav linked): in the spirit of the
  two layers, **`tools/video-proxy.py`** (host ffmpeg) decodes a source video ONCE into a content-addressed
  **proxy** — a dir of decimated JPEG frames (`cache://video/<name>/f%05d.jpg`) + `index.json` (fps/frames/w/h)
  — and registers a **`type:"video"` asset** (`--into P --key K` patches the assets map). The editor's
  compositor adds a **`video` branch**: map the playhead → the clip's local time → a **frame index**
  (`in`/`speed`/`loop` retime) → a texture, via an **LRU frame cache** (`get_frame_tex`, ~96 resident → a long
  scrub never pins VRAM). Drawn like an image: full `transform` (Ken Burns over moving footage) + draw-time
  `dim`/`temperature`/`tint`; identical in preview + export (both walk `composite_frame`). **A video clip needs a
  `type:"video"` ROW** (clip type comes from its row; `slop.py` now maps `video → r_video`). Verified end-to-end:
  `examples/video-broll.slop.json` (the **new self-demo** — the real CRT footage in motion + a lower-third +
  vignette); headless `--shot-frame` at t=0.5/4/8/11.5 render **distinct frames** (boot screen → XP setup wizard).
  The eventual libav/NVDEC in-process decoder slots behind the same **clip+time→SRV** contract. (Limits: proxy
  `fps` caps B-roll smoothness — re-extract higher for the master; `blur`/`saturation`/`contrast` are skipped on
  video to bound the cache — keep a still for a blurred backdrop.) **Now LIVE in the real video:** the luckymas
  **cold-open's floating CRT inset** plays the LuckyMas mascots dancing on the XP desktop IN MOTION (`vid_crtcopy`,
  the 18–28s segment) — replaced the `crt-copy.png` STILL (`c_im_crt`→`c_vid_crt` on a new `type:"video"` row, same
  bezel/pos/spring-pop, scale 0.34→0.51 since the proxy is 1280px vs the still's 1920px → identical 653×367 on
  screen). Next polish: drag-drop a video onto the timeline (auto-proxy).
- **★ Gradient/vignette primitive — NEW 2026-06-28.** A new native clip type **`gradient`** (full-frame wash,
  pure ImDrawList): `kind` = vignette (darken toward an `anchor`, edges biased away) / linear (`dir` down/up/
  left/right fade); keyframeable **`strength`** + **`feather`**, `color`, `anchor` [0..1]. Dispatched in
  `composite_frame` + inspector widgets in `draw_native_params`. luckymas uses it on a new **Grade track**
  (between Footage and the host, so it darkens the busy `minkit-desktop` but the host stays bright) over the
  Act-1 hook — directly tames the busy-background complaint.
- **★ `tools/slop.py` — LLM-facing CLI over the primitives — NEW 2026-06-28.** So an agent can author/edit a cut
  without hand-writing JSON. `overview` (compact ASCII-lane timeline, no JSON dump); `add`/`insert` (insert
  ripples after), `set` (dotted keys), `mv`/`trim`/`rm` (ripple by default like the editor), `ripple`/`rebase`;
  **`addtrack`/`mvtrack`** (+ `set CLIP row=…` moves between rows); **`transition CLIP --preset`** (drop-soft/
  drop-bounce/rise/slide/pop/fade — springs on the START keyframe = real easing); **`template NAME`** (scene
  templates). Same ripple math as the editor; preserves key order + unicode.
- **★ Reusable scene templates + transition presets — NEW 2026-06-28.** Good-defaults-that-adapt (the recurring
  ask, like the code typewriter's polish). **`slop.py template explain --shot KEY --text …`** instantiates a full
  scene in one command: Gemma in her room + a framed screenshot floating beside her (not full-bg) + a pointing
  arrow + a vignette taming the room + a term caption, each with a pop/drop/rise transition. Verified by render.
  Next for templates: image-gen host **poses/props** (the glasses + teacher-stick "explaining" pose) + more
  templates/variations to interleave with ad-hoc scenes.
- **★ Timeline UX round 2 — NEW 2026-06-28.** Fixed: clips overdrawing the row-label gutter (clip-rect + hit-box
  clamp); can't-scroll-to-0 (auto-fit forces scroll 0; clamp keeps the start reachable). Zoom now anchors on the
  **playhead**; **middle-mouse-drag pans**. **Multiple tracks per type + ▲▼ reorder** (z-order; persists on save)
  — e.g. a second image track so overlapping images (the floating CRT) each get a lane. **Transition presets**
  fixed the robotic title (easing must live on a segment's START keyframe).
- **★ Timeline UX round 3 + gen/asset UX — NEW 2026-06-28.** Fixed the auto-fit floor (a 244s cut showed only
  ~70s; `fitpps` floor was 20px/s → lowered so the whole project fits); **middle-mouse-drag pans** (when zoomed
  in). **Track ▸ Add track…** menu modal (type picker; `add_track` patches model + doc). Image clips: a **Browse…
  file picker** (Win32) + a **live thumbnail preview** in Properties. **Regenerate (fresh)** bumps the seed so it
  does a NEW gen (not the content-cached one), with a per-clip **gen history** (thumbnails for images) to restore
  past gens. **Drag-and-drop an image** onto a timeline image-lane → adds an image clip at the drop time. Still
  pending: **video B-roll decode** (the big one) + the emotion-voice investigation (queued) + richer scene templates.
- **★ Timeline transport (Play/Pause/Stop) — NEW 2026-06-27.** A real transport below the
  preview (also **spacebar**): Play advances the playhead on the **wall clock** (drives the
  whole composite live — avatar lip-sync, transforms, badges) while a small **waveOut mixer**
  streams the timeline's audio in sync. The mixer sums every audio clip at its start offset +
  per-clip `gain_db` (the SAME recipe as export, so preview and export agree), lazily decodes +
  resamples WAV (`get_pcm`), and refills device buffers each UI frame (streaming → any length,
  live edits, scrub-reseek). **WAV only** in-editor (no mp3 decoder — music/mp3 clips are silent
  in preview but still mixed by ffmpeg on export). Stop → playhead 0; Play at the end → replays.
  Builds clean; renders verified — **playback audio itself is a pending HUMAN check** (needs a
  sound device on Windows).
- **★ Timeline editing + Save + voice editor — NEW 2026-06-27.** Direct clip editing on the
  timeline: **drag the body to move** in time, **drag the L/R edge handles to trim** (clamped at
  t=0 / 0.05s min; ResizeEW/All cursors). **File ▸ Save project (Ctrl+S)** syncs every clip's
  start/dur/transform/params back into the `.slop.json` (generate only persisted the touched clip,
  so arrangement edits were in-memory only and never reached `--export`) — now arrangements
  survive reload + export. **Tools ▸ Voice preset editor**: design/tweak a `voice` instruct,
  **Preview** it (one-off TTS synth on a worker thread, auto-played), **Save** as a new
  `presets/voices/<name>.json` (appears immediately in the per-clip voice selector). Also fixed:
  the timeline waveform no longer stretches when a clip is resized (maps by audio time, not clip
  width).
- **Editor → provider generate-on-demand — LIVE, verified end-to-end** — a WinHTTP client on
  a worker thread submits a clip's job (params resolved from row+clip + the voice preset),
  polls `/jobs/{id}`, downloads the asset into `cache/<provider>/`, then the UI thread patches
  the project (`assets[hash]` + `clip.asset` + edited params), **persists it** (key order kept
  via `ordered_json` → clean diffs) and reloads — the last asset keeps showing meanwhile. UI:
  editable `text`/`emotion`/`prompt`, a **Generate/Regenerate** button + live status, per-clip
  timeline badges, provider health dots (config-driven URLs). Smoke-tested against live lame
  TTS: regenerated `c_vo1` in ~7s → real 4.64s WAV, project round-tripped. Also a headless
  `--generate <clipId>` mode (no window) for automation/LLM-driving. **The loop is interactive.**

- **★ Motion-graphics primitives — NEW, the video-001 toolkit (verified).** Four native,
  LIVE-composited clip types (NOT generated images → instant, keyframe-animatable, zero provider
  dependency), built for `docs/video/001-luckymaster.md` and reused by every technical video:
  - **`code`** — syntax-highlighted decompilation/source cards. A C++ lexer colours keyword/type/
    string/number/comment/preproc/func (C/C++/Ghidra vocab + Lua + TOML); a themed card (decompiler-
    dark) with a traffic-light title bar, line numbers (`first_line` for real refs), per-line
    `highlight`, a `typewrite` 0..1 reveal + caret, and `scroll`. Renders via **Consolas** (loaded
    48px → AddText scales crisply). `draw_code_clip`.
  - **`caption`/`text`** — styled overlays: up to 3 lines (text/sub/gloss) over an optional panel +
    accent stripe, with `style` presets (plain · lower_third · term-pill · jp_lesson). Uses a 2nd,
    48px CJK font (`g_captionFont`) with an **extended glyph range** (☆★, smart quotes, ♥♪, arrows —
    so らき☆マス / "fufu~ ♥" render, not '?'). `draw_caption_clip`.
  - **`shape`** — vector callouts: box (frame a code line), ellipse, line, arrow (animatable `grow`),
    bracket; stroke + optional fill. `draw_shape_clip`.
  - **Keyframe animation** — the project format's per-param keyframe curves are now EVALUATED
    (`eval_kf`): linear · constant · bezier (Newton-solved cubic) · spring (damped step). Sampled at
    the **timeline playhead (absolute time)** → Ken Burns (animated transform), pop-ins (spring
    scale), typewriter (animated `typewrite`), fades (opacity). Any numeric param animates via a
    dotted path; `anim_xform`/`anim_param` fold keyframes over the static fallback.
  All four verified headlessly on real LuckyMaster RE content; the committed **`examples/luckymas.slop.json`**
  (the video — opening + Act 1, ~3:16) exercises every one and **self-demonstrates on load by scrubbing** (no gen).
- **★ video-001 BODY: Act 1 + clone re-voice — NEW 2026-06-28.** `examples/luckymas.slop.json` grew from the
  ~80s opening to a **~3:16 cut**. (1) The whole **VO row was re-voiced to `gemma-san-deep-clone`** (canonical
  stable clone; ~16% slower than `gemma-san-deep`) and the entire cut **re-timed** to the new durations via a
  **piecewise-linear timeline remap** (`/tmp/reflow_author.py` pattern: anchor vo1, preserve gaps, warp every
  clip start/dur + keyframe `t` through the old→new spine; the hand-tuned copy-hook framing survived verbatim).
  (2) **Act 1 — transparent mascots (layered windows)** appended (`c_vo11`–`c_vo17` + ~25 visual clips): a pivot
  off the copy-hook → the floating-mascot hook (Ken-Burns `minkit-desktop`) → **3 code cards** (our own minimal
  C — `WS_EX_LAYERED` `CreateWindowExW`, `UpdateLayeredWindow` + premultiplied-alpha, and the **`Launch.exe`
  import-table attestation**: the only honest evidence, since the layered mascots are the launcher, attested by
  imports, NOT decompiled) → term/lower-third captions + the **"GDI screenshots can't see a layered window"**
  preservation reveal (the bg keyframes blur+dim+desaturate into a "dead" capture — the backdrop-blur legibility
  rule, on-theme). 17 VO + 5 avatar viseme tracks generated on lame; pruned 10 orphaned old-voice assets. **Tune
  items:** `minkit-desktop.png` is busy on the image beats (tighter crop / cleaner plate); some Act-1 lines are
  long (15–19s) from the slow clone → per-sentence splits would tighten pacing.
- **★ Defocus blur + full-res frame capture — NEW 2026-06-27.** A keyframeable **`blur`** on footage/
  image clips (`get_blurred_srv`: a cached, low-res CPU separable-gaussian copy keyed by `uri|sigma` —
  cheap HEAVY "frosted-glass" backdrop, identical in preview + export, no device-state risk) → the
  "text over bright footage = blur the bg" rule, paired with a keyframeable **`dim`** companion on the
  same clips (RGB-multiply darken, 1=full bright) so white titles pop over a *dimmed*, defocused
  backdrop (the luckymas title blurs + dims to 0.4 under it — user's call: dim a lot for legibility).
  Plus **`--shot-frame <png>`**: a headless ONE-frame full-res (project-res)
  composite capture via the export RT — for verification + thumbnails (the editor's `--shot` only grabs
  the small UI preview). The mono code-card font gained em/en-dash + `·…→` glyphs (titles/comments were
  rendering `?`).
- **★ Live auto-reload + full param reference + fufu lab — NEW 2026-06-27 (user: "make everything
  hand-adjustable").** The editor now **watches the `.slop.json` mtime and auto-reloads** external/hand
  edits (was only manual File ▸ Reload; a mid-save partial parse just retries) → hand-tweak any
  clip/effect param and see it live. `docs/PROJECT_FORMAT.md` gained a complete **"clip params by type"**
  reference (image `blur`/`dim` · tts clone-vs-design · avatar bob/light-up/dim · music). And
  **`examples/fufu-lab.slop.json`** = a **design-mode catchphrase lab** (7 generated smug "fufu~"
  candidates in romaji — the hiragana ふふ reads as a laugh) to find a golden, emphasised delivery the
  flat clone can't do (clone = stable timbre but drops per-line emotion → the user's "monotone VO" note;
  design = expressive but drifts). **Pending HUMAN: pick a golden fufu** (audition the lab; bake → ref).

## Live infra state
- **lame** unlocked + kept alive (`/tmp/stay`); **3080** ~6/10 GB (tts+align), **7800XT**
  runs ComfyUI (~7 GB with Illustrious resident). Up (docker, restart=unless-stopped):
  `slop-tts` `:8010`, `slop-align` `:8014`, `comfyui` `:8188` + `slop-image` `:8011` (on
  `slopnet`), **`slop-rembg` `:8015`** (CPU bg-removal). If lame reboots, re-unlock: `ssh root@code "cold-unlock --host lame --stay"`.
  Full ops + pointers: `docs/INFRA.md`.

## Build / run / verify (all from repo root, inside the dev shell)
- Editor build: `nix develop --command make -C editor`  → `build/slopstudio.exe` (now **incremental** —
  ImGui compiles to `build/obj/*.o` once; only `main.o` rebuilds on edits → ~20s, was ~46s). Headless
  inspector shots: `--select <clipId>` pre-selects a clip so `--shot` captures its Properties panel.
- Run (interactive — the host's signature opener): `./build/slopstudio.exe examples/signature-opener.slop.json --cache cache`
  → **scrub 0→1.6s: the host giggles toward the camera** (keyframed avatar scale — no gen needed
  to *see* the zoom), then backs off for "Welcome back, mortals." She's the Anima host on the canon
  day room, integrated (grade + contact shadow + scene vignette). Run `tools/demo-cache.sh` to add
  the VO audio + viseme tracks; select a **Gemma VO** clip → edit text/emotion → **Generate** →
  **Play**; drag playhead/transforms = instant preview.
  (The `.exe` runs on the Windows desktop via WSLInterop — no X/WSLg; just run it from WSL.)
- **★ See the video-001 cut (full re-script, ~4:04):** `./build/slopstudio.exe examples/luckymas.slop.json --cache cache`
  → **scrub 0→244s**: signature giggle-zoom opener (over the real CRT footage) + the goonery intro →
  smooth spring **title drop** → what-is (らき☆マス / シグナス) → the copy-hook decompile dive → **Act 1:
  transparent mascots** (layered-window code cards + `Launch.exe` import attestation + the corrected
  "GDI BitBlt can't capture a layered window" reveal + the **LLM/TTS self-aware** beat). All live
  compositing; VO is per-sentence `gemma-san-deep-clone` (regenerable via `--generate`; cache gitignored).
  The cold-open's **floating CRT inset now plays in MOTION** (mascots dancing on XP) — regenerate its gitignored
  proxy: `nix develop --command python tools/video-proxy.py assets-src/luckymas-crt.mp4 --ss 18 --t 10 --name crt-copy
  --into examples/luckymas.slop.json --key vid_crtcopy` (no-op if the asset's already registered; needs the source mp4).
  Headless beat checks: `--time 1.1` (giggle-zoom), `--time 6` (moving CRT inset), `--time 26` (title drop),
  `--time 139` (`WS_EX_LAYERED`), `--time 222` (the LLM-joke beat).
- **Provider URLs:** the editor reads `config.toml`, falling back to the committed
  `config.example.toml` (lame URLs) — so generate works out of the box; copy → `config.toml`
  to point elsewhere, or pass `--config PATH`. Menu-bar dots show per-provider `/healthz`.
- Headless **generate** (automation/LLM-driving + smoke test, no window):
  `./build/slopstudio.exe PROJECT.slop.json --cache cache --generate <clipId>` → runs the same
  submit/poll/download/persist path and writes the asset + patched project. (NOTE: persists to
  the project file — use a copy, not the committed example, if you don't want the diff.)
- Headless shot → feed: `./build/slopstudio.exe examples/signature-opener.slop.json --cache cache --time 2.0 --shot build/shot.png` then `nix run nixpkgs#python3 -- /opt/src/llm-feed/feed.py image build/shot.png --title T --note N`
- Provider tests: `nix develop --command python -m pytest providers/tests -q` (**16 pass**)
- TTS deploy/redeploy: `bash deploy/lame/deploy-tts.sh`; health: `curl http://lame:8010/healthz`
- image: ComfyUI engine `bash deploy/lame/deploy-comfyui.sh` (7800XT/ROCm; first build pip-pulls
  torch-rocm ~4GB — runs **detached on lame**, logs `/lamedata/comfy/build.log`), then the adapter
  `bash deploy/lame/deploy-image.sh`. Health: `curl http://lame:8011/healthz`,
  `curl http://lame:8188/system_stats`. Checkpoint: `/lamedata/comfy/models/checkpoints/`.
- align deploy/redeploy: `bash deploy/lame/deploy-align.sh`; health: `curl http://lame:8014/healthz`.
  Smoke test (real VO → word timings + visemes): the align provider resolves the TTS audio
  by content-hash from the sibling cache (mounted read-only); see `providers/align/__init__.py`
  `_resolve_audio`. First `word_timing` downloads the WhisperX model (~90s once), then resident.
- **Animated rig (`gemma-chibi`) — drop in an authored face sheet:** `nix develop --command python
  tools/slice-face-sheet.py "<sheet.png>"` (parses `face_<expr> pose_<pose>` from the name; slices
  2×6 → `raw/<expr>-[o|c][0..5].png` + updates the manifest) → `RIG=gemma-chibi bash
  tools/cutout-sprites.sh` (rembg → alpha → **align to a common baseline**) → `nix run
  nixpkgs#pngquant -- --quality 82-98 --force --ext .png presets/avatars/gemma-chibi/*.png`. Repeat
  per expression; the manifest accretes (fallback=neutral). Sheets live in
  `/mnt/f/Pictures/oc/gemma-san/chibi-animation/`.
- **SD pose library (`gemma-pngtuber`) — regenerate single poses:** `nix develop --command python
  tools/gen-avatar-sprites.py` (one frame per emotion; `--only neutral,smug` to probe) → `bash
  tools/cutout-sprites.sh` → pngquant (as above). **The full-figure framing in the prompt is
  load-bearing** (gotcha) — without it some poses render as busts. This rig is static (no flap).
- **Author/edit a cut from the CLI (no JSON by hand):** `nix run nixpkgs#python3 -- tools/slop.py overview PROJECT.slop.json`
  (compact timeline) · `… add/insert/set/mv/trim/rm/ripple/rebase …` (`-h` per subcommand). Ripples like the editor.
- **★ See the IN-PROCESS video DECODE path (no proxy step):** `./build/slopstudio.exe examples/video-broll.slop.json
  --cache cache` → **scrub 0→12s: the CRT footage MOVES** (boot screen → XP setup wizard) under a lower-third + vignette,
  with a gentle Ken Burns — the editor decodes `assets-src/luckymas-crt.mp4` directly with libav (just needs the gitignored
  source mp4). Headless proof of frame-advance: `--time 0.5/8 --shot-frame build/v.png`; A/B the proxy fallback with
  `--no-video-decode` (needs `tools/video-proxy.py` extracted). Video clip params (`in`/`speed`/`loop` + transform/dim/
  temp/tint): `docs/PROJECT_FORMAT.md §video`. **Drag-drop an mp4** (Explorer OR a library video item) onto the timeline → it
  auto-creates a `video` row + plays.
- **★ rembg (bg removal) deploy + use:** `bash deploy/lame/deploy-rembg.sh` (CPU container on lame, `:8015`; first cut
  downloads the isnet-anime ONNX ~170MB). Health: `curl http://lame:8015/healthz`. In the editor: **Tools ▸ Viewer**, select an
  image item → **remove bg ▸ rembg (AI)** ▸ pick a model ▸ **Cut out**. Headless (automation + verify, no window):
  `./build/slopstudio.exe --cache cache --lib-removebg library/images/<file>.png [isnet-anime]` → caches the cutout +
  records `removebg.method=rembg` in the item's sidecar. Provider tests: included in `pytest providers/tests -q` (21 pass).
- **★ Library gen items (L4/L5):** in the editor, **Tools ▸ Library ▸ + image / + voice** authors a gen item; **Tools ▸
  Viewer** (focus an item to pan/zoom/scrub + edit recipe + Regenerate + restore from history). Headless (automation +
  smoke test, no window): `./build/slopstudio.exe --cache cache --lib-gen image gemma-wink "gemma-san, chibi, wink, …"`
  (→ `library/images/gemma-wink.png` + `.meta.json`), `--lib-gen voice line-01 "Fufu~ …" gemma-san-deep-jp`,
  `--lib-regen library/images/gemma-wink.png` (fresh seed, keeps history). Re-seed the self-demo sidecars:
  `nix develop --command python tools/seed-lib-sidecars.py`.
- Rebuild the (gitignored) playable demo cache: `nix develop --command bash tools/demo-cache.sh`
- Export to mp4: `nix develop --command bash tools/export.sh examples/signature-opener.slop.json --cache cache`
  → `exports/<name>.mp4` (libx264 1080p + AAC; `SLOP_NVENC=1` to try NVENC). `--out FILE` to
  override. The editor streams frames; ffmpeg encodes + muxes the project audio + prints credits.

## ⏳ Pending HUMAN checks (next session: surface these first)
- **Transport audio playback: CONFIRMED working by the user (2026-06-27).** WAV-only in-editor.
**Still un-eyeballed in motion** (stills + headless renders verified):
1. **Avatar bob/light-up feel** while playing — the host should bob + brighten on speech. Tune via
   the inspector sliders or `c_av1` params `bob`/`bob_speed`/`lightup` (0 disables either).
2. **Pick a host voice** — A/B `gemma-san` / `gemma-san-mid` / `gemma-san-deep` (clip 'voice'
   selector). The demo currently sits on `gemma-san-mid` (the user's last experiment, uncommitted).
3. **Author the rest of the expression poses.** Only **neutral** is wired. For each new
   `face_<expr> pose_neutral …png` in `chibi-animation/`: `slice-face-sheet.py "<sheet>"` →
   `RIG=gemma-chibi cutout-sprites.sh` → pngquant (see build/run). Until an expression exists the
   rig falls back to neutral. (I can run these as sheets land.)
4. **Avatar placement** — `c_av1` scale is 1.9 for the 256×512 sheet rig (lower-left). Retune to taste.

## NEXT (priority order)
**★ luckymas 10-MIN CUT — FIRST PASS ASSEMBLED (2026-06-30, commit `a02ffbc`). Now ITERATE (accept-by-scrub).**
The full 11-act cut is in `examples/luckymas.slop.json` — **~14.3 min, 175 clips (88 VO)**. The 2026-06-30
interactive session: polished the script per the user's review, shipped **3 more editor features** (blur OPAQUE +
2s default, reusable **`diagram`** clip, **video-clip audio** at 20% + `slop.py splitaudio`), generated **57 new
VO lines** (`gemma-san-deep-clone`, headless `slopstudio.exe … --generate`), and re-sequenced the timeline into
script order (re-anchor-to-VO-spine; copy-hook + layered acts kept their visuals; compact `c_vo_h*` retired, chest
take → Act 4). Diagrams (Act 7 gcalsrv, Act 9 font→size), the laugh-point meme (Act 5, 20% audio), CRT shots, and
blur transitions at all 10 act boundaries are placed; verified via `--shot-frame`.
  - **Iterate next:** trim toward **10 min** (the layered act is long — `rate`-up slow clips / tighten the 0.2s gaps
    / cut weaker beats); **per-image framing/pan** for the tall site screenshots (they fit-width, show the top slice);
    **more per-beat visuals** in the new acts (only the key shots are in); cold-open CRT timing; optionally add ✓/✗
    glyphs to the caption font (Act-9 diagram edge labels currently use words). Author via `tools/slop.py`.
  - **Reproduce gen:** lame must be awake (`ssh root@code "cold-unlock --host lame --stay"`) + TTS up on `:8010`;
    `config.toml` (gitignored) copied from the example. The meme asset is gitignored — `nix run nixpkgs#yt-dlp -- -o
    assets-src/meme-laughpoint.mp4 https://www.youtube.com/watch?v=L8XbI9aJOXk`.
  - **Optional editor upgrades** noted this session: a true full-composite **RT/shader blur** (the current `blur`
    clip blurs the scene PLATE; native caption/code layers below aren't blurred); further pruning of plain-image
    **pos/scale** pan keyframes on scrub (kept this round — they overlap signature pop-ins/slides).

**★ EDITOR LIBRARY PUSH — COMPLETE 2026-06-28 (all of L1–L6 + "add a gen item" landed).** The editor now has the
golden/global library, sprite-sheet processor, audio→timeline, **gen items (author + regenerate + history), the
viewer (pan/zoom + scrub)**, and TTS pacing split — "the level it needs for human fine-tuning." Small follow-ups if
they come up: a true mp3/seeked audio scrub (needs the waveOut mixer wired into the viewer), committed self-demo
history (cache is gitignored → strip populates only after a regen). (Drag-drop a video onto the timeline: ✅ DONE 2026-06-29.)
**⇒ DIRECT IN-EDITOR VIDEO DECODE — ✅ DONE 2026-06-29.** libav (libav{format,codec,util,swscale}+swresample) is
cross-compiled into the editor (`ffmpegCross` in the flake) and decodes frames **in-process** behind the SAME
`clip+time→SRV` contract — only the "resolve a frame at local time" leaf changed (proxy JPEGs → `VideoDecoder`
seek+decode+swscale→RGBA→texture, sharing the LRU `FrameTex` pool). Proxy kept as a `--no-video-decode` fallback;
**drag-drop a video just works**. See the ★ IN-PROCESS VIDEO DECODE bullet under Built+working. NVDEC/d3d11va (swap the
decode source inside `VideoDecoder`) + a higher-fps master proxy are the open follow-ups. **rembg bg-removal also landed
this session** (provider on `:8015` + the editor `method:"rembg"` path — ★ rembg bullet). **⇒ XP CAPTURE HARNESS — ✅ DONE 2026-06-29 (core + the BitBlt demo).** `tools/xp/` boots an autonomous QEMU
Windows-XP golden (unattended SP3 install) and drives it host-side via QMP + agent-less SMB; the BitBlt-vs-
screenshot demo is proven end-to-end (`demo-comparison.png`). See the ★ XP CAPTURE HARNESS bullet under Built+working
and `tools/xp/README.md`. **⇒ NEXT on this arc** (unblocks POST-/CLEAR items 2–3): bump the golden to 1024×768,
run `choreograph run`→mp4→slop-asset, then **recreate the actual video-001 demos** (the file-copy hook + the real
layered-mascot reproduction, captured into `luckymas.slop.json`) + Camtasia for interactive human capture.

**★★ POST-/CLEAR PLAN (user-stated 2026-06-28, do in order):**
1. **Video-clip DECODE path — ✅ DONE 2026-06-28** (proxy-frame approach; `tools/video-proxy.py` + the editor's
   LRU `clip+time→SRV` frame cache + a `video` compositor branch; `examples/video-broll.slop.json` self-demos the
   CRT footage in motion). **✅ ALSO DONE: the moving CRT footage is swapped into the luckymas cold-open** (the
   floating inset plays the mascots dancing on XP; `c_im_crt`→`c_vid_crt`). See the ★ VIDEO-CLIP DECODE bullet.
   **Follow-up polish still open:** drag-drop a video file onto the timeline (auto-proxy via a worker thread / WSL
   relay), and a higher-fps/full-res proxy for the master export.
2. **Write the actual demos** that recreate BOTH tricks — ✅ **DONE 2026-06-29** (on the QEMU XP guest, autonomously
   captured): the **file-copy hook** (`copyanim.c`) and the **transparent layered-window mascot** (`mascot.c` + a real
   sprite) — minimal C, built i686, run on real XP, captured to mp4/PNG. See the ★ XP VIDEO-001 DEMOS bullet.
3. **Pack the captures into the video** — ✅ **DONE 2026-06-29**: the copy-hook repro, the floating mascot, and the
   BitBlt-vs-screendump reveal are wired into `examples/luckymas.slop.json` (@106/155/204s, verified by render). **The
   user polishes manually from here** (framing/timing of the insets; the cluttered ck1 beat got the redundant
   stock-dialog clip removed). Remaining XP follow-up: Camtasia / interactive human capture.
Also still queued: emotion-with-stable-voice ([next-session task]) + richer scene templates (image-gen host poses).

**Done:** Phase-1 spine + mp4 export + Gemma LoRA v2 + transport/waveOut audio + static avatar +
voice DESIGN+CLONE + voice editor + CJK font + timeline drag/trim + Save + **the motion-graphics
toolkit (code / caption / shape / keyframes)** + **the luckymas video OPENING assembled**
(`examples/luckymas.slop.json`, content-verified vs `../LuckyMasterEN`).
**FOCUS: finish video 001 — `docs/video/001-luckymaster.md`.** The HOST SYSTEM is done (Anima LoRA host +
`gemma-san-deep-clone` STABLE cloned voice + signature opener + auto bg-match + keyframe panel); the host
opening (`examples/opening.slop.json`, ~25s, `exports/opening.mp4`) is assembled + voice-stable (confirmed).
**★ DONE 2026-06-28 (session 2 — full content re-script, user-directed):** `luckymas.slop.json` rebuilt to
**~4:04, 35 per-sentence VO clips** (was 17). Folded the **signature opener recipe** (giggle-zoom + "welcome
back, mortals" + the goonery/power-level MoistCr1TiKaL intro, reusing opening.slop.json's cloned assets) in as
the cold-open over the **real CRT footage** (`crt-copy.png`); rewrote the whole script per-sentence in that
humor voice; fixed pronunciations (**らきマス** spoken JP name, **シグナス** for SYGNAS — both **need a human
audition**, they're in the audio); **corrected the screenshot beat** (a normal PrtScn DOES capture the layered
mascot — only raw **GDI BitBlt** / headless capture gets bare desktop) + added an **LLM/TTS self-aware** beat
("I'm a language model with a bolted-on TTS voice… my own capture pipeline face-planted on a 2007 anime girl");
**smooth spring title-drop**. Two-pass builder (VO script → generate → timeline layout); 6 opener visemes reused,
7 host visemes gen'd. **Flags:** ~4min is long (trim per clip); the **clone is flat per-clip** (splitting gives
pacing control, not emotion — punchlines may want design-mode for real variation); busy `minkit-desktop` bg
persists on copy-hook/act1 (the queued vignette primitive will help). Prior session (Act 1 + first clone re-voice)
below.
**★ EARLIER 2026-06-28:** the video BODY grew — **Act 1: transparent mascots (layered windows)**
appended to `luckymas.slop.json` (now ~3:16), AND the **whole VO row re-voiced to `gemma-san-deep-clone`**
(the canonical clone; ~16% slower → the entire cut was re-timed via a piecewise-linear remap, tuned framing
preserved). Act 1 = pivot → floating-mascot hook → 3 code cards (our minimal C + the `Launch.exe` import
attestation) → the "GDI screenshots can't see a layered window" reveal (bg blurs/dims/desaturates). 17 VO +
5 viseme tracks generated on lame; self-demonstrates on scrub. See the new Act-1 bullet below + `docs/video/001`.
**★ NEXT — the EDITOR usability track (user-prioritized, queued as tasks):** (1) **per-clip tweak UX** — inspector
widgets + keyframe editor for code/caption/shape (today JSON-only + the keyframe panel is hidden for them inside
`if(generable)`), a real **code editor** for the typewriter clip, rotation/anchor widgets; split the Makefile for
incremental builds; (2) **timeline scroll + zoom** (the `pps`/`T2X` mapping is centralized ~`DrawTimeline`); (3) a
**vignette/gradient overlay primitive** (tame busy backgrounds — hooks into `composite_frame` like the scene
vignette); (4) **video-file B-roll decode** (drop in the CRT mp4 by the timestamps in `docs/video/001` — biggest
lift, no libav linked yet). Also still queued: audition the らきマス/シグナス + trim the ~4min cut; the
live-rebuild capture path; design-mode punchlines for real vocal variation.

### ★ FIRST-REVIEW FEEDBACK (user, 2026-06-27) — ①②③ DONE this session
- **WIN:** the **code typewriter reveal** is the user's favourite primitive — lean into it.
- **① RE-TIME to real VO — DONE.** Generated all 10 `Gemma VO` on `gemma-san-deep` (real durs: c_vo1
  6.8 · vo3 10.3 · vo4 9.8 · vo9 8.6 … much longer than my guesses) and **re-flowed the whole opening**
  to them (≈80.8 s), every visual beat re-anchored + keyframe times shifted with their clips. (Note:
  `--generate` writes the real dur back into the clip; the re-flow was a one-shot script from a base
  snapshot.) **Still TODO:** the 2 avatar clips need **viseme** generation for lip-sync; a **"Fufu~"**
  emphasis clip.
- **② BACKDROP BLUR — DONE.** Keyframeable **`blur`** param (source-px gaussian sigma) on footage/image
  clips → a cached low-res CPU separable-gaussian copy swapped in (param-hash by `uri|sigma`); cheap
  HEAVY blur, no scrim, identical in preview + export. The title reads cleanly over the blurred desktop.
  (Chose CPU-cache over a D3D shader/RT pass — simpler + export-safe; a shader pass can come later.)
- **③ Copy-hook framing — DONE.** `c_im4` re-centered on the Copying… dialog (focal src px (560,68) of
  the 800×600 desktop; `pos = -scale·(f-center)`), `c_sh1` resized to frame the animation. Also fixed
  the SetHook + class-check beats: highlight/box/arrow now land on the right decompiled line (the two
  manual boxes were authored a line off) and code-card titles render em-dashes (mono font fix). All
  verified with the new `--shot-frame` full-res capture (frames pushed to llm-feed).

### ⇒ NEXT SESSION — run TWO tracks in parallel (user's call)
- **Track A — Krea-2** (image gen): download finishing on lame (see handoff below). Stand up a Krea-2
  ComfyUI workflow in `providers/image`, **A/B reaction pics vs Illustrious**, then **fine-tune
  Gemma-san** (ai-toolkit, on the raw bf16). Verify the LICENSE first.
- **Track B — video**: the **video-clip DECODE path** (moving B-roll — ffmpeg/NVDEC per ARCHITECTURE
  §video; the CRT footage IN MOTION, esp. the copy animation for the cold-open) + the **blur effect**
  (②) + **VO re-timing** (①) + the **framing fixes** (③).

From here (detail):
1. **★ Generate the opening's audio + lip-sync** → then **re-time** (see ① above). The 10 `Gemma VO`
   clips are authored (script in the project); Generate each, then the 2 avatar clips for visemes.
2. **Extend the cut**: Act-1 proper (layered-window mascots — honest framing: launcher = real
   `UpdateLayeredWindow`, attested by imports, NOT decompiled here) + Act-2 finish (the live
   rebuild/run-and-capture demo). Author more beats in the same primitives.
3. **Editor gaps the cut wants** (clip split/delete + `--split`/`--delete` DONE; build the rest as
   friction shows): snapping + undo/redo; an inspector UI for the new `code`/`caption`/`shape` params (today
   they're JSON-authored + live-reloaded). **Export**: skip "(loading?)" placeholders.
4. **★ Explore Krea-2 for reaction pics + a Gemma-san fine-tune** (downloading now — see below).
5. **Later**: Inochi2D Tier-B mesh rig; procedural video motion; music (Jamendo + ACE-Step) +
   credits overlay; in-editor mp3 preview; the autonomous run-an-example-and-record capture path.

### ⇒ IMAGE + TTS MODEL EVALS — CONCLUDED (2026-06-28). The emerging TOOLBOX:
- **Backgrounds/scenery + stylized stickers → Anima** (2B; `circlestone-labs` NC weights but **outputs
  commercial-OK**; NVIDIA-Cosmos base → "Built on NVIDIA Cosmos" attribution). Tested + on the feed:
  **best anime-screencap backgrounds** + a great die-cut **sticker** look; the host **chibi is too
  flat/low-detail** to be the workhorse. **Fits 16 GB (5.7 GB, no offload, ~52 s/img)** and the win —
  **LoRA-trains LOCALLY on the 3080 via kohya/sd-scripts** (native `anima_train_network.py`, 6–10 GB).
  Files: `/lamedata/comfy/models/{diffusion_models/anima-base-v1.0,text_encoders/qwen_3_06b_base}`.
- **Detailed host chibi (Gemma) → Illustrious + its Gemma LoRA** (the detail + the proven local path; unchanged).
- **NL composition / in-image text / reaction pics → Qwen-Image 20B-2512** (Apache; beats Illustrious on
  NL/text/composition; **but ~144 s + VRAM-offloads on 16 GB** → wants a GGUF/Lightning for practical use;
  deleted locally to free disk, re-downloadable). ⚠️ **The "7B Qwen-Image 2.0" is API-ONLY — no open
  weights** (verified); don't chase it.
- **Krea-2: DROPPED + DELETED** (reclaimed). Its <$1M-revocable Community License is dominated by
  Qwen-Image-2512 (≈ same quality, clean Apache). Shared `qwen_image_vae` kept.
- **TTS: Qwen3-TTS stays the winner.** dots.tts (Apache, 48 kHz, clean) A/B'd more robotic/artifacty in
  our setup — handicapped by a **24 kHz-upsampled ref + 10 steps**; its context-aware emotion DOES work
  objectively (sad-quote −3.9 dB vs excited +7.6 dB). Retry path (native-48 kHz ref + 25–32 steps) shelved.
- **DISK (lame ZFS):** was 98% full — the **103 GB of `zfs-auto-snap` snapshots were destroyed** (user OK:
  models don't change + are on cold backup) and **auto-snapshot disabled on `lamedata` at runtime** (make
  declarative in `../nix-lab` if permanent). Now ~46% / ~89 GB free.
- **Eval scaffolding committed:** `deploy/lame/krea-eval/genimg.py` (`--arch` illustrious/krea2/qwen-image/
  qwen-gguf/anima) + `deploy/lame/dotstts/` + additive `providers/image` arch branches (NOT deployed).
- **★ CONCLUDED (2026-06-28): Anima IS the golden host model + our DEFAULT/MAIN image engine.** The local
  `gemma-san-anima` LoRA locks the character + is stable across framings (beats Illustrious); wired as
  `providers/image` `arch='anima'`, canon backgrounds added, per-clip color grade shipped. See the
  "Anima host engine" bullet under Built+working. Open follow-ups: editor avatar grade/shadow port +
  the auto bg→character match; a screens-as-animated-layer.
- **★ Image-model survey → `docs/research/image-models-2026-06.md`** (commissioned this session): newer,
  CLEANER-licensed options to weigh — **Anima** (May'26 anime base, beats Illustrious per community,
  outputs-commercial OK), **Qwen-Image / -Edit-Anime** (Apache; best prompt+in-image text → the
  reaction-pic composer), **HiDream-O1** (MIT), **Neta-Lumina** (Apache anime), **Z-Image-Turbo**
  (Apache, ~6 GB). Flags: CLAUDE.md's "**SANA (Apache)**" is wrong (it's nvidia-open-model-license —
  verify + correct); avoid NoobAI-tainted Civitai merges (NC clause on outputs).
- **Model:** https://huggingface.co/Comfy-Org/Krea-2 — turbo = OOB base (fast), raw = the fine-tune
  base. Architecture: Qwen-Image-based (TE = **Qwen3-VL-4B**, vae = `qwen_image_vae`).
- **Fine-tuning:** https://github.com/ostris/ai-toolkit (train Gemma-san LoRA on raw bf16; 3080/CUDA
  path like the Illustrious LoRA, or the 7800XT — check VRAM: bf16 is 26 GB, won't fit 16/10 GB
  without offload, so likely fp8/quantized training or gradient-offload).
- **Recommended add-on:** https://github.com/nova452/ComfyUI-Conditioning-Rebalance (conditioning
  control). Wire as a new ComfyUI workflow in `providers/image` (the adapter owns the template).
- **Plan:** stand up a Krea-2 ComfyUI workflow → A/B reaction pics vs Illustrious → if better,
  fine-tune Gemma-san on it + use its features (the style LoRAs, the rebalance node). Check
  `docker logs krea2-dl` / `du -sh /lamedata/krea2` for download status first.

## Gotchas a fresh session must know
- Editor is a **Windows PE** (mingw cross from the flake) run via **WSLInterop**; UI is
  D3D11 + ImGui. `make` uses `CXX:=$(MINGW_CXX)` (the dev shell pre-sets CXX to host g++ —
  must override).
- **WSL exe-image cache (bit me hard):** after you rebuild `build/slopstudio.exe` and immediately
  re-run the **same path**, Windows/WSLInterop can serve the **stale previous image** (overwriting in
  place OR `rm`+relink does NOT invalidate it — the cache is keyed on the path). Symptom: new flags/code
  silently don't take effect. **Fix: run a freshly-NAMED copy** (`cp build/slopstudio.exe build/v$RANDOM.exe && ./build/v….exe …`),
  or relaunch in a new shell. Verify a rebuild took by grepping the binary for a new string (`grep -ac NEEDLE build/slopstudio.exe`).
- Clips reference assets by **key**; the real path is in the project `assets` map (`uri`),
  resolved `cache://…`→ the `--cache` dir, `file://…`→ path.
- **Generate writes the project file** (it's the source of truth): a finished job patches
  `assets[hash]` + `clip.asset` + edited params and rewrites the `.slop.json` (then reloads).
  Key order is preserved (`using json = nlohmann::ordered_json`) → clean diffs. HTTP is
  WinHTTP on a worker thread (no shared editor↔provider storage on this topology, so assets
  are always fetched via `GET /assets/{hash}.{ext}`, never the provider's `file://` path).
- Provider heavy deps (torch/qwen_tts/whisperx) are **lazy-imported**; providers run in
  **docker on lame** (no system python). GPU via **CDI**: `docker run --device nvidia.com/gpu=0 …`.
  Set `SLOP_ALIGN_FAKE=1` for the align provider's deterministic model-free path (tests +
  no-GPU degradation). All handler I/O (audio fetch, model, file write) runs in an executor —
  never block the event loop, or `GET /jobs` freezes (this bit the first align cut).
- **Export** = editor renders video, ffmpeg encodes + does audio. The editor has no audio
  decoder, so it never touches the wav/mp3 — it lists clips/offsets/gains in the plan and
  ffmpeg mixes (`adelay`+`volume`+`amix`). `--export` writes raw RGBA to **stdout in binary
  mode** (no logs there; progress is on stderr). NVENC is opt-in (`SLOP_NVENC=1`) since the
  WSL CUDA path isn't guaranteed; libx264 is the default. `exports/` is gitignored.
- **lame is NixOS**: only **published docker ports** (`-p`) are LAN-reachable; host-net binds
  on unlisted ports are dropped. So providers exchange large media **by content-hash from a
  shared/RO-mounted cache** (align reads tts's cache RO via `SLOP_ASSET_ROOTS`), not
  provider↔provider HTTP. The exception is the **image** stack: `slop-image` + `comfyui` share
  a user network **`slopnet`** so the adapter reaches `comfyui:8188` by name. Details: `docs/INFRA.md`.
- **ComfyUI on ROCm**: 7800XT (gfx1101) works with `HSA_OVERRIDE_GFX_VERSION=11.0.0` (reports
  as gfx1100) + `--device /dev/kfd --device /dev/dri --ipc=host`; the torch-rocm wheels bundle
  the ROCm runtime (slim base is fine). `cudnn` is auto-disabled for AMD; it's still `cuda:0`.
- **LoRA training — use the 3080/CUDA path (`slop-train-cuda`).** kohya sd-scripts;
  `deploy/lame/train-cuda/` + `run-train.sh` (config-driven: env `PREC/RES/DIM/ALPHA/OPT/ATTN/TE_LR/
  REPEATS/EPOCHS/SAVE_EVERY/LORA_NAME`). **CUDA bf16 trains clean** at 1024/dim64/+TE with
  `AdamW8bit`+`xformers` (fits the 3080's 10GB with `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True`
  — needed or it OOMs by ~40MB). **Stop tts+align first** to free the 3080; restart after.
  - **Captions**: WD14 tags → **prune the design tags** (wings/horns/tail/outfit/hair+eye color) so
    the trigger owns the design → pose/view/expression stay controllable. (Big consistency lever.)
  - **AVOID the 7800XT/ROCm path** for training: **bf16 AND fp16 NaN at step 2 on RDNA3** (valid
    step-1, then the first backward blows up — ROCm bug, not config). The `slop-train`/7800XT fp32
    workaround (`PREC=no RES=768`) works but is slow + low-res; only for when the 3080 is unavailable.
  - Dataset: `/lamedata/comfy/train/gemma-chibi/`; refs at `/mnt/f/Pictures/oc/gemma-san/chibi/`.
    LoRA → ComfyUI `models/loras`. Run **detached on lame**, log to a file (never `ssh | tail`).
- **Avatar rigs — the lessons (so we don't relearn them):** (0) **SD CANNOT ANIMATE a character
  — settled.** Fixed-seed txt2img tag-swap drifts the whole body (every frame a different
  pose/scale/rotation); low-denoise img2img locks the body but won't open a small mouth;
  masked inpaint opens it but bleeds artifacts (open eyes through a blink mask) and can't tell
  apart >2 mouth-openness levels. **AND authored GPT face-sheets, while well-registered, still
  don't feel good flapping frame-to-frame.** ⇒ **Decision: no per-frame avatar animation until an
  Inochi2D mesh rig.** The host is a STATIC pose per expression + an audio-reactive **bob +
  light-up** (additive brighten ∝ openness; tweakable). Don't re-attempt SD/sheet frame animation —
  that's what this session proved out; the next real step is Inochi2D. (1) provider job terminal status is
  **`"done"`** not `"ready"` — poll `done|ready`. (2) **Don't chroma-key**: Illustrious bleeds a
  `green background` tag onto purple hair (→ teal) and "simple bg" is a *gradient* — both
  unkeyable; generate on a plain bg + matte with **rembg `isnet-anime`** (shape segmentation,
  cuts purple-on-purple). (3) **Keep every frame the same canvas size** (no per-frame crop) so a
  rig shares one coordinate frame, AND **`cutout-sprites.sh` aligns** every frame to a common
  feet-baseline + x-center (authored sheets offset the eyes-open vs eyes-closed rows by ~40px →
  a blink hop without this; `ALIGN=0` to skip). (4) `rembg` = `nix run nixpkgs#rembg`; `pngquant`
  shrinks committed RGBA ~3-4×. (5) **SD pose framing is load-bearing:** `full body, full figure,
  head to toe` + strong SD tags AND negatives `(upper body:1.5),(portrait:1.5),(close-up:1.4),bust`
  + a tall 768×1344 bucket — else Illustrious renders a semi-realistic ("Konosuba") bust. (6)
  **Authored sheets** are 2 rows (eyes open/closed) × 6 mouth cols, 1536×1024 → 256×512 cells;
  `slice-face-sheet.py` slices + writes the `mouths`/`mouths_blink` manifest. The compositor sizes
  by native px × clip scale, so the 256×512 sheet rig needs a bigger clip scale (~1.9) than the
  768×1344 SD rig (~0.72) for the same on-screen size.
- **An avatar clip with no viseme track shows a frozen (closed) mouth — by design.** Mouth
  openness comes *only* from the clip's viseme asset; no asset ⇒ openness 0 ⇒ closed, forever
  (procedural OR sprite — it's not an editor bug). The signature-opener self-demonstrates the
  giggle ZOOM on load via keyframes (no gen); the VO + avatar viseme tracks come from
  `tools/demo-cache.sh` (which `--generate`s each clip). If you author a new avatar clip, give
  it a viseme asset (Generate it, or point `asset` at a track) or the host won't move its mouth.
- **Export bakes in whatever the preview shows — including placeholders.** An image/avatar clip
  with no ready asset renders its "(loading?)" placeholder box into the exported mp4 (the demo's
  reaction clip has no cached asset, so it shows one at ~1.6–3.4s). Generate the assets (or run
  `tools/demo-cache.sh`) before a clean export. (TODO: skip placeholders in the export context.)
- **The motion-graphics clip types (`code`/`caption`/`shape`/`diagram`) — authoring notes:** they're pure
  compositing (no provider; `map_type` returns false → no Generate button), authored in the project
  JSON + live-reloaded. **Keyframe `t` is TIMELINE seconds (absolute), not clip-local** — matches the
  authored examples; sampling is at the playhead (move a clip and its keyframes don't follow yet).
  Animatable numeric params via dotted paths: `transform.pos/scale/opacity/anchor` and clip params
  `params.typewrite`/`params.scroll`/`params.grow`/`params.w`/… A `code` clip's lexer is C by default
  (`lang: "c"`), so set `lang` for lua/toml/text. **Image/footage clips reference assets by KEY** —
  add an entry to the top-level `assets` map (`{uri: "examples/assets/…png", status:"ready"}`) and
  point `clip.asset` at the key; a bare path as the asset value won't resolve (it bit luckymas once).
  The **`diagram`** clip (boxes+arrows — a `flow:[…]` chain or `nodes`/`edges` graph, animatable
  `reveal` stage-in; `tools/slop.py --type diagram`) and the **`blur`** transition are the same kind
  of native clip. **blur is now OPAQUE** (2026-06-30 fix): full-frame, transparent source → black, so
  the unblurred scene no longer shows through; default duration **2s** (`slop.py add --type blur`).
- **Video clips:** (1) **a clip's `type` comes from its ROW** (`c.type = rows[c.row].type`), so a `video`
  clip MUST live on a `type:"video"` row — putting it on the image footage row renders it as a still
  (get_texture on the mp4 → fail → placeholder). `slop.py --type video` / dropping a video auto-creates the
  right row. (2) The editor decodes the **source mp4 IN-PROCESS** (libav) — the asset's `uri` (or an explicit
  `src`) is the decode input; `vm.fps/frames/dims` are filled on first open. If you see `video (can't decode
  source)` the file's missing/unreadable (sources live in `assets-src/`, gitignored); `video (no source …)` =
  the asset has neither a `uri`/`src` nor a `proxy`. The **proxy** (`cache://video/<name>` JPEG dir from
  `tools/video-proxy.py`) is now only a FALLBACK (used when `--no-video-decode`, or libav can't open the src).
  Frame textures are LRU-capped (~96) across BOTH the decode + proxy sources. (3) **Video clips carry
  their own audio** (NEW 2026-06-30) — ON by default at **12%** (`video_volume` 0..1; libav-decoded by
  `get_video_pcm`, mixed in `collect_audio` AND the export plan, so preview == export). It is NOT shown
  as a separate audio track; `mute_audio:true` drops it; `tools/slop.py splitaudio <clip>` extracts the
  audio to a real audio-track clip (and mutes the source) for advanced editing.
- **Fonts**: code uses **Consolas** (`g_monoFont`, ASCII), captions use a **48px CJK** font
  (`g_captionFont`) whose glyph range is JP + an explicit symbol set (☆★ smart-quotes ♥♪ arrows).
  A glyph outside both → '?'; add it to the `ImFontGlyphRangesBuilder.AddText(...)` list in `main()`.
- **Big builds/downloads on lame: run DETACHED on lame** (`nohup … > /lamedata/…/build.log 2>&1 &`
  or a `docker run -d` container like `krea2-dl`) and check the log/`du -sh` — do NOT pipe a long
  `docker build`/download through `ssh … | tail` from wslop: output is hidden until EOF and an ssh
  drop wedges/orphans it (this bit the ComfyUI build). Also: the local shell here is **fish** — `for`
  loops / bash-isms in an inline `ssh '…'` arg break; feed remote scripts via `ssh root@lame bash -s <<'EOF'`.
- The playable demo's local assets (`cache/tts/a_vo1.wav`, `cache/image/a_react1.png`) are
  **gitignored** — regenerate with `tools/demo-cache.sh` (needs lame + the TTS provider up;
  ~seconds when warm, longer on a cold provider that must download weights).
- Commercial-safe models only. Secrets → `config.toml` (gitignored). Visuals → llm-feed
  (`:8777`, start if down). Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context)
  <noreply@anthropic.com>`. Push only when asked.

## Pointers
`docs/ARCHITECTURE.md` · `docs/RESEARCH.md` · `docs/ROADMAP.md` · `docs/PROJECT_FORMAT.md` ·
`docs/PROVIDER_PROTOCOL.md` · `docs/INFRA.md`
