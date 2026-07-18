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
CACHE="cache"; OUT=""; TARGET_MB=""; FPS_OVERRIDE=""; SCALE_H=""; ABITRATE="160k"; REMUX_FROM=""; AUDIO_ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --cache) CACHE="$2"; shift 2 ;;
    --out)   OUT="$2";   shift 2 ;;
    --target-mb) TARGET_MB="$2"; shift 2 ;;   # space-efficient 2-pass ABR to hit ~N MB total
    --fps)   FPS_OVERRIDE="$2"; shift 2 ;;     # override the project fps (e.g. 60 for the final)
    --scale) SCALE_H="$2"; shift 2 ;;          # scale to this height (e.g. 1080), keep aspect
    --abitrate) ABITRATE="$2"; shift 2 ;;
    --remux-from) REMUX_FROM="$2"; shift 2 ;;  # re-mux NEW audio onto this mp4's video (-c:v copy) — skip the video render (audio-only edits)
    --audio-only) AUDIO_ONLY=1; shift ;;       # output ONLY the mixed audio track (no video render) — verify a music arrangement fast
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

EXE="build/slopstudio.exe"
[ -x "$EXE" ] || { echo "build the editor first: nix develop --command make -C editor" >&2; exit 1; }

PLAN="$(mktemp --suffix=.slop-export.json)"
ELOG="$(mktemp --suffix=.slop-export.log)"
AMIX="$(mktemp --suffix=.slop-mix.wav)"   # the finished audio mix (a clean WAV; each branch re-encodes it)
trap 'rm -f "$PLAN" "$ELOG" "$AMIX"' EXIT

"$EXE" "$PROJ" --cache "$CACHE" --export-plan "$PLAN"
W=$(jq -r .width "$PLAN"); H=$(jq -r .height "$PLAN")
FPS=$(jq -r .fps "$PLAN"); DUR=$(jq -r .duration "$PLAN"); TITLE=$(jq -r .title "$PLAN")
NAUDIO=$(jq '.audio | length' "$PLAN")
MG=$(jq -r '.master_gain_db // 0' "$PLAN")   # project final-mix gain (meta.gain_db) — after amix
# Default output lands in an `exports/` dir BESIDE the project (…/projects/<video>/exports/),
# not a repo-root catch-all — keeps each video's renders with its own project dir.
[ -n "$OUT" ] || OUT="$(dirname "$PROJ")/exports/$(basename "${PROJ%.slop.json}").mp4"
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
# The editor is a WINDOWS PE (WSLInterop), so it resolves cache/asset paths to Windows UNC form
# (//wsl.localhost/<distro>/... or //wsl$/<distro>/...). ffmpeg here runs in the LINUX nix shell
# and can't read those — strip the UNC prefix back to a native /… path so every WAV is found.
# (Without this the audio probe fails on ALL clips → a silent export.)
wsl_native() { printf '%s' "$1" | sed -E 's#^//(wsl\.localhost|wsl\$)/[^/]+##'; }

AIN=(); FC=""; MIXLBL=""; NKEPT=0; ffidx=0
for i in $(seq 0 $((NAUDIO - 1))); do
  p=$(wsl_native "$(jq -r ".audio[$i].path" "$PLAN")")
  # skip if the source has no audio stream (probe once; cheap)
  if ! ffprobe -v error -select_streams a -show_entries stream=index -of csv=p=0 "$p" 2>/dev/null | grep -q .; then
    echo ">> skip (no audio stream): $p" >&2; continue
  fi
  st=$(jq -r ".audio[$i].start" "$PLAN")
  g=$(jq -r ".audio[$i].gain_db" "$PLAN")
  inp=$(jq -r ".audio[$i].in // 0" "$PLAN")
  dur=$(jq -r ".audio[$i].dur // 0" "$PLAN")
  rate=$(jq -r ".audio[$i].rate // 1" "$PLAN")
  vexpr=$(jq -r ".audio[$i].vol_expr // empty" "$PLAN")   # music duck around gag cues (editor-built)
  ms=$(awk "BEGIN{printf \"%d\", $st*1000}")
  AIN+=(-i "$p"); ffidx=$((ffidx + 1))
  TRIM="atrim=start=${inp}"
  # consume dur*rate seconds of source so atempo (pitch-preserving) yields `dur` of output
  srcdur=$(awk "BEGIN{printf \"%.6f\", ($dur>0 ? $dur*$rate : 0)}")
  awk "BEGIN{exit !($dur>0)}" && TRIM="${TRIM}:duration=${srcdur}"
  ATEMPO=""
  awk "BEGIN{exit !($rate!=1)}" && ATEMPO="atempo=${rate},"
  VDUCK=""
  [ -n "$vexpr" ] && VDUCK=",volume=volume='${vexpr}':eval=frame"   # quotes protect the expr's commas
  # Normalize EVERY input to 48 kHz stereo BEFORE amix. The takes are a mix of rates (Qwen TTS
  # 24 kHz, recorded snips 48 kHz, music 44.1 kHz) and amix does NOT resample — feeding it mixed
  # rates mangled the mix into a short, sped-up blur (the user's "comes out sped up"). aresample +
  # aformat make amix see one uniform format so `duration=longest` and timing are correct.
  FC="${FC}[${ffidx}:a]${TRIM},asetpts=PTS-STARTPTS,${ATEMPO}volume=${g}dB${VDUCK},aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,adelay=${ms}|${ms}[a${NKEPT}];"
  MIXLBL="${MIXLBL}[a${NKEPT}]"
  NKEPT=$((NKEPT + 1))
done
NAUDIO="$NKEPT"

OUTW="$W"; OUTH="$H"; [ -n "$SCALE_H" ] && { OUTH="$SCALE_H"; OUTW="scaled"; }
echo ">> rendering '$TITLE'  ${W}x${H} @${OUTFPS}fps  ${DUR}s  (${NAUDIO} audio)  -> $OUT" >&2
VFARGS=(); [ -n "$VF" ] && VFARGS=(-vf "$VF")

# ── Mix the audio ONCE to a clean WAV, then every branch re-encodes from it. Two hard-won reasons:
#   (1) amix misbehaves when input 0 is the live rawvideo PIPE (it truncated the mix to a few seconds
#       — the "sped up" bug), so we mix separately with a tiny anullsrc as input 0.
#   (2) encoding amix DIRECTLY to AAC also truncated it (a timestamp quirk); a PCM/WAV intermediate
#       launders the timestamps, so the later AAC encode of the WAV is full-length.
if [ "$NAUDIO" -gt 0 ]; then
  FC="${FC}${MIXLBL}amix=inputs=${NAUDIO}:duration=longest:normalize=0,volume=${MG}dB[aout]"
  ffmpeg -hide_banner -loglevel error -y -f lavfi -t 0.1 -i anullsrc=r=48000:cl=stereo "${AIN[@]}" \
    -filter_complex "$FC" -map "[aout]" -c:a pcm_s16le -t "$DUR" "$AMIX"
fi

if [ -n "$AUDIO_ONLY" ]; then
  # just the mixed audio (no video render) — for verifying a music arrangement quickly
  [ -n "$OUT" ] || OUT="$(dirname "$PROJ")/exports/$(basename "${PROJ%.slop.json}").audio.m4a"
  echo ">> audio-only mix -> $OUT ($NAUDIO inputs, ${DUR}s)" >&2
  if [ "$NAUDIO" -gt 0 ]; then
    ffmpeg -hide_banner -y -i "$AMIX" -c:a aac -b:a "$ABITRATE" -t "$DUR" "$OUT"
  else echo "no audio in plan" >&2; fi
elif [ -n "$REMUX_FROM" ]; then
  # video is unchanged since $REMUX_FROM — copy its stream, drop in the freshly-mixed audio.
  [ -f "$REMUX_FROM" ] || { echo "--remux-from not found: $REMUX_FROM" >&2; exit 1; }
  echo ">> remux audio onto $REMUX_FROM (video copied) -> $OUT" >&2
  if [ "$NAUDIO" -gt 0 ]; then
    ffmpeg -hide_banner -y -i "$REMUX_FROM" -i "$AMIX" -map 0:v -map 1:a \
      -c:v copy -c:a aac -b:a "$ABITRATE" -t "$DUR" "$OUT"
  else
    ffmpeg -hide_banner -y -i "$REMUX_FROM" -map 0:v -c:v copy -an -t "$DUR" "$OUT"
  fi
elif [ "$NAUDIO" -gt 0 ]; then
  "$EXE" "$PROJ" --cache "$CACHE" --export 2>"$ELOG" | \
    ffmpeg -hide_banner -y -f rawvideo -pix_fmt rgba -s "${W}x${H}" -r "$FPS" -i - \
      -i "$AMIX" -map 0:v -map 1:a \
      "${VFARGS[@]}" -r "$OUTFPS" "${VC[@]}" -pix_fmt yuv420p -c:a aac -b:a "$ABITRATE" -t "$DUR" "$OUT"
else
  "$EXE" "$PROJ" --cache "$CACHE" --export 2>"$ELOG" | \
    ffmpeg -hide_banner -y -f rawvideo -pix_fmt rgba -s "${W}x${H}" -r "$FPS" -i - \
      "${VFARGS[@]}" -r "$OUTFPS" "${VC[@]}" -pix_fmt yuv420p -t "$DUR" "$OUT"
fi

SZ=$(du -h "$OUT" 2>/dev/null | cut -f1)
echo ">> exported -> $OUT  (${SZ:-?})" >&2
jq -r '.credits[]? | "credit: " + .' "$PLAN" >&2 || true
# a paste-ready credits block next to the video (music attribution for the YouTube description).
CRED_TXT="${OUT%.*}.credits.txt"
if jq -e '(.credits // []) | length > 0' "$PLAN" >/dev/null 2>&1; then
  { echo "Music:"; jq -r '.credits[]? | "• " + .' "$PLAN"; } > "$CRED_TXT"
  echo ">> credits -> $CRED_TXT  ($(jq -r '.credits | length' "$PLAN") track(s))" >&2
fi
