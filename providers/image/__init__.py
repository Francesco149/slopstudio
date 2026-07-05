"""image provider — a thin adapter over ComfyUI (docs/ARCHITECTURE.md §6).

ComfyUI is the actual engine (SDXL / Illustrious-XL 2.0 on lame's 7800XT via ROCm); this
provider owns the workflow template, injects params, submits to ComfyUI's HTTP API, waits,
and returns the produced image as a slopstudio asset. So a new model/workflow is a template
change, not editor code.

Capability **text2image**: reaction pics + the host (base Illustrious-XL; the trained
Gemma LoRA drops in via an optional `lora` param + a LoraLoader node). The same capability
also does **img2img** and **inpaint** when the job carries an init image — the lever the
avatar sprite set needs: redraw only the mouth/eyes from a fixed base so the body never
jumps (txt2img with a fixed seed does NOT register across tag changes). Pass:
  - ``init_b64`` (base64 PNG) + ``denoise`` (<1.0) ⇒ img2img: keeps the init's low-freq
    structure (pose/scale/orientation), redraws high-freq detail the prompt changed.
  - additionally ``mask_b64`` (base64 PNG, white = repaint) ⇒ inpaint: only the masked
    region is renoised (SetLatentNoiseMask), the rest is copied pixel-exact from the init.
The adapter has no torch — it just speaks HTTP to ComfyUI (``COMFYUI_URL``, default
http://comfyui:8188 on the shared `slopnet`); init/mask images are pushed to ComfyUI's input
dir via its ``/upload/image`` endpoint and referenced by LoadImage/LoadImageMask. Set
``SLOP_IMAGE_FAKE=1`` for a deterministic model-free path (tests + graceful degradation when
ComfyUI is down): it writes a small solid-color PNG.

ComfyUI API flow: POST /prompt {prompt: graph, client_id} → {prompt_id}; poll
/history/{prompt_id} until the entry appears (success/error); the SaveImage node's outputs
list {filename, subfolder, type}; GET /view?... streams the bytes.
"""
from __future__ import annotations

import asyncio
import base64
import json
import os
import urllib.parse
import urllib.request
from pathlib import Path

from ..base import Asset, Provider, Result, TypeSpec, make_app

_COMFY = os.environ.get("COMFYUI_URL", "http://comfyui:8188").rstrip("/")
_CKPT = os.environ.get("IMAGE_CHECKPOINT", "Illustrious-XL-v2.0.safetensors")
# Krea-2 (Qwen-Image DiT) path — selected by params['arch']=='krea2' (ADDITIVE; the default
# SDXL/Illustrious graph is unchanged). Models on lame: diffusion_models/krea2_turbo_fp8_scaled,
# text_encoders/qwen3vl_4b_fp8_scaled, vae/qwen_image_vae. License is the revocable <$1M Krea-2
# Community License (see docs/research/image-models-2026-06.md) — eval only, weigh vs Apache Qwen-Image.
_KREA2_UNET = os.environ.get("KREA2_UNET", "krea2_turbo_fp8_scaled.safetensors")
# Qwen-Image-2512 (the "Qwen-Image-2.0" Dec-2025 update; same Qwen-Image MMDiT family as Krea-2,
# but Apache-2.0 — see docs/research/image-models-2026-06.md) — selected by params['arch']=='qwen-image'
# (ADDITIVE; the default SDXL/Illustrious graph is unchanged, and 'krea2' still routes to Krea above).
# Comfy-Org ComfyUI-ready files (lame: diffusion_models/qwen_image_2512_fp8_e4m3fn,
# text_encoders/qwen_2.5_vl_7b_fp8_scaled [Qwen2.5-VL-7B, CLIPLoader type 'qwen_image'], vae/qwen_image_vae
# [byte-identical to the Krea VAE → reused]). NOT distilled like Krea-Turbo: real CFG + negative +
# a ModelSamplingAuraFlow shift, per ComfyUI's bundled image_qwen_image template.
_QWEN_UNET = os.environ.get("QWEN_IMAGE_UNET", "qwen_image_2512_fp8_e4m3fn.safetensors")
_QWEN_CLIP = os.environ.get("QWEN_IMAGE_CLIP", "qwen_2.5_vl_7b_fp8_scaled.safetensors")
_QWEN_VAE = os.environ.get("QWEN_IMAGE_VAE", "qwen_image_vae.safetensors")
# Anima base-v1.0 (CircleStone Labs × Comfy-Org, anime finetune of NVIDIA Cosmos-Predict2-2B;
# weights are NC but OUTPUTS are commercial-OK → carry a "Built on NVIDIA Cosmos" attribution; see
# docs/research/image-models-2026-06.md) — selected by params['arch']=='anima' (ADDITIVE; SDXL/krea2/
# qwen paths untouched). This is the GOLDEN host engine: the locally-trained `gemma-san-anima` LoRA
# (model-only, network_train_unet_only) locks the character + is stable across framings — pass it via
# `lora` (+ `lora_strength` ~0.9). Chibi-vs-tall is a prompt knob: say "chibi" to force chibi, or use
# "cowboy shot"/"action pose" for the occasional full-height succubus. Files on lame: diffusion_models/
# anima-base-v1.0, text_encoders/qwen_3_06b_base (CLIPLoader type 'stable_diffusion'), vae/qwen_image_vae.
_ANIMA_UNET = os.environ.get("ANIMA_UNET", "anima-base-v1.0.safetensors")
_ANIMA_CLIP = os.environ.get("ANIMA_CLIP", "qwen_3_06b_base.safetensors")
_ANIMA_VAE = os.environ.get("ANIMA_VAE", "qwen_image_vae.safetensors")
_ANIMA_NEG = os.environ.get(
    "ANIMA_NEG", "worst quality, low quality, score_1, score_2, score_3, blurry, jpeg artifacts, sepia")
_WORKFLOWS = Path(__file__).parent / "workflows"
# Illustrious is a booru-tag SDXL model; a sane default negative keeps reaction pics clean.
_DEFAULT_NEG = os.environ.get(
    "IMAGE_NEG",
    "worst quality, low quality, bad anatomy, bad hands, jpeg artifacts, watermark, signature, text",
)


def _fake() -> bool:
    return os.environ.get("SLOP_IMAGE_FAKE", "") not in ("", "0", "false", "False")


def _http_json(url: str, payload: dict | None = None, timeout: float = 30.0) -> dict:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"content-type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers, method="POST" if data else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def _http_bytes(url: str, timeout: float = 60.0) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def _upload_image(png: bytes, name: str, timeout: float = 60.0) -> str:
    """POST a PNG to ComfyUI's input dir (/upload/image) so LoadImage can reference it.
    Returns the LoadImage filename ('subfolder/name' or just 'name'). Overwrites by name so
    a re-run of the same job reuses the slot (jobs are serial; concurrency=1)."""
    boundary = "----slopstudio-image-upload"
    parts = [
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="image"; filename="{name}"\r\n'
        f"Content-Type: image/png\r\n\r\n".encode() + png + b"\r\n",
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="overwrite"\r\n\r\ntrue\r\n'.encode(),
        f"--{boundary}--\r\n".encode(),
    ]
    body = b"".join(parts)
    req = urllib.request.Request(
        f"{_COMFY}/upload/image", data=body, method="POST",
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        resp = json.loads(r.read().decode())
    sub = resp.get("subfolder", "")
    return f"{sub}/{resp['name']}" if sub else resp["name"]


def _build_krea2_graph(params: dict) -> dict:
    """Krea-2 Turbo (Qwen-Image DiT) text2image — mirrors Comfy-Org's bundled
    `image_krea2_turbo_t2i.json` (UNETLoader + CLIPLoader[type=krea2] + VAELoader, a
    ConditioningZeroOut negative, KSampler 8 steps / cfg 1 / euler / simple). Selected by
    ``arch='krea2'``; the SDXL path below is untouched. NOTE: img2img/inpaint + SDXL LoRAs do
    NOT apply here (different arch) — an optional ``lora`` is wired as a Krea *style* LoRA
    (LoraLoaderModelOnly, e.g. ``krea2_retroanime``)."""
    g = json.loads((_WORKFLOWS / "krea2_text2image.json").read_text())
    g["10"]["inputs"]["unet_name"] = params.get("checkpoint", _KREA2_UNET)
    g["6"]["inputs"]["text"] = params.get("prompt", "")
    g["5"]["inputs"]["width"] = int(params.get("width", 1024))
    g["5"]["inputs"]["height"] = int(params.get("height", 1024))
    k = g["3"]["inputs"]
    k["seed"] = int(params.get("seed", 0))
    k["steps"] = int(params.get("steps", 8))      # distilled turbo default
    k["cfg"] = float(params.get("cfg", 1.0))       # turbo runs CFG 1 (negative is zeroed-out)
    k["sampler_name"] = params.get("sampler", "euler")
    k["scheduler"] = params.get("scheduler", "simple")
    lora = params.get("lora", "")
    if lora:  # Krea style LoRA, model-only, inserted between the UNET and the sampler
        lora_name = lora if lora.endswith(".safetensors") else lora + ".safetensors"
        g["15"] = {"class_type": "LoraLoaderModelOnly", "inputs": {
            "lora_name": lora_name, "strength_model": float(params.get("lora_strength", 1.0)),
            "model": ["10", 0]}}
        g["3"]["inputs"]["model"] = ["15", 0]
    return g


def _build_qwen_image_graph(params: dict) -> dict:
    """Qwen-Image-2512 text2image (Qwen-Image MMDiT, Apache-2.0) — mirrors ComfyUI's bundled
    `image_qwen_image` template: UNETLoader + CLIPLoader[type=qwen_image] + VAELoader, a
    ModelSamplingAuraFlow shift, a REAL negative CLIPTextEncode (base model is not distilled →
    needs CFG>1), KSampler 20 steps / cfg 4 / euler / simple at the native 1328². Selected by
    ``arch='qwen-image'``; the SDXL/Illustrious path is untouched. An optional ``lora`` is wired
    model-only (e.g. a Qwen-Image-Lightning turbo LoRA), inserted UNET→LoRA→ModelSamplingAuraFlow.
    img2img/inpaint + SDXL LoRAs do NOT apply here (different arch)."""
    g = json.loads((_WORKFLOWS / "qwen_image_text2image.json").read_text())
    g["10"]["inputs"]["unet_name"] = params.get("checkpoint", _QWEN_UNET)
    g["11"]["inputs"]["clip_name"] = params.get("text_encoder", _QWEN_CLIP)
    g["12"]["inputs"]["vae_name"] = params.get("vae", _QWEN_VAE)
    g["6"]["inputs"]["text"] = params.get("prompt", "")
    g["7"]["inputs"]["text"] = params.get("negative", "")  # real negative; default empty
    g["5"]["inputs"]["width"] = int(params.get("width", 1328))
    g["5"]["inputs"]["height"] = int(params.get("height", 1328))
    g["16"]["inputs"]["shift"] = float(params.get("shift", 3.1))
    k = g["3"]["inputs"]
    k["seed"] = int(params.get("seed", 0))
    k["steps"] = int(params.get("steps", 20))     # base default (8 with a Lightning LoRA)
    k["cfg"] = float(params.get("cfg", 4.0))       # base runs real CFG (not the turbo CFG-1)
    k["sampler_name"] = params.get("sampler", "euler")
    k["scheduler"] = params.get("scheduler", "simple")
    lora = params.get("lora", "")
    if lora:  # Qwen-Image LoRA, model-only, inserted between the UNET and ModelSamplingAuraFlow
        lora_name = lora if lora.endswith(".safetensors") else lora + ".safetensors"
        g["15"] = {"class_type": "LoraLoaderModelOnly", "inputs": {
            "lora_name": lora_name, "strength_model": float(params.get("lora_strength", 1.0)),
            "model": ["10", 0]}}
        g["16"]["inputs"]["model"] = ["15", 0]
    return g


def _build_anima_graph(params: dict) -> dict:
    """Anima base-v1.0 (anime finetune of NVIDIA Cosmos-Predict2-2B; weights NC, OUTPUTS
    commercial-OK) text2image — mirrors ComfyUI's bundled `image_anima_base_v1` template:
    UNETLoader + CLIPLoader[type=stable_diffusion] + VAELoader, a REAL negative CLIPTextEncode
    (base is not distilled → needs CFG>1), KSampler 30 / cfg 4 / er_sde / simple, plain
    EmptyLatentImage. Selected by ``arch='anima'``; the SDXL/krea2/qwen paths are untouched.
    The GOLDEN host engine: the trained ``gemma-san-anima`` LoRA locks the character — pass it via
    ``lora`` (+ ``lora_strength`` ~0.9), wired model-only (it was trained unet-only).
    img2img/inpaint + SDXL LoRAs do NOT apply here (different arch)."""
    g = json.loads((_WORKFLOWS / "anima_text2image.json").read_text())
    g["10"]["inputs"]["unet_name"] = params.get("checkpoint", _ANIMA_UNET)
    g["11"]["inputs"]["clip_name"] = params.get("text_encoder", _ANIMA_CLIP)
    g["12"]["inputs"]["vae_name"] = params.get("vae", _ANIMA_VAE)
    g["6"]["inputs"]["text"] = params.get("prompt", "")
    g["7"]["inputs"]["text"] = params.get("negative", _ANIMA_NEG)  # real negative (CFG>1)
    g["5"]["inputs"]["width"] = int(params.get("width", 1024))
    g["5"]["inputs"]["height"] = int(params.get("height", 1024))
    k = g["3"]["inputs"]
    k["seed"] = int(params.get("seed", 0))
    k["steps"] = int(params.get("steps", 30))
    k["cfg"] = float(params.get("cfg", 4.0))
    k["sampler_name"] = params.get("sampler", "er_sde")
    k["scheduler"] = params.get("scheduler", "simple")
    lora = params.get("lora", "")
    if lora:  # gemma-san-anima host LoRA, model-only, inserted between the UNET and the sampler
        lora_name = lora if lora.endswith(".safetensors") else lora + ".safetensors"
        g["15"] = {"class_type": "LoraLoaderModelOnly", "inputs": {
            "lora_name": lora_name, "strength_model": float(params.get("lora_strength", 0.9)),
            "model": ["10", 0]}}
        g["3"]["inputs"]["model"] = ["15", 0]
    return g


def _build_graph(params: dict) -> dict:
    arch = str(params.get("arch", "")).lower()
    if arch in ("krea2", "krea-2"):
        return _build_krea2_graph(params)
    if arch in ("qwen-image", "qwen_image", "qwen-image-2512", "qwen-image-2.0", "qwen-image-2"):
        return _build_qwen_image_graph(params)
    if arch == "anima":
        return _build_anima_graph(params)
    g = json.loads((_WORKFLOWS / "text2image.json").read_text())
    g["4"]["inputs"]["ckpt_name"] = params.get("checkpoint", _CKPT)
    g["6"]["inputs"]["text"] = params.get("prompt", "")
    g["7"]["inputs"]["text"] = params.get("negative", _DEFAULT_NEG)
    g["5"]["inputs"]["width"] = int(params.get("width", 1024))
    g["5"]["inputs"]["height"] = int(params.get("height", 1024))
    k = g["3"]["inputs"]
    k["seed"] = int(params.get("seed", 0))
    k["steps"] = int(params.get("steps", 28))
    k["cfg"] = float(params.get("cfg", 5.0))
    k["sampler_name"] = params.get("sampler", "euler_ancestral")
    k["scheduler"] = params.get("scheduler", "normal")
    # optional LoRA (e.g. the trained gemma-san chibi): insert a LoraLoader between the
    # checkpoint and the sampler/clip so a `lora` param yields the consistent host on demand.
    lora = params.get("lora", "")
    if lora:
        lora_name = lora if lora.endswith(".safetensors") else lora + ".safetensors"
        strength = float(params.get("lora_strength", 0.85))
        g["10"] = {"class_type": "LoraLoader", "inputs": {
            "lora_name": lora_name, "strength_model": strength, "strength_clip": strength,
            "model": ["4", 0], "clip": ["4", 1]}}
        g["3"]["inputs"]["model"] = ["10", 0]      # KSampler reads the LoRA-patched model
        g["6"]["inputs"]["clip"] = ["10", 1]       # positive/negative read the LoRA-patched clip
        g["7"]["inputs"]["clip"] = ["10", 1]
    # img2img / inpaint: when an init image is supplied (uploaded by the handler), drive the
    # sampler from a VAE-encoded init latent — denoise<1 keeps the init's low-freq structure
    # (pose/scale/orientation), so only what the prompt changed gets redrawn. A mask makes it
    # an inpaint: SetLatentNoiseMask renoises only the white region; everything else is copied
    # pixel-exact from the init. The init's size wins, so width/height are ignored here.
    init_file = params.get("_init_file")
    if init_file:
        g["11"] = {"class_type": "LoadImage", "inputs": {"image": init_file}}
        g["12"] = {"class_type": "VAEEncode", "inputs": {"pixels": ["11", 0], "vae": ["4", 2]}}
        latent = ["12", 0]
        mask_file = params.get("_mask_file")
        if mask_file:
            g["13"] = {"class_type": "LoadImageMask", "inputs": {"image": mask_file, "channel": "red"}}
            mask_src = ["13", 0]
            grow = int(params.get("mask_grow", 8))
            if grow > 0:  # feather past the hard mask edge so the repaint blends in
                g["15"] = {"class_type": "GrowMask", "inputs": {
                    "mask": ["13", 0], "expand": grow, "tapered_corners": True}}
                mask_src = ["15", 0]
            g["14"] = {"class_type": "SetLatentNoiseMask", "inputs": {
                "samples": ["12", 0], "mask": mask_src}}
            latent = ["14", 0]
        g["3"]["inputs"]["latent_image"] = latent
        g["3"]["inputs"]["denoise"] = float(params.get("denoise", 0.55))
        g.pop("5", None)  # EmptyLatentImage unused in img2img (init defines the latent + size)
    return g


def _comfy_run(graph: dict, dest_png: str) -> dict:
    """Submit the graph to ComfyUI, wait for the image, write it to dest_png. Blocking."""
    client_id = "slop-image"
    sub = _http_json(f"{_COMFY}/prompt", {"prompt": graph, "client_id": client_id})
    if sub.get("node_errors"):
        raise RuntimeError(f"comfy node_errors: {json.dumps(sub['node_errors'])[:300]}")
    pid = sub["prompt_id"]
    import time

    deadline = time.monotonic() + float(os.environ.get("IMAGE_TIMEOUT", "240"))
    while time.monotonic() < deadline:
        time.sleep(0.6)
        hist = _http_json(f"{_COMFY}/history/{pid}")
        if pid not in hist:
            continue
        entry = hist[pid]
        status = entry.get("status", {})
        if status.get("status_str") == "error":
            raise RuntimeError(f"comfy error: {json.dumps(status.get('messages', []))[:300]}")
        for node_out in entry.get("outputs", {}).values():
            for img in node_out.get("images", []):
                q = urllib.parse.urlencode(
                    {"filename": img["filename"], "subfolder": img.get("subfolder", ""),
                     "type": img.get("type", "output")}
                )
                Path(dest_png).write_bytes(_http_bytes(f"{_COMFY}/view?{q}"))
                return {"filename": img["filename"]}
        # entry present but no images → completed without output
        raise RuntimeError("comfy produced no image")
    raise TimeoutError("comfy generation timed out")


def _fake_png(dest_png: str, params: dict):
    """Deterministic solid-color PNG (no ComfyUI) keyed by seed — for tests / degradation."""
    import struct
    import zlib

    w, h = int(params.get("width", 64)), int(params.get("height", 64))
    w, h = min(w, 64), min(h, 64)  # tiny for tests
    seed = int(params.get("seed", 0))
    r, gc, b = (seed * 53) % 256, (seed * 97) % 256, (seed * 193) % 256
    row = b"\x00" + bytes([r, gc, b]) * w
    raw = row * h

    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    Path(dest_png).write_bytes(png)


async def _text2image(params, inputs, ctx) -> Result:
    h = ctx.job.hash
    dest = ctx.cache.asset_path(h, "png")
    await ctx.report(0.2, "queueing" if not _fake() else "rendering")
    if _fake():
        _fake_png(dest, params)
    else:
        p = dict(params)  # copy: we stash uploaded filenames without touching the hashed job
        loop = asyncio.get_event_loop()
        if p.get("init_b64"):  # img2img / inpaint: push the init (+mask) to ComfyUI's input dir
            init_png = base64.b64decode(p["init_b64"])
            p["_init_file"] = await loop.run_in_executor(None, _upload_image, init_png, f"slopinit_{h}.png")
            if p.get("mask_b64"):
                mask_png = base64.b64decode(p["mask_b64"])
                p["_mask_file"] = await loop.run_in_executor(None, _upload_image, mask_png, f"slopmask_{h}.png")
        graph = _build_graph(p)
        await ctx.report(0.4, "sampling")
        await loop.run_in_executor(None, _comfy_run, graph, dest)
    arch = str(params.get("arch", "")).lower()
    default_model = (_ANIMA_UNET if arch == "anima" else _KREA2_UNET if arch.startswith("krea")
                     else _QWEN_UNET if arch.startswith("qwen") else _CKPT)
    return Result(assets=[Asset("image", ctx.cache.uri(h, "png"),
                                {"width": int(params.get("width", 1024)),
                                 "height": int(params.get("height", 1024)),
                                 "model": params.get("checkpoint") or default_model,
                                 "seed": int(params.get("seed", 0))})])


# ── capability: upscale — a STRICT anime super-resolution (NOT img2img/diffusion). The image
#    content is unchanged; the ESRGAN model just adds line/edge detail at higher res. Used to make
#    the host sprites / reaction images sharper (img2img drifts the design, so it's not an option). ──
_UPSCALE_MODEL = os.environ.get("UPSCALE_MODEL", "RealESRGAN_x4plus_anime_6B.pth")

def _build_upscale_graph(params: dict) -> dict:
    """LoadImage -> UpscaleModelLoader -> ImageUpscaleWithModel -> SaveImage. `upscale_model`
    selects the .pth in ComfyUI's models/upscale_models (default the anime RealESRGAN 4x)."""
    return {
        "1": {"class_type": "LoadImage", "inputs": {"image": params["_init_file"]}},
        "2": {"class_type": "UpscaleModelLoader",
              "inputs": {"model_name": params.get("upscale_model", _UPSCALE_MODEL)}},
        "3": {"class_type": "ImageUpscaleWithModel",
              "inputs": {"upscale_model": ["2", 0], "image": ["1", 0]}},
        "9": {"class_type": "SaveImage", "inputs": {"filename_prefix": "slopupscale", "images": ["3", 0]}},
    }


async def _upscale(params, inputs, ctx) -> Result:
    """Strict anime super-resolution of `init_b64` — content preserved, just higher-res."""
    h = ctx.job.hash
    dest = ctx.cache.asset_path(h, "png")
    if not params.get("init_b64"):
        raise ValueError("upscale requires init_b64")
    await ctx.report(0.2, "queueing" if not _fake() else "upscaling")
    if _fake():
        _fake_png(dest, params)
    else:
        p = dict(params)
        loop = asyncio.get_event_loop()
        init_png = base64.b64decode(p["init_b64"])
        p["_init_file"] = await loop.run_in_executor(None, _upload_image, init_png, f"slopup_{h}.png")
        await ctx.report(0.4, "upscaling")
        await loop.run_in_executor(None, _comfy_run, _build_upscale_graph(p), dest)
    return Result(assets=[Asset("image", ctx.cache.uri(h, "png"),
                                {"model": params.get("upscale_model", _UPSCALE_MODEL), "op": "upscale"})])


provider = Provider(
    "image",
    os.environ.get("IMAGE_VERSION", "comfyui+illustrious-xl-2.0@2026.01"),
    cache_dir=os.environ.get("SLOP_CACHE", "cache/image"),
    concurrency=1,  # one GPU
)

provider.register(TypeSpec(
    "text2image", _text2image,
    params_schema={
        "type": "object",
        "properties": {
            "prompt": {"type": "string"},
            "negative": {"type": "string", "title": "Negative prompt"},
            "seed": {"type": "integer", "default": 0},
            "width": {"type": "integer", "default": 1024},
            "height": {"type": "integer", "default": 1024},
            "steps": {"type": "integer", "default": 28},
            "cfg": {"type": "number", "default": 5.0},
            "sampler": {"type": "string", "default": "euler_ancestral"},
            "checkpoint": {"type": "string", "title": "SDXL checkpoint / Krea-2 / Qwen-Image UNET"},
            "arch": {"type": "string", "title": "Arch: default SDXL; 'anima' = Anima-2B + gemma-san-anima LoRA (the host); 'qwen-image' = Qwen-Image-2512 (Apache); 'krea2' = Krea-2"},
            "shift": {"type": "number", "default": 3.1, "title": "Qwen-Image ModelSamplingAuraFlow shift"},
            "lora": {"type": "string", "title": "LoRA: gemma-san-anima (host, arch=anima, ~0.9); gemma-san-chibi-v2 (SDXL); or a krea2_*/qwen Lightning LoRA"},
            "lora_strength": {"type": "number", "default": 0.85},
            "init_b64": {"type": "string", "title": "Init image (base64 PNG) → img2img"},
            "mask_b64": {"type": "string", "title": "Inpaint mask (base64 PNG, white=repaint)"},
            "denoise": {"type": "number", "default": 0.55, "title": "img2img/inpaint denoise (<1)"},
            "mask_grow": {"type": "integer", "default": 8, "title": "Inpaint mask feather (px)"},
        },
        "required": ["prompt"],
    },
    outputs=["image"],
))

provider.register(TypeSpec(
    "upscale", _upscale,
    params_schema={
        "type": "object",
        "properties": {
            "init_b64": {"type": "string", "title": "Image to upscale (base64 PNG)"},
            "upscale_model": {"type": "string", "default": _UPSCALE_MODEL,
                              "title": "ESRGAN model in ComfyUI models/upscale_models (strict SR, no diffusion)"},
        },
        "required": ["init_b64"],
    },
    outputs=["image"],
))

app = make_app(provider)
