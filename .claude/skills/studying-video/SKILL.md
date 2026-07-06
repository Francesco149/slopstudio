---
name: studying-video
description: Digest and analyze a reference YouTube video (or a creator's format) into durable study notes — transcript, pacing metrics, contact sheets, then a written STUDY.md of its writing + editing craft. Use when asked to "study this video/creator/format", to grow the reference corpus, or before imitating a format.
---

# Studying a reference video

Turns a video URL into analysis the composing agent can learn from. Corpus lives in
`../slopstudio-projects/research/video-study/` (one dir per video; `SYNTHESIS.md` =
the distilled cross-video patterns — **read it before studying, update it after**).

## Pipeline (from slopstudio repo root, inside `nix develop`)
1. `python tools/study.py run <url-or-id> …` — fetch (≤480p video + subs + metadata),
   analyze (scene cuts, per-minute cuts/visual-events/words, WPM, longest static span),
   digest → `DIGEST.md` + timestamped `transcript.txt` + `sheets/*.jpg` (1 frame / 5s,
   auto-pushed to the llm-feed). Batch multiple URLs in one call. `list` shows the corpus.
2. **Read `DIGEST.md` fully** (metrics table + transcript) and **Read the contact sheets
   as images** — the sheets are ground truth for "what is on screen while X is said";
   cross-reference transcript timestamps against sheet timestamps.
3. Write `STUDY.md` in the video's dir covering BOTH dimensions:
   - **Writing/structure:** the cold open (quote it), how the title promise is paid,
     section chaining (therefore/but), open loops + where paid, history/context beats and
     what they cash into, verification ethos (exact numbers? demos? myth-checks? dead
     ends kept in? depth delegated with pointers?), objection-voicing, callback close,
     comedy mechanics.
   - **Editing/visuals:** what's on screen during narration (host-over-footage ratio,
     ambient motion), say-it-show-it latency, recurring/growing diagrams, purpose-built
     demonstrations/mockups, receipts (papers/patents/code), chart handling, stat cards,
     pattern-interrupt cadence, how the metrics (cuts/min vs visual events/min) explain
     the feel.
   - **Transferable to slopstudio:** concrete mappings onto our primitives (skeleton
     beats, diagram rows, typewriter, XP-VM captures, insets) — not vibes.
4. If patterns recur across ≥3 corpus videos, promote them into `SYNTHESIS.md` and (when
   they change how we author) into the `retention-editing` / `script-doctor` /
   `composing-slop` skills. Cite measured numbers, keep creator-neutral.
5. Commit the projects repo (research/) same session; commit any skill/tool changes here.

## Gotchas
- Interpretation caveat: LaurieWired-style ambient-motion sets read LOW on
  visual-events/min (small pixel fraction) — always eyeball the sheets before calling a
  video "static".
- Manual-sub videos pack lines into one seg; `study.py` tokenizes for honest WPM (fixed
  2026-07-06) — if WPM looks <100 on a talking video, re-run `analyze`.
- yt-dlp sub-langs are pinned to `en,en-orig`; broad `en.*` pulls dozens of
  auto-translated tracks and 429s.
- Study dirs gitignore `video.*`/`subs.*` — committed artifacts are the digests, studies,
  metrics, transcripts, and sheets.
