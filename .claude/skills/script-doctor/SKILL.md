---
name: script-doctor
description: Mechanically lint + improve a draft script (ABT causality, hook strength, open-loop cadence, filler words, WPM, comedic timing) and rank best-of-N openings. Use after a first script draft, before generating VO — to tighten it and pick the strongest cold-open.
---

# Script doctor

Makes scripting mechanical + self-improving. Runs on the locked-facts draft, styled by `writing-gemma`,
before TTS. Grounding: `docs/AGENT_TOOLING.md`.

## Deterministic passes (cheap, pre-TTS)
- **ABT scan:** flag every "and then" connector (inert) — must become "but" (reversal) or "therefore"
  (consequence), or cut the beat.
- **Filler scan:** delete `basically / actually / just / really / very / literally` + phrasal fillers
  (`you know / I mean / sort of / so`).
- **Sentence/one-idea:** one idea per line; each sentence must hook, teach, or land a joke — else cut.
- **Hook present** in the first N words; **open-loop cadence** (a live loop every ~45–60s long-form).
- **WPM estimate** vs 130–150 (120–140 dense) — flag over/under.
- **Comedy:** punch word LAST on joke lines; rule-of-three with the twist last; a "(BEAT)" before the punch.

## Best-of-N openings (reference-anchored)
Generate N cold-opens (misconception / bold-claim / curiosity-gap angles), rank **pairwise** against the
rubric + the golden cut (`luckymas3`), keep the best. Cap N 4–16; keep an absolute-rubric gate (don't just
trust the ranking). Never loop "is this good?" on itself — anchor on the rubric + golden reference.

## Output
A ranked issue list + the fixed script + the chosen opening. (This is the spec for the planned
`slop.py scriptlint`.)
