# rembg provider container — CPU background removal / alpha matting (MIT).
# Build from the REPO ROOT so providers/ is in the context:
#   docker build -t slop-rembg -f providers/rembg/Dockerfile .
#
# CPU on purpose: a single ~1k cutout is a few seconds and it dodges the 3080 (tts/align)
# + 7800XT (ComfyUI) gen contention. onnxruntime-gpu can drop in later behind the same handler.
FROM python:3.11-slim

ENV PIP_NO_CACHE_DIR=1 \
    SLOP_CACHE=/cache/assets \
    U2NET_HOME=/cache/models \
    OMP_NUM_THREADS=8 \
    PYTHONUNBUFFERED=1
WORKDIR /app

# rembg pulls opencv-python-headless (libglib) + onnxruntime (libgomp).
RUN apt-get update && apt-get install -y --no-install-recommends \
      libglib2.0-0 libgomp1 ca-certificates && rm -rf /var/lib/apt/lists/*

# rembg[cpu] = the matting lib + onnxruntime (CPU). The service layer mirrors the other providers.
RUN pip install -U \
      "rembg[cpu]" \
      pillow numpy \
      "fastapi" "uvicorn[standard]" "websockets"

COPY providers /app/providers

EXPOSE 8015
CMD ["python", "-m", "providers.rembg"]
