#!/usr/bin/env bash
# Deploy ComfyUI (the image/video engine) to lame's RX 7800XT via ROCm. ComfyUI and the
# `image` provider share a docker network (`slopnet`) so the adapter reaches it by name
# (http://comfyui:8188) — lame's nixos-fw only exposes published ports, and a bridge
# container can't reach a sibling's published port via the host IP. Run from the repo (wslop):
#
#   bash deploy/lame/deploy-comfyui.sh
#
# First build pip-installs torch-rocm (~4 GB) + ComfyUI deps. Checkpoints live in
# /lamedata/comfy/models/checkpoints (download Illustrious-XL-v2.0.safetensors there).
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/comfy}"
PORT="${PORT:-8188}"
HERE="$(cd "$(dirname "$0")" && pwd)"

echo ">> shipping ComfyUI Dockerfile to ${LAME}:${DEST}/build"
ssh "$LAME" "bash -lc 'mkdir -p ${DEST}/build ${DEST}/models/checkpoints ${DEST}/models/loras ${DEST}/output ${DEST}/input ${DEST}/custom_nodes ${DEST}/user'"
tar -C "$HERE/comfyui" -czf - . | ssh "$LAME" "bash -lc 'tar -xzf - -C ${DEST}/build'"

echo ">> building + running slop-comfyui on lame (7800XT via ROCm)"
ssh "$LAME" "bash -l -s" <<EOF
set -e
docker network inspect slopnet >/dev/null 2>&1 || docker network create slopnet
cd ${DEST}/build
docker build -t slop-comfyui -f Dockerfile .
docker rm -f comfyui 2>/dev/null || true
# /dev/kfd + /dev/dri are world-rw on this host (no group-add needed). HSA_OVERRIDE makes
# gfx1101 use the well-trodden gfx1100 kernels. --ipc=host for ROCm shared memory.
docker run -d --name comfyui --restart unless-stopped --network slopnet \
  --device /dev/kfd --device /dev/dri --security-opt seccomp=unconfined --ipc=host \
  -e HSA_OVERRIDE_GFX_VERSION=11.0.0 -e HIP_VISIBLE_DEVICES=0 \
  -p ${PORT}:8188 \
  -v ${DEST}/models:/app/ComfyUI/models \
  -v ${DEST}/output:/app/ComfyUI/output \
  -v ${DEST}/input:/app/ComfyUI/input \
  -v ${DEST}/custom_nodes:/app/ComfyUI/custom_nodes \
  -v ${DEST}/user:/app/ComfyUI/user \
  slop-comfyui
sleep 6
docker ps --filter name=comfyui --format '{{.Names}}  {{.Status}}'
EOF

echo ">> waiting for ComfyUI to come up (model load + ROCm init can take a bit)…"
for _ in $(seq 1 30); do
  if curl -sf "http://lame:${PORT}/system_stats" >/dev/null 2>&1; then
    echo "  <- ComfyUI healthy on :${PORT}"; break
  fi; sleep 3
done
curl -sf "http://lame:${PORT}/system_stats" 2>/dev/null | head -c 400 || \
  echo "  (not up yet — check: ssh ${LAME} docker logs comfyui)"
