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
`--portrait` = a 1080×1920 Short (1.3× speech, bottom-band host); omit for 16:9.

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
- Localize extra languages if wanted: `python tools/localize-project.py <name>.slop.json`.
- Upload video + thumbnail; queue runners-up in Test & Compare; set an end screen → next
  lecture. Socials: `python tools/social.py status` and `../gemma-branding/SOCIAL.md`.

---

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
| where is it | `vid status <name>` |
