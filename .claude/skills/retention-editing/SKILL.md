---
name: retention-editing
description: Apply YouTube retention/pacing rules when structuring or reviewing a cut (hook timing, open loops, cut cadence, visual-change floor, long-form vs shorts). Use before rendering any video — as a review gate over the beats/timeline — and when a cut feels slow or a hook is weak.
---

# Retention editing

A pre-render review gate. Only YouTube's 30s "Intro" metric is official; the rest is well-corroborated —
treat as defaults, not laws. Full findings + sources: `docs/AGENT_TOOLING.md`.

## Long-form (10–15 min)
- **Hook ≤15s (hard cap 30s)**, no branded cold-open. Pay off the title/thumbnail promise *literally* here.
- Target **APV 40–55%** (≥50% = A-grade). Optimize the percentage + the hook, not raw minutes.
- Keep **≥1 open loop live at all times**; open a new one every ~45–60s; add a **mid-video re-hook at 40–60%**.
- **Visual-change floor: never >~8–15s of static host** without a cut / b-roll / zoom / on-screen text / SFX.
  Prefer show-don't-tell b-roll for any explained process.
- Pacing ramp: min 0–3 fast → 3–7 steadier → after 8 calm + a burst sequence every 2–3 min. **Reintroduce the
  premise** after each act. Narration 130–150 WPM (120–140 dense). **End abruptly** after the payoff.

## Shorts (portrait, 1.3× speech)
- **Hook ≤2.5s**, first frame high-contrast/in-focus/moving. **Reverse-structure** (payoff first, then explain).
- Cut every 2–4s; 6–12 visual changes per 20s; captions always. Author the **first 3s + last 3s first**, loop
  the end into the start. Target **VVSA 70–90%**. Length = the longest you can hold near-100% VVSA (don't cap at 30s).

## Review checklist (run over the timeline before render)
Hook paid off ≤15s / ≤2.5s? · any static stretch >8–15s? · an open loop always live? · payoff in the last
15–25%? · shorts cut cadence 2–4s? · no dead air? · WPM in range? → flag violations like `lint` does.
(This drives the planned `slop.py critique`.)
