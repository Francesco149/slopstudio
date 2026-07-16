# Authoring audit — 2026-07

Durable working ledger for the July 2026 audit of LLM and human video authoring.
This file records evidence and decisions while the audit is in progress so the work can
resume cleanly across sessions. It is not a replacement for `STATUS.md`; finished product
truth is promoted to the canonical workflow and format docs.

## Scope

- Reduce iteration for LLM-authored composition, pacing, and scripts.
- Recompose `../slopstudio-projects/lotr2/lotr2-v2.slop.json` with the scene-animation
  library as a smoke test; improve the flicker-prone battle capture first.
- Finish the same quality loop for the incomplete Kirby cut where changes generalize.
- Evaluate more stable TTS options and a repeatable pronunciation/tone test harness.
- Make human-only authoring viable for months without a frontier LLM.
- Add restrained long-form animation/transition SFX and revisit the 1.3x Shorts rate.
- Consolidate stale documentation and contradictory guidance.

## Baseline evidence

- `lotr2-v2.slop.json`: 668.2 s, 270 clips, 0 lint warnings / 5 overlap notes.
- Its visual language predates the Lua scene library: 7 native code cards, 24 static
  image placements, 9 video placements, and no `scene` clips. The 32 s battle capture is
  reused across the cold open and Acts 1–2.
- Existing authoring guidance already states the right principles (say-it/show-it,
  perceived-motion floor, one idea per line, verification pass), but important gates are
  still described as planned: `slop.py scriptlint`, `critique`, and `designcheck`.
- Guidance drift exists: `.claude/skills/composing-slop/SKILL.md` says the basic look uses
  vignette 0.5 while current `docs/STATUS.md` says the default was moved to the project
  radial vignette 0.10 and removed from the noir filter to avoid doubling.
- The shipped scene widget surface is much larger than the CLI help and authoring docs
  expose. `presets/lua/std.lua` includes reveal/cardflip/perspective/drag/slidein/code/
  rays/waves/youtube_comment/timeline/callout/zoomout/filmstrip/chart/DCM/octant/table,
  while `slop.py scene --widget` help names only the early seven widgets.

## Working hypotheses to validate

1. More prose will not fix composition reliably; cheap temporal and structural checks
   need to run before VO/render and emit beat-local fixes.
2. Skeleton visuals need first-class scene-widget authoring, not post-compile JSON surgery.
3. A widget catalogue with editable examples is the shared bridge between LLM and human
   authors; today discovery depends on reading Lua/status history.
4. Long-form SFX should be semantic, opt-in cues with a restrained default mix, not an
   automatic sound on every motion primitive.
5. Shorts speech rate should become an evidence-backed profile or per-project choice,
   not a format-wide 1.3x assumption.

## Work log

- 2026-07-16: mapped repo/project state, read the composing/script/retention/frame-study
  guidance, baselined the lotr2 timeline and lint, and started parallel TTS/rate,
  human-docs, and battle-capture investigations.
- 2026-07-16: added first-class scene widgets to skeleton beats plus `scene-widgets`,
  `scriptlint`, and deterministic `critique`. The first lotr2 render caught scene-title +
  legacy-plate duplication; the candidate was corrected and the check was generalized.
- 2026-07-16: built `lotr2-animated.slop.json` from the canonical skeleton with six scene
  animations (perspective receipt, count-up stats, and three before/after comparisons),
  reused all 99 existing VO takes, and reached 0 lint warnings / 5 benign overlap notes.
- 2026-07-16: battle flicker traced to asynchronous framebuffer reads during sprite repaint.
  The temporal-mix candidate reduces isolated one-frame transients from 0.766% median /
  3.700% p95 to 0.084% / 0.287% (~89% median reduction) for a 5.2% sharpness cost. A true
  synchronized recapture currently terminates the synthetic battle after ~15 frames; the
  original is preserved and the recipe is documented in the project manifest.
- 2026-07-16: added three restrained, deterministic long-form SFX (`soft-swish`,
  `soft-settle`, `soft-tick`) and attached them at -15 to -17 dB without music ducking to
  the new lotr2 animations.
- 2026-07-16: removed the portrait-only 1.3x multiplier. New portrait and landscape cuts
  share the same base rate and aim for ~180 WPM (175–190); a voice preset may still need
  e.g. 1.15x for both formats. Existing explicit project rates are unchanged.
- 2026-07-16: TTS shortlist is Chatterbox first, CosyVoice 3 second, a bounded dots.tts
  retry third, with Azure as the deterministic SSML escape hatch. The reproducible test
  suite and pass criteria live in `docs/TTS_EVALUATION.md`.
- 2026-07-16: owner review of the animated LotR II cut exposed three composition defaults:
  the signature opener was only partially reused, native code lagged behind Kirby's VS Code
  scene, and held solo screenshots could summon a presenter on later beats and shift sideways.
  The compiler now locks the complete LuckyMas opener, defaults code to `widgets.code` with
  mouse carry-in, and remembers when held content owns the frame. `lotr2-rework.slop.json`
  is the resulting candidate; five rewritten lines intentionally await new VO.
- 2026-07-16: captured full provenance frames and legible 1080p edit crops for the real
  Steam “This Game CHEATS!!!” thread and GOG empirical-stats post, then replaced the two
  generic-map placeholders. Source review caught that the GOG post measures movement,
  range, armour, and deterministic hits but does not publish the executable's seven resolver
  weights; the script now keeps the fan testing and extracted table as separate claims.
  The recompiled candidate adopts 91 existing VO takes; seven rewritten lines await VO.

## Deliverables / status

- [x] Lower-flicker battle capture candidate and reproducible recipe.
- [x] `lotr2` animated recomposition candidate with lint + representative frame review.
- [x] Deterministic authoring quality gates (regression coverage remains partial).
- [x] Updated LLM skills and human runbook/widget catalogue.
- [x] Long-form SFX mapping/presets and examples.
- [x] TTS shortlist, integration plan, and A/B corpus specification.
- [x] Shorts-rate decision backed by measured evidence and the existing creator corpus.
- [ ] Documentation cleanup map applied.

## Remaining prioritized gaps

1. `critique` correctly exposes four long stretches in lotr2 with 5–10 spoken beats between
   primary content changes and a measured 157.5 WPM narration pace. These are real next-cut
   issues, not lint blockers; the six-scene smoke proves the path but is not a final owner cut.
2. Add schema metadata beside each Lua widget, then drive editor forms/gallery, CLI validation,
   and generated cookbook tables from it. The current JSON editor still assumes technical users.
3. Run the 30-line TTS suite on Chatterbox and CosyVoice; no alternate engine should become
   default on vendor claims alone.
4. Replace temporal postprocessing with a true repaint-safe battle capture after fixing the
   synchronized synthetic-battle lifetime. The tmix clip is a useful candidate, not source truth.
5. Introduce a project manifest (`live_cut`, `source_skeleton`) and generated-cut overwrite
   protection. Multiple `kirby`/`lotr2` experiments remain ambiguous to `video.py`.
6. Archive the chronological bulk in `STATUS.md`, `UX_NEXT.md`, and `UX_OVERHAUL.md`; this pass
   corrected the active runbook/format/preset docs but intentionally did not rewrite 200 KB of
   user history in the same code change.
