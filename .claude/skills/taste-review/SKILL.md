---
name: taste-review
description: The final "is this good enough to show the human?" gate before render/publish — an evidence-driven meta-critic that scores only what it has evidence for, calibrated against the golden cut. Use once a cut is assembled and lint/critique pass, as the last check before exporting or presenting to the owner.
---

# Taste review (the ship gate)

The single gate before "show the human." Evidence-driven: **score only dimensions you have concrete evidence
for** (a rendered frame, the digest, the transcript) — never hallucinate a score. Anchor every judgment on an
EXTERNAL reference (the golden cut `luckymas3` / the deterministic rubric / rendered pixels), never a closed
"is this good?" self-loop (that degrades output). Grounding: `docs/AGENT_TOOLING.md` (eval loops).

## What it composes (run these first, then judge)
1. **Deterministic gates** — `lint` (structure) + the retention checklist (`retention-editing`) + design
   checks (`frame-critic`'s deterministic half). Any hard-fail → fix, don't ship.
2. **Script** — `script-doctor` passes green (ABT, hook, filler, WPM, a joke-from-her every few beats).
3. **Packaging** — title ≥9/13, thumbnail rubric passes, title+thumbnail complement (don't repeat).
4. **Visual** — `frame-critic` on the key beats (hook frame, each section's establishing frame).

## The verdict
Compare against the golden cut as the reference exemplar. Output: **SHIP / FIX** + a grounded, prioritized
note list (each tied to evidence + the rubric line it fails). Binary checklists beat 1–5 Likert. On genuine
uncertainty or judge disagreement, **route to the human** — that's the "human at the end of the loop." Keep a
scored record of accepted/rejected cuts (the taste eval-set) so this gate can be re-aligned when it drifts.
