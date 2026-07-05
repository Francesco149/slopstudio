# Making the Agent a Better Editor: Research → Build Plan for slopstudio

**Prepared:** 2026-07-03 · **Audience:** slopstudio engineering (the LLM-driven AI video NLE behind @GemmaExplains)

> **Framing.** In slopstudio a Claude *agent* composes videos by driving `tools/slop.py` + the `.slop.json` project format, with a human only at the end of the loop. So "make me a better video editor" literally means **"make the LLM agent make better editing, scripting, and design decisions — and give it the tools, representations, and feedback signals to do so."** This report is filtered through that lens: every finding lands as either (a) a heuristic the agent can *apply*, or (b) a concrete thing to *build* into the repo (a skill, a `slop.py` subcommand, or an eval loop).

> **The one architectural bet.** The research converges hard on a single insight: slopstudio's "agent edits a structured JSON project via a CLI" model is *exactly* the pattern Anthropic recommends for Skills (low-freedom scripts + plan-validate-execute + a structured intermediate file) **and** exactly the pattern the best research video-editing agents use (a small verb action-space over a compact timeline *digest*, with an Editor/Critic loop). The highest-leverage builds are therefore: **(1)** a compact timeline **digest** the agent reasons over instead of raw JSON; **(2)** a set of **Skills** that encode taste as rubrics + few-shot examples; and **(3)** **external-anchor eval loops** (deterministic checks + a VLM frame-critic + best-of-N with a judge), because the literature is unambiguous that LLMs *cannot* reliably self-improve creative output without an external signal.

---

## Executive summary — the 15 highest-leverage moves

1. **Wrap `slop.py` in a first-class Claude Skill (`composing-slop`)** using `allowed-tools: Bash(python tools/slop.py *)` to kill permission prompts and a dynamic ``!`python tools/slop.py overview <project>` `` line so every session starts grounded in the live cut. Skill frontmatter fields verified real in current Claude Code docs. [1][2]
2. **Encode taste as skills, not vibes.** Mirror Anthropic's two in-house patterns: `canvas-design` (taste-as-prose *philosophy*) for Gemma's art direction, and `brand-guidelines` (design-system-as-*data*: exact hex/type/spacing) for a `gemma-brand` skill. Add an evidence-driven review skill (VOIDXAI/`taste` pattern) that only scores dimensions it has evidence for. [3][4][5]
3. **Build `slop.py digest` — a compact timeline view for the LLM.** An ordered per-clip table `{id, row, in–out (play+source), dur, one-line content, framing, transcript excerpt}`. Every strong research agent (EditDuet, LAVE, L-Storyboard) feeds the LLM *this*, never raw project JSON. Single highest-ROI build. [6][7][8]
4. **Make transcript-with-word-timestamps the primary edit address.** It's the surface Descript/Gling/Opus and the research systems all anchor to; you already have WhisperX word timing — expose it. "Delete this sentence" = "cut this time range." [6][9][10]
5. **Split every quality signal into deterministic-checks vs LLM/VLM-judgment.** Contrast ratio, safe-area geometry, font px, chars/line, caption dwell, 8pt-grid snapping, dead-air gaps, hook-timing — all computable, 100% reliable; reserve the fuzzy 20% (balance, focal clarity, "does the joke land") for a judge. Do **not** ask a VLM for anything you can compute. [11][12]
6. **Extend `lint` (structure-only today) with a `critique` sibling** that checks *retention rules*: hook paid off ≤15s (long)/≤2.5s (short), no static stretch >~8–15s without a visual change, an open loop always live, payoff in the last 15–25%, VVSA-style shorts pacing. All grounded in YouTube-official + primary-source retention data. [13][14][15]
7. **Author a `writing-gemma` script skill** = voice sheet (DO/DON'T registers) + **8–12 gold few-shot lines** (examples beat adjective lists) + the August/Mazin 5-question self-check. Run **voice and fact as separate passes** — persona prompting degrades factual accuracy. [16][17][18]
8. **Adopt the misconception-first teaching spine (Veritasium) as the default for deep-dives** — its retention edge is the most empirically-grounded scripting lever here (rooted in Muller's PhD): state the wrong belief → show why → give the correct model. [19][20]
9. **Make comedic timing mechanical.** Encode the ABT causality pass ("and-then" = dead; must be "but"/"therefore"), rule-of-three with punch-word-last + a "(BEAT)" → your existing awkward-sting SFX, and **bathos** (cosmic register → mundane deflation) as Gemma's signature rhythm. [21][22][23]
10. **Expose single-frame render to the agent** (`--shot-frame T` → PNG) and auto-push to the llm-feed. Today the only way to "see" a frame is a full render; a frame-critic loop is impossible without this. It's the unlock for all visual self-correction. [codebase]
11. **Build a `frame-critic` on the render→critique→fix loop**, but respect the ceiling: VLMs hit only ~72% on binary aesthetic judgments and ~0.20 IoU localization zero-shot, with reasoning models giving *no* advantage. So: feed it your explicit numeric rubric, prefer **pairwise A/B of rendered variants** over absolute 1–10 scores, and use few-shot annotated exemplars (+55% critique quality). [11][24][25]
12. **Best-of-N + judge is the highest-ROI, lowest-risk loop.** Generate N script openings / N seed frames / N TTS takes → judge → keep best. You already do seed shootouts; generalize it. Cap N modest (4–16) and keep an absolute-rubric gate — judges are reward-hackable (a 5-token suffix can max the score). [26][27][28]
13. **Use the committed golden cut (`luckymas3.slop.json`) as the reference in every judge prompt.** Reference-guided judging (Prometheus/G-Eval/MT-Bench) is far more reliable than open-ended "is this good?" — and you have a locked gold standard. [29][30]
14. **Keep a scored eval-set of past accepted/rejected cuts** as a "regression test for taste," and periodically re-align any judge to your real labels (Hamel's loop). This is the moat; without it, judges silently drift. [18][31]
15. **Never build a "will it go viral" predictor** (independent replications plateau ~60%, barely above chance). Build **rule-conformance** proxies grounded in real retention mechanics instead — that's the defensible, checkable signal. [13][32]

---

## The central architecture: how the agent should think about an edit

Three research systems independently arrived at the same design; it should be slopstudio's spine.

- **Small, closed, verb-based action space over the timeline.** EditDuet (Sep 2025) drives a non-linear timeline through exactly **6 functions** (`search_collection`, `add_to_timeline`, `remove_from_timeline`, `switch_clip_positions`, `move_clip`, `DONE`); LAVE (ACM IUI 2024) exposes **5 LLM functions**, two emitting strict JSON (`{"storyboard":…,"video_ids":[…]}`, `{"segment":["start","end","rationale"]}`). Small verb sets are easier to validate, undo, and audit than free-form JSON authoring. slopstudio already has this shape (`add/insert/mv/trim/rm/ripple/rebase`); the gap is a **read** verb (`digest`) and a **search** verb. [6][7]
- **Represent the timeline back to the LLM as a compact digest, not raw JSON.** EditDuet hands the LLM an ordered list of `{file, duration, content_description, shot_type, camera_motion}`; L-Storyboard uses a **Markdown table** (ID, shot size, angle, movement, content, subtitles) and emits ordering as `Shot 1 -> Shot 2 -> Shot 3`. LLMs reason far better over this than over a node graph. [6][8]
- **Plan → approve → execute, with a semantic Critic and an explicit RENDER gate.** LAVE emits a `GOAL`/`ACTIONS` plan the user approves *before* execution. EditDuet runs an **Editor agent (mutates) + Critic agent (natural-language critique until satisfied → RENDER)**; its automated LLM "NLE judge" agreed with humans **80.6%** (vs 78.7% human–human) — usable as an automated "does this cut satisfy the brief?" check. This maps 1:1 onto slopstudio's "human at the end of the loop." [6][7]
- **Measure twice, cut once:** do full semantic comprehension of all assets *before* emitting any timestamps; caption/summarize every clip first, then plan. Reduces localization error. [33]
- **Deterministic detectors do grunt work; the LLM does judgment.** Silence/filler/bad-take detection is threshold arithmetic — run it deterministically and hand the LLM a *candidate cut list* to approve/veto. Reserve tokens for narrative and selection decisions. [9][10]

Why this matters for Skills specifically: Anthropic's own best-practices doc describes the ideal skill for a fragile, ordered task as a **low-freedom script + "plan-validate-execute"**: Claude writes a structured intermediate file, a **script validates it before applying**, then executes and verifies, with **verbose validators** ("Field X not found. Available: …"). Your `skeleton.json → slop.py skeleton → lint → render` pipeline is already this pattern; the work is to sharpen the validators and add the missing verbs. [2]

---

## JOB 1 — Editing decisions (pacing, cut rhythm, retention, b-roll, music sync)

### Key findings (cited)

**Retention is measurable, and only a few numbers are actually official** (the rest is well-corroborated vendor folklore — treat as heuristics, not laws):

- **The one official, load-bearing metric:** YouTube Analytics' "key moments for audience retention" defines **Intro = % of viewers still watching after the first 30 seconds**, and the official graph-reading: *flat* = watched start-to-finish, *spikes* = rewatched/shared, *dips* = skipped/abandoned. Verbatim-confirmed on support.google.com. [13]
- **APV (Average Percentage Viewed) targets by runtime** are corroborated across ≥3 independent sources: <5 min → **50–70%**; 5–15 min → **40–55%**; 15–30 min → **30–45%**; >30 min → **25–35%**. For a 10–15 min deep-dive, **≥50% is A-grade**. Only ~1 in 6 videos beats 50% retention. [14][34]
- **By 2025–26 YouTube weights watch time by *satisfaction* and *session contribution*, not raw AVD** — a 6-min@80% video out-recommends a 20-min@30% one. And "quality CTR" is real: high CTR + a fast first-30s drop gets *demoted*. Implication: optimize the *percentage* and the hook, not raw length. [35][36]
- **MrBeast's leaked production handbook** (primary doc): "the first minute is the most important minute"; front-load the best content + exact title/thumbnail match in minute 0–1; "crazy progression" (show don't tell) minutes 1–3; **re-engagement beats at ~min 3 and ~min 6**; **reintroduce the premise** after each segment; **end abruptly**; integrate sponsors as content. Avg MrBeast video = 13m37s. [37][38]
- **Paddy Galloway's Shorts study** (primary data: 3.3B views / 5,400 Shorts / 33 channels): the key metric is **VVSA (Viewed-vs-Swiped-Away); best performers 70–90%, <60% → distribution collapse**. Likes/shares/comments showed **no strong correlation**. "Treat your intro like a thumbnail." Also: longer Shorts performed *better* on average — don't hard-cap at 30s. [39][40]
- **Pattern-interrupt cadence is section-dependent, not a global constant** (sources range 5–8s to 20–40s): intro tighter (visual reset every 10–20s + a deliberate interrupt at the 25–35s mark), widen once hooked, and every 2–3 min drop a "burst sequence" of 5–10 quick cuts. Demonstrative b-roll ("show don't tell") adds ~+19% watch time in tutorials (single-origin — directional). [41][42]
- **Narration pace** (corroborated across VO-industry sources): long-form **130 WPM clear / 150 conversational**; technical/educational **120–140** for processing time; Shorts **140–150**; **>160 WPM degrades comprehension**. Your shorts run ~1.3× — keep source narration at the low end so the sped result lands ~150. [43]
- **A slopstudio-specific risk signal:** one 2025 report attributes ~70% lower retention to "monotonous AI narration." Single-sourced and specifically about *flat/monotone* delivery — which your per-clip-intonation, character-performance TTS workflow is designed to defeat. Heed it as a reason to lean *hard* on visual cadence + varied intonation, not as "AI can't retain." [34]

**Editing mechanics from the tool landscape:**

- **Transcript is the universal LLM edit surface.** Descript/Underlord makes the cut when you delete a word in the doc; Gling deterministically removes silences/fillers/repeats/bad-takes (Aggressive↔Natural slider); Opus/Vizard score transcript segments for hook + virality. WhisperX gives **<100ms word-level timestamps** (native Whisper drifts hundreds of ms) and `--highlight_words` emits karaoke SRT. [9][10][44]
- **Deterministic silence-cut defaults worth copying:** auto-editor uses audio-activity threshold **0.04 (≈ −19 dB)**, **0.2s margin** padding around cuts. Filler lists: `um, uh, hmm, mm, mhm` (Whisper drops these) plus phrasal `you know, I mean, sort of, basically, like, so`. [45][46]
- **Auto-zoom keyed to vocal emphasis** (Submagic's rule): louder volume, pitch changes, and pauses → subtle punch-ins. Emphasis-driven scale keyframes are a cheap dynamism win. (Marketing-described mechanism — plausible, unconfirmed.) [47]
- **Beat-synced cutting:** `librosa.beat.beat_track` (onset-strength → tempo → peak-pick); madmom most accurate offline. Reference pipeline: detect the beat grid from the **percussive stem** → frame-accurate cut timeline → snap cuts/volume ramps to it. [48][49]
- **Opus's virality rubric** (Hook + Flow + Value + Trend, weights undisclosed) is a weak *predictor* but a useful *legibility* device: forcing per-clip named-axis scores + rationale makes selection tunable. Treat as rubric inspiration, not a model. [50]

### Heuristics the agent can apply (encodable)

**Long-form (10–15 min):**
1. Hook budget = **15s, hard cap 30s**; no branded cold-open. 0–5s striking claim/visual → 5–15s the specific promise → 15–30s stakes. Pay off the title/thumbnail *literally* here.
2. Target APV **40–55%** for 10–15 min; treat ≥50% as A-grade. Optimize percentage, not minutes.
3. Keep ≥1 **open loop** live at all times; open a new one every **45–60s**; add a **mid-video re-hook at 40–60%** of runtime.
4. **Pacing ramp by section:** min 0–3 fast (visual change ~5–15s) → min 3–7 steadier (cuts 20–40s + more b-roll) → after min 8 calm + a **burst sequence every 2–3 min**.
5. **Reintroduce the premise** after each act; place re-engagement spikes near ~3-min and ~6-min marks.
6. **Visual-change floor:** never let >~8–15s of static host pass without a cut, b-roll, zoom, on-screen text, or SFX. Prefer show-don't-tell b-roll for any explained process.
7. Narration **130–150 WPM** (120–140 for dense technical). Cut every filler, pause, repeated point, courtesy sentence.
8. **End abruptly** after the payoff — no long outro cliff.

**Shorts (portrait, 1.3× speech):**
9. Hook in **≤2.5s**, first frame high-contrast/in-focus/moving; **reverse-structure** (payoff first, then explain — the long-form slow build is fatal).
10. Cut every **2–4s**; **6–12 visual changes per 20s**; always burn in captions.
11. Author the **first 3s (hook) and last 3s (payoff) first**, then design the end to **loop** into the start.
12. Target **VVSA 70–90%**, >70% 3-second retention; if a beat can't sustain that, cut it. Length = the longest you can hold near-100% VVSA — don't reflexively cap at 30s.

### What to BUILD into slopstudio (Job 1)

| Build | What it is | Grounds on |
|---|---|---|
| **`slop.py digest`** | The LLM-facing timeline: compact per-clip table `{id, row, play in–out, source in–out, dur, 1-line content, framing tag, transcript excerpt, has-visual?}`. Feed this to the agent instead of raw JSON. | EditDuet/LAVE/L-Storyboard representation [6][7][8] |
| **Verb ops: `move`, `swap`, `search`** | Round out the closed action-space (you have add/insert/mv/trim/rm/ripple/rebase). `search` = embedding/CLIP retrieval over the asset library so "find a shot of X" is a tool call, not a context scan. | EditDuet 6-verb API; LAVE retrieval [6][7] |
| **`slop.py autocut`** (deterministic) | WhisperX word timestamps → silence cut (`--threshold -19dB --min-silence 0.5 --margin 0.2`), filler strip, repeated-take detect → emit a **candidate cut list** the agent approves (non-destructive). Ties into 1.3× shorts. | auto-editor/Gling/WhisperX [9][45][46] |
| **`slop.py critique`** (semantic sibling of `lint`) | Structural retention checks over the digest: hook paid off ≤15s/≤2.5s? any static stretch >8–15s? open loop always live? payoff in last 15–25%? shorts cut cadence 2–4s? Warns like `lint` but on *quality*, not *structure*. | YouTube-official + MrBeast/Paddy [13][37][39] |
| **`slop.py beats <audio>` + snap-to-beat** | librosa/madmom on the bed's percussive stem → beat/bar grid; a `--snap-beat` option on cut points and on the existing editable volume-slope keyframes. | librosa/BeatSync [48][49] |
| **Auto-zoom generator** | Emit subtle `transform.scale` spring-keyframes on emphasized words (volume/pitch/pause from the aligned VO). Reuse the rate-aware play→source map so it survives 1.3× speed. | Submagic auto-zoom [47] |
| **`slop.py score`** (clip selection) | Per-clip `{hook, flow, value, payoff, gag, rationale}` rubric for a shorts candidate window, so highlight-picking is legible/tunable. | Opus Hook/Flow/Value/Trend [50] |
| **Codify karaoke-caption numbers** | Into the existing `r_transcript` animator: ≤4–6 words visible, 50–100ms inter-word gap, high-contrast active color (white→yellow), highlight synced to WhisperX word onset. | karaoke best-practice [44][51] |
| **Edit-Critic pass** (2-agent) | After a proposed cut, a second LLM critiques against the brief (first-3s hook, redundancy, pacing) and returns edit deltas before render. | EditDuet Editor/Critic [6] |

*(Optional/later: an **OpenTimelineIO export adapter** — your project is already JSON with clips/tracks; a thin OTIO writer buys round-trip to Resolve/Premiere and gives the agent a standard vocabulary. [52])*

---

## JOB 2 — Script writing (hooks, structure, comedic timing, Gemma's voice)

### Key findings (cited)

**Structure & retention scripting:**
- **Idea-first / packaging-first.** Lock the **title + thumbnail (the promise)** *before* scripting; top creators spend ~30% of effort on ideation/packaging vs ~5% for small creators. "The thumbnail and title sell the click; the first thirty seconds sell the watch." The script's job is to pay off the packaged promise. [15][53]
- **Misconception-first (Veritasium) is the most empirically-grounded spine** — rooted in Derek Muller's PhD: viewers holding a wrong preconception *tune out* a straight explanation, so **state the wrong belief → show why it's wrong → give the correct model**. Ideal for tech/anime "deep dives." [19][20]
- **ABT causality (Trey Parker's "but/therefore" rule):** if the connector between two beats is "**and then**," it's inert; every beat must join to the next by "**therefore**" (consequence) or "**but**" (reversal). The single most encodable causality test. [21]
- **Open loops are grounded in real cognition,** not folklore: Loewenstein's **Information-Gap Theory** (curiosity = felt gap, fires the dopamine reward pathway) + the **Zeigarnik effect** (unfinished things stay mentally open). So "tease the next point before resolving the current" is legitimate. [54]
- **Sugarman's "slippery slide":** each sentence's only job is to make you read the next; end sentences/segments on a small hook. Pair with ruthless concision (one idea per line; delete *basically/actually/just/really/very/literally*; every sentence must hook, teach, or land a joke — else cut). [55]
- **Humor measurably aids learning/retention — but only when topic-relevant.** Off-topic or overly-complex jokes *reduce* comprehension. So Gemma's jokes must be built *out of* the subject matter, never bolted on. [56][57]

**Comedic timing (mechanical, applicable):**
- **Rule of three:** two items establish a pattern, the third breaks it; the twist goes **last**. (grand, grand, mundane) [22]
- **Punch word last + a beat before it.** Rewrite so the funniest/most-surprising word ends the line; mark "(BEAT)" → realize as a hard cut, held frame, or the existing **awkward-sting SFX**. Never bury the punch. [58]
- **Bathos / undercutting is Gemma's primary engine:** build a clause in cosmic register, then deflate to the mundane. *"Behold the artifact by which your civilization schedules its doom — [BEAT] — you call it 'a cron job.'"* [23]
- **Specificity is funny:** exact, odd, hard-consonant nouns/numbers beat vague ones ("4,000 unread emails and one (1) functioning brain cell"). [59]
- **Heighten within logic** ("if this is true, what else is true?") — escalate the *same* absurd premise; don't pile on unrelated randomness. [60]
- **Callbacks / running gags:** plant early, space out, return escalated; the biggest laugh is when the established pattern *breaks*. Her "fufu~ ♥" and any recurring misread human-custom are ready-made anchors. [61]
- **Don't explain the joke** — end the beat, let the flat non-reaction do the work. [16]

**Deadpan / character voice at scale:**
- **Deadpan = cognitive dissonance:** absurd content + neutral delivery; treat the absurd as normal, stay literal, never telegraph. MoistCr1TiKaL's engine is the *contrast* (low-energy voice vs high-energy chaos) and unwavering **consistency across topics** — that consistency is *why viewers trust him*. [16][62]
- **Mock-epic / mock-heroic:** epic language on a trivial subject = the absurd contrast — directly maps to a "cosmic architect" narrating a `for` loop. **Chuunibyou is funny *because the character commits 100%* and can be played completely straight.** Gemma is both the committed weirdo *and* the deadpan outsider — both roles run on contrast. [63][64]
- **Voice at scale = a bible + audits:** TV rooms keep a character-bible speech section, assign a "character champion," and audit by **extracting all of one character's lines into one file** to check drift. The **August/Mazin five voice tests** are an excellent LLM self-check: (1) could another character say this line? (2) is she speaking for herself or the writer? (3) her feeling *now* or just plot? (4) **what does a *joke* sound like from her?** (5) can you picture a specific actor? [65][66]
- **LLM persona encoding:** **few-shot example lines beat adjective lists**; state signature phrases explicitly — **but** persona prompting helps subjective/tone tasks while *degrading* objective/factual ones ("tone steers easier than truth"). **Run voice and facts as separate passes:** research + fact-check first (Kurzgesagt-style), then apply the Gemma voice layer over locked content. [17][18][67]

### Heuristics the agent can apply (templates)

**Long-form spine:** cold-open hook (0–10s, a curiosity gap *or* a misconception stated as a bold in-character claim) → **signature intro beat** (the "ふふふ～ / welcome back, mortals" snap-in — keep it, it's a running-gag anchor) → tight context bridge → **3–5 escalating segments** (each setup→development→mini-payoff, joined by an open loop) with a **misconception-first** teaching spine → payoff that explicitly answers the cold-open → outro gag (bathos/callback).

**Shorts spine:** cold hook ≤2s → **one** idea → **one** payoff → boom-on-punchline. No context bridge; the hook *is* the premise. Front-load the single best line.

**Passes to run on every draft:**
- **ABT pass:** scan connectors; replace every "and then" with "but"/"therefore" or cut the beat.
- **Slippery-slide pass:** every sentence ends on a small hook; one idea per line; delete filler; each sentence must hook/teach/joke.
- **Humor-budget pass:** every joke rides *on* the concept being taught.
- **Voice self-check:** run August/Mazin Q1 + Q4 on each beat — if a generic narrator could say it, rewrite; every few beats must "sound like a joke from *her*."

### What to BUILD into slopstudio (Job 2)

| Build | What it is | Grounds on |
|---|---|---|
| **`writing-gemma` skill** | Voice sheet (DO: deadpan-flat, mock-epic, literal-about-customs, specific numbers, bathos endings; DON'T: winking, "lol/haha," modern slang, exclamation-hype, explaining jokes, "as an AI") + **8–12 gold few-shot lines** (hook/teach/joke/deflate/outro) + the August/Mazin self-check. Extends `../gemma-branding/CHARACTER.md`. | canvas-design philosophy-as-prose; few-shot > adjectives [3][17][66] |
| **Two-pass pipeline (fact → voice)** | Formalize a **research/fact pass** (claim→evidence table, already in `LLM_WORKFLOW.md`) that produces locked content, *then* a **voice pass** that styles it. Never mix — persona prompting degrades facts. | [18][67] |
| **`slop.py scriptlint`** | Deterministic script checks: ABT connector scan (flag "and then"), filler-word scan, sentence-length/one-idea heuristics, hook present in first N words, open-loop cadence, WPM estimate vs 130–150 target. Structural, cheap, pre-TTS. | ABT/slippery-slide/WPM [21][43][55] |
| **`packaging-first` workflow step** | Require a `title` + `thumbnail-concept` (the promise) as an input to `skeleton`, and have `critique` verify the first 30s pays it off. | Paddy/MrBeast idea-first [15][37] |
| **Best-of-N script openings + pairwise judge** | Generate N cold-opens / N hooks, rank pairwise against the rubric + the golden cut, keep best. Generalizes your seed-shootout habit to text. | best-of-N + pairwise [26][29] |
| **Line-consistency audit tool** | A command that extracts *all* Gemma VO lines across the projects repo into one file for a drift read (the writers'-room method), optionally judged against the voice sheet. | character-champion audit [65] |

---

## JOB 3 — Aesthetic / visual design (composition, color, kinetic captions, motion)

### Key findings (cited)

**The big architectural finding: split deterministic checks from VLM judgment.** Almost every "good design" rule is *computable* and should be enforced in code (100% reliable); the VLM is only for the fuzzy residue. [11][12]

**Deterministic, encodable rules (with numbers):**
- **Safe areas** (enforceable geometry): broadcast title-safe = inner 90%, action-safe = inner ~93% (SMPTE). Portrait Shorts: keep all text inside a **900×1400 centered box** in the 1080×1920 frame, captions at **60–70% frame height** (lower-middle, *not* the absolute bottom), clear of the right ~164px button column and bottom 20–25% platform UI. [12][68]
- **Legibility floor: WCAG 4.5:1 contrast** for normal text (3:1 for ≥24px or ≥18.66px bold). Over footage use a plate (60–80% opacity) or stroke+shadow, **not a dim scrim** (matches your existing "backdrop blur, not scrim" rule). ~8% of men are red/green colorblind → avoid those pairings. [69][70]
- **Typography:** body line length **45–75 chars (66 ideal)**; on narrow Shorts panels **wrap hard** rather than shrink; body ≥16–18px, line-height 1.4–1.65; hierarchy by a type scale (≥1.25×) using size *and* weight/color; **max 2 typefaces** (one display for captions, one mono for code). [71][72]
- **Color & layout "looks designed" mechanics:** **60-30-10** (dominant/secondary/accent, accent used *sparingly*); **8pt spacing grid** (every margin/padding a multiple of 8); **CRAP** (Contrast/Repetition/Alignment/Proximity); proximity rule (space *around* a group > space *within*). These are *conventions that work because everyone uses them* (consistency), not proven optima — encode as defaults. [73][74]
- **Composition:** host eyes on the **upper-third line**, small headroom; **lead room** in the gaze/point direction (put the code/diagram panel in that lead space); **one focal point per frame** (don't let host + code + caption compete at equal weight). [75]
- **Motion (Material/NN-g):** UI-motion sweet spot **200–500ms**; **ease-out** for entrances (`cubic-bezier(0,0,0.2,1)`), ease-in for exits; micro-feedback 100–200ms; >500ms drags; **stagger children 20–50ms**; a touch of anticipation before big moves. Springs = natural (you already use critically-damped springs — keep, they read as follow-through). Lower-thirds dwell **3–6s**, off the exterior 10%, never over the host's face. [76][77]

**Kinetic-caption spec (practitioner consensus — high practical value, vendor-lore rigor):**
- **Captions materially raise watch time/completion** (Verizon/Publicis: +12% avg watch time, 80% completion with captions) — *direction* robust, exact % soft. [78][79]
- **Word-by-word "karaoke" active-word highlight is the retention-winning style**, but **animate only meaningful words** (nouns/numbers) — constant per-word bouncing causes fatigue. Font: heavy geometric sans (**Montserrat Bold/ExtraBold** is the de-facto standard). Size portrait **≥48px (52–60px typical)**; long-form ≥40–60px. Each chunk on screen **≥1.2s** (reading floor ~13 chars/sec); sync highlight to WhisperX word onset. **Design sound-off first** (the punchline must read muted). [51][80]

**VLM-as-art-critic — real but bounded (verified numbers):**
- Best VLMs hit only **~72% on binary "is A more aesthetic than B" judgments** (GPT-5 72.52%, GPT-4o 70.31% on a graphic-design-aesthetics benchmark spanning typography/layout/color); **reasoning models give NO advantage**; precise localization collapses to **~0.20 IoU** (they know *that* something's off better than *where*). [11][24]
- **But the ceiling is a zero-shot/prompting wall, not absolute:** the same paper shows **fine-tuning a small VLM lifts localization +17% IoU, beating GPT-5** — relevant if a `frame-critic` ever justifies a small trained model on the 7800XT. [11]
- **Proven ways to lift VLM critique quality:** **few-shot with annotated exemplars → +55%** (UICrit); **iterative refinement + visual grounding** (emit bbox per issue → re-feed a zoomed patch) closed **50% of the human gap**; **prefer relative/pairwise A/B over absolute scores.** [25][81]
- Legacy CNN aesthetic scorers (**NIMA**, **LAION-Aesthetic v2**, ~r=0.78 to humans) are cheap gates but saturate and are trained on photos not typography — a soft prior at best. [82]

### Heuristics the agent can apply (checkable rubric)

Push everything below the line into **code** (deterministic); reserve the VLM for the top layer.

*Deterministic (compute from project + rendered frame):* caption/text contrast ≥4.5:1; all text inside title-safe 90% / portrait 900×1400; captions at 60–70% height, clear of UI zones; font ≥48px portrait / ≥40px long-form; ≤4–6 words visible; chunk dwell ≥1.2s; body line 45–75 chars; ≤2 typefaces; palette ≤2–3 hues; margins on 8pt grid; entrance 200–300ms ease-out; lower-third dwell 3–6s; one dominant focal element; host eyes on upper-third; lead room toward gaze.

*VLM-judged (fuzzy 20%):* overall balance/clutter, focal clarity ("does the host read as the subject?"), color harmony, tonal fit to the beat, "does this look designed or auto-generated?"

### What to BUILD into slopstudio (Job 3)

| Build | What it is | Grounds on |
|---|---|---|
| **Expose single-frame render** | `slop.py shot --at T` (or editor `--shot-frame T`) → PNG, auto-pushed to the llm-feed (:8777). **Prerequisite for all visual self-correction** — today only a full render exists. | codebase gap |
| **`slop.py designcheck`** (deterministic) | Compute the rubric above from the project + a rendered frame: contrast, safe-area, font px, chars/line, caption dwell, palette count, 8pt-grid, motion durations. 100% reliable; hard-fails block render. | deterministic-vs-VLM split [11][12] |
| **`frame-critic` skill/tool** (VLM) | Render N frames → VLM rubric-scorer returns structured JSON per named dimension + grounded issue list; **pairwise A/B** rank variants (seeds/layouts) rather than absolute scores; few-shot annotated exemplars (incl. golden-cut frames). Feed it the numeric rubric as its checklist. | UICrit/iterative-refinement [24][25] |
| **`gemma-brand` skill** (design-as-data) | The `brand-guidelines` analog: exact caption/plate hex, type hierarchy, 8pt spacing tokens, sprite art-direction, expression↔pngtuber-state map, avatar-framing-as-positioning (face-box, foot-shadow), bg→character integration. `paths`-gated. | brand-guidelines template [4] |
| **Codify kinetic-caption numbers** | Bake the caption spec (Montserrat-heavy, ≥48px portrait, active-word highlight of meaningful words only, ≥1.2s dwell, plate/stroke not scrim) into the `r_transcript` animator + `caption` defaults. | karaoke spec [51][80] |
| **Layout-aware host positioning** | Let `framing`/`transform` read whether content is present: host centered when solo, shifted to lead-room when a code/diagram panel is beside her (composition rule). | lead-room/focal [75] |

---

## CROSS-CUTTING — Evaluation loops (the engine that makes self-improvement real)

This is the part most likely to be skipped and most likely to determine whether the agent actually gets *better* rather than just *different*.

### Key findings (cited)

- **LLM-as-judge is trustworthy enough to gate on:** MT-Bench — GPT-4-as-judge reaches **>80% agreement with humans, the same level as human–human**. Verbatim-confirmed. [30]
- **The load-bearing caution:** Huang et al. (ICLR 2024) — **without external feedback, intrinsic self-correction often *degrades* output** (the model second-guesses correct answers). Verbatim-confirmed. **Therefore never loop "agent, is your cut good?" — always loop against an EXTERNAL anchor:** a rubric, the golden reference cut, rendered pixels, or a reward model. [27]
- **Best-of-N + judge is the highest-ROI, lowest-risk loop** (best-of-64 preferred 68% over a single sample in WebGPT). Keep N modest (4–16); gains *flatten and can overoptimize* on open-ended creative work. [26][83]
- **Judges are reward-hackable:** a **universal 5-token adversarial suffix can max the score** and transfers across judge models; RL loops hack judges within a few steps. Mitigations: cap N, keep an **absolute-rubric gate** independent of the ranking judge, periodically **re-align to real human labels**. [28]
- **Binary checklists > Likert.** Decomposing "is the pacing good? 1–5" into yes/no criteria ("hook paid off ≤15s? Y/N", "every code beat uses typewriter? Y/N") significantly raises inter-rater agreement. [31][84]
- **Pairwise for ranking, absolute-rubric for gating.** Pairwise aligns better with human preference (and cuts ties 59.8%→3.9%) but is more vulnerable to "distracted evaluation" (a distractor flips pairwise ~35% vs ~9% for absolute). Use pairwise to pick best-of-N, absolute-boolean to gate "show the human?". [85]
- **Reference-guided judging wins.** Comparing against the committed golden intro / `luckymas3` is far more reliable than open-ended "is this good." G-Eval, Prometheus, MT-Bench all confirm rubric+reference beats bare prompting. [29][30]
- **A cheap local judge exists:** **Prometheus 2 (7B / 8×7B) is open-weight**, does both direct scoring and pairwise, and is the **best open evaluator, narrowing the gap with GPT-4** (not equalling it) — runnable on the free 7800XT. [30]
- **VLM judges are weaker than text judges** (~0.57–0.75 Spearman vs >0.8) and have an "informativeness bias" (reward verbose answers even against the pixels) — for scoring rendered frames prefer a purpose-built reward model (**ImageReward / VideoScore / VisionReward**) over a raw VLM prompt, and never trust a single frame-score as a hard gate. [86][87]
- **Loop patterns that work** (all supply external signal): **Self-Refine** (~+20% across 7 tasks), **Reflexion** (verbal self-reflection stored in memory → "notes file of what the human rejected last time"), **CriticGPT / Constitutional** (a critic finds specific flaws → generator fixes; your `CHARACTER.md`/house-style *is* the constitution). [88][89][90]
- **Retention proxies — realistic vs wishful:** DON'T build a virality predictor (independent replication plateaus ~60%, ≈ coin-flip; vendor "78–85% accurate" claims are unevidenced marketing). DO build **rule-conformance** proxies grounded in real mechanics (hook ≤5s, no dead air, cut-on-drop, punchline density, caption legibility) — framed as "does this obey the known retention rules," and score hook frames with a media reward model. [32][50]

### What to BUILD into slopstudio (eval)

| Build | What it is |
|---|---|
| **Reference-anchored judge prompt** | Every judge call includes `luckymas3` (or the golden opener) as the gold exemplar + the explicit numeric rubric. |
| **Best-of-N harness** | Generalize seed-shootouts: N script openings / N seed frames / N TTS takes → pairwise rank → keep best, absolute-rubric gate before "show human." Cap N 4–16. |
| **Taste eval-set** (the moat) | A scored corpus of past *accepted/rejected* cuts + a runner that reports judge precision/recall against your labels; re-align the judge prompt when it drifts. A "regression test for taste." |
| **`critique`/`designcheck`/`frame-critic` compose into one loop** | render frame → deterministic hard-fails → VLM/reward-model soft issues (grounded bbox) → agent fixes → optional pairwise re-score. |
| **Local Prometheus-2 judge** | Stand up on the 7800XT (Vulkan, free) as the cheap always-on rubric scorer; reserve a frontier judge for the final "ship?" gate. |
| **Panel-of-judges for the ship gate only** | 3 small cross-provider judges (PoLL) for the expensive final decision; treat disagreement as a flag for the human, not certainty (judges share correlated errors). [91] |

---

## Claude Skills to author for this project

All are **project skills** committed to `.claude/skills/` in the slopstudio repo (so any in-repo agent session inherits them). Names are gerund/lowercase-hyphen, ≤64 chars; descriptions state *what* + *when* with trigger keywords. Use `allowed-tools` to pre-approve `slop.py`, `disable-model-invocation` for side-effecting task skills (render/publish), and `context: fork` + `agent: Explore` for read-only research/critique sub-passes to keep the main context lean. [1][2]

| Skill | What it does | Inputs → Outputs | Why it helps |
|---|---|---|---|
| **`composing-slop`** | The CLI-driver skill: wraps `slop.py` (overview/digest/add/insert/ripple/lint/critique/bed/genvo/render); encodes the beats→skeleton→adopt/genvo→lint→render checklist with a hard "lint+critique must pass before render" gate. `allowed-tools: Bash(python tools/slop.py *)`; dynamic ``!`… overview`` injection so it opens grounded in the live cut. | brief / project path → CLI actions + a validated cut | Removes permission friction; makes the gold-path workflow the default; the "low-freedom script + plan-validate-execute" pattern Anthropic recommends. [2] |
| **`writing-gemma`** | Gemma-san voice: DO/DON'T registers, signature phrases, 8–12 gold few-shot lines, August/Mazin self-check; enforces fact-pass-then-voice-pass. | topic + locked facts → styled VO beats | Consistent distinctive voice at scale; few-shot examples steer voice far better than adjective lists. [16][66] |
| **`retention-editing`** | Encodes the pacing/hook/open-loop/cut-cadence rules (long vs shorts) as a review checklist run before render; drives `slop.py critique`. | digest → pass/fail + fix list | Turns YouTube-official + MrBeast/Paddy retention science into an applied gate. [13][37][39] |
| **`frame-critic`** | VLM design-review: renders frames, scores the fuzzy rubric dimensions, emits grounded bbox issues, ranks variants pairwise; carries golden-frame exemplars. `context: fork`. | rendered frame(s) → JSON critique + variant pick | The visual self-correction loop, built to respect the ~72% VLM ceiling (pairwise, few-shot, grounding). [24][25] |
| **`gemma-brand`** | Design-system-as-data: caption/plate hex + type + 8pt tokens, sprite art-direction, expression↔state map, framing-as-positioning, bg-match rules. `paths`-gated. | frame/layout intent → concrete style params | The `brand-guidelines` analog; makes "on-brand" checkable, not vibes. [4] |
| **`shorts-format`** | Portrait conventions (1080×1920, 1.3× speech, bottom-band host, cover backdrops, animated transcript, reverse structure, loop). `disable-model-invocation` (explicit task). | source cut → portrait skeleton | Codifies the shorts grammar that differs sharply from long-form. [39][40] |
| **`script-doctor`** | ABT/hook/open-loop/filler/WPM lint + best-of-N opening ranking against the rubric + golden cut. | draft script → scored issues + ranked openings | Makes comedic/retention scripting mechanical and self-improving. [21][26][55] |
| **`packaging-first`** | Forces title + thumbnail concept before scripting; verifies the first 30s pays the promise off. | idea → promise spec + hook check | Idea-first is the top creators' highest-leverage habit. [15][37] |
| **`taste-review`** | Evidence-driven meta-critic (VOIDXAI pattern): scores only dimensions it has evidence for, routes before `/render`, uses the golden cut as calibration. | finished cut → verdict + grounded notes | A single "is this good enough to show the human?" gate that won't hallucinate scores. [5][29] |
| **`gemma-voice-tts`** | Codifies the TTS workflow gotchas (design-in-JP→switch-EN, golden = gemma-san-deep-jp, one clip per sentence, per-clip intonation, clone-fallback) + rate-aware sampling trap. | line + emotion → TTS params | Bakes hard-won gotchas into standing instructions — the highest-signal skill content per Anthropic. [1] |

Distribution note: keep them versioned in-repo; grow each from a few lines + **one real gotcha** (Anthropic: "most of our best skills began as a few lines and a single gotcha"); build a **≥3-scenario eval per skill** and iterate with the two-Claude loop (author-Claude refines, fresh-Claude uses, feed failures back). [1][2]

---

## Prioritized recommendations: quick wins → bigger builds

### Quick wins (hours–days; mostly authoring, no new engine)
1. **Author the "cheap" skills from existing docs:** `composing-slop`, `writing-gemma`, `retention-editing`, `shorts-format`, `gemma-voice-tts`. These are pure prose + your existing `CHARACTER.md`/`LLM_WORKFLOW.md` distilled into activatable, low-token skills. Immediate lift, near-zero risk. [1][2]
2. **Expose single-frame render** (`slop.py shot --at T` → PNG → llm-feed). Small change, unlocks every downstream visual loop. [codebase]
3. **Extend `lint` with cheap structural retention/design checks** (hook timing, dead-air > threshold, caption contrast/safe-area/dwell, shorts cut cadence). Deterministic, fast, high-signal. [13][69]
4. **Codify the kinetic-caption numbers** into the `r_transcript` animator + caption defaults (font/size/dwell/active-word/plate). [51][80]
5. **Formalize fact-pass → voice-pass separation** in the workflow (prevents persona-induced factual drift). [18][67]

### Medium builds (days–2 weeks)
6. **`slop.py digest`** — the LLM-facing timeline representation. Highest single ROI. [6]
7. **`slop.py critique` + `designcheck`** — the semantic/design quality gates (structural retention + deterministic design rubric). [11][13]
8. **`script-doctor`** — ABT/hook/filler lint + best-of-N opening ranking (pairwise, reference-anchored). [21][26]
9. **`frame-critic`** — VLM render→critique→pairwise-rank loop, built to the reliability caveats (few-shot, grounding, no absolute scores). [24][25]
10. **`packaging-first` step** feeding the promise into `skeleton` + `critique`. [15]
11. **Taste eval-set** seeded from your accepted/rejected history + a judge-alignment runner. [18]

### Bigger builds (weeks; new engines)
12. **`slop.py autocut`** (WhisperX silence/filler/repeat → candidate cut list). [9][45]
13. **`slop.py beats` + snap-to-beat** for montage/BGM sync. [48]
14. **Auto-zoom generator** keyed to vocal emphasis (rate-aware keyframes). [47]
15. **Edit-Critic two-agent loop** (Editor mutates / Critic critiques against brief → RENDER gate). [6]
16. **Local Prometheus-2 judge** on the 7800XT + best-of-N harness as always-on infra. [30]
17. **(Optional) OTIO export adapter** for Resolve/Premiere interop. [52]

---

## What's solid vs directional (read before over-indexing)

- **Research-backed / primary (build on confidently):** Claude Skills mechanics & frontmatter [1][2]; MT-Bench >80% judge agreement [30]; "LLMs can't self-correct without external feedback" [27]; VLM aesthetic ceiling ~72%/0.20 IoU, reasoning-no-help [11]; Prometheus 2 open-weight (best *open*, narrows GPT-4 gap — not equal) [30]; YouTube-official Intro/graph metric [13]; WCAG 4.5:1 & SMPTE safe areas [12][69]; motion 200–500ms/Material tokens [76]; ABT rule, Veritasium misconception method (Muller PhD), Loewenstein info-gap [19][21][54]; UICrit +55% / iterative-refinement 50%-gap-close [25]; best-of-N & reward-hacking findings [26][28].
- **Corroborated but vendor/practitioner-sourced (encode as defaults, not laws):** APV-by-length bands [14]; kinetic-caption specifics (48–60px, Montserrat, 60–70% height) [51]; 60-30-10 / 8pt grid / CRAP (conventions that work *because* they're consistent); pattern-interrupt cadence (section-dependent ranges, not a constant) [41].
- **Single-origin / flagged (directional only):** the "+32% open loops / +23% pattern-interrupt / +19% b-roll" percentages (mostly one VidIQ study) [42]; the "23.7% avg retention / one-minute-wall" figures (one 2025 marketing report) [34]; Submagic's auto-zoom mechanism (marketing description) [47]; "monotonous AI narration → 70% lower retention" (single-source, and specifically about *flat* delivery). [34]
- **Actively skeptical (do not build):** commercial "virality predictors" claiming 78–85% (unevidenced; independent replication ~60% ≈ chance) [32]; Opus Virality Score as a real predictor (black-box, weak in practice) [50].
- **Honesty note:** the Gemma example lines in Job 2 are constructed demonstrations of each technique, not sourced quotes. The two softened figures from verification: the skill-body "~5K tokens" is a community approximation (Anthropic's stated cap is **500 lines**; metadata median ~80 tokens), and `allowed-tools` *pre-approves* but does not *restrict* (that's `disallowed-tools`). [2]

---

## Sources

**Claude Skills / agent architecture**
[1] Anthropic engineering — Agent Skills: https://www.anthropic.com/engineering/equipping-agents-for-the-real-world-with-agent-skills · overview: https://platform.claude.com/docs/en/agents-and-tools/agent-skills/overview
[2] Skills best-practices: https://platform.claude.com/docs/en/agents-and-tools/agent-skills/best-practices · Claude Code skills (frontmatter, dynamic injection): https://code.claude.com/docs/en/skills · skills-guide (API): https://platform.claude.com/docs/en/build-with-claude/skills-guide
[3] canvas-design SKILL.md: https://github.com/anthropics/skills/blob/main/skills/canvas-design/SKILL.md
[4] brand-guidelines SKILL.md: https://raw.githubusercontent.com/anthropics/skills/main/skills/brand-guidelines/SKILL.md · anthropics/skills: https://github.com/anthropics/skills
[5] VOIDXAI/taste: https://github.com/VOIDXAI/taste · Dragoon0x/taste-skills: https://github.com/Dragoon0x/taste-skills · "taste skills as review infrastructure": https://www.developersdigest.tech/blog/taste-skills-ai-agents-design-review · obra/superpowers: https://github.com/obra/superpowers

**Agentic / LLM video editing**
[6] EditDuet (Editor+Critic, 6-verb API, NLE-judge 80.6%): https://arxiv.org/html/2509.10761v1
[7] LAVE (5 LLM functions, JSON storyboard/trim, plan-approve-execute): https://arxiv.org/html/2402.10294v1
[8] L-Storyboard / From Shots to Stories (markdown-table shots, ordering 29.82% top-1): https://arxiv.org/html/2505.12237v1
[9] Descript/Underlord: https://www.descript.com/underlord · Gling (silence/filler/bad-take): https://www.gling.ai/save-time
[10] WhisperX (<100ms word timestamps, `--highlight_words`): https://github.com/m-bain/whisperx
[33] Measure Twice, Cut Once: https://arxiv.org/pdf/2503.09027 · REGen (long→short): https://arxiv.org/pdf/2505.18880 · Prompt-Driven Agentic Video Editing (JSON plan): https://arxiv.org/pdf/2509.16811
[44] Karaoke caption best-practice (4–6 words, 50–100ms): https://vidno.ai/blog/karaoke-style-word-highlight-captions
[45] auto-editor (0.04 / −19dB / 0.2s margin): https://github.com/WyattBlue/auto-editor
[46] filler-word handling / Whisper: https://huggingface.co/spaces/openai/whisper/discussions/30
[47] Submagic auto-zoom (vocal emphasis): https://www.submagic.co/features/auto-zooms
[48] librosa beat_track: https://librosa.org/doc/main/generated/librosa.beat.beat_track.html · BeatSync Engine: https://github.com/Merserk/BeatSync-Engine
[49] Adobe beat-detection music-synced edits: https://helpx.adobe.com/premiere/mobile/audio-editing/use-beat-detection-for-music-synced-edits.html
[50] Opus Virality Score (Hook/Flow/Value/Trend): https://help.opus.pro/docs/article/virality-score
[52] OpenTimelineIO format spec: https://opentimelineio.readthedocs.io/en/latest/tutorials/otio-file-format-specification.html
Other tools: AutoPod https://www.autopod.fm/ · Adobe Auto Reframe https://helpx.adobe.com/premiere-elements/using/auto-reframe.html · NVIDIA active-speaker https://build.nvidia.com/nvidia/active-speaker-detection/modelcard · Captions.ai https://captions.ai/features/edit-with-ai · VideoAgent https://github.com/HKUDS/VideoAgent

**Retention & pacing**
[13] YouTube official — audience-retention key moments (Intro/graph reading): https://support.google.com/youtube/answer/9314415
[14] APV benchmarks: https://humbleandbrag.com/blog/youtube-audience-retention-benchmarks · https://longstories.ai/blog/youtube-analytics-metrics-long-form-videos
[15] Paddy Galloway via Colin & Samir (idea-first, promise/payoff): https://www.colinandsamir.com/resources/the-new-rules-of-youtube-from-paddy-galloway
[34] Retention Rabbit 2025 report (directional): https://www.retentionrabbit.com/blog/2025-youtube-audience-retention-benchmark-report
[35] AVD vs retention / session contribution: https://virvid.ai/blog/average-view-duration-vs-retention-youtube-2026
[36] Quality-CTR / click-promise: https://www.overseeros.com/blog/youtube-click-promise
[37] MrBeast leaked handbook (summary): https://www.danielscrivner.com/how-to-succeed-in-mrbeast-production-summary/ · https://www.shaanpuri.com/essays/mrbeast-leaked-memo
[38] YouTube on MrBeast thumbnails: https://www.socialmediatoday.com/news/youtube-shares-insights-mrbeasts-team-creates-compelling-thumbnail/709817/
[39] Paddy Galloway Shorts study (VVSA, 5,400 Shorts): https://threadreaderapp.com/thread/1646898356419981315.html · https://x.com/PaddyG96/status/1646898368495382528
[40] Shorts length/format/retention: https://www.opus.pro/blog/ideal-youtube-shorts-length-format-retention · hook formulas: https://www.opus.pro/blog/youtube-shorts-hook-formulas
[41] Retention editing / cut cadence: https://air.io/en/youtube-hacks/advanced-retention-editing-cutting-patterns-that-keep-viewers-past-minute-8 · pattern interrupts: https://joyspace.ai/pattern-interrupt-reset-attention-span
[42] B-roll watch-time / show-don't-tell: https://ganknow.com/blog/what-is-b-roll/ · hook-strategy stats: https://www.retentionrabbit.com/blog/youtube-hook-strategy-to-keep-viewers-watching
[43] Narration WPM: https://flowshorts.app/blog/words-per-minute-speaking · https://www.voiceovers.com/blog/how-many-words-per-minute-voice-over
YouTube Creator Playbook (first 15): https://blog.youtube/creator-and-artist-stories/youtube-creator-playbook-tips-first-15/

**Scriptwriting & comedy**
[16] Deadpan in screenwriting: https://www.numberanalytics.com/blog/ultimate-guide-to-deadpan-in-screenwriting · dry humor: https://zanyzune.com/dry-humor-explained
[17] Role/persona prompting: https://learnprompting.org/docs/advanced/zero_shot/role_prompting · https://medium.com/@stunspot/on-persona-prompting-8c37e8b2f58c
[18] Hamel — evals & aligning judges: https://hamel.dev/blog/posts/evals/ · Kurzgesagt research/fact process: https://medium.com/@Kurzgesagt/how-research-and-factchecking-work-at-kurzgesagt-f5b239188255
[19] Veritasium / Derek Muller (misconception-first, PhD): https://en.wikipedia.org/wiki/Derek_Muller
[20] Scientific American on Muller's method: https://www.scientificamerican.com/article/how-youtube-star-derek-muller-of-veritasium-is-challenging-scientific/
[21] Trey Parker "but/therefore" rule: https://gointothestory.blcklst.com/writing-advice-from-matt-stone-and-trey-parker-30941b2cd98c · https://perell.com/note/but-therefore-rule/
[22] Rule of three: https://en.wikipedia.org/wiki/Rule_of_three_(writing) · https://punchlinecopy.com/funnier-copy-rule-of-3/
[23] Bathos: https://en.wikipedia.org/wiki/Bathos
[54] Loewenstein Information-Gap Theory: https://www.cmu.edu/dietrich/sds/docs/golman/golman_loewenstein_curiosity.pdf · Zeigarnik/curiosity: https://modelthinkers.com/mental-model/curiosity-zone
[55] Sugarman slippery slide: https://credible-content.com/blog/slippery-slide-copywriting-increase-sales/ · concision: https://writingcenter.unc.edu/tips-and-tools/conciseness-handout/
[56] Humor aids learning: https://www.edutopia.org/blog/laughter-learning-humor-boosts-retention-sarah-henderson
[57] Frontiers 5-decade humor review: https://www.frontiersin.org/journals/psychology/articles/10.3389/fpsyg.2025.1445362/full
[58] Comic timing / the pause: https://en.wikipedia.org/wiki/Comic_timing · https://movieschoolfree.com/script-writing-courses/comedy-script-writing/the-art-of-the-pause-mastering-comedic-timing-in-scriptwriting/
[59] Specificity is funny: https://medium.com/the-reckless-muse/the-art-of-humor-gratuitous-specificity-5a19288af30d
[60] UCB game / heightening: https://funnyshmunny.wordpress.com/ucb-the-game/
[61] Callbacks / running gags: https://en.wikipedia.org/wiki/Callback_(comedy)
[62] MoistCr1TiKaL (consistency, contrast): https://tvtropes.org/pmwiki/pmwiki.php/WebVideo/Critikal
[63] Mock-epic / mock-heroic: https://www.britannica.com/art/mock-epic · https://en.wikipedia.org/wiki/Mock-heroic
[64] Chuunibyou (committed-to-the-bit): https://tvtropes.org/pmwiki/pmwiki.php/Main/Chuunibyou · straight man: https://en.wikipedia.org/wiki/Straight_man
[65] Character bibles / consistency audits: https://scriptation.com/blog/tv-show-bible-and-character-bibles-guide/ · https://fiveable.me/tv-writing/unit-3/character-consistency-episodes/study-guide/eU0OyEeAbkIA0Xvw
[66] August/Mazin five voice tests: https://nofilmschool.com/2012/05/tests-characters-voice-working-from-john
[67] Johnny Harris visual storytelling: https://medium.com/@LMK_writing/how-johnny-harris-mastered-visual-storytelling-on-youtube-343ddf9160ec · Vox question-driven: https://jasperpictures.com.au/blog/vox-explained-a-case-study/ · Dan Harmon circle: https://www.studiobinder.com/blog/dan-harmon-story-circle/
[53] Paddy Galloway profile: https://sigmastory.in/paddy-galloway-the-youtube-mastermind-behind-mrbeast-viral-success/

**Design / aesthetics / VLM-critic**
[11] Can VLMs Assess Graphic Design Aesthetics (GPT-5 72.52% binary, ~0.20 IoU, fine-tune +17%): https://arxiv.org/abs/2603.01083 (HTML https://arxiv.org/html/2603.01083)
[12] SMPTE safe areas: https://en.wikipedia.org/wiki/Safe_area_(television) · title-safe still matters: https://eks.tv/title-safe-still-matters/
[24] AesBench (rudimentary MLLM aesthetic perception): https://arxiv.org/abs/2401.08276
[25] UICrit (+55% few-shot visual prompting): https://arxiv.org/abs/2407.08850 · iterative refinement (closes 50% gap): https://arxiv.org/abs/2412.16829
[68] TikTok/Shorts safe zones & caption placement: https://blitzcutai.com/blog/best-caption-placement-short-form-video · https://zeely.ai/blog/tiktok-safe-zones/
[69] WCAG 2.2 contrast: https://www.w3.org/TR/WCAG22/ · subtitle styling: https://convertaudiototext.com/blog/subtitle-styling-best-practices
[70] Accessible palettes / 60-30-10: https://www.visionaustralia.org/business-consulting/digital-access/Creating-accessible-digital-colour-palettes-60-30-10-design-rule
[71] Line length 45–75: https://www.uxpin.com/studio/blog/optimal-line-length-for-readability/ · http://webtypography.net/2.1.2
[72] Type scale / hierarchy: https://iamsteve.me/blog/type-scale-line-height-lengths
[73] 60-30-10: https://www.wix.com/wixel/resources/60-30-10-color-rule · 8pt grid: https://www.rejuvenate.digital/news/designing-rhythm-power-8pt-grid-ui-design
[74] CRAP principles: https://vwo.com/blog/crap-design-principles/ · Gestalt proximity: https://www.nngroup.com/articles/gestalt-proximity/ · Refactoring UI: https://www.refactoringui.com/
[75] Composition/lead-room/headroom: https://neiloseman.com/lead-room-nose-room-or-looking-space/ · https://guides.lib.unc.edu/media-design-center/video-concepts
[76] Animation duration (NN/g): https://www.nngroup.com/articles/animation-duration/ · Material motion tokens: https://m3.material.io/styles/motion/easing-and-duration/tokens-specs
[77] 12 principles for UI motion: https://ixdf.org/literature/article/ui-animation-how-to-apply-disney-s-12-principles-of-animation-to-ui-design
[78] Captions boost watch time: https://vizard.ai/knowledge-base/video-production/how-to-use-captions-to-boost-watch-time · https://www.3playmedia.com/blog/studies-find-captions-improve-engagement/
[79] Caption stats roundup: https://www.rev.com/blog/ultimate-roundup-closed-captions-statistics
[80] Caption presets/retention + kinetic typography: https://www.opus.pro/blog/best-caption-presets-styles-boost-retention · https://www.influencers-time.com/kinetic-typography-boost-video-retention-on-tiktok-and-reels/
[81] UICrit dataset (paper): https://people.eecs.berkeley.edu/~bjoern/papers/duan-uicrit-uist2024.pdf
[82] NIMA: https://arxiv.org/abs/1709.05424 · LAION-Aesthetic v2: https://laion.ai/blog/laion-aesthetics/ · human-correlation/saturation: https://onlinelibrary.wiley.com/doi/10.1155/2024/8223586

**Evaluation loops**
[26] Rejection sampling / best-of-N (RLHF book): https://rlhfbook.com/c/09-rejection-sampling
[27] LLMs Cannot Self-Correct Reasoning Yet (ICLR 2024): https://arxiv.org/abs/2310.01798
[28] Universal adversarial attacks on LLM judges: https://arxiv.org/abs/2402.14016 · reward-hacking in loops: https://arxiv.org/html/2506.09443v1
[29] G-Eval: https://arxiv.org/abs/2303.16634
[30] MT-Bench (>80% agreement): https://arxiv.org/abs/2306.05685 · Prometheus 2 (open-weight, direct+pairwise): https://arxiv.org/abs/2405.01535 · repo: https://github.com/prometheus-eval/prometheus-eval
[31] Binary>Likert / rubric decomposition: https://arxiv.org/html/2603.00077 · pairwise vs pointwise guide: https://www.evidentlyai.com/llm-guide/llm-as-a-judge
[32] Independent virality-prediction replication (~60% plateau): https://medium.com/@olimiemma/predicting-viral-content-i-trained-an-ai-on-40-000-news-articles-1bf99eb098a4 · vendor tools (skeptical): https://viralitypredictor.net/
[83] Inference-time scaling limits on open-ended tasks: https://arxiv.org/html/2504.00294v1
[84] Pairwise-vs-pointwise distractor flips: https://arxiv.org/abs/2504.14716
[85] (see [31]/[84]) pairwise ties 59.8%→3.9%: https://www.evidentlyai.com/llm-guide/llm-as-a-judge
[86] Video-LLM judge reliability (0.57–0.75): https://arxiv.org/html/2503.05977v1 · VLM informativeness bias: https://arxiv.org/pdf/2604.17768
[87] ImageReward: https://arxiv.org/abs/2304.05977 · VideoScore/VisionReward: https://arxiv.org/abs/2412.21059
[88] Self-Refine: https://arxiv.org/abs/2303.17651
[89] Reflexion: https://arxiv.org/abs/2303.11366
[90] CriticGPT / LLM critics: https://arxiv.org/abs/2407.00215 · Constitutional AI: https://www.anthropic.com/research/constitutional-ai-harmlessness-from-ai-feedback
[91] PoLL — Replacing Judges with Juries: https://arxiv.org/pdf/2404.18796 · correlated errors in panels: https://arxiv.org/html/2605.29800

**slopstudio codebase (internal — grounding for "what to build")**
`tools/slop.py` (subcommands), `docs/PROJECT_FORMAT.md`, `docs/LLM_WORKFLOW.md`, `../gemma-branding/CHARACTER.md`, `docs/STATUS.md`, `tools/export.sh`, `tools/gen-sfx.py`, `tools/process-pose-sheet.py`; reference cut `/opt/src/slopstudio-projects/luckymas/luckymas3.slop.json`; llm-feed `/opt/src/llm-feed/feed.py` (:8777).
