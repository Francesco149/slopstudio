# ComfyUI on AMD ROCm — the image/video generation engine, on lame's RX 7800XT (gfx1101).
# The torch-rocm wheels bundle the ROCm runtime libs, so a slim base is enough; the host
# only provides the amdgpu kernel driver (/dev/kfd, /dev/dri). Build from this dir:
#   docker build -t slop-comfyui -f Dockerfile .
FROM python:3.11-slim

ENV PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      git libgl1 libglib2.0-0 ca-certificates && rm -rf /var/lib/apt/lists/*

# torch + rocm 6.2 (gfx1101 is supported). Install BEFORE ComfyUI's requirements so its
# unpinned `torch` is already satisfied and pip won't pull the CPU build over it.
RUN pip install --index-url https://download.pytorch.org/whl/rocm6.2 \
      torch torchvision torchaudio

ARG COMFY_REF=master
RUN git clone --depth 1 --branch ${COMFY_REF} https://github.com/comfyanonymous/ComfyUI /app/ComfyUI
WORKDIR /app/ComfyUI
RUN pip install -r requirements.txt

EXPOSE 8188
# models/ output/ input/ custom_nodes/ user/ are mounted from the host (/lamedata/comfy).
CMD ["python", "main.py", "--listen", "0.0.0.0", "--port", "8188"]
