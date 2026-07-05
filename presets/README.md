# presets/

Committed "golden" presets — the reusable library that grows as good results are found.
Both humans and the LLM save here (the LLM is expected to save a transition/effect it
builds well as a reusable preset). Presets are text (JSON + GLSL), so they diff and review
cleanly.

- `voices/`      — TTS voice presets (the Qwen3-TTS instruct string + metadata; a sample
                   reference clip is regenerable and gitignored).
- `effects/`     — effect presets: a GLSL shader + a param manifest (defaults/ranges).
- `transitions/` — transition presets (two-input effects: `srcA`, `srcB`, `t`).

Project-local presets live inside the `.slop.json`; promote a good one here to share it
across projects.
