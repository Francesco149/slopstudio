---
name: packaging-first
description: Design the title + thumbnail + angle as one unit BEFORE writing or composing a video, and make the script deliver that promise. Use at the very start of any new @GemmaExplains video — before scripting/beats — and whenever choosing/ranking a title or thumbnail concept.
---

# Package before you produce

The #1 growth lever. Decide the **title + thumbnail + angle together, first**; the packaging becomes the
spec the script pays off. Full playbook: `../gemma-branding/PACKAGING.md`. Brand kit (locked palette/wordmark/pillars):
`../gemma-branding/PACKAGING.md` §4 + `/mnt/f/Pictures/oc/gemma-san/branding/`.

## Produce a packaging brief (before beats)
Given the topic, emit: **{best angle, 8–12 title candidates across ≥4 formulas, 3 thumbnail concepts, the
intended first-15s hook}**. This channel grows on **Browse/Suggested**, so package for **intrigue/stakes/
story**, not keywords. Proper-noun-forward (anime/otaku scan for the name). Deadpan-cosmic voice IS the
differentiator.

**Title formulas** (use ≥4): Absurd-Stakes/curiosity · Specific-number/compression · Versus/progression ·
Authority-"Explains" · Identity-"you" · Contrarian-"…was a lie" · "How [thing] happened".

**Title checklist (score each; ship ≥9/13, non-zero on curiosity + deliverable):** curiosity gap (0–2) ·
specificity (0–2) · complements the thumbnail, no word-repeat (0–2) · deliverable, not clickbait (0–2) ·
~50–60 chars, hook in first ~40 (0–1) · proper-noun front-loaded (0–1) · on-brand deadpan voice (0–2) ·
restraint: ≤2 power words, no gratuitous year (0–1). Reject a 2-curiosity/0-deliverable title.

**Thumbnail:** one big emotional **Gemma reaction face** (glasses; the discrete expression set is a free
variant engine — pick the expression that IS the punchline), one focal point, ≤3 words of text that do NOT
repeat the title, the locked palette (black/purple-magenta/gold/white), legible at 168px. Generate 3–5
distinct ARCHETYPES (not recolors) → the top 2–3 go to YouTube Test & Compare (watch-time-optimized).
**Build them with the thumbnail tool** — `tools/thumb.py` + slopthumb over the gemma brand package
(`docs/THUMBNAIL_TOOL.md`; VTuber-meta geometry rules in the gemma-brand skill): one doc per archetype in
`<video>/thumbs/`, `render --proof` to the feed, `lint` gates, `snapshot` into the A/B bank, frame-critic
ranks the finalists at full size AND 168px.

## Then
Hand the locked promise to `composing-slop` as the constraint; `retention-editing` verifies the first 30s
pays it off. On publish: auto-fill description/hashtags/chapters/end-screen from the brief; launch Test &
Compare; log the winning pattern back into the brand bible.
