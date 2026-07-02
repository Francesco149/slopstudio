#!/usr/bin/env bash
# Rebuild the (gitignored) playable cache for examples/signature-opener.slop.json (or any project)
# by Generating each clip through the editor — the TTS + align providers must be up on lame, and
# the editor must be built. The zoom + layout self-demonstrate on load (keyframes); this adds the
# VO audio + the avatar viseme tracks (so the host bobs/reacts in playback).
#   nix develop --command bash tools/demo-cache.sh                 # the signature opener
#   nix develop --command bash tools/demo-cache.sh PROJECT.slop.json
#   CLIPS="c_vo1 c_av1" nix develop --command bash tools/demo-cache.sh PROJECT.slop.json
set -euo pipefail
cd "$(dirname "$0")/.."

PROJ="${1:-examples/signature-opener.slop.json}"
CLIPS="${CLIPS:-c_giggle c_line c_av_giggle c_av_line}"
EXE=./build/slopstudio.exe
[ -x "$EXE" ] || { echo "build the editor first: nix develop --command make -C editor"; exit 1; }

for clip in $CLIPS; do
  echo ">> generate $clip"
  "$EXE" "$PROJ" --cache cache --generate "$clip"
done
echo "demo cache ready → $EXE $PROJ --cache cache"
