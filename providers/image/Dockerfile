# image provider — a thin ComfyUI adapter (no torch; just speaks HTTP to ComfyUI).
# Runs on the shared `slopnet` so it reaches ComfyUI at http://comfyui:8188.
# Build from the REPO ROOT:  docker build -t slop-image -f providers/image/Dockerfile .
FROM python:3.11-slim

ENV PIP_NO_CACHE_DIR=1 \
    SLOP_CACHE=/cache/assets \
    COMFYUI_URL=http://comfyui:8188 \
    PYTHONUNBUFFERED=1
WORKDIR /app

RUN pip install -U "fastapi" "uvicorn[standard]" "websockets"

COPY providers /app/providers

EXPOSE 8011
CMD ["python", "-m", "providers.image"]
