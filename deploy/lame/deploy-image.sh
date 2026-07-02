#!/usr/bin/env bash
# Deploy the image provider (ComfyUI adapter) to lame. It runs on the shared `slopnet` so it
# reaches the ComfyUI engine at http://comfyui:8188 (deploy-comfyui.sh first), and publishes
# :8011 so the editor can reach it. No GPU on this container — ComfyUI does the work.
#
#   bash deploy/lame/deploy-comfyui.sh   # the engine (once)
#   bash deploy/lame/deploy-image.sh     # this adapter
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/slopstudio}"
PORT="${PORT:-8011}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

echo ">> shipping providers/ to ${LAME}:${DEST}/repo"
ssh "$LAME" "bash -lc 'mkdir -p ${DEST}/repo ${DEST}/cache/image'"
tar -C "$REPO" -czf - providers | ssh "$LAME" "bash -lc 'tar -xzf - -C ${DEST}/repo'"

echo ">> building + running slop-image on lame (on slopnet → comfyui:8188)"
ssh "$LAME" "bash -l -s" <<EOF
set -e
docker network inspect slopnet >/dev/null 2>&1 || docker network create slopnet
cd ${DEST}/repo
docker build -t slop-image -f providers/image/Dockerfile .
docker rm -f slop-image 2>/dev/null || true
docker run -d --name slop-image --restart unless-stopped --network slopnet \
  -p ${PORT}:8011 \
  -e SLOP_CACHE=/cache/assets -e COMFYUI_URL=http://comfyui:8188 \
  -v ${DEST}/cache/image:/cache/assets \
  slop-image
sleep 3
docker ps --filter name=slop-image --format '{{.Names}}  {{.Status}}'
EOF

echo ">> health check"
sleep 2
curl -sf "http://lame:${PORT}/healthz" && echo "  <- slop-image healthy on :${PORT}" || \
  echo "  (not healthy yet — check: ssh ${LAME} docker logs slop-image)"
