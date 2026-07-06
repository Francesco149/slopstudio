---
name: script-doctor
description: Mechanically lint + improve a draft script (ABT causality, hook strength, verification/anti-handwave pass, open-loop cadence, filler words, WPM, comedic timing) and rank best-of-N openings. Use after a first script draft, before generating VO — to tighten it and pick the strongest cold-open.
---

# Script doctor

Makes scripting mechanical + self-improving. Runs on the locked-facts draft, styled by
`writing-gemma`, before TTS. Grounding: `docs/AGENT_TOOLING.md` + the studied reference
corpus (`../slopstudio-projects/research/video-study/SYNTHESIS.md`).

## Deterministic passes (cheap, pre-TTS)
- **ABT scan:** flag every "and then" connector (inert) — must become "but" (reversal) or
  "therefore" (consequence), or cut the beat. Each section's conclusion should raise the
  next section's question.
- **Filler scan:** delete `basically / actually / just / really / very / literally` +
  phrasal fillers (`you know / I mean / sort of / so`).
- **Sentence/one-idea:** one idea per line; each sentence must hook, teach, or land a
  joke — else cut.
- **Hook present** in the first N words, branding AFTER the hook; **open-loop cadence**
  (a live loop every ~45–60s long-form; long teases must be re-teased mid-way).
- **WPM estimate** vs 160–195 (measured winning register; <150 only for dense math).
- **Comedy:** punch word LAST on joke lines; rule-of-three with the twist last; a "(BEAT)"
  before the punch. Specificity is funny — deadpan recitation of absurdly exact numbers.

## Verification pass (the anti-handwave — run line by line)
The signature LLM failure is gliding over detail. Every factual claim must carry ONE of:
1. **an exact number/name with a source** ("frame 16", "128 of 255", "filed June 2012") —
   never "very fast", "a lot", "old", "recently";
2. **a demonstration the edit can film** (staged repro, A/B mockup, live run, worked
   example) — if reality doesn't provide footage, the script must SAY what gets built;
3. **an explicit depth-delegation** ("glossed over — link in the description / see the
   repo") — silent omission is the bug, a pointer is fine.
Flag any claim carrying none. Also check:
- **Myth-check beats** where sources disagree ("the internet says X — the code says Y").
- **Dead ends stay in**: a failed approach that teaches the constraint shaping the real
  solution is an act, not waste. Honest negative results ("I don't know why") build trust.
- **Steelman obstacles**: anything blocking the protagonist gets a "why it exists and is
  reasonable" beat before being defeated.
- **History cashed out**: every context/history beat must end by paying into the present
  ("here's why I told you that") — no chronology dumps.
- **Worked A/B example** whenever ≥3 variables interact (two contrasting concrete
  instances, computed on screen).
- **Metaphor registry:** one metaphor per concept, declared once, repeated VERBATIM every
  recurrence (no synonym drift), and ideally visualized/built by the climax. Lint drift.
- **Objection-voice beat** near the climax: speak the viewer's objection ("maybe you're
  thinking…"), then escalate it into the payoff.
- **Callback close:** the ending echoes the cold open; CTA woven into the theme.

## Best-of-N openings (reference-anchored)
Generate N cold-opens (misconception / bold-claim / curiosity-gap / countdown-threat /
question-ladder angles), rank **pairwise** against the rubric + the golden cut
(`luckymas3`), keep the best. Cap N 4–16; keep an absolute-rubric gate (don't just trust
the ranking). Never loop "is this good?" on itself — anchor on the rubric + golden
reference.

## Output
A ranked issue list + the fixed script + the chosen opening. (This is the spec for the
planned `slop.py scriptlint`.)
