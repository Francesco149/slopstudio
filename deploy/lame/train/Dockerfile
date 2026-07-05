# LoRA trainer — kohya sd-scripts on AMD ROCm (lame's RX 7800XT, gfx1101).
# SDXL/Illustrious character LoRA + WD14 captioning. Build from this dir:
#   docker build -t slop-train -f Dockerfile .
# bitsandbytes/xformers/triton are dropped (CUDA-only) — we use AdamW + sdpa attention.
FROM python:3.11-slim

ENV PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1 HF_HOME=/cache/hf
RUN apt-get update && apt-get install -y --no-install-recommends \
      git libgl1 libglib2.0-0 ca-certificates && rm -rf /var/lib/apt/lists/*

# torch+rocm (wheels bundle the runtime; host provides the kernel driver)
RUN pip install --index-url https://download.pytorch.org/whl/rocm6.2 torch torchvision

ARG SD_REF=main
RUN git clone --depth 1 --branch ${SD_REF} https://github.com/kohya-ss/sd-scripts /app/sd-scripts
WORKDIR /app/sd-scripts
# kohya's pinned deps minus the CUDA-only ones (and minus torch — keep the rocm build).
RUN grep -ivE "bitsandbytes|xformers|triton|^torch( |=|>|<|$)" requirements.txt > /tmp/req.txt && \
    pip install -r /tmp/req.txt && \
    pip install "onnx" "onnxruntime" "huggingface-hub"   # WD14 tagger (CPU) + hub

# accelerate: single ROCm GPU, bf16, no distributed.
RUN mkdir -p /root/.cache/huggingface/accelerate && \
    printf 'compute_environment: LOCAL_MACHINE\ndistributed_type: NO\nmixed_precision: bf16\nnum_processes: 1\nuse_cpu: false\n' \
      > /root/.cache/huggingface/accelerate/default_config.yaml

CMD ["sleep", "infinity"]
