---
name: frame-critic
description: Visually review rendered frames/thumbnails with a VLM against an explicit design rubric — score dimensions, emit grounded fix notes, and rank variants pairwise. Use to check composition/legibility/on-brand-ness of a rendered beat or thumbnail, or to pick between visual variants (seeds/layouts/expressions).
---

# Frame critic (VLM design review)

The visual self-correction loop. **Prerequisite:** a rendered frame — use `slop.py shot --at T` (planned) or
the editor `--shot-frame T`, push to the llm-feed, and read it back. Respect the VLM ceiling (~72% on binary
aesthetic calls, ~0.20 IoU localization) — this FILTERS and RANKS; the human makes the final call. Grounding:
`docs/AGENT_TOOLING.md`, `../gemma-branding/PACKAGING.md` (thumbnail rubric).

## First, the DETERMINISTIC checks (never ask the VLM these — compute them)
Caption contrast ≥4.5:1 · text inside safe-area (portrait 900×1400, captions 60–70% height, clear of UI) ·
font ≥48px portrait · ≤4–6 words visible · dwell ≥1.2s · palette ≤2–3 hues · one focal point · host eyes on
the upper-third · lead-room toward content · on-brand palette (see `gemma-brand`). Hard-fails block render.

## Then the VLM (the fuzzy ~20%)
Feed it the numeric rubric as its checklist + few-shot golden-cut exemplars. Ask it to reason step-by-step,
THEN emit structured JSON per named dimension (balance/clutter, focal clarity "does the host read as the
subject?", color harmony, tonal fit, "does this look designed or auto-generated?") + a grounded fix list +
`one_fix` (the single highest-impact change).

## Variant picking (thumbnails/layouts/expressions)
Prefer **pairwise A/B of rendered variants** over absolute 1–10 scores. **Always run BOTH orderings** of each
pair (position bias); trust only a consistent winner. Enforce diversity (span archetypes, not near-dupes).
Route judge disagreement to the human. Thumbnail rubric + machine schema: `../gemma-branding/research/thumbnail-pipeline-2026-07.md` §5.
