# providers/

Out-of-process generation services, one per capability, all speaking the slopstudio
provider protocol (`../docs/PROVIDER_PROTOCOL.md`). Out-of-process by design: a model
crash/OOM is isolated and can never take down the editor. Endpoints live in `config.toml`
so providers can run on the GPU host `lame` or locally — that indirection is what makes
slopstudio redeployable.

Modules:
- `base.py` — ✅ the reference skeleton every provider builds on: `/capabilities`,
  `/jobs` (+ WS progress), content-hash cache (idempotent, atomic, crash-safe), bounded
  async job queue, `/assets/{hash}`, structured errors. Tested in `tests/`.
- `mock/` — ✅ model-free reference provider (`speech`) for developing the editor against
  a live endpoint before the GPU providers exist: `python -m providers.mock`.
- `tts/` — ✅ Qwen3-TTS 1.7B VoiceDesign (`speech`, `voice_design`). CUDA container
  (`tts/Dockerfile`) deployed to lame's 3080 via `deploy/lame/deploy-tts.sh`. Designs an
  original voice from an instruct string; per-line emotion appended per request.
- `image/` — ComfyUI adapter (Illustrious-XL 2.0 + Gemma LoRA; SANA for diagrams).
- `video/` — procedural 2.5D motion + Wan 2.2 I2V.
- `music/` — Jamendo CC-BY search + ACE-Step local gen (emits attribution provenance).
- `align/` — WhisperX word timing + Rhubarb visemes.
- `rembg/` — ✅ background removal / alpha matting (`remove_bg`): image (base64) → RGBA
  cutout via rembg isnet-anime (model-selectable). **CPU** container (`rembg/Dockerfile`)
  deployed to lame via `deploy/lame/deploy-rembg.sh`; the editor's library `removebg`
  `method:"rembg"` path drives it. `SLOP_REMBG_FAKE=1` = a deterministic model-free path (tests).

Run one (inside the dev shell):
```sh
nix develop --command python -m providers.tts        # serves on its configured port
```
Heavy ML deps (torch, comfyui, qwen-tts, whisperx, …) are installed on the GPU host, not
baked into the flake's dev env; the dev env covers the service layer + light media tooling.
