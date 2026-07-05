#!/usr/bin/env bash
# Deploy the LoRA trainer (kohya sd-scripts on ROCm) to lame's 7800XT, then leave it idle
# (sleep infinity) so we can exec caption+train jobs into it. Build is big (torch-rocm ~4GB) —
# the FIRST build is best run detached on lame (see note); re-runs are cached.
#
#   bash deploy/lame/deploy-train.sh         # build + run the trainer container
#   docker exec -e REPEATS=5 -e EPOCHS=8 -e PREC=no -e RES=768 -i slop-train \
#     bash -lc 'bash /app/run-train.sh > /dataset/train.log 2>&1'   # then train (see run-train.sh)
#
# NOTE: bf16/fp16 NaN on RDNA3 → use PREC=no (fp32). Stop ComfyUI first to free the 16GB.
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/comfy/train}"
HERE="$(cd "$(dirname "$0")" && pwd)"

echo ">> shipping trainer Dockerfile to ${LAME}:${DEST}/build"
ssh "$LAME" "bash -lc 'mkdir -p ${DEST}/build ${DEST}/gemma-chibi /lamedata/comfy/models/loras'"
tar -C "$HERE/train" -czf - . | ssh "$LAME" "bash -lc 'tar -xzf - -C ${DEST}/build'"

echo ">> building slop-train (run detached for the first build: nohup … > ${DEST}/build.log 2>&1 &)"
ssh "$LAME" "bash -l -s" <<EOF
set -e
docker network inspect slopnet >/dev/null 2>&1 || docker network create slopnet
cd ${DEST}/build
DOCKER_BUILDKIT=1 docker build -t slop-train -f Dockerfile .
docker rm -f slop-train 2>/dev/null || true
# 7800XT passthrough; HSA_OVERRIDE for gfx1101; mounts: base ckpt (ro), dataset, lora output, hf cache.
docker run -d --name slop-train --network slopnet \
  --device /dev/kfd --device /dev/dri --security-opt seccomp=unconfined --ipc=host \
  -e HSA_OVERRIDE_GFX_VERSION=11.0.0 -e HIP_VISIBLE_DEVICES=0 -e HF_HOME=/cache/hf \
  -v /lamedata/comfy/models/checkpoints:/models:ro \
  -v ${DEST}/gemma-chibi:/dataset \
  -v /lamedata/comfy/models/loras:/output \
  -v /lamedata/comfy/hf-cache:/cache/hf \
  slop-train
docker ps --filter name=slop-train --format '{{.Names}}  {{.Status}}'
EOF
echo ">> trainer ready. Put refs in /dataset, then exec run-train.sh (see header)."
