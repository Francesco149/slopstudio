# assets/

Committed project art assets (small; large binaries go through git-lfs — see
`../.gitattributes`). Model weights are never committed (they're gitignored and fetched
per-deploy).

- `gemma-san/` — the host character: the pngtuber expression + viseme **sprite set**
  (sliced from the chibi ref sheet), the sprite **manifest** (which sprite = which
  viseme/expression state), and any Inochi2D rig (`.inp`) added later. The source ref
  sheet + extra references live outside the repo at
  `/mnt/f/Pictures/oc/gemma-san/` and are the bootstrap material for the character LoRA
  and the sprite slicing.

Sprite manifests are JSON and map cleanly onto the avatar state machine
(`../docs/ARCHITECTURE.md` §7).
