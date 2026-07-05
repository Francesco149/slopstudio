# Qwen3-TTS provider container (runs on lame's RTX 3080).
# Build from the REPO ROOT so providers/ is in the context:
#   docker build -t slop-tts -f providers/tts/Dockerfile .
FROM pytorch/pytorch:2.5.1-cuda12.4-cudnn9-runtime

ENV PIP_NO_CACHE_DIR=1 \
    HF_HOME=/cache/hf \
    SLOP_CACHE=/cache/assets \
    PYTHONUNBUFFERED=1
WORKDIR /app

# Service layer + Qwen3-TTS. flash-attn is intentionally NOT installed — we default to
# attn_implementation="sdpa" (no CUDA compile step, robust); set QWEN_TTS_ATTN to override.
RUN pip install -U \
      qwen-tts \
      soundfile \
      "fastapi" "uvicorn[standard]" "websockets" "numpy"

COPY providers /app/providers

EXPOSE 8010
CMD ["python", "-m", "providers.tts"]
