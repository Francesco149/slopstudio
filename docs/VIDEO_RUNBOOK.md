# VIDEO RUNBOOK — build a @GemmaExplains video end to end (human edition)

The minimal-friction path from idea → uploadable video. `docs/LLM_WORKFLOW.md` is the
agent-facing version; **this** is the one to follow by hand. One front-door tool wraps the
whole pipeline so you don't memorize commands or type long paths:

```
nix develop --command python tools/video.py <cmd>      # everything runs in the dev shell
```

> Tip: `alias vid='nix develop --command python tools/video.py'` in your shell, then just
> `vid doctor`, `vid build luckymas`, etc. `<name>` is a folder under
> `../slopstudio-projects` (e.g. `luckymas`) or a direct `.slop.json` path.

The gold path in one line:
**package → research → outline → script → `build` → `voice` → `lint`/`look` → tune → thumbnail → `export` → publish.**

## No frontier LLM available?

Nothing in the gold path requires one. Slash-skills are optional reviewers; use these
written gates instead:

1. **Package:** write one sentence, “This video proves ___ by showing ___.” Draft three
   titles that promise that fact and three thumbnail sketches with one focal subject and
   at most four words. Pick the pair that makes the same promise.
2. **Outline:** each act answers one question, ends in one reveal, and names the visual
   receipt that proves it. If an act has no receipt, research or cut it.
3. **Script:** one idea/sentence per beat. Every line must hook, teach, or land a joke.
   Replace “and then” with a consequence/reversal; verify every claim with an exact source,
   filmable demo, or explicit pointer. Run `slop.py scriptlint <skeleton>` before voice.
4. **Compose:** say-it/show-it in the same beat; change the visual every 2–4 spoken beats;
   use `vid animations` and `docs/SCENE_COOKBOOK.md` for stats, comparisons, receipts, and
   charts. A self-labeling scene does not also need a term plate.
5. **Review:** `vid lint` runs structural lint plus pacing/composition critique. Watch once
   muted (hierarchy/captions), once audio-only (script/voice), then normally (sync/tone).
   Check the first 30 seconds, every act boundary, and the final payoff separately.
6. **Voice:** keep a pronunciation list, regenerate only the bad line, and compare takes
   blind at matched loudness. See `docs/TTS_EVALUATION.md` for the stable-engine test suite.

Before hand-tuning the compiled cut, commit the skeleton and cut and mark the skeleton
frozen in its notes. Rebuilding replaces generated composition; after that checkpoint use
the editor or `slop.py set/mv/trim` and do not rebuild without intentionally discarding
those adjustments.

---

## 0. Is my machine ready?

```
vid doctor      # ✓/✗ on: dev shell · config.toml · editor+thumbtool builds · lame · feed
vid wake        # if lame is ✗ — wakes + unlocks the GPU box (VO/gen need it). ~1 min.
```
Green on dev-shell + builds is enough to start. `lame` is only needed for `voice`; `feed`
only for `look`. If a build is ✗: `make -C editor` / `make -C thumbtool` (in the dev shell).

## 1. Package FIRST (the #1 growth lever)

Decide **title + thumbnail + angle together, before scripting** — the packaging becomes the
spec the script pays off. Run the **`/packaging-first`** skill; it emits the angle, 8–12
scored titles, and 3 thumbnail concepts. Park them in `<name>/docs/<name>-packaging.md`.

## 2. Scaffold the project

```
vid new <name> [--portrait]     # creates ../slopstudio-projects/<name>/ with:
                                #   <name>.skeleton.json  (template beats)
                                #   docs/research-<name>.md   docs/<name>-packaging.md
```
`--portrait` = a 1080×1920 Short (natural-rate speech, bottom-band host); omit for 16:9.
Shorts get their urgency from structure, captions, and visual cadence—not an extra format
multiplier. Target about 180 WPM (roughly 175–190); if the long-form voice preset needs
1.15× to reach that register, use the same rate for its Shorts rather than accelerating again.

## 3. Research → 4. Outline → 5. Script

- **Research** into `docs/research-<name>.md`: every on-screen claim traces to a source you
  actually read. No mechanism you can't explain in one beat → research it or cut it.
- **Outline** as **acts, each with a reveal** (5–11). The single most interesting fact gets
  its own act + its best visual. **Checkpoint here** — an outline review is cheap; a
  re-scripted video is not.
- **Script** into the skeleton's `beats` (`<name>.skeleton.json`): **one sentence per beat**
  (= one TTS clip; long gens drift). Voice = deadpan cosmic-architect (`/writing-gemma`,
  `../gemma-branding/CHARACTER.md`). Each beat sets a `visual` only when it *changes* (a
  visual holds until the next); aim for a change every 2–4 beats. `/composing-slop` and
  `/retention-editing` are your co-pilots here.

## 6. Build the cut

```
vid build <name>     # compiles the skeleton → <name>.slop.json (placement + polish = defaults)
```

## 7. Voice it (needs lame)

```
vid wake             # if not already up
vid voice <name>     # generate VO + visemes, then retime the timeline to real durations
```
Bad take (misread word) or flat catchphrase? → seed shootout / split take — see
`/gemma-voice-tts`. Re-word + regenerate one clip until the intonation lands.

## 8. Review — look at renders, not JSON

```
vid lint <name>      # gaps / repeats / stale VO / missing assets / overlaps
vid look <name>      # renders frames across the cut → montage → llm-feed
```
Scan the montage for: host covering content, illegible text, dead stretches, the same
visual too many times in a row. Fix in the **skeleton** and `vid build` again *while no
hand-edits exist*; once you hand-tune, edit the cut with `tools/slop.py set/mv/trim` instead
(recompiling would clobber). Gate with **`/retention-editing`** then **`/taste-review`**.

## 9. Thumbnail

Build 3–5 distinct archetypes with the thumbnail tool, not recolors:
```
python tools/thumb.py new <name>/thumbs/a.thumb.json --brand ../gemma-branding/brand-package
python tools/thumb.py render <name>/thumbs/a.thumb.json --proof   # → llm-feed
python tools/thumb.py lint   <name>/thumbs/a.thumb.json --title "the paired title"
```
`/frame-critic` ranks finalists at full size AND 168px. Top 2–3 → YouTube Test & Compare.

## 10. Export

```
vid export <name> --final     # 1080p60 mp4 in exports/ (+ a .credits.txt for CC-BY music)
```
(Plain `vid export <name>` = project-native fps/res for a fast check.)

## 11. Package + publish

- Finish `docs/<name>-packaging.md`: lock the title, paste the description (chapters from the
  act cards, CC-BY credits from the export's `.credits.txt`, links). See
  `luckymas/docs/001-packaging.md` for the reference shape.
- **Accurate captions** (better than YouTube's auto-generated): `vid transcript <name>` copies
  the exact spoken transcript to the clipboard → YouTube Studio → Subtitles → *paste + auto-sync*.
  For exact timing instead, `vid transcript <name> --srt` (or upload the written `.srt`).
- Localize extra languages if wanted: `python tools/localize-project.py <name>.slop.json`.
- Upload video + thumbnail; queue runners-up in Test & Compare; set an end screen → next
  lecture. Socials: `python tools/social.py status` and `../gemma-branding/SOCIAL.md`.

---

## The control panel (web dashboard)

For a browser view of the whole operation instead of the CLI:

```
vid dashboard                 # → http://localhost:8080  (auto-opens; Ctrl-C to stop)
```

Three tabs: **Social** (the post queue grouped by ready/upcoming/trigger, per-platform cadence
with DUE flags + queue depth, the "post today" picks, and a gallery of the GPT gens showing which
post each is paired to — mark-posted / pair-an-image / copy-a-body without opening a file);
**Launcher** (one-click the actions below with their options, plus open the editor or the Gemma
thumbnail editor on a project); **Projects** (the per-project pipeline checklist). It just drives
`tools/video.py` + `tools/social.py` and streams their output in a live log — no new state. Social
content is read from `../gemma-branding/social/`; gens from the owner's social gen folder.

## Where am I?

```
vid status            # list all projects
vid status <name>     # pipeline checklist: skeleton · cut · thumbnails · packaging · export + lint
vid show <name>       # compact timeline (durations, act cards, clip counts)
```

## Cheat sheet

| want to… | command |
|---|---|
| check the machine | `vid doctor` · `vid wake` |
| start a video | `vid new <name>` |
| compile skeleton → cut | `vid build <name>` |
| generate voice | `vid voice <name>` |
| find problems | `vid lint <name>` |
| eyeball the cut | `vid look <name>` |
| render mp4 | `vid export <name> --final` |
| captions → clipboard | `vid transcript <name> [--srt]` |
| where is it | `vid status <name>` |
| social + launcher dashboard | `vid dashboard` |
