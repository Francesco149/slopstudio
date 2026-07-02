#!/usr/bin/env bash
# Export a slopstudio project to an mp4. The editor renders the deterministic full-res
# composite to raw RGBA frames (stdout); ffmpeg encodes the video and muxes the audio
# assembled from the editor's export plan (clip paths + offsets + gains). Run inside the
# dev shell (needs ffmpeg + jq from the flake):
#
#   nix develop --command bash tools/export.sh examples/signature-opener.slop.json --cache cache
#
# Encoder: libx264 by default (reliable everywhere); SLOP_NVENC=1 tries h264_nvenc.
set -euo pipefail
cd "$(dirname "$0")/.."

PROJ="${1:?usage: export.sh PROJECT.slop.json [--cache DIR] [--out FILE]}"; shift || true
CACHE="cache"; OUT=""; TARGET_MB=""; FPS_OVERRIDE=""; SCALE_H=""; ABITRATE="160k"
while [ $# -gt 0 ]; do
  case "$1" in
    --cache) CACHE="$2"; shift 2 ;;
    --out)   OUT="$2";   shift 2 ;;
    --target-mb) TARGET_MB="$2"; shift 2 ;;   # space-efficient 2-pass ABR to hit ~N MB total
    --fps)   FPS_OVERRIDE="$2"; shift 2 ;;     # override the project fps (e.g. 60 for the final)
    --scale) SCALE_H="$2"; shift 2 ;;          # scale to this height (e.g. 1080), keep aspect
    --abitrate) ABITRATE="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

EXE="build/slopstudio.exe"
[ -x "$EXE" ] || { echo "build the editor first: nix develop --command make -C editor" >&2; exit 1; }

PLAN="$(mktemp --suffix=.slop-export.json)"
ELOG="$(mktemp --suffix=.slop-export.log)"
trap 'rm -f "$PLAN" "$ELOG"' EXIT

"$EXE" "$PROJ" --cache "$CACHE" --export-plan "$PLAN"
W=$(jq -r .width "$PLAN"); H=$(jq -r .height "$PLAN")
FPS=$(jq -r .fps "$PLAN"); DUR=$(jq -r .duration "$PLAN"); TITLE=$(jq -r .title "$PLAN")
NAUDIO=$(jq '.audio | length' "$PLAN")
[ -n "$OUT" ] || OUT="exports/$(basename "${PROJ%.slop.json}").mp4"
mkdir -p "$(dirname "$OUT")"

# The editor renders at the PROJECT fps; --fps only changes the ffmpeg output frame rate
# (frames are duplicated/dropped by ffmpeg). To render MORE unique frames, raise meta.fps.
OUTFPS="${FPS_OVERRIDE:-$FPS}"

# Optional downscale (keep aspect, even dims). Applied as a -vf on the raw input.
VF=""
if [ -n "$SCALE_H" ] && [ "$SCALE_H" != "$H" ]; then VF="scale=-2:${SCALE_H}"; fi

# Video encoder. --target-mb → space-efficient 2-pass libx264 ABR sized to hit ~N MB total
# (YouTube handles x264 1080p well without an overkill bitrate — the user's "~300 MB first"):
# video_kbps = (target_MB*8192 - audio_kbps*dur) / dur. Else CRF (quality) or NVENC (opt-in).
TWO_PASS=0
if [ -n "$TARGET_MB" ]; then
  AKBPS=$(awk "BEGIN{print ${ABITRATE%k}+0}")
  VKBPS=$(awk "BEGIN{v=(${TARGET_MB}*8192 - ${AKBPS}*${DUR})/${DUR}; if(v<200)v=200; printf \"%d\", v}")
  VC=(-c:v libx264 -preset medium -b:v "${VKBPS}k" -maxrate "$((VKBPS*3/2))k" -bufsize "$((VKBPS*2))k")
  echo ">> target ${TARGET_MB}MB → video ${VKBPS}k + audio ${ABITRATE} (1-pass ABR x264)" >&2
elif [ "${SLOP_NVENC:-0}" = "1" ] && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_nvenc; then
  VC=(-c:v h264_nvenc -preset p5 -rc vbr -cq 22)
else
  VC=(-c:v libx264 -preset medium -crf 20)
fi

# Audio: one ffmpeg input per audio clip; trim to its [in, in+dur) slice (clip split sets an
# asset in-point), delay to its start, apply gain, mix them all. A `from_video` entry whose
# source has NO audio stream (a silent screen-capture mp4) is SKIPPED — otherwise its [N:a]
# matches no stream and the whole filtergraph fails. `ffidx` is the real ffmpeg input index
# (input 0 is the rawvideo pipe), incremented only for kept entries so it never desyncs.
AIN=(); FC=""; MIXLBL=""; NKEPT=0; ffidx=0
for i in $(seq 0 $((NAUDIO - 1))); do
  p=$(jq -r ".audio[$i].path" "$PLAN")
  # skip if the source has no audio stream (probe once; cheap)
  if ! ffprobe -v error -select_streams a -show_entries stream=index -of csv=p=0 "$p" 2>/dev/null | grep -q .; then
    echo ">> skip (no audio stream): $p" >&2; continue
  fi
  st=$(jq -r ".audio[$i].start" "$PLAN")
  g=$(jq -r ".audio[$i].gain_db" "$PLAN")
  inp=$(jq -r ".audio[$i].in // 0" "$PLAN")
  dur=$(jq -r ".audio[$i].dur // 0" "$PLAN")
  rate=$(jq -r ".audio[$i].rate // 1" "$PLAN")
  ms=$(awk "BEGIN{printf \"%d\", $st*1000}")
  AIN+=(-i "$p"); ffidx=$((ffidx + 1))
  TRIM="atrim=start=${inp}"
  # consume dur*rate seconds of source so atempo (pitch-preserving) yields `dur` of output
  srcdur=$(awk "BEGIN{printf \"%.6f\", ($dur>0 ? $dur*$rate : 0)}")
  awk "BEGIN{exit !($dur>0)}" && TRIM="${TRIM}:duration=${srcdur}"
  ATEMPO=""
  awk "BEGIN{exit !($rate!=1)}" && ATEMPO="atempo=${rate},"
  FC="${FC}[${ffidx}:a]${TRIM},asetpts=PTS-STARTPTS,${ATEMPO}volume=${g}dB,adelay=${ms}|${ms}[a${NKEPT}];"
  MIXLBL="${MIXLBL}[a${NKEPT}]"
  NKEPT=$((NKEPT + 1))
done
NAUDIO="$NKEPT"

OUTW="$W"; OUTH="$H"; [ -n "$SCALE_H" ] && { OUTH="$SCALE_H"; OUTW="scaled"; }
echo ">> rendering '$TITLE'  ${W}x${H} @${OUTFPS}fps  ${DUR}s  (${NAUDIO} audio)  -> $OUT" >&2
VFARGS=(); [ -n "$VF" ] && VFARGS=(-vf "$VF")

if [ "$NAUDIO" -gt 0 ]; then
  FC="${FC}${MIXLBL}amix=inputs=${NAUDIO}:duration=longest:normalize=0[aout]"
  "$EXE" "$PROJ" --cache "$CACHE" --export 2>"$ELOG" | \
    ffmpeg -hide_banner -y -f rawvideo -pix_fmt rgba -s "${W}x${H}" -r "$FPS" -i - \
      "${AIN[@]}" -filter_complex "$FC" -map 0:v -map "[aout]" \
      "${VFARGS[@]}" -r "$OUTFPS" "${VC[@]}" -pix_fmt yuv420p -c:a aac -b:a "$ABITRATE" -t "$DUR" "$OUT"
else
  "$EXE" "$PROJ" --cache "$CACHE" --export 2>"$ELOG" | \
    ffmpeg -hide_banner -y -f rawvideo -pix_fmt rgba -s "${W}x${H}" -r "$FPS" -i - \
      "${VFARGS[@]}" -r "$OUTFPS" "${VC[@]}" -pix_fmt yuv420p -t "$DUR" "$OUT"
fi

SZ=$(du -h "$OUT" 2>/dev/null | cut -f1)
echo ">> exported -> $OUT  (${SZ:-?})" >&2
jq -r '.credits[]? | "credit: " + .' "$PLAN" >&2 || true
