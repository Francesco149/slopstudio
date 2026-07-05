# align provider container (runs on lame's RTX 3080, queued behind the gen stack).
# Build from the REPO ROOT so providers/ is in the context:
#   docker build -t slop-align -f providers/align/Dockerfile .
#
# Two engines: WhisperX (CUDA word-timing) + Rhubarb Lip Sync (MIT, CPU visemes binary).
FROM pytorch/pytorch:2.5.1-cuda12.4-cudnn9-runtime

ENV PIP_NO_CACHE_DIR=1 \
    HF_HOME=/cache/hf \
    SLOP_CACHE=/cache/assets \
    RHUBARB_BIN=/opt/rhubarb/rhubarb \
    WHISPERX_MODEL=small \
    PYTHONUNBUFFERED=1
WORKDIR /app

# ffmpeg: WhisperX audio decode. libgomp1: Rhubarb's OpenMP runtime. wget/unzip: fetch Rhubarb.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ffmpeg wget unzip libgomp1 ca-certificates && rm -rf /var/lib/apt/lists/*

# Rhubarb Lip Sync (MIT) — prebuilt Linux release. Keep the whole dir: the binary needs the
# adjacent res/ (pocketSphinx + phonetic models).
ARG RHUBARB_VER=1.14.0
RUN wget -qO /tmp/rhubarb.zip \
      https://github.com/DanielSWolf/rhubarb-lip-sync/releases/download/v${RHUBARB_VER}/Rhubarb-Lip-Sync-${RHUBARB_VER}-Linux.zip \
 && unzip -q /tmp/rhubarb.zip -d /opt \
 && mv "/opt/Rhubarb-Lip-Sync-${RHUBARB_VER}-Linux" /opt/rhubarb \
 && chmod +x /opt/rhubarb/rhubarb && rm /tmp/rhubarb.zip

# Service layer + WhisperX. torch/cuda come from the base image; whisperx adds
# faster-whisper (ctranslate2) for transcription and torchaudio for forced alignment.
RUN pip install -U \
      whisperx \
      soundfile \
      "fastapi" "uvicorn[standard]" "websockets" "numpy"

COPY providers /app/providers

EXPOSE 8014
CMD ["python", "-m", "providers.align"]
