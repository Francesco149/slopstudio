# AGENT TOOLING — making the LLM a better editor, writer & designer

Roadmap for the capabilities that make the composing agent (Claude, driving `slop.py` + `.slop.json`)
make better **editing, scripting, and visual-design** decisions. This is the *applied* front end —
the skills to author, the `slop.py` builds, the eval loops, and the load-bearing heuristics. Full cited
research (retention science, agentic-editing systems, VLM-critic limits, ~90 sources) lives in
`docs/research/llm-video-tooling-2026-07.md`. Related: `../gemma-branding/PACKAGING.md` (packaging-first, titles,
thumbnails), `docs/LLM_WORKFLOW.md` (the current authoring path), `../gemma-branding/CHARACTER.md` (Gemma's voice).

> **The one architectural bet.** slopstudio's "agent edits a structured JSON project via a CLI" model is
> *exactly* the pattern Anthropic recommends for Skills (low-freedom script + plan-validate-execute) **and**
> the pattern the best research video-editing agents use (a small verb action-space over a compact timeline
> *digest*, with an Editor/Critic loop). So the highest-leverage builds are: **(1) a `digest`** the agent
> reasons over instead of raw JSON, **(2) Skills** that encode taste as rubrics + few-shot examples, and
> **(3) external-anchor eval loops** — because the literature is unambiguous that LLMs **cannot** reliably
> self-improve creative output from a closed "is this good?" self-loop; they need an external signal.

## The load-bearing principles (internalize these)
1. **Split every quality signal into deterministic-checks vs LLM/VLM-judgment.** Contrast, safe-area
   geometry, font px, chars/line, caption dwell, dead-air gaps, hook timing, WPM — all **computable and
   100% reliable**. Never ask a VLM for anything you can compute; reserve it for the fuzzy ~20% (balance,
   focal clarity, "does the joke land").
2. **Never loop "agent, is this good?"** Intrinsic self-correction often *degrades* creative output. Always
   anchor on an **external** signal: the deterministic rubric, the **golden reference cut** (`luckymas3`),
   rendered pixels, or a reward model.
3. **Best-of-N + a reference-anchored judge is the highest-ROI, lowest-risk loop** — you already do seed
   shootouts; generalize it (N openings / N frames / N takes → pairwise rank → keep best). Cap N (4–16),
   keep an absolute-rubric gate (judges are reward-hackable).
4. **Packaging-first** (see `../gemma-branding/PACKAGING.md`): the title+thumbnail promise is the spec the script pays off.
5. **Run voice and facts as SEPARATE passes** — persona prompting steers tone but *degrades* factual
   accuracy. Research/fact-check to a locked draft, then apply the Gemma voice layer.

## Encodable heuristics (the ones worth hard-coding)
**Retention / pacing** (only YouTube's 30s "Intro" metric is official; the rest is corroborated research
**recalibrated 2026-07-06 against a measured reference corpus** — 5 top explainers digested with
`tools/study.py`, patterns + numbers in `../slopstudio-projects/research/video-study/SYNTHESIS.md`):
long-form hook ≤15s (cap 30s) with branding AFTER the hook, target APV **40–55%** for 10–15min, ≥1 open
loop live at all times (long teases OK if re-teased + overpaid), re-hook at 40–60%, **visual floor =
perceived motion** (host over footage / in-shot animation / ambient backdrop — measured hard-cut rates
vary 2.8–7.5/min with no invariant; static-host-over-static-frame >15s is the violation), per-minute
pacing FLAT across the runtime (consistency beats escalation), **end abruptly** with a callback close,
narration **160–195 WPM measured** (reserve <150 for dense math; the old 130–150 default is below the
winning register). Shorts: hook ≤2.5s, reverse-structure (payoff first), cut every 2–4s, target **VVSA
70–90%**, author the first 3s + last 3s first and loop the end into the start. Length = the longest you
can hold near-100% VVSA — don't reflexively cap at 30s.

**Verification ethos (anti-handwave — from the corpus, enforced by `script-doctor`):** every claim
carries an exact sourced number, a filmable demonstration (stage it if reality doesn't provide it), or an
explicit depth-delegation pointer — never silent omission. Dead ends + honest negative results are acts,
not waste. History is told as character-arcs-with-receipts and always cashed out into the present.
Metaphors: one per concept, verbatim on every recurrence, visualized by the climax.

**Scripting / comedy** (mechanical, applicable): **misconception-first** teaching spine (Veritasium) for
deep-dives; the **ABT rule** ("and-then" = dead → must be "but"/"therefore"); **rule of three** with the
punch word **last** + a "(BEAT)" → the existing awkward-sting SFX; **bathos** (cosmic register → mundane
deflation) as Gemma's signature engine; **specificity is funny**; jokes must ride *on* the subject, never
bolted on; **don't explain the joke**. Deadpan = absurd content + neutral delivery, held consistently.

**Design** (deterministic — enforce in code): caption contrast **≥4.5:1** (WCAG); text inside title-safe
90% / portrait 900×1400, captions at 60–70% height clear of UI zones; caption font **≥48px portrait**,
≤4–6 words visible, chunk dwell ≥1.2s, active-word (karaoke) highlight of *meaningful* words only synced
to WhisperX onset; body line 45–75 chars; ≤2 typefaces; palette ≤2–3 hues; 8pt spacing grid; entrances
200–300ms ease-out; lower-third dwell 3–6s; one focal point; host eyes on the upper-third with lead-room
toward the content. Kinetic captions designed **sound-off first**. VLM only for the fuzzy residue —
respect its ceiling (~72% on binary aesthetic calls, ~0.20 IoU localization; reasoning gives no help) →
feed it the numeric rubric, prefer **pairwise A/B of rendered variants** over 1–10 scores, use few-shot
annotated exemplars.

---

## Skills (✅ AUTHORED 2026-07-03 in `.claude/skills/`, in-repo so any session inherits them)
All ten below are authored + committed. Keep them tight (small skills are correct — the research says a few
lines + one real gotcha); grow each with a ≥3-scenario eval as it's used in anger. The Recettear video
(`../recettear-study/docs/video-plan.md`) is the first real test of the suite.

| Skill | What it does |
|---|---|
| **`composing-slop`** | The CLI-driver: wraps `slop.py` (overview/digest/add/insert/ripple/lint/critique/bed/genvo/render); enforces the beats→skeleton→adopt/genvo→lint→render checklist with a hard "lint+critique pass before render" gate. Dynamic ``!`…overview`` so it opens grounded in the live cut. |
| **`writing-gemma`** | Voice sheet (DO/DON'T registers, signature phrases, **8–12 gold few-shot lines**, the August/Mazin voice self-check); enforces fact-pass-then-voice-pass. Extends `../gemma-branding/CHARACTER.md`. |
| **`retention-editing`** | The pacing/hook/open-loop/cut-cadence rules (long vs shorts) as a pre-render checklist; drives `slop.py critique`. |
| **`frame-critic`** | VLM design review: render frames → score the fuzzy rubric → grounded bbox issues → pairwise variant rank; carries golden-frame exemplars. Built to the VLM ceiling (pairwise, few-shot, no absolute scores). |
| **`gemma-brand`** | Design-system-as-data: caption/plate hex, type + 8pt tokens, sprite art-direction, expression↔pngtuber-state map, framing-as-positioning, bg-match rules. |
| **`shorts-format`** | Portrait conventions (1080×1920, 1.3× speech, bottom-band host, cover backdrops, animated transcript, reverse structure, loop). |
| **`script-doctor`** | ABT/hook/open-loop/filler/WPM lint + best-of-N opening ranking (pairwise, reference-anchored). |
| **`packaging-first`** | Forces title + thumbnail concept before scripting; verifies the first 30s pays the promise off. (See `../gemma-branding/PACKAGING.md`.) |
| **`taste-review`** | Evidence-driven meta-critic that scores only dimensions it has evidence for; the single "good enough to show the human?" gate, calibrated on the golden cut. |
| **`gemma-voice-tts`** | Codifies the TTS gotchas (design-in-JP→switch-EN, golden = gemma-san-deep-jp, one clip/sentence, per-clip intonation, clone-fallback, rate-aware sampling trap). |
| **`studying-video`** (added 2026-07-06) | The reference-video study pipeline: `tools/study.py` (fetch/analyze/digest → metrics + transcript + contact sheets) → hand-written STUDY.md → promote recurring patterns into SYNTHESIS.md + the skills above. Corpus: `../slopstudio-projects/research/video-study/`. |

---

## `slop.py` / editor builds (prioritized)
### Quick wins (hours–days; mostly authoring)
- Author the cheap skills above from existing docs (`composing-slop`, `writing-gemma`, `retention-editing`,
  `shorts-format`, `gemma-voice-tts`).
- **Expose single-frame render to the agent** (`slop.py shot --at T` → PNG → llm-feed :8777). Small change,
  **prerequisite for all visual self-correction** (today only a full render exists — see the exported-frame
  smoke-check pattern used this session).
- **Extend `lint` with cheap structural retention/design checks** (hook timing, dead-air > threshold,
  caption contrast/safe-area/dwell, shorts cut cadence).
- Codify the kinetic-caption numbers into the `r_transcript` animator + caption defaults.
- Formalize fact-pass → voice-pass separation in `docs/LLM_WORKFLOW.md`.

### Medium (days–2wk)
- **`slop.py digest`** — the LLM-facing compact per-clip table `{id, row, play in–out, source in–out, dur,
  1-line content, framing, transcript excerpt, has-visual?}`. **Highest single ROI** — feed this, never raw JSON.
- **`slop.py critique`** (semantic sibling of `lint`) + **`designcheck`** (deterministic design rubric).
- **`script-doctor`** (ABT/hook/filler + best-of-N openings) and **`frame-critic`** (VLM render→critique→rank).
- **Taste eval-set** — a scored corpus of past accepted/rejected cuts + a judge-alignment runner ("regression
  test for taste"; the moat).

### Bigger (weeks; new engines)
- **`slop.py autocut`** (WhisperX word timestamps → silence/filler/repeat detect → a *candidate* cut list the
  agent vetoes; deterministic defaults: −19dB / 0.5s min-silence / 0.2s margin; rate-aware for 1.3× shorts).
- **`slop.py beats <audio>` + snap-to-beat** (librosa/madmom on the percussive stem) for montage/BGM sync.
- **Auto-zoom generator** keyed to vocal emphasis (rate-aware `transform.scale` spring keyframes).
- **Edit-Critic two-agent loop** (Editor mutates / Critic critiques the brief → RENDER gate).
- **Local Prometheus-2 judge** on the free 7800XT (Vulkan) as the always-on rubric scorer; a frontier judge
  only for the final "ship?" gate.
- Add a **read/search verb** (`search` = CLIP/embedding retrieval over the asset library) to round out the
  closed action-space (we have add/insert/mv/trim/rm/ripple/rebase).

## What's solid vs directional
Build confidently on: Skills mechanics; "LLMs can't self-correct without external feedback"; the VLM
aesthetic ceiling; MT-Bench >80% judge agreement; YouTube's official Intro metric; WCAG/SMPTE; motion
tokens; ABT / Veritasium-misconception / info-gap. Treat as *defaults not laws*: APV bands, kinetic-caption
specifics, 60-30-10 / 8pt grid, pattern-interrupt cadence. **Do NOT build:** a "will it go viral" predictor
(independent replications plateau ~60% ≈ chance) — build **rule-conformance** proxies instead. Full
solid-vs-directional breakdown in `docs/research/llm-video-tooling-2026-07.md`.
