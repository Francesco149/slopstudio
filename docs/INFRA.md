# slopstudio — Infrastructure & Ops (homelab)

How slopstudio drives *this* homelab (the GPU backend etc.). Not private — single
contributor — so it lives in the repo. **The authoritative config is in `../nix-lab`;**
this file is the operational quick-reference + the slopstudio-specific bits. Don't
duplicate nix-lab here — point to it.

## Authoritative sources (read in `../nix-lab`, don't copy here)
- `lib/lab.nix` — host IPs/MACs, ports, ssh host keys, secrets paths (single source of magic values).
- `docs/OPERATIONS.md` — backup/unlock/recovery flow, the nightly cycle, `/tmp/stay`.
- `docs/UPDATING.md` — update/redeploy runbook + `utils/lab-check.sh`.
- `hosts/lame/*.nix` — the GPU host: GPU wiring (`lame.nix`), `llama.nix`, `ingest.nix`, `ollama-proxy.nix`, `open-webui.nix`, docker data-root on `/lamedata`.
- `hosts/code/backup.nix` (+ `backup/cold-unlock.py`) — the `cold-unlock`/`cold-backup` orchestration.

## Hosts slopstudio touches
| host | addr | role | reach as |
|---|---|---|---|
| **lame** | `10.0.10.56` (initrd `10.0.10.61`) | the **GPU backend** — providers + ComfyUI run here | `root@lame`; `backup@lame` for unlock-created files |
| **code** | resolves as `code` | orchestrator — runs `cold-unlock` + the nightly backup | **`root@code`** ✓ |
| **wslop** | NAT'd `172.30.x` | the editor (Windows-native) + dev box | local |

- wslop is **NAT'd behind the WSL2 vswitch** → it **cannot** send Wake-on-LAN or reach
  lame's initrd directly. All wake/unlock is **relayed through `code`**.
- `headpats@{code,lame}` may be key-rejected here — use **`root@`**.

## Wake + unlock lame (the one command)
lame is normally **off, or parked at its LUKS unlock prompt** (initrd ssh on the custom
`ports.ssh-initrd`). From `code`, one command does WoL + LUKS unlock + zfs mount + keep-alive:
```sh
ssh root@code "cold-unlock --host lame --stay"
```
- `--host lame` targets lame (same script the nightly backup uses).
- `--stay` ssh's `touch /tmp/stay` onto **lame itself** (cold-unlock.py line ~218 uses the
  `--host` target) → disables the auto-shutdown the nightly `cold-backup` (runs on `code`
  at 01:30) would otherwise trigger. **Use `--stay` whenever we're actively working** so
  lame survives the night.
- Verify: `ping 10.0.10.56` = booted; `ping 10.0.10.61` = still at the unlock prompt.
- Re-enable auto-shutdown: remove `/tmp/stay` on lame (also cleared on reboot).

## lame GPU + storage layout
- **RTX 3080 10GB — CUDA + NVENC.** Shared into Docker via nvidia-container-toolkit (see
  `../nix-lab/hosts/lame/lame.nix`). **slopstudio's default gen GPU** (image/TTS/encode).
- **RX 7800XT 16GB — Vulkan** (the user's backend of choice; llama.cpp runs Vulkan/RADV
  here — `VK_ICD_FILENAMES` radv. ROCm only if a specific model is meaningfully faster on
  it). The bigger card; haruness is **stood down** (not running now — tear it down if it
  ever is), so the 7800XT is **free** too. Both GPUs are available to slopstudio.
- **GPU strategy:** the near-term gen stack — **image (Illustrious/ComfyUI), TTS
  (Qwen3-TTS), align (WhisperX), music (ACE-Step)** — runs on the **3080 / CUDA**.
  PyTorch-CUDA is frictionless, each fits 10 GB, jobs are async/queued (rarely two heavy
  models resident at once), and it dodges both AMD/ROCm friction and haruness contention.
  The 7800XT (Vulkan, 16 GB) is **free headroom** — the local-LLM script-writing option
  (llama.cpp Vulkan), a second concurrent model, or 16 GB-class **video** (Wan 2.2); weigh
  Vulkan vs ROCm per-model then (PyTorch video is ROCm-only, so AMD video may instead run
  as Wan 5B-FP8 on the 3080/CUDA).
- Models: `/opt/ai-lab/models` (today: GGUF text/VLM, whisper, nomic — no diffusion/TTS yet).
  **Put big new downloads (diffusion checkpoints, TTS weights, torch) on `/lamedata`**
  (~182 G free); root is only ~58 G free.
- **No global `python3` on lame** (NixOS). Run GPU workloads via **Docker** (the lab
  pattern — open-webui is containerized) or a pinned nix/uv env. Don't assume a system
  python. Docker exposes GPUs via **CDI** (default runtime is `runc`), so request them by
  CDI name, not `--gpus`: `docker run --device nvidia.com/gpu=0 …` for the 3080
  (`=all` for both). Both cards show under `/dev/dri` (card0/card1, renderD128/129).

## Existing lame services (don't trample)
`docker` + `docker-open-webui` (open-webui on `:3000`), `ingest` (`:8083`). The llama.cpp
services are currently disabled (`hosts/lame/llama.nix`). Listening at recon: 22, 53, 3000,
8083 (+ ephemerals).

## slopstudio service ports on lame (see `config.example.toml`)
tts `8010` (**live**, `slop-tts`, 3080) · image `8011` (**live**, `slop-image`, adapter) ·
video `8012` · music `8013` · align `8014` (**live**, `slop-align`, 3080) · rembg `8015`
(**live**, `slop-rembg`, **CPU** — bg removal / alpha matting, isnet-anime; ONNX on
`/lamedata/rembg-models`). **ComfyUI** (`comfyui`, the image engine, **7800XT/ROCm**) on
`:8188`. `slop-image` + `comfyui` share the `slopnet` docker network (adapter →
`comfyui:8188`). Checkpoints: `/lamedata/comfy/models`.
- **Firewall**: lame is NixOS — the `nixos-fw` chain only exposes **published docker ports**
  (`-p`), so every provider must run on a **bridge** network with `-p PORT:PORT`. A
  host-network bind on an unlisted port is reachable on lame's localhost but **dropped from
  the LAN** (the editor can't reach it). Corollary: a bridge container also can't HTTP a
  sibling's published port via the host IP — so providers exchange large media **by
  content-hash from a shared/RO-mounted cache**, not provider↔provider HTTP. `slop-align`
  reads the tts cache read-only (`SLOP_ASSET_ROOTS=/cache/assets:/inputs/tts`).

## Other lab bits (pointers, not duplication)
- **llm-feed** — `/opt/src/llm-feed/feed.py` on wslop (`:8777`); push visuals here.
- Windows host (wslop's host) LAN name: **`cutestation.soy`**.
- `cold` (`10.0.10.60`) = backup target; `timemachine` = XP/Win7 courier. See `lib/lab.nix`.

## Recon baseline (2026-06-26, first unlock)
3080 free (0% util, 1 MiB used); 7800XT not reporting via rocm-smi (likely idle/haruness);
docker up (open-webui, ingest); no ComfyUI/diffusion/TTS installed; `/` 58 G free,
`/lamedata` 182 G free.
