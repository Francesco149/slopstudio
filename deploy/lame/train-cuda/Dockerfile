# LoRA trainer (CUDA) — kohya sd-scripts on lame's RTX 3080. Unlike RDNA3, CUDA bf16 training
# is stable, so this trains at bf16 @ 1024 (fine appendage detail) — and we CAN use the
# CUDA-only bitsandbytes (8-bit AdamW) + xformers to fit SDXL LoRA in the 3080's 10GB.
# Build from this dir:  docker build -t slop-train-cuda -f Dockerfile .
FROM pytorch/pytorch:2.5.1-cuda12.4-cudnn9-runtime

ENV PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1 HF_HOME=/cache/hf
RUN apt-get update && apt-get install -y --no-install-recommends \
      git libgl1 libglib2.0-0 ca-certificates && rm -rf /var/lib/apt/lists/*

ARG SD_REF=main
RUN git clone --depth 1 --branch ${SD_REF} https://github.com/kohya-ss/sd-scripts /app/sd-scripts
WORKDIR /app/sd-scripts
# Keep the base image's torch/torchvision (cu124); add kohya's other deps + the CUDA extras.
RUN grep -ivE "^torch( |=|>|<|$)|^torchvision" requirements.txt > /tmp/req.txt && \
    pip install -r /tmp/req.txt && \
    pip install "bitsandbytes" "xformers==0.0.28.post3" "onnx" "onnxruntime" "huggingface-hub"

RUN mkdir -p /root/.cache/huggingface/accelerate && \
    printf 'compute_environment: LOCAL_MACHINE\ndistributed_type: NO\nmixed_precision: bf16\nnum_processes: 1\nuse_cpu: false\n' \
      > /root/.cache/huggingface/accelerate/default_config.yaml

CMD ["sleep", "infinity"]
