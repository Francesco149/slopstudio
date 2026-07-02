#!/usr/bin/env bash
# Deploy the Qwen3-TTS provider to lame's RTX 3080. Run from anywhere in the repo (wslop).
# Ships providers/ to lame over tar+ssh (no rsync dep), builds the CUDA container, runs it
# with the 3080 via CDI, and mounts the asset + HF caches on /lamedata.
#
#   bash deploy/lame/deploy-tts.sh
#
# First synth downloads ~5 GB of weights into the HF cache (persisted on /lamedata).
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/slopstudio}"
PORT="${PORT:-8010}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

echo ">> shipping providers/ to ${LAME}:${DEST}/repo"
ssh "$LAME" "bash -lc 'mkdir -p ${DEST}/repo ${DEST}/cache/tts ${DEST}/hf-cache'"
tar -C "$REPO" -czf - providers | ssh "$LAME" "bash -lc 'tar -xzf - -C ${DEST}/repo'"

echo ">> building + running slop-tts on lame (3080 via CDI)"
ssh "$LAME" "bash -l -s" <<EOF
set -e
cd ${DEST}/repo
docker build -t slop-tts -f providers/tts/Dockerfile .
docker rm -f slop-tts 2>/dev/null || true
docker run -d --name slop-tts --restart unless-stopped \
  --device nvidia.com/gpu=0 -p ${PORT}:8010 \
  -e SLOP_CACHE=/cache/assets -e HF_HOME=/cache/hf \
  -v ${DEST}/cache/tts:/cache/assets \
  -v ${DEST}/hf-cache:/cache/hf \
  slop-tts
sleep 3
docker ps --filter name=slop-tts --format '{{.Names}}  {{.Status}}'
EOF

echo ">> health check"
sleep 2
curl -sf "http://lame:${PORT}/healthz" && echo "  <- slop-tts healthy on :${PORT}" || \
  echo "  (not healthy yet — check: ssh ${LAME} docker logs slop-tts)"
