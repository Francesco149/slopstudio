# presets/

Committed "golden" presets — the reusable library that grows as good results are found.
Both humans and agents save here. Presets are text/data plus small licensed audio assets,
so they diff and review cleanly.

- `voices/`      — TTS voice presets (the Qwen3-TTS instruct string + metadata; a sample
                   reference clip is regenerable and gitignored).
- `avatars/`     — host rig manifests and expression/pose mappings.
- `backgrounds/` — standing-set backgrounds used by the skeleton defaults.
- `lua/`         — scene-animation stdlib (`std.lua`); catalogue and recipes are in
                   `docs/SCENE_COOKBOOK.md`.
- `voice-snips/` — stable recorded vocalizations that TTS should not regenerate.

Project-local presets live inside the `.slop.json`; promote a good one here to share it
across projects.
