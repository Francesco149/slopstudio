---
name: shorts-format
description: The portrait/Shorts conventions for slopstudio (1080×1920, natural-rate speech, bottom-band host, cover backdrops, animated transcript, reverse structure, loop). Use when cutting a YouTube Short — a portrait project or a short clipped from a long-form cut.
---

# Shorts format (portrait)

Shorts are a separate grammar from long-form (top-of-funnel discovery; they rarely auto-convert). Compile
via `slop.py skeleton` with `"format":"portrait"`. Shorts recipe + the landscape-byte-identical guarantee:
`docs/LLM_WORKFLOW.md`. Retention rules: the `retention-editing` skill (shorts section).

## Render defaults (portrait)
- **1080×1920, speech 1.0× by default** (`meta.speech_rate`; mixer/export/retime/lint are all rate-aware — a clip's
  played dur = raw ÷ rate). Gap 0.35→0.2. Short act cards.
- Target about 180 measured WPM (roughly 175–190). Urgency comes from reverse structure,
  captions, and visual cadence. Match the source long-form cut's rate; portrait mode must
  not apply an additional multiplier.
- **Host = bottom-band presenter** (or solo-BIG room-shot when nothing else is on screen: sized to the middle
  band, horns below the top status strip, body above the bottom controls strip). Content = TOP band; the
  **animated transcript** rides just under it (room scenes) or in the **top safe band** over a scene/code card
  (never overlapping it — see *Scene animations & code cards* below).
- New **`layout:"cover"`** for backdrops (never degrades). Insets ~86%W/42%H. `pos` is an OFFSET from the smart
  default corner (not absolute) — `[0,0]` = the clean default.
- Portrait strap 16% from the bottom (clears the Shorts title/controls). Clueless-gag label above the horns,
  arrow tip stops ABOVE the head.

## Scene animations & code cards (portrait) — locked 2026-07-17
The layout engine (`visual.scene` widgets in a skeleton beat) is how fancy portrait animations are authored —
via `slop.py`, never hand-edited JSON. Reuse a proven widget when one fits (`code`, `chart`, `comparison`,
`versus`, `battlegrid`, `event_proof`, `reveal`, `stat`, …); pass a custom `script` only when none does.
- **Code cards MUST be authored NARROW for portrait.** A widescreen card (long right-aligned trailing
  comments) clips the right edge on 1080-wide portrait. Reformat so comments sit on their OWN lines, the
  widest line is **≤ ~30 chars**, and the card is **≤ ~14 lines** — it then renders at a large, legible font.
  Set **`data.fill: true`** so the card GROWS to span the frame (`fit_w`, default 0.92 = ~91% width with a
  small margin) instead of sitting narrow in a sea of margin — the widget's plain auto-fit only ever *shrinks*
  a too-wide card, so a short-lined portrait card stays tiny without `fill`. A `fit_h` guard (default 0.82·H)
  keeps a tall filled card from overflowing. Author narrow AND `fill:true` = big font, edge-to-edge.
- **Captions must never obstruct a code/scene card.** The animated transcript sits in the **top safe band**
  (`tr_room` anchor, ~y 240–340px) and the centred code card clears below it (even a `fill`ed 14-line card's
  top ≈ 475px = clear gap). Keep captions clear of the top ~10% (Shorts progress bar) and the bottom ~16%
  (Shorts title/like/comment/share UI). If a card must be taller, split it across two beats rather than let it
  climb into the caption. **GOTCHA:** the compiler routes a code beat's transcript to the top band via the
  *room* path (`span_has_content`=false) — but a scene-code card isn't counted as "content", so if a
  **fullscreen video/image from an earlier beat lingers into the code beat's span**, `span_has_content` fires
  and the transcript drops to `tr_content` (CENTER), colliding with the card (hit on lotr2 short6 b04, whose
  b02 video overran). Fix without a recompile: a caption-anchor clip lifting that beat's chunks back to the
  top band — `slop.py anchor <proj> --beat bNN --pos 0,-603` (tr_content→tr_room delta) — it survives
  transcript regen.
- **Full-frame scene widgets carry NO top title/subtitle on portrait** — the caption band owns the top, so a
  widget title collides with it. Drop `title`/`sub` and let the caption + VO name it; carry detail with the
  widget's own in-body labels (chart markers/vlines/axis labels, versus bar notes). For a chart, also push the
  plot down (`top≈0.20`) so the top axis-tick label clears the caption, and keep the rotated y-label short so
  it doesn't clip the left frame edge.
- **Numeric head-to-heads → `widgets.versus`** (count-up value bars with an eased pop; bar height = the real
  magnitude, so "who's bigger" reads instantly). **Native animated line/bar charts → `widgets.chart`.**
- **Gameplay footage is NOT `solo`** — a landscape 4:3 clip goes in the top content-band with the host as the
  bottom-band presenter (this fills the frame). `blur_fill` is image-only; `layout:"cover"` crops a 4:3 source
  too hard for 9:16 (you get a narrow centre slice).
- **A landscape SCREENSHOT/receipt (forum post, article, patent) — zoom it with the `document` widget, `fit:
  "contain"`, and make EVERY excerpt (and `from`) the SAME aspect ratio.** Two traps to avoid: `fit:"cover"`
  **stretches** the image non-uniformly to fill 9:16 (owner-rejected — the text distorts vertically); and
  `contain` with tight, *differently*-shaped crops both bands it thin AND reshapes the inset as it pans. The fix
  that reads as a clean push-in: keep all `rect`s at one fixed aspect (**~1.5:1** works — a landscape inset ≈
  1080×720, ~37% frame height, aspect-correct), each progressively tighter + panned onto the payoff. Same aspect
  → the inset is one FIXED rectangle and only the *content* scale-zooms (no stretch, no reshaping). **Put the
  crop's x on the TEXT column, not the whole image** — a forum post has a wide left avatar/margin gutter (~first
  third); start `x≈0.30` or the inset fills its left half with empty page. The wide paragraphs crop at the right
  edge (fine — "the other text isn't fully visible horizontally"); the tables/numbers stay whole. **Anchor that
  beat's transcript to the top band** (`slop.py anchor --beat bNN --pos 0,-603`) so captions clear the inset;
  blank the excerpt `tr`. Solo (no host) is fine — it's a scene card, but a filled screenshot grades like footage
  so extend the noir filter over it (only the bare-grid code cards stay ungraded). (lotr2 short6: GOG "units
  stats" post — movement table on "measured by eye" → scale-zoom into ranges 16/8/20 on "byte for byte, they
  were right".) *Alt layout if you want the host present:* an `image` clip (`layout:"fit"`, not `solo`) top-band
  + host bottom-band, "zoom" via two `crop` rects — but the owner prefers the solo `document` inset-zoom.
- **The now-playing song chip is OFF on portrait** by default (`slop.py skeleton` sets `meta.song_credits`
  false) — it otherwise covers the first frame = the Short's thumbnail; the song credit goes in the description.
- **TTS gotcha:** the voice mispronounces some ordinary words (it can't say "deserts" → write "disbands");
  keep the on-screen spelling via `params.transcript` when the spoken respell differs from the display text.

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
