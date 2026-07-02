#!/usr/bin/env bash
# Deploy the align provider (WhisperX word-timing + Rhubarb visemes) to lame's RTX 3080.
# Run from anywhere in the repo (wslop). Ships providers/ over tar+ssh, builds the CUDA
# container, runs it with the 3080 via CDI, and mounts the asset + HF caches on /lamedata.
#
#   bash deploy/lame/deploy-align.sh
#
# First word_timing downloads the WhisperX small model + the per-language wav2vec2 align
# model into the HF cache (persisted on /lamedata). Rhubarb is baked into the image.
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/slopstudio}"
PORT="${PORT:-8014}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

echo ">> shipping providers/ to ${LAME}:${DEST}/repo"
ssh "$LAME" "bash -lc 'mkdir -p ${DEST}/repo ${DEST}/cache/align ${DEST}/hf-cache'"
tar -C "$REPO" -czf - providers | ssh "$LAME" "bash -lc 'tar -xzf - -C ${DEST}/repo'"

echo ">> building + running slop-align on lame (3080 via CDI)"
ssh "$LAME" "bash -l -s" <<EOF
set -e
cd ${DEST}/repo
docker build -t slop-align -f providers/align/Dockerfile .
docker rm -f slop-align 2>/dev/null || true
# Bridge + -p so the editor reaches us (lame's nixos-fw only exposes published docker
# ports). align reads its audio INPUT zero-copy by content-hash from sibling providers'
# caches mounted read-only (SLOP_ASSET_ROOTS) — no provider-to-provider HTTP, which a
# bridge container can't do to the host's published ports anyway.
docker run -d --name slop-align --restart unless-stopped \
  --device nvidia.com/gpu=0 -p ${PORT}:8014 \
  -e SLOP_CACHE=/cache/assets -e HF_HOME=/cache/hf \
  -e SLOP_ASSET_ROOTS=/cache/assets:/inputs/tts \
  -v ${DEST}/cache/align:/cache/assets \
  -v ${DEST}/cache/tts:/inputs/tts:ro \
  -v ${DEST}/hf-cache:/cache/hf \
  slop-align
sleep 3
docker ps --filter name=slop-align --format '{{.Names}}  {{.Status}}'
EOF

echo ">> health check"
sleep 2
curl -sf "http://lame:${PORT}/healthz" && echo "  <- slop-align healthy on :${PORT}" || \
  echo "  (not healthy yet — check: ssh ${LAME} docker logs slop-align)"
