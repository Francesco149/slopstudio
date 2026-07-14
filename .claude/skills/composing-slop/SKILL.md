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
3. **Compile** — `slop.py skeleton <skel> --out <proj>` → a full project. DEFAULTS the compiler now applies
   (owner-tuned, override per-beat): a whole-video **noir "basic look"** — the filter clip is the TOPMOST track
   so it grades EVERYTHING, at strength 0.25 · **vignette 0.5** (`look:false` to drop it); **insets sit on the
   bare checker** (`blur_fill:true` opts a nice photo into the blurred backdrop); a **fullscreen video beat
   solos** (no host over wide footage); **fullscreen SCREENSHOTS become framed insets on the checker,
   alternating** hosted-beside ↔ centered-solo card (`cover:true`/`host:true` opt out) — a HOSTED inset
   corners the host SMALL (a lower-corner commentator, wings clear of the card); **code beats compile SOLO +
   centered on the BARE bright grid** (no filler, and the filter carves out code spans so the grade never
   crushes the grid); **host-only beats rotate a room↔desk backdrop**; **quote/term/caption beats sit on the
   checker**; **term PILLS use a condensed all-caps display font** (`meta.pill_font` bebas|anton|archivo,
   default bebas); sections = blur-swaps; captions/code native.
4. **VO** — `adopt` verbatim lines from an existing tuned cut where possible; `genvo` bulk-generates the
   rest on lame; then `retime` time-warps the timeline onto the real audio durations (rate-aware for shorts)
   + adds a **scene crossfade** on full-frame cuts. Split every sentence into its own TTS clip; tune
   intonation per clip (see `gemma-voice-tts`).
   - **NEVER genvo a laugh/giggle** — the TTS can't say "fufu" and stumbles on "heh", so the signature giggle
     is the RECORDED **`presets/voice-snips/gemma-heh.wav`** (the "Heh~" from luckymas3 b01). The compiler now
     does this AUTOMATICALLY: a beat whose LINE is just a laugh (`Heh~` / `Fufu~` / `heh heh` / `ふふふ` /
     `*giggle*`, or `"laugh": true`) auto-wires the golden take (genvo skips it) and renders it as a smug
     closeup that snaps in. A line that LEADS with a laugh then speaks (`"Heh~ Welcome back, mortals"`) auto-
     SPLITS into a golden-giggle beat + the remainder as normal TTS. So just write the laugh into the line —
     don't hand-wire it. Same for other stumble-prone vocalizations → grow `presets/voice-snips/`, reuse verbatim.
5. **GATE — lint MUST pass before render.** `slop.py lint <proj>` flags black spans, no-visual narration,
   asset repeats, missing files, stale VO, stale caption overrides (a reworded line whose `params.transcript`
   kept the OLD wording), overlaps, negative starts, unknown emotions. Also run the
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
`transcript`/`anchor`), NOT hand-written JSON — extend the CLI rather than one-off scripts. `overview` is the
LLM-facing timeline view today (a compact `digest` is planned). After any take swap, **run `retime`** (it's
the canonical follow-up — durs must be ÷speech_rate for 1.3× shorts, or you leave a dead gap).
**Moving captions over a time range** = a caption-anchor clip (`slop.py anchor <proj> --beat b04 --pos 0,442`),
NEVER per-chunk pos nudges — `transcript` regen wipes chunk transforms but anchors survive (their own row).

## House rules
- **Show visuals on the llm-feed** as you go (`python /opt/src/llm-feed/feed.py image shot.png --title "…"`,
  inside the dev shell) — fire-and-forget. (There is NO `curl …/push` endpoint — use feed.py.)
- Everything is undoable by default (doc-snapshot on gesture settle) — just mutate the project.
- **Commit** logical units as you land them (project edits in the projects repo; code in slopstudio). Build +
  lint before committing. **Push only when asked.**
- Reusable Gemma vocal snippets live in `presets/voice-snips/` (e.g. `gemma-heh.wav`) — reuse verbatim
  instead of regenerating sounds the TTS renders inconsistently.
