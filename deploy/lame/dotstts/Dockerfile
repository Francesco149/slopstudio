# dots.tts eval container (runs on lame's RTX 3080).
#   rednote-hilab/dots.tts — Apache-2.0 code AND weights. Zero-shot voice CLONING
#   (reference audio + transcript), 24 languages, 48 kHz mono, 2B params.
#
# Base = lame's ALREADY-LOCAL pytorch 2.5.1 image (the Qwen3-TTS base) to skip a ~3.7 GB
# Docker Hub pull on lame's ~1 MB/s link. dots.tts REQUIRES torch>=2.8 (pyproject) and pins
# torch==2.8.0 in constraints/recommended.txt — so `pip install -e . -c constraints` UPGRADES
# the base's 2.5.1 to the TESTED 2.8.0 from PyPI (~12 MB/s on lame, ~3 min; pulls the matching
# nvidia-*-cu12 wheels). The base's bundled CUDA 12.4 is then unused. Python 3.11 (the image)
# is inside dots.tts's >=3.10,<3.13. Driver 595.80 runs the cu12.x torch fine.
#
# (Equivalent exact-recommended alternative: FROM pytorch/pytorch:2.8.0-cuda12.6-cudnn9-runtime
#  with torch preinstalled — avoids the pip torch pull but costs the 3.7 GB Docker base pull.)
#
# Build (no repo context needed — source is cloned from GitHub):
#   docker build -t slop-dotstts -f deploy/lame/dotstts/Dockerfile deploy/lame/dotstts
FROM pytorch/pytorch:2.5.1-cuda12.4-cudnn9-runtime

ENV PIP_NO_CACHE_DIR=1 \
    HF_HOME=/cache/hf \
    PYTHONUNBUFFERED=1 \
    DEBIAN_FRONTEND=noninteractive
WORKDIR /app

# git to clone the source; build-essential in case WeTextProcessing/pynini has no wheel for
# this interpreter and falls back to a source build.
RUN apt-get update && \
    apt-get install -y --no-install-recommends git build-essential && \
    rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/rednote-hilab/dots.tts.git /app/dots.tts
WORKDIR /app/dots.tts

# Editable install with the project's recommended version pins (torch==2.8.0, torchaudio==2.8.0,
# transformers==4.57.0, numpy==2.2.6, safetensors, librosa, torchdiffeq, ...). This upgrades the
# base's torch 2.5.1 -> the tested 2.8.0 from PyPI.
RUN pip install -e . -c constraints/recommended.txt

# The base image shipped torchvision 0.20.1 (built for torch 2.5.1); upgrading torch to 2.8.0 left
# it mismatched -> `RuntimeError: operator torchvision::nms does not exist`, which breaks even the
# text-only `from transformers import Qwen2ForCausalLM` (transformers' lazy loader probes the broken
# torchvision). dots.tts is audio-only and never needs torchvision, so drop the orphan.
RUN pip uninstall -y torchvision

CMD ["dots.tts", "--help"]
