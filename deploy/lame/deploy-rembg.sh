#!/usr/bin/env bash
# Deploy the rembg provider (background removal / alpha matting) to lame — CPU.
# Run from anywhere in the repo (wslop). Ships providers/ over tar+ssh, builds the CPU
# container, and runs it on a bridge with -p so the editor reaches it (lame's nixos-fw
# only exposes published docker ports).
#
#   bash deploy/lame/deploy-rembg.sh
#
# First remove_bg of a model downloads its ONNX (~170MB for isnet-anime) into U2NET_HOME,
# persisted on /lamedata so a container rebuild doesn't refetch.
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/slopstudio}"
PORT="${PORT:-8015}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

echo ">> shipping providers/ to ${LAME}:${DEST}/repo"
ssh "$LAME" "bash -lc 'mkdir -p ${DEST}/repo ${DEST}/cache/rembg ${DEST}/rembg-models'"
tar -C "$REPO" -czf - providers | ssh "$LAME" "bash -lc 'tar -xzf - -C ${DEST}/repo'"

echo ">> building + running slop-rembg on lame (CPU)"
ssh "$LAME" "bash -l -s" <<EOF
set -e
cd ${DEST}/repo
docker build -t slop-rembg -f providers/rembg/Dockerfile .
docker rm -f slop-rembg 2>/dev/null || true
docker run -d --name slop-rembg --restart unless-stopped \
  -p ${PORT}:8015 \
  -e SLOP_CACHE=/cache/assets -e U2NET_HOME=/cache/models \
  -v ${DEST}/cache/rembg:/cache/assets \
  -v ${DEST}/rembg-models:/cache/models \
  slop-rembg
sleep 3
docker ps --filter name=slop-rembg --format '{{.Names}}  {{.Status}}'
EOF

echo ">> health check"
sleep 2
curl -sf "http://lame:${PORT}/healthz" && echo "  <- slop-rembg healthy on :${PORT}" || \
  echo "  (not healthy yet — check: ssh ${LAME} docker logs slop-rembg)"
