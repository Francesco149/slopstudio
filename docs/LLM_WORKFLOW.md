# LLM VIDEO WORKFLOW — compose a watchable cut out of the box

How an agent (or a human in a hurry) goes from topic → watchable video using the
distilled primitives. The design goal: **the LLM decides sequence + content; the
editor's defaults decide placement + polish.** Read with `docs/UX_NEXT.md` (product
direction) and `docs/CHARACTER.md` (the host's voice — read BEFORE writing lines).

**THE FORMAT IS LOCKED (user-approved 2026-07-02).** The reference cut is
`../slopstudio-projects/luckymas/luckymas3.slop.json` — scrub it before composing
anything new; match its shape rather than inventing one. Videos live in that projects
repo (one portable dir per video, its own git — commit project changes there), tools
run from this repo root with relative paths.

## The pipeline

1. **Research first, script never-first.** Collect sources into `docs/research/<topic>.md`
   with a claim→evidence table. Every on-screen claim must trace to a source you actually
   read (the sygnas credits beat exists because the archived page was re-fetched and read).
   No claim without evidence; no "somehow/magically/it just works" handwaving — if you
   can't explain the mechanism in one beat, research until you can or cut the claim.
2. **Outline = acts with a reveal each.** 5–11 acts; each act states (a) the question it
   answers, (b) the reveal/punchline, (c) which VISUAL carries it (a capture, a code beat,
   a diagram — decide the visual *while outlining*, not after). Emphasis check: the
   video's single most interesting fact must own its own act and its best visual — don't
   bury the lede under setup. **Checkpoint with the user here** (an outline review is
   cheap; a re-scripted video is not).
3. **Script into beats.** One sentence per beat (one TTS clip each — long gens drift).
   Style: deadpan + absurd analogies, played straight, chuuni flourish sparingly
   (docs/CHARACTER.md; tone ref = the committed fufu-lab intro). Continuity read: play
   the whole script top-to-bottom and check every "this/that/it" still points at what's
   on screen. Emotions from the deadpan set (neutral/deadpan/smug/explaining/teaching/
   pointing/thinking/confused/annoyed) — no theatrics.
4. **Author the skeleton** (`*.skeleton.json`, format in `tools/slop.py` — beats of
   `{line, emotion, visual}`). Visual rules:
   - A visual HOLDS until you set the next one — so only author *changes*. Aim for a
     change every 2–4 beats; `"host"` clears content for solo/reaction beats.
   - Never reuse an asset more than ~3 times (lint flags it). Retrieve/capture more
     material instead — real screenshots/footage beat generated filler for software topics.
   - Code beats = the typewriter (the strongest primitive — lean on it); diagrams for
     architecture; a caption for a punchline; `layout: fullscreen` for hero footage.
5. **Compile + generate + lint** (all inside `nix develop`; `P=../slopstudio-projects/<name>`):
   ```
   python tools/slop.py skeleton $P/<name>.skeleton.json --out $P/<name>.slop.json
   python tools/slop.py genvo $P/<name>.slop.json     # VO + visemes + retime (lame must be up)
   python tools/slop.py lint  $P/<name>.slop.json     # gaps/repeats/stale-VO/overlaps
   ```
6. **Review renders, not JSON.** `--shot-frame` beats at several timestamps → montage →
   llm-feed. Look for: host covering content, illegible text, dead stretches, same-visual
   fatigue. Fix in the skeleton and recompile while no hand-edits exist; after hand-tuning
   starts, edit the project via `tools/slop.py` (set/mv/trim) — recompiling would clobber.
7. **Iterate lines per clip.** Re-word + `--generate` a line until the intonation lands
   (see [[gemma-voice-workflow]]); `lint` catches clips whose text changed after gen.
   - **A bad take (misread word) = a seed shootout:** set `params.seed=<n>` on the VO
     clip and `--generate` a few takes (the provider caches per seed); score them by
     WhisperX word timing (align provider) and pick the cleanest — the flagged word's
     duration is the misread signal. Leave the losing seeds cached for auditioning.
   - **A mid-sentence comedic pause = a SPLIT take, never a re-word:** split the VO clip
     (part 1 `dur=t`, part 2 `params.in=t` starting later). retime/lint understand splits
     (params.in / shared asset) and leave them alone — never resize one back by hand.
8. **Ship + localize.** `python tools/localize-project.py $P/<name>.slop.json` pulls
   cache:// gen output into `$P/assets/<name>/` with project-relative uris (the portable
   project dir), then `tools/export.sh $P/<name>.slop.json --scale 1080 --fps 60
   --target-mb 300`. Commit the project in the PROJECTS repo as you go.

## The locked format (what the approved cut does — match it)

- **Acts with title cards** (`cutNN_title` captions, `ACT n · NAME`) over a blur-swap;
  5–11 acts, each owning one reveal and its best visual.
- **Term plates** (`style:term`, `place:auto`) name every artifact/date/number as it
  comes up — headline + a dry sub-line joke (`"22 character packs" / "23 girls — the
  twins share one"`).
- **The clueless gag:** a `term` caption + a yellow arrow (`shape:arrow` + `grow`
  sweep) pointed at the host's forehead when she says something self-owning. Sparingly
  — twice per video.
- **Code beats = the typewriter card** (the strongest primitive), host in `teaching`;
  **jp_lesson** big-centered for JP readings; diagrams only when a flow genuinely needs
  boxes-and-arrows (weakest primitive).
- **Real captures over generated filler**: XP harness footage, CRT phone footage,
  archived pages via Playwright — every claim's visual is sourced.
- **Pre-outro "minimal recreation" section**: rebuild the topic's core trick in a few
  code cards + a github CTA plate, then the **outro on the desk bg** — thanks, a "send
  me strange software" ask, and the fufu~ sign-off.
- **One sentence per VO clip**, hard cuts between host beats (no slide bounce; overlap
  on r_av = a double-sprite flash — lint warns). Emotions from the deadpan set.
- Humor: penguinz0-style deadpan + absurd analogies, played straight, chuuni flourish
  sparingly ([[video-script-moistcritical]]; tone ref = the committed fufu-lab intro).

## What the defaults already do (don't fight them)

- Media placement: `layout` inset/fullscreen/fit adapts to any source size; insets get
  border+shadow+glow+filler backdrop automatically; the host auto-sits opposite content
  and turns toward it (¾ pose) with bust framing per line.
- Per-line host clips hard-cut between beats (no slide-out bounce); emotion picks the
  sprite (exact rig key first, canonical fallback), missing keys fall back quietly.
- Ken Burns on media, gated-RMS loudness on VO, scene vignette, auto color-grade.
- Section markers become blur-swap scene cuts.

If a default produces a bad frame, prefer fixing the DEFAULT (editor/main.cpp) over
hand-tuning one clip — that's how the format gets stronger (this file exists because
hand-tuned beats kept getting distilled back into code).
