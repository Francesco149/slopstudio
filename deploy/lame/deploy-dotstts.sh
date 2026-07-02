#!/usr/bin/env bash
# Evaluate dots.tts (rednote-hilab/dots.tts, Apache-2.0 code+weights) on lame's RTX 3080,
# A/B against the live Qwen3-TTS clone. Zero-shot voice CLONING only (ref audio + transcript)
# — NO voice design — so the A/B clones the SAME baked Gemma reference on both engines.
# This is the EXACT flow the 2026-06-27 eval ran (verified end-to-end).
#
# Run from anywhere in the repo (wslop):  bash deploy/lame/deploy-dotstts.sh
#
# Order of operations:
#   1. ship the Dockerfile + driver + manifest + the Gemma ref to lame:/lamedata/dotstts
#   2. build the slop-dotstts image (local pytorch base; pip pulls torch 2.8.0 from PyPI,
#      see the Dockerfile; ~10 min cold, cached after)
#   3. STOP slop-tts + slop-align to free the 3080 (dots.tts ~5.4 GB won't fit alongside them
#      — same gotcha as LoRA training)
#   4. run driver.py in the container: loads dots.tts-soar ONCE (downloads ~4.4 GB weights to
#      HF_HOME on first run; +torch.compile warmup ~4 min from optimize=True), then synthesizes
#      every manifest line (~10 s/line steady-state) -> /lamedata/dotstts/out/*.wav
#   5. RESTART slop-tts + slop-align (the live editor TTS path)
#
# VRAM: ~5.4 GB resident / 6.6 GB peak on the 3080. Output is 48 kHz mono (Qwen is 24 kHz).
set -euo pipefail

LAME="${LAME:-root@lame}"
DEST="${DEST:-/lamedata/dotstts}"
MODEL="${MODEL:-rednote-hilab/dots.tts-soar}"   # soar=best clone; -mf=fast(4 steps); -base
HERE="$(cd "$(dirname "$0")/dotstts" && pwd)"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
REF_WAV="${REF_WAV:-$REPO/presets/voices/gemma-san-deep.ref.wav}"

echo ">> staging on ${LAME}:${DEST}"
ssh "$LAME" "mkdir -p ${DEST}/work ${DEST}/hf ${DEST}/out"
tar -C "$HERE" -czf - Dockerfile driver.py ab_manifest.json | ssh "$LAME" "tar -xzf - -C ${DEST}"
scp -q "$REF_WAV" "$LAME:${DEST}/work/ref.wav"
# NOTE: the design->clone manifest entry needs /work/design.wav — a Qwen3-TTS VoiceDesign
# sample. Bake it first via the live slop-tts (design mode) and scp it to ${DEST}/work/design.wav,
# or drop that manifest entry. (The eval used a smug-gemma designed sample.)

echo ">> building slop-dotstts (DETACHED on lame; log: ${DEST}/build.log)"
ssh "$LAME" "bash -lc 'cd ${DEST} && nohup docker build -t slop-dotstts -f Dockerfile . > build.log 2>&1 &'"
echo "   waiting for the image..."
until ssh "$LAME" "docker image inspect slop-dotstts >/dev/null 2>&1"; do sleep 15; done
echo "   image ready."

echo ">> freeing the 3080 (stop slop-tts + slop-align)"
ssh "$LAME" "docker stop slop-tts slop-align"

echo ">> running the dots.tts A/B synth (model loads once; first run downloads weights)"
ssh "$LAME" "docker rm -f dots-eval 2>/dev/null; docker run -d --name dots-eval --device nvidia.com/gpu=0 \
  -e HF_HOME=/cache/hf -e PYTHONUNBUFFERED=1 -e HF_HUB_DISABLE_TELEMETRY=1 \
  -v ${DEST}/hf:/cache/hf -v ${DEST}/work:/work -v ${DEST}/out:/out \
  slop-dotstts python /work/driver.py /work/ab_manifest.json ${MODEL} /out"
echo "   follow: ssh ${LAME} docker logs -f dots-eval"
until ssh "$LAME" "docker logs dots-eval 2>&1 | grep -qa 'ALL DONE'" 2>/dev/null; do
  ssh "$LAME" "docker ps --filter name=dots-eval --format '{{.Names}}'" | grep -qa dots-eval || { echo "   container exited"; break; }
  sleep 15
done
ssh "$LAME" "docker logs dots-eval 2>&1 | grep -aE '\[ok\]|FAILED|ALL DONE'; docker rm -f dots-eval >/dev/null 2>&1 || true"

echo ">> restarting the live TTS path (slop-tts + slop-align)"
ssh "$LAME" "docker start slop-tts slop-align"

echo ">> outputs in ${DEST}/out — pull with: scp '${LAME}:${DEST}/out/*.wav' build/dotstts-ab/"
echo ">> DONE."
