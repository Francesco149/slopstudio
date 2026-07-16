# LLM VIDEO WORKFLOW — compose a watchable cut out of the box

> **Building by hand? Read `docs/VIDEO_RUNBOOK.md`** — the human runbook + the `tools/video.py`
> front-door (`doctor`/`new`/`build`/`voice`/`lint`/`look`/`export`/`status`). This file is the
> agent-facing detail underneath it.

How an agent (or a human in a hurry) goes from topic → watchable video using the
distilled primitives. The design goal: **the LLM decides sequence + content; the
editor's defaults decide placement + polish.** Read with `docs/UX_NEXT.md` (product
direction) and `../gemma-branding/CHARACTER.md` (the host's voice — read BEFORE writing lines).

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
   (../gemma-branding/CHARACTER.md; tone ref = the fufu-lab intro (../slopstudio-projects/demos/)). Continuity read: play
   the whole script top-to-bottom and check every "this/that/it" still points at what's
   on screen. Emotions from the deadpan set (neutral/deadpan/smug/explaining/teaching/
   pointing/thinking/confused/annoyed) — no theatrics.
   The opener is a locked exception: write exactly `Heh~ Welcome back, mortals.` (or set
   `"signature": true`) and the compiler copies the complete LuckyMas take from
   `presets/voice-snips/gemma-welcome-back-mortals.wav`. Never regenerate or split it.
4. **Author the skeleton** (`*.skeleton.json`, format in `tools/slop.py` — beats of
   `{line, emotion, visual}`). Visual rules:
   - A visual HOLDS until you set the next one — so only author *changes*. Aim for a
     change every 2–4 beats; `"host"` clears content for solo/reaction beats.
   - Never reuse an asset more than ~3 times (lint flags it). Retrieve/capture more
     material instead — real screenshots/footage beat generated filler for software topics.
     Every asset needs a one-line **claim match** in the research/shot ledger: the exact
     noun, action, number, or UI state visible in frame that proves the narration. A merely
     topical image is filler and does not count as coverage.
   - A side inset is a two-subject composition. Use it only with `"host":true` on that
     same beat. Without a host, screenshots compile centered so an empty presenter-shaped
     hole cannot appear before the avatar arrives.
   - Code beats default to Kirby's animated VS Code scene (One Dark syntax, line reveal,
     and mouse cursor); `style:"native"` is the explicit legacy escape hatch. Diagrams for
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
- **Code beats = the Kirby VS Code card** (One Dark, line reveal, mouse carry-in; the
  strongest primitive), normally solo so the code owns the frame;
  **jp_lesson** big-centered for JP readings; diagrams only when a flow genuinely needs
  boxes-and-arrows (weakest primitive).
- **Real captures over generated filler**: XP harness footage, CRT phone footage,
  archived pages via Playwright — every claim's visual is sourced and its claim match is
  recorded. Search-result pages and uncropped “something about the topic” screenshots fail.
  For a webpage that will be zoomed, use `nix develop .#webcapture --command python
  tools/capture-web.py URL assets/web/source/name.png --dismiss "Reject All"`. The default
  is a 1920x1080 CSS viewport at DPR 2 (3840x2160 output) and writes a URL/time/hash
  provenance sidecar. Keep the whole high-DPI source and animate editorial crops with
  `widgets.document`; do not bake a low-resolution crop into the only source image.
  For interactive source inspection, this repo also declares the pinned Playwright MCP
  server in `.codex/config.toml`; after restarting the Codex/ChatGPT host, use its browser
  tools to navigate, inspect text and choose excerpts. Use `capture-web.py` for the final
  deterministic bitmap and provenance sidecar.
- **Pre-outro "minimal recreation" section**: rebuild the topic's core trick in a few
  code cards + a github CTA plate, then the **outro on the desk bg** — thanks, a "send
  me strange software" ask, and the fufu~ sign-off.
- **One sentence per VO clip**, hard cuts between host beats (no slide bounce; overlap
  on r_av = a double-sprite flash — lint warns). Emotions from the deadpan set.
- Humor: penguinz0-style deadpan + absurd analogies, played straight, chuuni flourish
  sparingly ([[video-script-moistcritical]]; tone ref = the fufu-lab intro (../slopstudio-projects/demos/)).

## Shorts (portrait) — cut a 1–2 min short FROM a finished video

A short is a **separate project** in the projects repo, never an edit of the full cut.
Put it **beside the source cut in the SAME project dir** (`$P/<name>-shortN.slop.json`)
so the project-relative `assets/…` uris keep resolving. Same skeleton→genvo→lint loop
with the portrait profile on top:

- **Skeleton header:** `"format": "portrait"` → 1080x1920 canvas, built-in transition
  SFX ON (`meta.sfx`), speech **1.0x by default** (`meta.speech_rate` — the editor/export/retime
  all inherit it; per-clip `params.rate` overrides), tighter beat gaps + shorter act
  cards, geometry squeeze-mapped to the tall frame. Target about 180 measured WPM
  (roughly 175–190). A Short derived from a long-form cut uses the same speech rate as
  that cut; portrait mode never adds a second speed-up.
- **Reuse the video's material:** `slop.py adopt --src $P/<video>.slop.json` pulls the
  tuned VO takes + viseme tracks by text match — keep lines VERBATIM where you can
  (free reuse), REWRITE terser where the long-form pacing drags (genvo fills those).
  Footage/screenshots reference the same project-relative assets.
- **Pace:** one hook beat in the first ~2s (the reveal, not the setup), a visual change
  every 1–2 beats, no acts/recaps — a short is ONE reveal from the video + the CTA.
- **Animated transcript is automatic** (portrait only): compile/retime generate big
  pop-in caption chunks on `r_transcript`, loosely word-timed from each take's viseme
  silences. A line written weird for the TTS ("Heh~", phonetic hacks) gets a
  `"transcript": "..."` beat key = what the viewer READS (`slop.py transcript` re-runs
  it by hand; `params.transcript` on the tts clip is the project-level field). The
  on-screen chunks drop a **trailing full-stop period** (a period reads unnatural at the
  end of a big caption); `? ! … ,` and mid-text punctuation stay (tone/context for a
  muted viewer). Sentence boundaries still use the original punctuation for chunking.
  **Timing:** speech/pause segments come from the take's own WAV (RMS gate; the viseme
  track is only a fallback — Rhubarb hides soft pauses as viseme B), chunk weights use the
  SPOKEN text (`_tts_norm`: "2007" weighs as "two thousand seven"), and sentence boundaries
  snap to real pauses via a global order-preserving match (comma pauses rejected). The
  command prints `timing: N wav / N viseme / N linear` — a non-wav count means asset uris
  didn't resolve (run against the real project dir). The last chunk always **holds until
  the audio end** (never vanishes during a trailing word/pause). **Position:** each chunk
  rides a project ANCHOR (`params.anchor` → `meta.anchors`): `tr_room` (solo room, top) /
  `tr_scene` (room with a beat backdrop, lower) / `tr_content` / `tr_code` — one Project-panel
  knob per band, per project.
- **Portrait code cards:** wrap the code NARROW (~30–40 cols — there's vertical room,
  and width costs font size); put comments on their OWN LINE ABOVE the call, not
  trailing. Keep card titles short (`file.c · Function`) — long titles clip.
- **Portrait beats stack VERTICALLY (automatic):** content (image/video/crop) lifts into
  the **TOP band** (`fit`/`inset`), the transcript tucks just under it, and the **host is
  BIG at the bottom** (feet off-frame) — uses the vertical space instead of a small
  dead-center shot. Code/diagram card → host tucked lower-right; NOTHING else on screen
  (room shots) → she renders solo-BIGGEST (avatar_fit `solo`), like the full video.
- **`backdrop`** (beat key) = a fullscreen COVER image behind the beat (not the room) so
  the host reads big over it instead of a black fill — e.g. `bg-desk` under a caption-only
  moment.
- **`place:"bottom"`** on an image/video sub-visual = a bottom **reaction OVERLAY** (a meme)
  drawn OVER the host's lower body, `width_frac` wide (~0.8), face uncovered; `params.overlay`
  keeps it out of the content check so the host stays solo-big (pair with a `backdrop`).
- **Do not touch the full-length cut** — shorts defaults live behind `meta.format`;
  a landscape project compiles bit-identically.

## Music (the owner-assigned palette — pick by role)

`library/music/catalogue.json` `core:true` entries carry authoritative `role`s:
**space-jazz** = the main bgm default; **edm-detection-mode** = the intense alternate;
**chill-wave** = story-opening/title-drop; **voxel-revolution** = speech-free montages
ONLY; **deadly-roulette** = gag scenes / a cold-open bed; **bossa-antigua** = beach/
relaxing gag intros; **pixelland** = credits / "to be continued" (even as a joke). Bed
gain default is **-18 dB**. The mixer does NOT loop audio — `slop.py bed` lays repeated
clips (continuous `in`) for long spans; the skeleton `music` key is fine for a ≤3 min short.

**Volume automation — `slop.py bed --ramp "t:dB,t:dB,…"`** (absolute project time : dB
breakpoints, linearly interpolated → keyframed `gain_db`, honoured in preview + export).
This is the "arrange the music" tool — reuse a song at several points (bed ids are
per-(file,start), no collision), fade each to silence (`…:-50`), and start the next
**on a voice line** with a short silence gap between, matched to the video's pacing:
- **deadly-roulette = Gemma's THEME** — recurs on her host-forward / scheming / smug acts.
- **chill-wave = the SWELL song** — the intro montage + reused at resolution/triumph/epic
  beats, each entering quiet and swelling to a peak (`--ramp "t0:-28,…,climax:-7,…,-18"`).
- **edm-detection-mode = the intense alternate**; **SILENCE** over a somber beat (the
  apology) for impact, the music returning on the redemption/rebuild line.
luckymas3 arranges exactly this across its 8 acts — read its music clips for the reference arc.

**Song credits (automatic).** Every music asset should carry `meta.title` + `meta.artist` +
`meta.attribution.attribution_text`. A song dragged into the editor auto-fills all three from its
**ID3 tags**; for a cut assembled by tools, run **`slop.py musicmeta <proj> [--sidecar]`** to backfill
title/artist/credit on every music asset from its tags (fills gaps only — a hand-tuned credit is kept;
`--sidecar` also drops a `<file>.meta.json` next to each song). That metadata then drives, with zero
extra authoring: (1) the on-screen **"now playing" chip** (`♪ Title — Artist`, ~10s at each song start,
dodges title plates — `meta.song_credits`/`song_credit_corner`), and (2) the **export description
credits** (`export.sh` writes a paste-ready `<video>.credits.txt`; only songs actually placed are
listed, deduped). Kevin MacLeod tags → the canonical incompetech CC BY 4.0 line automatically.

**Speechless insert montage** (a rhythmic screenshot b-roll on a music swell — "everyone
will know your power level" → flash the payoff shots): it's a **brand-new insert that
RIPPLES the rest of the video**, not an overlay. `slop.py ripple --at <t> --by <dur>` opens
the gap, then drop N `layout:"cover"` fullscreen flashes (cover so they never letterbox;
`transition:false` + a small scale punch = hard rhythmic cuts; a wide-ish screenshot set)
filling the gap edge-to-edge (butt the last flash against the next VO so there's no black),
and re-lay the beds so the swell peaks across it. A `section` blur-cut that straddled the
insert point must be **moved** to the new scene boundary (it doesn't ripple with its title).

**Gag cues:** `{"sound":"boom","sound_at":s}` — `sound_at` must land on the PUNCHLINE
phrase, not the sentence before it (read the take's viseme silences: speech resumes at
the last `X` segment's end / 1.3 for the timeline offset). **Use stings SPARINGLY
(owner feedback 2026-07-08):** an awkward sting or vine boom only where a real
punchline/comedic pause actually LANDS — a written joke with a beat after it — never
as ambient seasoning on lines that merely sound wry. Same bar for `gag:"clueless"`
(it auto-cues the awkward sting). Default for a whole video: a handful, not one per
scene; when in doubt, no sting. TTS word gotchas: never
"fufu" (write "Heh~") and never "cursed" (it stumbles; say "weird") — ../gemma-branding/CHARACTER.md.

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
