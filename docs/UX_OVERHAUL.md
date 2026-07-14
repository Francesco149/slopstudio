# UX OVERHAUL — plan + handoff (the big low-friction-UX arc)

**Started 2026-07-13. Live handoff — read this to resume the arc in a fresh session.**
Goal (owner): make the editor a **low-friction, mouse-driven** tool (à la `../teidraw`'s
intent-based UX) with **clean, professional UI** (à la `../cosmic2d`), where **compositions
come out well-formed by default** — and **no patchwork** (design coherently). Resume with:
`CLAUDE.md` → `docs/STATUS.md` → this file. **Line numbers below drift** (this arc added
~400 lines to `editor/src/main.cpp`) — **function names are the stable anchors**; grep them.

---

## STATUS (2026-07-14) — ⭐ AUTONOMOUS OVERNIGHT MANDATE ACTIVE

**Owner (2026-07-14):** "finish the rest of the UX arc in an autonomous overnight session after the /clear."
So a fresh post-`/clear` session should **read this file and keep building** — VO-editing cluster first,
then Phase 4 → 5 → 6 — committing reviewable chunks. **Full self-contained brief: the next section
(⭐ AUTONOMOUS OVERNIGHT SESSION) — read it before touching code.** The owner will live-test in the morning.

**DONE + committed + owner-CONFIRMED working this session:** Phase 0 (data safety) · Phase 1 (timeline quick
wins) · video-duck · Phase 2 (cosmic2d theme + review fixes) · Phase 3 add-tools · **gap-fill on click**
(`b18bd6d` ✓) · **A-B video loop points** (`6853938` ✓ "feels good") · **drag-drop ARMS placement + portable
uris** (`35b0d0b`+`6f9d796` ✓) · **marquee multi-select + act-on-selection + R=regen** (`6feff28`+`9f3525d` ✓
"this works correctly"). Every item above is confirmed by the owner — a stable base.

**IMMEDIATE NEXT (fully scoped below — do NOT re-discover): VO-clip editing cluster.** VO (tts) clips get
extra timeline lane(s) ABOVE the clip body: an editable **TTS-text** box + a **caption-override** box (only if
`params.transcript` present); **waveform 2× height**; **Enter in a box → regen** the clip. Needs a
**variable-row-height** change (taller tts rows). Design + code anchors + step list in the brief below.

---

## ⭐ AUTONOMOUS OVERNIGHT SESSION — full self-contained brief (read before coding)

**Mission:** finish the UX arc without the owner in the loop. Work order: **(1) VO-clip editing cluster**
(fully scoped below) → **(2) Phase 4** (layout engine, in sub-parts, see below) → **(3) Phase 5** (thumbtool
tldraw-ish) → **(4) Phase 6** (kirby smoke test). Do as much as you can; commit each coherent chunk. Leave
the tree building + committed so the owner can pull and test each in the morning.

### How to work when you CANNOT test interactive UI (the core constraint)
The compositor renders headlessly (`--shot-frame`) but **ImGui chrome / mouse interaction cannot be
screenshotted or driven headlessly**. So for every interactive change:
1. **Build clean** — `taskkill.exe /IM slopstudio.exe /F 2>/dev/null; nix develop --command make -C editor`
   (warnings about misleading-indentation are pre-existing; look for `error:`/`built`).
2. **Reason through the gesture logic** carefully (ImGui rule in THIS codebase: **first-submitted wins** an
   overlapping press — edge-trim handles, the `##place` catcher, and the marquee `##lanes_bg` all rely on it).
3. **Regression-render** an existing project and confirm **byte-identical** output (chrome/selection changes
   must not alter the video): `build/slopstudio.exe ../slopstudio-projects/recettear/recettear.slop.json
   --shot-frame /tmp/x.png --time 46 --cache cache` then `md5sum` vs a baseline. Compositor changes (waveform
   height is chrome, but the VO layout may shift the *timeline* only, not the export — verify).
4. **Commit with an honest message** noting "interactive … = owner-tested live." The owner confirms in the AM.
5. Env vars to the Windows PE need `WSLENV=NAME NAME=1 build/slopstudio.exe …` (WSLInterop won't pass a bare
   env var). Handy for a temporary debug `fprintf(stderr,…)` gated on `getenv("NAME")` — REMOVE before commit.

### Operational facts (learned this session — don't rediscover)
- **Editor = one file** `editor/src/main.cpp` (~11.3k lines). Compositor = ImGui `ImDrawList` + stb_truetype,
  no shaders. Master fn `composite_frame`; export via `render_export_frame`.
- **Regen a clip:** `start_generate(p, clipId)` (safe on any type — non-regenerable sets an error status).
  The inspector "Regenerate (fresh)" bumps `params.seed` then calls it; gates on `gen[id].state==1` (in-flight).
  `R` key already does fresh-regen of the selection (`regenSel` in DrawUI). Regen needs a **provider on lame**
  (TTS) → **wake lame**: `ssh root@code "cold-unlock --host lame --stay"` (only needed to actually RUN a gen;
  code builds/commits without it). Providers are config-driven (`config.toml`).
- **Deferred structural ops:** anything calling `p = parse_project_json(...)` (split/dup/delete/undo) invalidates
  all `Clip&`/`Row&` → defer via request vars in DrawUI (`splitReq/delReq/dupReq/delSel/regenSel`), applied after
  the panels. `add_*`/`place_*` mutate in place (safe mid-frame).
- **Undo** is automatic at gesture settle (`undo_checkpoint`, last each frame). Edits ending outside an ImGui
  active item set `g_undoDirty=true`. No manual push_undo.
- **`sync_to_doc(p)`** copies parsed `Clip.params` wholesale into `p.doc` before save/undo/split — so inspector
  edits that mutate `c.params` (incl. `.erase(key)`) persist. This is why the A-B `loop_out` erase works.
- **Pre-existing quirk (do NOT chase):** the video decoder can return ±1 source frame for the *same* index
  depending on decode history — sub-frame, invisible in motion, orthogonal to A-B loop (which computes the
  right index; verified via a decoder-boundary trace).
- **Commit discipline:** logical units as you go; build first; trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; **do NOT push**; no `git add -A`.
  Editor/tooling → THIS repo; project files/assets → `../slopstudio-projects`. Update STATUS + this file in the
  same change. Doc commits reference the code commit's hash → commit code first, then docs (separate commit).

### TASK 1 — VO-clip editing cluster (DO THIS FIRST) — full design
**Goal:** on the timeline, a VO (`tts`) clip shows, stacked ABOVE its body: an editable **TTS-text** textbox and
(only if `params.transcript` is set) an editable **caption-override** textbox; a **2× taller waveform** in the
body; **Enter in a box regens** that clip. (Owner: "the transcript can be just a textbox I can edit, it doesn't
need to fit the whole text on screen. and pressing enter in the textbox should also regen.")

**The hard part = variable row height.** `DrawTimeline` (editor/src/main.cpp, starts ~`static void DrawTimeline`)
assumes a uniform `const float ROWH = 30.0f` in ~a dozen spots. Introduce a per-row height and thread it through.
Concrete anchors (grep these; line numbers drift):
- Add a lambda near the top of DrawTimeline: `auto rowPx = [&](const std::string& rid)->float { … }` returning
  `ROWH` for non-tts, and for `tts`: `ROWH*1.8f` (body w/ 2× waveform) `+ VO_TEXT_H` (text lane) `+ (rowHasOverride ? VO_TEXT_H : 0)`
  where `VO_TEXT_H≈22f` and `rowHasOverride` = any clip in the row has `params.transcript`. Define `VO_TEXT_H` as a const.
- `int nLanes … contentH = RULER + nLanes*ROWH;` → `contentH = RULER + Σ rowPx(rid)`.
- The track loop `for (auto& tk : p.tracks)`: `float trackTop = laneTop + lane*ROWH;` and `trackH = trackRowN*ROWH`
  → convert to a running **`float curY = laneTop;`** accumulator; `trackTop = curY`; precompute `trackH = Σ rowPx`
  over the track's rows (before the `##th` InvisibleButton that uses trackH).
- The row loop `for (auto& rid : tk.rows)`: `float ly = laneTop + lane*ROWH;` → `float ly = curY; float rh = rowPx(rid);`
  … then `curY += rh; lane++;` (keep `lane` int ONLY for the `(lane%2)` row-stripe color). Row bg rects use `ly+rh-2`.
- Store heights for hit-testing: alongside `laneY.push_back({rid, ly});` add `rowH[rid] = rh;` (a
  `std::map<std::string,float> rowH;`). Then EVERY `pr.second + ROWH` / `+ ROWH` hit-test becomes `+ rowH[pr.first]`:
  the armed-placement `rowAt` lambda, the vertical-drag-onto-lane test, the **marquee** `ly + ROWH > y0`
  (grep `##lanes_bg`), and the OS-drop + LIB-drop row detection (`dy >= pr.second && dy < pr.second+ROWH`).
- **Clip band:** the clip rect is `ImVec2 a(x0+1, ly+2), b(…, ly+ROWH-4);`. For a `tts` clip, push the body to the
  BOTTOM of the tall band: `float topLanes = VO_TEXT_H + (clipHasOverride?VO_TEXT_H:0); a.y = ly + topLanes + 2;
  b.y = ly + rh - 4;`. Non-tts unchanged. The body button / trim handles / label / waveform all key off `a`,`b`.
- **2× waveform:** the waveform is drawn in the clip body (grep `get_wave` inside the clip loop; amplitude was
  `~0.45*ROWH`). With the taller tts body just draw at `~0.9*bodyH` (bodyH = b.y-a.y) → naturally ~2×.
- **The text lanes (editable):** for each tts clip, above the body, submit ImGui InputTexts spanning the clip's
  x-range: `SetCursorScreenPos(ImVec2(x0, ly + (clipHasOverride?VO_TEXT_H:0)))` for the **text** box (params.text)
  and `SetCursorScreenPos(ImVec2(x0, ly))` for the **override** box (params.transcript, only if present).
  `imgui_stdlib` is linked → `ImGui::InputText(id, &stdstring, flags)`. **Persistent buffer per clip** (the
  classic sync trap): keep `static std::map<std::string,std::string> g_voEdit;` keyed `c.id+"|text"` /
  `c.id+"|ovr"`, and an `static std::set<std::string> g_voEditActive;`. Each frame: `if (!g_voEditActive.count(key))
  buf = jstr(c.params, field);` (sync from source only when NOT being edited, else typing is wiped). On
  `IsItemActivated()` insert key; on `IsItemDeactivated()` erase key + commit `c.params[field]=buf` + `g_undoDirty=true`.
  Use `ImGuiInputTextFlags_EnterReturnsTrue` → on true: commit + **request a regen** of this clip.
- **Enter-regen wiring:** add a `std::string voRegenReq;` local in DrawUI (thread it out of DrawTimeline via a
  global like the others, e.g. `g_voRegenReq`), and in the deferred block do `start_generate(p, id)` WITHOUT a
  seed bump (the text change drives the new gen; keep the same voice). NB caption-override edits don't change the
  audio — Enter there is a cheap cache-hit regen (harmless) OR just commit; owner said "the textbox should also
  regen" → wire Enter on the TEXT box to regen for sure; override-box Enter can just commit (note it in the msg).
- **Gotchas:** (a) the tts row is now tall (~76–98px) — make sure the `laneY`/`rowH` maps are used everywhere a
  hit-test assumed ROWH or clips land on the wrong lane. (b) InputTexts submitted at scrolled y positions get
  clipped by the timeline child's clip-rect — fine. (c) The InputText hit-boxes sit ABOVE the clip body button
  (different y bands) so they don't fight. (d) Editing `params.text` should mark the caption STALE the same way
  the inspector does (there's a STALE-CAPTION note in the tts inspector — grep `captions display the override`).
  (e) Keep `g_captionFont`/`g_monoFont` for the VIDEO; the UI/InputText uses the default Inter font — fine.
- **Verify:** build clean; render an existing tts-bearing project (`../slopstudio-projects/luckymas/luckymas3.slop.json`
  or `recettear.slop.json`) at a few times and confirm the EXPORT frame is byte-identical (the VO lanes are
  timeline chrome, not composited into the video). Commit. Owner tests the inline editing + Enter-regen live
  (regen itself needs lame up).

### TASK 2 — Phase 4 (layout engine) — see the "Phase 4" section below for the full spec
The owner previously flagged "checkpoint before Phase 4"; the overnight mandate relaxes that, BUT 4a is a big
architectural refactor — do the **low-risk sub-parts first** and keep each independently revertable:
- **4d first** (cheapest, self-contained, high visual value): diagonal-scrolling soft-checkerboard background,
  **default ON**, as a new bg dispatch at the base clear in `composite_frame` (procedural, no readback). Blur-fill
  becomes opt-in. Parse from `meta`, per-clip override. This one is compositor → **you CAN `--shot-frame` verify it.**
- **4b** spatial linter in `slop.py cmd_lint` (host-over-footage / off-frame / overlap → warn). Pure Python, testable.
- **4c** host-transition robustness (glide reads resolved on-screen pos; blur-aware avatar swaps). Compositor-ish.
- **4a** the big one (generalize `draw_diagram_clip`'s two-pass measure/pack into a shared frame-region slot solver)
  — attempt LAST, in a branch of thought; if it balloons, land a smaller slice + document the rest. Don't break
  existing cuts: every current `.slop.json` must still render byte-identical unless a clip opts into the new layout.

### TASK 3/4 — Phase 5 (thumbtool tldraw-ish) + Phase 6 (kirby smoke test)
See their sections below. Phase 6 needs lame for TTS. Phase 5 is in `thumbtool/` (separate app, `engine.h` has
per-layer AABBs already). Lower priority than 1/2 — only if 1/2 land cleanly with time to spare.

### If blocked / risky
Prefer landing a smaller, correct, committed slice over a big untested change. If something needs the owner
(a genuine design fork, or lame is down and a task strictly needs a gen), STOP that task, commit what's safe,
write a clear note here + in STATUS, and move to the next independent task. Never push. Never `git add -A`.

---

**Just landed (⏳ owner to test): drag-drop ARMS the placement tool + portable asset uris** (`35b0d0b`, `6f9d796`).
Owner: "drag-drop should trigger the same add-clip UX, forced to the asset dropped in" → then "it just places
immediately, doesn't trigger the add mode; likely taking the click-release from the drop." Both fixed:
(1) A timeline drop now ARMS the placement tool loaded with the asset (`g_placeType="__asset__"` + `g_placeAsset`)
— live ghost preview, click fresh to position, overlap-aware (clicked > free lane > new track), natural length
(`asset_natural_dur`). `g_placeIgnoreUntilUp` swallows the drop's OWN release so it can't instant-commit.
OS file-drop + Media-pane drag arm (media); a rig / double-click still add immediately (`add_asset_clip_placed`).
(2) `portable_asset_uri` relativizes an imported path to `assets/…`/`library/…` before it becomes a clip uri
(in add_image/audio/video_clip_at) — fixes the kirby missing-images bug class. Verified: build clean, renders, boundary cases pass.

**Owner-approved earlier this session:**
- **gap-fill on a plain placement click** (`b18bd6d`, ✓). Click with a type armed fills the gap to the next
  clip on the target lane (`place_fill_to_next`); drag still sizes exactly.
- **A-B video loop points** (`6853938`, ✓). Inspector ▸ playback ▸ "loop segment (A-B)": A/B fields + "set
  A/B from playhead" scrub-to-set; `A=params.in`, `B=params.loop_out`; A-B sub-loop mutes own audio. NB a
  pre-existing sub-frame decode quirk (same idx → ±1 frame by decode history) is orthogonal, invisible in motion.

**Then:** Phase 4 (the layout engine — the big architectural piece; **checkpoint with owner first**),
Phase 5 (tldraw-like visual composer), Phase 6 (kirby smoke test). Details below.

### Commits this session
**slopstudio** (13): `867ae03` resolution+backups+media-scope+dashboard-cut · `d80ae01` Del/Ctrl-drag-dup/video-loop ·
`e915be8`+`bfab33d` video-duck (+silent-RMS fix) · `c95f2a2` STATUS · `36ed3e5` cosmic2d theme+Inter ·
`8d10d3c` track-buttons+resizable-panels+draggable-tracks · `e0d3ddb` mid-mouse-vpan+timeline/preview-divider ·
`6c90431` quick-add+click-to-place · `352c129` overlap-aware placement · `1382ef0` generic-add(A) · `8220ac4` add-mode-polish ·
`b18bd6d` gap-fill-on-click · `0b0b488` docs · `6853938` A-B-video-loop · `befb3c2` docs · `35b0d0b` drag-drop-add-UX+portable-uris · `6f9d796` drag-drop-ARMS-placement · `6feff28` marquee+R-regen · `9f3525d` multi-drag-fix.
**slopstudio-projects** (2): `3eb0dd6` kirby music reconstruction · `f5d804f` kirby Pictures copy+repoint.

---

## THE PLAN (approved arc)

Two **shared systems** on a **correctness foundation**; every ask is an instance of one, not a point fix:
- **A · Intent-based interaction** (teidraw): click-vs-drag, act-on-selection, context gizmos not modes,
  smart defaults on drop, snapping/guides. Drives the timeline AND the visual composer.
- **B · Well-formed-by-default composition** (the layout engine): a declarative frame-region layer that
  slots host/content/captions non-overlapping — generalizing `draw_diagram_clip`'s content-measuring
  auto-fit. Makes "host closeup blocking footage" structurally impossible.
- **0 · Correctness & data safety** foundation.

### Phase 0 — Correctness & data safety ✅ DONE (`867ae03`, projects `3eb0dd6`/`f5d804f`)
- **CWD-independent asset resolution** — `g_repoRoot` (derived from the exe path, marker-verified) anchors
  `library/` beds + `cache/`; project `assets/` art absolutises against `g_projDir`. Fixed the dashboard-
  launch **empty Media library** + the launcher's dropped beds (both families resolve from either CWD;
  verified 104 audio inputs from repo-root AND project-dir). Fns: `resolve_asset`, `derive_repo_root`,
  `parse_project_json` (g_projAssetDirs absolutise), `scan_library`.
- **Backup-on-save** — `save_project` → `backup_before_save`: `<proj>.slop.json.bak` + a 12-deep timestamped
  ring in `<dir>/.backups/` (`prune_backups`). Undo is in-memory only; this is the on-disk net (the kirby
  music was lost to a bad save). Verified.
- **Media pane unclutter** — hides superseded rigs (gemma-chibi/pngtuber/host/teacher-test) + loose
  `library/images` sprites & stray Pictures imports in both/project scope; beds + current rigs stay;
  "common" scope = full library. In the `scan_library` display loop (grep `LEGACY_RIGS`).
- **Dashboard** (`tools/dashboard.py`) — `projects_state` sorts non-shorts first so one-click "open editor"
  targets the MAIN cut; a cut-picker `<select>` opens any cut.
- **kirby music RECONSTRUCTED** — git had nothing recoverable (never committed). House-style arrangement via
  `slop.py bed` (recettear template: deadly-roulette bookends, chill-wave swells, space-jazz body, ramped)
  onto the owner's edits; all 267 existing clips byte-identical. Owner to fine-tune levels.
- **kirby Pictures** — copied the 2 dragged `F:\Pictures` images into `assets/kirby/images/` + repointed URIs.

### Phase 1 — Timeline quick wins ✅ DONE (`d80ae01`)
`Del`/`Backspace` deletes the selected clip (guarded `!WantTextInput`); `Ctrl`+drag duplicates (copy stays,
original drags off — via `g_dragDupReq`/`g_dupAtStart`, `duplicate_clip` gained a placement arg); new video
clips default to **loop** (`add_video_clip_at`).

### Video audio ducks the music bed ✅ DONE (`e915be8`, `bfab33d`)
A video clip whose source has REAL audio dips the bed over its span, gentle floor (`DUCK_VIDEO_FLOOR`=0.42 vs
SFX `DUCK_FLOOR`=0.07). `struct DuckWin` carries a per-window floor; preview (`duck_factor`) + export vol_expr
identical (`1 - max_w((1-floor_w)*ramp_w)`). Silent tracks (RMS<`VIDEO_AUDIO_SILENCE_RMS`) are audio-less — a
low playback volume like the 2% cat meme still ducks (loud source). `get_video_pcm` now computes `rms` (it
didn't; only `get_pcm` did). Per-clip `params.duck_music` + inspector toggle. In `collect_duck_windows`.

### Phase 2 — UI polish (cosmic2d) ✅ DONE (`36ed3e5`, `8d10d3c`, `e0d3ddb`)
- `apply_editor_theme()` — cosmic2d palette (deep-purple `#141220` base, `#1e1b2e` panels + darker child
  inset, mint `#7fd8a8` accent, periwinkle `#8878d0` focus, lavender `#e8e4ff` text) + rounded metrics.
  **CHROME ONLY** — video fonts (`g_captionFont`/`g_monoFont` = YuGothM/Consolas) untouched (verified via a
  t=46 kirby shot). ImGui 1.92.4 enum names.
- **Inter** UI-chrome font (bundled `assets-src/fonts/InterVariable.ttf`, OFL) with the CJK face merged; falls
  back to system CJK. Loaded via `g_repoRoot`. Video-content `dl->AddText` paths use the dedicated fonts;
  font-less badge AddText falls back to Inter+CJK (renders everything).
- **Owner review fixes:** compact track `^ v x` buttons (no timeline overlap); **resizable panels** —
  `v_splitter`/`h_splitter` drive `g_leftW`/`g_inspW`/`g_topH` (Media pane, Inspector, and the preview↕timeline
  divider); **vertically-draggable tracks** (drag the gutter → live-reorder `p.tracks`, `TrackRange`/`dragTrackId`);
  **middle-mouse also pans vertically** (`SetScrollY` in the mid-drag block).
- ⚠ **I cannot screenshot the ImGui chrome headlessly** (only the compositor via `--shot-frame`). UI look is
  verified by the OWNER launching. Build the exe and ask them to eyeball chrome changes.

### Phase 3 — Intent-based timeline 🔶 IN PROGRESS
**Add tools DONE** (`6c90431`, `352c129`, `1382ef0`, `8220ac4`):
- **Placement palette** in the transport row: `A` (generic) + `Host Voice Sound Music BG Caption Code Shape`.
  Arm a type → **click empty timeline to place, drag to draw length**. Esc cancels. Placement is deferred +
  undoable; **auto-selects the new clip + auto-exits** the mode after placing.
- **Overlap-aware row selection** (`choose_place_row`/`choose_row_of_type`, `row_span_free`): clicked lane (if
  right type + fits) > any existing lane of the type with room > a **new track**. Re-evaluated live as you
  drag — the preview draws on the actual target lane (mint) or a periwinkle "+ new track" ghost. Never overlaps.
- **Generic add (A)** (`add_generic_clip`): a clip of the **clicked lane's type**, inheriting the nearest
  reference clip's (occupying > before > after) transform + non-content params (rate/loop/framing/style/…);
  content stays empty to fill; typed defaults if the lane is empty; spills to a new lane of the same type
  (copying the lane's rig/voice) when there's no room.
- **Quick-add defaults** (`add_quick_clip` → `place_clip_on_row`/`create_place_row`): host=`gemma-gpt-static`
  (uses the target avatar row's OWN rig), voice=`gemma-san-deep-clone` tts line, sound=`presets/voice-snips/gemma-heh.wav`,
  music=`library/music/deadly-roulette.mp3`, backdrop=`presets/backgrounds/room-day.png` (layout:cover, sinks
  to bottom). Also on Clip ▸ Quick add menu.
- **Key mechanics:** the `##place` catcher is submitted **BEFORE the clip loop** in `DrawTimeline` so add mode
  wins clicks ON clips too (first-submitted-wins); preview/commit run after the loop from captured bools
  (`placeActivated/placeActive/placeDeact/placeHover`). Globals: `g_placeType` ("" | a kind | `"__generic__"`),
  `g_placeReq`/`g_placeReqRow`/`g_placeReqT`/`g_placeReqDur` (DrawTimeline→DrawUI deferred), applied in DrawUI.

**gap-fill-on-click DONE** (`b18bd6d`, ⏳ owner test): a plain placement click fills to the next clip on the
target lane (`place_fill_to_next`); the click path probes row-selection with ~0 width so it lands in the
clicked gap; `generic_ref_clip` factored out for the fallback length. Drag path unchanged.
**A-B video loop DONE** (`6853938`, owner ✓): `params.loop_out` (B) + `params.in` (A) = the loop window;
`video_src_time` (factored from `video_frame_index`) wraps `[A,B]`; inspector "set A/B from playhead" scrub-to-set;
A-B sub-loop mutes own audio. `docs/PROJECT_FORMAT.md` §clips updated.
**Drag-drop + portable uris DONE** (`35b0d0b`, ⏳ owner test): `add_asset_clip_placed` routes the OS drop /
Media-pane drag / double-click through the overlap-aware placement (clicked > free lane > new track), forced to
the dropped asset; `portable_asset_uri` relativizes the stored uri (kirby missing-images fix). See the Extra-task note below.
**Phase 3 REMAINING** (see NEXT above): marquee multi-select — the last item.

### Phase 4 — The layout engine (BIG; checkpoint with owner before starting)
- **4a** Generalize `draw_diagram_clip` (the ONE real content-measuring, two-pass auto-fit layout engine in the
  editor) into a **shared frame-region slot solver**: a declarative "host + these N visuals + captions share
  this frame" contract → non-overlapping slots adapting to content count / aspect / host orientation. Replaces
  (i) the hardcoded multi-image `CASCADE`/stack tables in `slop.py` (~`make_visual`), (ii) the inference-from-
  siblings host placement (`content_centroid_span`, `span_has_fullscreen_content`, `avatar_fit`, `autoOffX`),
  (iii) the per-clip `layout` enum. Per-clip pos/scale stays an override; roll out primitive-by-primitive.
- **4b** Spatial linter in `slop.py cmd_lint`: host-over-footage / off-frame / overlap → fail in code.
- **4c** Host-transition robustness (falls out of consistent placement): "aligns to previous image" (the glide
  in `clip_transition`/`clip_trans_info` reads the neighbor's AUTHORED pos, not the resolved on-screen pos);
  "unwanted transition mid-blur" (avatar swaps fire on the avatar row's own seams independent of `blur` clips —
  make swaps blur-aware). Revisit the `ctol=0.35` adjacency tolerance.
- **4d** Diagonal scrolling soft-checkerboard background, **default ON** — a new background-style dispatch at
  the base clear in `composite_frame` (procedural, no readback pre-pass, cheaper than the blur `filler`);
  blur-fill becomes an opt-in style. Parse from `meta`, per-clip override.

### Phase 5 — tldraw-like visual composer
- **5a** Upgrade `thumbtool/` (slopthumb) to tldraw-like direct manipulation — its model is already gizmo-ready
  (every layer has a computed AABB `engine.h`; block cache excludes pos/rot/opacity; GPU compositor draws
  rotated quads). Add 8 resize handles + rotate ring + marquee + snapping + drag-drop image import in
  `panel_canvas`. Keep the doc model / brand package / block cache / undo / hot-reload.
- **5b** Bring the same direct-manipulation + the Phase-4 solver to video card/diagram beats in the editor.
  Share the interaction MODEL + doc concepts with thumbtool, **not the code** (ImDrawList vs stb+D3D11
  divergence). ⚠ `tx_rot` is stored/inspector-editable but **never read by any editor draw path** — rotated
  cards need new compositor support first.

### Phase 6 — kirby smoke test (throwaway unless great)
Re-compose kirby as a human quickly would; nudge defaults until it composes well with minimal ad-hoc tweaks.
Needs lame for TTS (`ssh root@code "cold-unlock --host lame --stay"`).

### Extra task — drag-drop of external files copies into the project ✅ DONE (`35b0d0b`, ⏳ owner test)
Owner dragged `F:\Pictures` images expecting a copy into the project + repointed URI; they stayed absolute
(caused the kirby missing-images). The OS-drop already imported via `library_import` (copies into
`g_projLibDir`), but the returned path was ABSOLUTE → stored as the clip uri. Fixed with `portable_asset_uri`
(relativizes to `assets/…`/`library/…` in `add_image/audio/video_clip_at`; `resolve_asset` reverses it).
Plus (owner's follow-up asks) the drop ARMS the placement tool (`__asset__` mode, `6f9d796`) rather than
instant-placing — live ghost + a fresh click position it, same as the palette; `g_placeIgnoreUntilUp` swallows
the drop's own release so it can't self-commit. Commit routes through `add_asset_clip_placed` = the overlap-aware
placement forced to the asset. `asset_natural_dur` = the fit probe (image default / real audio / real video,
never truncated); `rig_name_from_path` handles a rig def (added immediately, not click-placed). Owner-tested live.

---

## ARCHITECTURE & RESEARCH (durable — don't re-investigate)

- **Editor = one file** `editor/src/main.cpp` (~11.2k lines). Compositor is **100% ImGui `ImDrawList` +
  stb_truetype — NO shaders** (the docs' "HLSL effect graph" is aspirational). A layout system works at the
  draw-list level. Master compositing fn: `composite_frame` (clear → auto bg→host match → the main
  track→row→clip painter walk → vignette → `draw_song_credit`). Preview calls it; export via `render_export_frame`.
- **Placement/layout facts:** `draw_diagram_clip` = the layout-engine prototype (measures labels, packs, two-pass
  shrink). Jank sources = slop.py hardcoded `CASCADE` stack tables + inference-based host placement + no spatial
  lint. `tx_rot` is a dead field in the editor. `meta.anchors` + `params.anchor` = per-project category base
  positions (`anchor_off`).
- **Deferred structural ops:** anything that calls `p = parse_project_json(...)` (split/dup/delete/track-delete/
  undo) invalidates all `Clip&`/`Row&` → **defer** via request vars (`splitReq/delReq/dupReq` in DrawUI, applied
  after the panels). NOTE: `add_track`, `add_*_clip_at`, `add_quick_clip`, `add_generic_clip` do NOT reassign p
  (direct mutation) — safe mid-frame, but the placement path defers anyway for cleanliness.
- **Undo** is automatic at gesture settle (`undo_checkpoint`, called last each frame). Interactive drags need
  nothing; edits ending outside an ImGui active item set `g_undoDirty`. No manual `push_undo()`.
- **ImGui overlap rule (this codebase):** **first-submitted wins** an overlapping press (edge-trim handles rely
  on it; the `##place` catcher relies on it). Submit an item earlier to make it win.
- **Fonts:** the editor's fonts render into BOTH chrome and the exported VIDEO. `g_captionFont` (YuGothM 48)
  and `g_monoFont` (Consolas) drive captions/code in the export — DO NOT change them for UI reasons. The UI
  default font is separate (now Inter + merged CJK).
- **teidraw UX to port** (`../teidraw/editor/src/main.cpp`): click-vs-drag 4px threshold (`DM_PENDING`);
  context gizmos not modes; act-on-selection; smart replace-on-drop; snapping guides (Ctrl opt-in); marquee
  partial-overlap select; right-click context menus over modals; drill-in re-click selection.
- **cosmic2d palette** (`../cosmic2d/engine/cm/ed.lua`): bg `#141220`, panel `#1e1b2e`, edge `#4a4370`, accent
  (mint) `#7fd8a8`, focus (periwinkle) `#8878d0`, text `#e8e4ff`, dim `#8a84b0`, unsaved `#ffb46e`. Fonts Inter
  + JetBrains Mono, RasterizerMultiply ≈1.35.

## Conventions
- Build: `nix develop --command make -C editor` (kill stale `slopstudio.exe` first: `taskkill.exe /IM slopstudio.exe /F`).
- Verify compositor headlessly: `build/slopstudio.exe <proj> --shot-frame <out.png> --time <t> --cache cache`
  (absolute exe path — cwd drifts). Export plan: `--export-plan <out> --cache cache`. Cannot screenshot chrome.
- Three repos, commit each in-session: editor/tooling → this repo; kirby.slop.json + assets → `../slopstudio-projects`.
  Co-author trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Push only when asked.
- Owner tests interactive UI **live** — deliver reviewable chunks and iterate from their feedback (the loop that
  drove the add-tools polish). It works well; keep it.
