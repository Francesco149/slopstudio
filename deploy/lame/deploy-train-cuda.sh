#!/usr/bin/env bash
# Deploy the CUDA LoRA trainer to lame's RTX 3080 (the preferred path — CUDA bf16 is stable,
# unlike RDNA3). Leaves an idle container; exec caption+train jobs into it. First build is best
# run detached on lame (the torch base is already cached, so it's quick anyway).
#
#   bash deploy/lame/deploy-train-cuda.sh
#   ssh root@lame docker stop slop-tts slop-align     # free the 3080 first
#   docker exec -d -e PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True \
#     -e LORA_NAME=gemma-san-chibi-v2 -e PREC=bf16 -e RES=1024 -e DIM=64 -e ALPHA=32 \
#     -e OPT=AdamW8bit -e ATTN=xformers -e TE_LR=5e-5 -e REPEATS=4 -e EPOCHS=10 -e SAVE_EVERY=5 \
#     slop-train-cuda bash -lc 'bash /app/run-train.sh > /dataset/train.log 2>&1'
#   ssh root@lame docker start slop-tts slop-align    # restore after
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/comfy/train-cuda}"
HERE="$(cd "$(dirname "$0")" && pwd)"

echo ">> shipping CUDA trainer Dockerfile to ${LAME}:${DEST}/build"
ssh "$LAME" "bash -lc 'mkdir -p ${DEST}/build /lamedata/comfy/train/gemma-chibi /lamedata/comfy/models/loras'"
tar -C "$HERE/train-cuda" -czf - . | ssh "$LAME" "bash -lc 'tar -xzf - -C ${DEST}/build'"

echo ">> building + running slop-train-cuda on lame (3080 via CDI)"
ssh "$LAME" "bash -l -s" <<EOF
set -e
docker network inspect slopnet >/dev/null 2>&1 || docker network create slopnet
cd ${DEST}/build
DOCKER_BUILDKIT=1 docker build -t slop-train-cuda -f Dockerfile .
docker rm -f slop-train-cuda 2>/dev/null || true
# Mounts match the ROCm trainer so run-train.sh is identical; --device = the 3080.
docker run -d --name slop-train-cuda --network slopnet --device nvidia.com/gpu=0 \
  -e HF_HOME=/cache/hf \
  -v /lamedata/comfy/models/checkpoints:/models:ro \
  -v /lamedata/comfy/train/gemma-chibi:/dataset \
  -v /lamedata/comfy/models/loras:/output \
  -v /lamedata/comfy/hf-cache:/cache/hf \
  slop-train-cuda
docker ps --filter name=slop-train-cuda --format '{{.Names}}  {{.Status}}'
EOF
echo ">> ready. Copy run-train.sh in (docker cp) + train (see header)."
