#!/usr/bin/env python3
"""Krea-2 vs Illustrious A/B eval helper — builds a ComfyUI API graph and runs it.

Runs INSIDE the comfyui container (talks to http://localhost:8188). Not part of the
shipped provider; lives under deploy/lame/krea-eval/ purely for the scoped eval.

  python genimg.py --arch krea2|illustrious --prompt "..." --seed N \
      --w 1024 --h 1024 --out /app/ComfyUI/output/eval/<tag>.png

Krea-2 Turbo graph mirrors Comfy-Org's bundled `image_krea2_turbo_t2i.json`
(UNETLoader + CLIPLoader[type=krea2] + VAELoader, ConditioningZeroOut negative,
KSampler 8 steps / cfg 1 / euler / simple). Illustrious mirrors the live
providers/image text2image.json (CheckpointLoaderSimple, 28 / cfg 5 / euler_ancestral).
"""
import argparse, json, time, urllib.request, urllib.parse, os, sys

COMFY = "http://localhost:8188"
ILLUST_NEG = ("worst quality, low quality, bad anatomy, bad hands, jpeg artifacts, "
              "watermark, signature, text")


def http_json(path, payload=None, timeout=30):
    url = COMFY + path
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"content-type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST" if data else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def http_bytes(path, timeout=120):
    with urllib.request.urlopen(COMFY + path, timeout=timeout) as r:
        return r.read()


def vram():
    try:
        st = http_json("/system_stats")
        d = st["devices"][0]
        return d.get("vram_total", 0), d.get("vram_free", 0), d.get("name", "?")
    except Exception as e:
        return 0, 0, f"err:{e}"


def graph_krea2(prompt, seed, w, h, steps, cfg, lora, lora_str):
    g = {
        "10": {"class_type": "UNETLoader",
               "inputs": {"unet_name": "krea2_turbo_fp8_scaled.safetensors",
                          "weight_dtype": "default"}},
        "11": {"class_type": "CLIPLoader",
               "inputs": {"clip_name": "qwen3vl_4b_fp8_scaled.safetensors",
                          "type": "krea2"}},
        "12": {"class_type": "VAELoader",
               "inputs": {"vae_name": "qwen_image_vae.safetensors"}},
        "6": {"class_type": "CLIPTextEncode",
              "inputs": {"text": prompt, "clip": ["11", 0]}},
        "13": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["6", 0]}},
        "5": {"class_type": "EmptyLatentImage",
              "inputs": {"width": w, "height": h, "batch_size": 1}},
        "3": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": steps, "cfg": cfg,
                         "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0,
                         "model": ["10", 0], "positive": ["6", 0],
                         "negative": ["13", 0], "latent_image": ["5", 0]}},
        "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["12", 0]}},
        "9": {"class_type": "SaveImage", "inputs": {"filename_prefix": "kreaeval", "images": ["8", 0]}},
    }
    if lora:
        g["15"] = {"class_type": "LoraLoaderModelOnly",
                   "inputs": {"lora_name": lora, "strength_model": lora_str, "model": ["10", 0]}}
        g["3"]["inputs"]["model"] = ["15", 0]
    return g


def graph_qwen_image(prompt, seed, w, h, steps, cfg, shift, neg, lora, lora_str):
    # Qwen-Image-2512 (Apache-2.0, Qwen-Image MMDiT) — mirrors ComfyUI's bundled
    # `image_qwen_image` template: UNETLoader + CLIPLoader[type=qwen_image] + VAELoader,
    # ModelSamplingAuraFlow shift, REAL negative (base is NOT distilled → CFG>1),
    # KSampler 20 / cfg 4 / euler / simple.
    g = {
        "10": {"class_type": "UNETLoader",
               "inputs": {"unet_name": "qwen_image_2512_fp8_e4m3fn.safetensors",
                          "weight_dtype": "default"}},
        "11": {"class_type": "CLIPLoader",
               "inputs": {"clip_name": "qwen_2.5_vl_7b_fp8_scaled.safetensors",
                          "type": "qwen_image"}},
        "12": {"class_type": "VAELoader",
               "inputs": {"vae_name": "qwen_image_vae.safetensors"}},
        "16": {"class_type": "ModelSamplingAuraFlow", "inputs": {"shift": shift, "model": ["10", 0]}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt, "clip": ["11", 0]}},
        "7": {"class_type": "CLIPTextEncode", "inputs": {"text": neg, "clip": ["11", 0]}},
        "5": {"class_type": "EmptySD3LatentImage", "inputs": {"width": w, "height": h, "batch_size": 1}},
        "3": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": steps, "cfg": cfg,
                         "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0,
                         "model": ["16", 0], "positive": ["6", 0],
                         "negative": ["7", 0], "latent_image": ["5", 0]}},
        "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["12", 0]}},
        "9": {"class_type": "SaveImage", "inputs": {"filename_prefix": "kreaeval", "images": ["8", 0]}},
    }
    if lora:
        g["15"] = {"class_type": "LoraLoaderModelOnly",
                   "inputs": {"lora_name": lora, "strength_model": lora_str, "model": ["10", 0]}}
        g["16"]["inputs"]["model"] = ["15", 0]
    return g


def graph_qwen_gguf(prompt, seed, w, h, steps, cfg, shift, neg, gguf, lightning,
                    light_str, lora, lora_str):
    # Qwen-Image-2512 via a City96 GGUF UNET (fits 16GB, no offload) — same MMDiT graph as
    # graph_qwen_image but UNETLoader -> UnetLoaderGGUF, reusing the fp8 text encoder + vae.
    # Optional Lightning 8-step LoRA (lightx2v, Apache) -> steps 8 / cfg 1. Optional style LoRA
    # (e.g. the flat-cel one) chained model-only: gguf -> lightning -> style -> ModelSampling.
    g = {
        "10": {"class_type": "UnetLoaderGGUF", "inputs": {"unet_name": gguf}},
        "11": {"class_type": "CLIPLoader",
               "inputs": {"clip_name": "qwen_2.5_vl_7b_fp8_scaled.safetensors", "type": "qwen_image"}},
        "12": {"class_type": "VAELoader", "inputs": {"vae_name": "qwen_image_vae.safetensors"}},
        "16": {"class_type": "ModelSamplingAuraFlow", "inputs": {"shift": shift, "model": ["10", 0]}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt, "clip": ["11", 0]}},
        "7": {"class_type": "CLIPTextEncode", "inputs": {"text": neg, "clip": ["11", 0]}},
        "5": {"class_type": "EmptySD3LatentImage", "inputs": {"width": w, "height": h, "batch_size": 1}},
        "3": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": steps, "cfg": cfg,
                         "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0,
                         "model": ["16", 0], "positive": ["6", 0],
                         "negative": ["7", 0], "latent_image": ["5", 0]}},
        "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["12", 0]}},
        "9": {"class_type": "SaveImage", "inputs": {"filename_prefix": "kreaeval", "images": ["8", 0]}},
    }
    src = ["10", 0]
    if lightning:
        g["14"] = {"class_type": "LoraLoaderModelOnly",
                   "inputs": {"lora_name": lightning, "strength_model": light_str, "model": src}}
        src = ["14", 0]
    if lora:
        g["15"] = {"class_type": "LoraLoaderModelOnly",
                   "inputs": {"lora_name": lora, "strength_model": lora_str, "model": src}}
        src = ["15", 0]
    g["16"]["inputs"]["model"] = src
    return g


ANIMA_NEG = ("worst quality, low quality, score_1, score_2, score_3, blurry, "
             "jpeg artifacts, sepia")


def graph_anima(prompt, seed, w, h, steps, cfg, neg, lora, lora_str):
    # Anima base-v1.0 (CircleStone Labs x Comfy-Org, NVIDIA Cosmos-Predict2-2B; weights NC
    # but OUTPUTS commercial-OK). Faithful replica of ComfyUI's bundled
    # `image_anima_base_v1.json` template: UNETLoader(anima-base-v1.0) +
    # CLIPLoader(qwen_3_06b_base, type=stable_diffusion) + VAELoader(qwen_image_vae,
    # already present), REAL negative (CFG>1), KSampler 30 / cfg 4 / er_sde / simple,
    # plain EmptyLatentImage (NOT EmptySD3LatentImage) exactly as the template ships.
    # NOTE: authored from the authoritative template but UNRUN as of 2026-06-27 (lame
    # /lamedata was disk-blocked, 2.86 GB free vs 5.37 GB needed) -> verify on first run.
    g = {
        "10": {"class_type": "UNETLoader",
               "inputs": {"unet_name": "anima-base-v1.0.safetensors",
                          "weight_dtype": "default"}},
        "11": {"class_type": "CLIPLoader",
               "inputs": {"clip_name": "qwen_3_06b_base.safetensors",
                          "type": "stable_diffusion"}},
        "12": {"class_type": "VAELoader",
               "inputs": {"vae_name": "qwen_image_vae.safetensors"}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt, "clip": ["11", 0]}},
        "7": {"class_type": "CLIPTextEncode", "inputs": {"text": neg, "clip": ["11", 0]}},
        "5": {"class_type": "EmptyLatentImage",
              "inputs": {"width": w, "height": h, "batch_size": 1}},
        "3": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": steps, "cfg": cfg,
                         "sampler_name": "er_sde", "scheduler": "simple", "denoise": 1.0,
                         "model": ["10", 0], "positive": ["6", 0],
                         "negative": ["7", 0], "latent_image": ["5", 0]}},
        "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["12", 0]}},
        "9": {"class_type": "SaveImage", "inputs": {"filename_prefix": "kreaeval", "images": ["8", 0]}},
    }
    if lora:
        g["15"] = {"class_type": "LoraLoaderModelOnly",
                   "inputs": {"lora_name": lora, "strength_model": lora_str, "model": ["10", 0]}}
        g["3"]["inputs"]["model"] = ["15", 0]
    return g


def graph_illustrious(prompt, seed, w, h, steps, cfg):
    return {
        "4": {"class_type": "CheckpointLoaderSimple",
              "inputs": {"ckpt_name": "Illustrious-XL-v2.0.safetensors"}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt, "clip": ["4", 1]}},
        "7": {"class_type": "CLIPTextEncode", "inputs": {"text": ILLUST_NEG, "clip": ["4", 1]}},
        "5": {"class_type": "EmptyLatentImage",
              "inputs": {"width": w, "height": h, "batch_size": 1}},
        "3": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": steps, "cfg": cfg,
                         "sampler_name": "euler_ancestral", "scheduler": "normal", "denoise": 1.0,
                         "model": ["4", 0], "positive": ["6", 0],
                         "negative": ["7", 0], "latent_image": ["5", 0]}},
        "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["4", 2]}},
        "9": {"class_type": "SaveImage", "inputs": {"filename_prefix": "kreaeval", "images": ["8", 0]}},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", required=True,
                    choices=["krea2", "qwen-image", "qwen-gguf", "anima", "illustrious"])
    ap.add_argument("--prompt", required=True)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--w", type=int, default=1024)
    ap.add_argument("--h", type=int, default=1024)
    ap.add_argument("--steps", type=int, default=0)   # 0 → arch default
    ap.add_argument("--cfg", type=float, default=0.0)  # 0 → arch default
    ap.add_argument("--shift", type=float, default=3.1)  # qwen-image ModelSamplingAuraFlow
    ap.add_argument("--neg", default="")                 # qwen-image real negative (default empty)
    ap.add_argument("--lora", default="")
    ap.add_argument("--lora-str", type=float, default=1.0)
    ap.add_argument("--unet-gguf", default="qwen-image-2512-Q4_K_M.gguf")  # qwen-gguf UNET
    ap.add_argument("--lightning", default="")                            # 8-step LoRA file ("" = off)
    ap.add_argument("--light-str", type=float, default=1.0)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    if args.arch == "krea2":
        steps = args.steps or 8
        cfg = args.cfg or 1.0
        g = graph_krea2(args.prompt, args.seed, args.w, args.h, steps, cfg, args.lora, args.lora_str)
    elif args.arch == "qwen-image":
        steps = args.steps or 20
        cfg = args.cfg or 4.0
        g = graph_qwen_image(args.prompt, args.seed, args.w, args.h, steps, cfg,
                             args.shift, args.neg, args.lora, args.lora_str)
    elif args.arch == "qwen-gguf":
        steps = args.steps or (8 if args.lightning else 20)
        cfg = args.cfg or (1.0 if args.lightning else 4.0)
        g = graph_qwen_gguf(args.prompt, args.seed, args.w, args.h, steps, cfg,
                            args.shift, args.neg, args.unet_gguf, args.lightning,
                            args.light_str, args.lora, args.lora_str)
    elif args.arch == "anima":
        steps = args.steps or 30
        cfg = args.cfg or 4.0
        neg = args.neg or ANIMA_NEG
        g = graph_anima(args.prompt, args.seed, args.w, args.h, steps, cfg,
                        neg, args.lora, args.lora_str)
    else:
        steps = args.steps or 28
        cfg = args.cfg or 5.0
        g = graph_illustrious(args.prompt, args.seed, args.w, args.h, steps, cfg)

    vt, vf0, name = vram()
    print(f"[{args.arch}] dev={name} vram_total={vt/1e9:.1f}G vram_free_before={vf0/1e9:.1f}G "
          f"steps={steps} cfg={cfg} seed={args.seed} {args.w}x{args.h}", flush=True)

    t0 = time.monotonic()
    sub = http_json("/prompt", {"prompt": g, "client_id": "krea-eval"})
    if sub.get("node_errors"):
        print("NODE_ERRORS:", json.dumps(sub["node_errors"])[:1200]); sys.exit(2)
    pid = sub["prompt_id"]
    print("prompt_id", pid, flush=True)

    deadline = t0 + 300
    out_img = None
    while time.monotonic() < deadline:
        time.sleep(0.7)
        hist = http_json(f"/history/{pid}")
        if pid not in hist:
            continue
        entry = hist[pid]
        st = entry.get("status", {})
        if st.get("status_str") == "error":
            print("RUN_ERROR:", json.dumps(st.get("messages", []))[:1500]); sys.exit(3)
        for node_out in entry.get("outputs", {}).values():
            for img in node_out.get("images", []):
                out_img = img
                break
            if out_img:
                break
        if out_img:
            break
    if not out_img:
        print("TIMEOUT / no image"); sys.exit(4)

    elapsed = time.monotonic() - t0
    q = urllib.parse.urlencode({"filename": out_img["filename"],
                                "subfolder": out_img.get("subfolder", ""),
                                "type": out_img.get("type", "output")})
    png = http_bytes(f"/view?{q}")
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(png)
    vt, vf1, _ = vram()
    print(f"OK elapsed={elapsed:.1f}s bytes={len(png)} out={args.out} "
          f"vram_used_during~={(vf0-vf1)/1e9:.1f}G vram_free_after={vf1/1e9:.1f}G", flush=True)


if __name__ == "__main__":
    main()
