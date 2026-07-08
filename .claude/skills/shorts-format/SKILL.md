---
name: shorts-format
description: The portrait/Shorts conventions for slopstudio (1080×1920, 1.3× speech, bottom-band host, cover backdrops, animated transcript, reverse structure, loop). Use when cutting a YouTube Short — a portrait project or a short clipped from a long-form cut.
---

# Shorts format (portrait)

Shorts are a separate grammar from long-form (top-of-funnel discovery; they rarely auto-convert). Compile
via `slop.py skeleton` with `"format":"portrait"`. Shorts recipe + the landscape-byte-identical guarantee:
`docs/LLM_WORKFLOW.md`. Retention rules: the `retention-editing` skill (shorts section).

## Render defaults (portrait)
- **1080×1920, speech ~1.3×** (`meta.speech_rate`; mixer/export/retime/lint are all rate-aware — a clip's
  played dur = raw ÷ rate). Gap 0.35→0.2. Short act cards.
- **Host = bottom-band presenter** (or solo-BIG room-shot when nothing else is on screen: sized to the middle
  band, horns below the top status strip, body above the bottom controls strip). Content = TOP band; the
  **animated transcript** rides just under it (room scenes) or center (code beats, y=0).
- New **`layout:"cover"`** for backdrops (never degrades). Insets ~86%W/42%H. `pos` is an OFFSET from the smart
  default corner (not absolute) — `[0,0]` = the clean default.
- Portrait strap 16% from the bottom (clears the Shorts title/controls). Clueless-gag label above the horns,
  arrow tip stops ABOVE the head.

## Structure
Cut a short as its OWN project BESIDE the source cut (so `assets/…` resolve): **adopt** the tuned lm3 takes +
visemes verbatim, rewrite terse; hook in ≤2s; ONE reveal + CTA; author first-3s/last-3s first and loop.
**Name + show the actual subject** (owner 2026-07-08): ≥1 beat must speak the subject's real name (Lucky
Star, Recettear — respell for TTS, display-correct via `params.transcript`) AND put the subject itself on
screen (the mascots / box art / title screen, not just surrounding footage). "This game"/"this disc" is
fine per-line only if the name lands elsewhere — a short that never identifies its subject breaks the
funnel to the long-form (the mascot short missing the actual Lucky Star mascots = the canonical miss).
Each carries its own music bed; a sting (boom/awkward) ONLY if a real punchline lands — boom-on-punchline
means on THE punchline, not one per short by quota (owner 2026-07-08: don't be trigger-happy; when in
doubt, no sting). The Short's first frame IS its thumbnail
(design it to stand alone; native A/B isn't available on Shorts). Deliberately funnel to the long-form
(end screen / pinned comment). See `[[shorts-mode-arc]]`.
