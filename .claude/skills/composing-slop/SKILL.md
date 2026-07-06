---
name: composing-slop
description: Drive the slopstudio CLI to compose or edit a video project (the beats → skeleton → adopt/genvo → lint/critique → render gold path). Use whenever building, cutting, retiming, rendering, or otherwise editing a .slop.json video with tools/slop.py — including "compose a video", "cut a short", "edit this beat", "re-render".
---

# Composing a video with slop.py

The agent authoring path for slopstudio. **Run every tool from the slopstudio repo root, inside the dev
shell** (`nix develop --command python tools/slop.py …`) — `command not found` means you forgot it.
Projects live in `../slopstudio-projects/<name>/` (their OWN git repo — **commit project edits there, same
session**). The reference cut + all conventions are in `docs/LLM_WORKFLOW.md`; format spec in
`docs/PROJECT_FORMAT.md`. The golden reference is `../slopstudio-projects/luckymas/luckymas3.slop.json`.

## The gold path (do NOT skip steps)
1. **Package first** → run the `packaging-first` skill: lock title + thumbnail + angle before scripting.
2. **Beats** — write `<name>.skeleton.json` (`{line, emotion, visual}` beats). Visuals HOLD until changed.
3. **Compile** — `slop.py skeleton <skel> --out <proj>` → a full project (insets get filler backdrops,
   sections = blur-swaps, captions/code native).
4. **VO** — `adopt` verbatim lines from an existing tuned cut where possible; `genvo` bulk-generates the
   rest on lame; then `retime` time-warps the timeline onto the real audio durations (rate-aware for shorts).
   Split every sentence into its own TTS clip; tune intonation per clip (see `gemma-voice-tts`).
5. **GATE — lint MUST pass before render.** `slop.py lint <proj>` flags black spans, no-visual narration,
   asset repeats, missing files, stale VO, overlaps, negative starts, unknown emotions. Also run the
   `retention-editing` review. (When built: `slop.py critique`/`designcheck`/`scriptlint` — the deterministic
   quality gates from `docs/AGENT_TOOLING.md`.)
6. **Render** — `bash tools/export.sh <proj> --cache cache`. Use `--remux-from` for audio-only re-mux.

## Visual authoring rules (from the studied reference corpus — SYNTHESIS.md in
`../slopstudio-projects/research/video-study/`)
- **Say it → show it, ≤1 beat.** Any named artifact (game, file, paper, person, UI
  element) gets a visual in the same beat — a named UI element gets a zoom-crop of
  exactly that element. A beat whose `visual` merely HOLDS while new things are named
  is a bug.
- **The visual floor is perceived motion**: host floats OVER footage/diagrams (never a
  full-screen talking head while a subject exists); bare-host beats need an ambient-
  animated backdrop; long shots are fine only while something in-shot animates
  (typewriter, bars, sim running).
- **One canonical diagram per system, grown across the video** — re-show the SAME diagram
  asset with layers added (cascade unveil) as rules accumulate; don't mint a new diagram
  per fact.
- **Receipts on screen:** quote cards with the key phrase highlighted, paper/patent/commit
  screenshots, code cropped to the load-bearing lines only.
- **Teach a chart's grammar before plotting the reveal on it**; big numbers get stat
  cards.
- When no footage exists for a claim, STAGE it (A/B mockup, repro save, XP-VM capture) —
  fabricating the demonstration is the normal path, not extra credit.

## Editing existing cuts
Use the CLI verbs (`overview` to read the timeline; `add`/`insert`/`ripple`/`rebase`/`retime`/`bed`/
`transcript`), NOT hand-written JSON — extend the CLI rather than one-off scripts. `overview` is the
LLM-facing timeline view today (a compact `digest` is planned). After any take swap, **run `retime`** (it's
the canonical follow-up — durs must be ÷speech_rate for 1.3× shorts, or you leave a dead gap).

## House rules
- **Show visuals on the llm-feed** as you go (`curl -F file=@shot.png localhost:8777/push`) — fire-and-forget.
- Everything is undoable by default (doc-snapshot on gesture settle) — just mutate the project.
- **Commit** logical units as you land them (project edits in the projects repo; code in slopstudio). Build +
  lint before committing. **Push only when asked.**
- Reusable Gemma vocal snippets live in `presets/voice-snips/` (e.g. `gemma-heh.wav`) — reuse verbatim
  instead of regenerating sounds the TTS renders inconsistently.
