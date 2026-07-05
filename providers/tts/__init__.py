"""Qwen3-TTS provider — voice DESIGN + stable CLONE synthesis.

Wraps the Qwen3-TTS-12Hz-1.7B family (Apache-2.0). Two synthesis modes behind one `speech`
capability:

  - **clone (stable, the production path):** when the job carries a reference clip
    (`ref_b64` + `ref_text`), synthesize with the **Base** model via
    `create_voice_clone_prompt(ref_audio=(arr,sr), ref_text)` (cached per ref hash) +
    `generate_voice_clone(text, language, voice_clone_prompt)`. The clip defines the voice,
    so the timbre is identical across lines. This is the design-once→clone fix for
    VoiceDesign's per-call drift (docs/RESEARCH.md). Clone has no per-line `instruct`.
  - **design (exploration / baking refs):** no ref → the **VoiceDesign** model derives a
    voice from the `voice`+`emotion` instruct (`generate_voice_design`). Used to PREVIEW a
    new instruct and to bake a preset's golden reference clip (the `voice_design` capability).

One model is resident at a time (Base for clone, VoiceDesign for design); the provider SWAPS
on demand so the two 1.7B checkpoints + the align models all fit lame's 3080 (10 GB). The
hot path (clone) stays on Base; baking/previewing a design swaps to VoiceDesign briefly.

Heavy deps (`torch`, `qwen_tts`, `soundfile`) are lazy-imported; models load on first use into
the persisted HF cache (deploy/lame/deploy-tts.sh). CUDA docker on lame's RTX 3080.
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import io
import os

from ..base import Asset, Provider, Result, TypeSpec, make_app

_DESIGN_ID = os.environ.get("QWEN_TTS_DESIGN_MODEL", "Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign")
_CLONE_ID = os.environ.get("QWEN_TTS_CLONE_MODEL", "Qwen/Qwen3-TTS-12Hz-1.7B-Base")
_DEFAULT_VOICE = "A clear, expressive young female voice, natural and warm."

_model = None            # the resident Qwen3TTSModel
_model_id = None         # which variant is resident (swap when a job needs the other)
_clone_prompts = {}      # ref-hash → prompt_items (only valid for the resident clone model)
_load_lock = asyncio.Lock()


def _load_model(model_id: str):
    import torch
    from qwen_tts import Qwen3TTSModel

    return Qwen3TTSModel.from_pretrained(
        model_id,
        device_map=os.environ.get("QWEN_TTS_DEVICE", "cuda:0"),
        dtype=torch.bfloat16,
        attn_implementation=os.environ.get("QWEN_TTS_ATTN", "sdpa"),
    )


def _free_model():
    global _model, _model_id, _clone_prompts
    import gc
    _model = None
    _model_id = None
    _clone_prompts = {}      # prompts are tied to the freed model instance
    gc.collect()
    try:
        import torch
        torch.cuda.empty_cache()
    except Exception:
        pass


async def _model_handle(ctx, model_id: str):
    """Return the requested model variant, swapping the resident one out if needed (so only
    one ~1.7B checkpoint is on the GPU at a time, leaving room for the align models)."""
    global _model, _model_id
    if _model is not None and _model_id == model_id:
        return _model
    async with _load_lock:
        if _model is not None and _model_id == model_id:
            return _model
        loop = asyncio.get_event_loop()
        if _model is not None:
            await ctx.report(0.1, "freeing " + _model_id.split("/")[-1])
            await loop.run_in_executor(None, _free_model)
        await ctx.report(0.15, "loading " + model_id.split("/")[-1])
        m = await loop.run_in_executor(None, _load_model, model_id)
        _model = m
        _model_id = model_id
    return _model


def _compose_instruct(voice: str, emotion: str) -> str:
    voice = (voice or _DEFAULT_VOICE).strip()
    emotion = (emotion or "").strip()
    return f"{voice} {emotion}".strip() if emotion else voice


def _synth_design(model, text, language, instruct, seed):
    import torch
    if seed is not None:
        torch.manual_seed(int(seed))
    wavs, sr = model.generate_voice_design(text=text, language=language, instruct=instruct)
    return wavs[0], int(sr)


def _synth_clone(model, text, language, ref_b64, ref_text, seed):
    """Clone from a reference clip — stable timbre across lines. The clone prompt (speaker
    features) is built once per ref and cached, then reused for every line."""
    import torch
    if seed is not None:
        torch.manual_seed(int(seed))
    raw = base64.b64decode(ref_b64)
    key = hashlib.sha1(raw).hexdigest()
    prompt = _clone_prompts.get(key)
    if prompt is None:
        import soundfile as sf
        arr, rsr = sf.read(io.BytesIO(raw), dtype="float32")
        if getattr(arr, "ndim", 1) > 1:
            arr = arr.mean(axis=1)  # mono
        prompt = model.create_voice_clone_prompt(ref_audio=(arr, int(rsr)), ref_text=ref_text)
        _clone_prompts[key] = prompt
    wavs, sr = model.generate_voice_clone(text=text, language=language, voice_clone_prompt=prompt)
    return wavs[0], int(sr)


async def _speech(params, inputs, ctx) -> Result:
    import soundfile as sf

    text = params.get("text", "")
    language = params.get("language", "English")
    seed = params.get("seed", 0)
    ref_b64 = params.get("ref_b64", "")
    ref_text = params.get("ref_text", "")
    loop = asyncio.get_event_loop()

    if ref_b64 and ref_text:  # CLONE: stable timbre from the preset's golden reference clip
        model = await _model_handle(ctx, _CLONE_ID)
        await ctx.report(0.5, "cloning")
        wav, sr = await loop.run_in_executor(
            None, _synth_clone, model, text, language, ref_b64, ref_text, seed)
    else:                     # DESIGN: derive the voice from the instruct (drifts across lines)
        instruct = _compose_instruct(
            params.get("voice", ""), params.get("emotion", "") or params.get("instruct", ""))
        model = await _model_handle(ctx, _DESIGN_ID)
        await ctx.report(0.5, "synthesizing")
        wav, sr = await loop.run_in_executor(
            None, _synth_design, model, text, language, instruct, seed)

    h = ctx.job.hash
    sf.write(ctx.cache.asset_path(h, "wav"), wav, sr)
    dur = float(len(wav)) / float(sr)
    return Result(assets=[Asset("audio", ctx.cache.uri(h, "wav"),
                                {"duration": round(dur, 3), "sample_rate": sr})])


async def _voice_design(params, inputs, ctx) -> Result:
    """Design a voice from a description → a sample clip + a saveable preset blob (the
    one-shot 'create a voice' op; also used to bake a preset's golden reference clip)."""
    import json
    import soundfile as sf

    instruct = params.get("instruct", _DEFAULT_VOICE)
    name = params.get("name", "voice")
    language = params.get("language", "English")
    sample_text = params.get("sample_text", "Fufu~ this is what I sound like.")
    seed = params.get("seed", 0)

    model = await _model_handle(ctx, _DESIGN_ID)
    await ctx.report(0.5, "rendering sample")
    wav, sr = await asyncio.get_event_loop().run_in_executor(
        None, _synth_design, model, sample_text, language, instruct, seed)
    h = ctx.job.hash
    sf.write(ctx.cache.asset_path(h, "wav"), wav, sr)
    preset = {"name": name, "voice": instruct, "language": language, "seed": seed}
    with open(ctx.cache.asset_path(h, "preset.json"), "w") as f:
        json.dump(preset, f, indent=2)
    dur = float(len(wav)) / float(sr)
    return Result(assets=[
        Asset("audio", ctx.cache.uri(h, "wav"), {"duration": round(dur, 3), "sample_rate": sr}),
        Asset("preset", ctx.cache.uri(h, "preset.json"), preset),
    ])


provider = Provider(
    "qwen3-tts",
    os.environ.get("QWEN_TTS_VERSION", "qwen3-tts-1.7b-design+clone@2026.06"),
    cache_dir=os.environ.get("SLOP_CACHE", "cache/tts"),
    concurrency=1,  # one GPU, one model → serialize synthesis
)

provider.register(TypeSpec(
    "speech", _speech,
    params_schema={
        "type": "object",
        "properties": {
            "text": {"type": "string"},
            "voice": {"type": "string", "title": "Voice instruct (design mode; ignored when cloning)"},
            "emotion": {"type": "string", "title": "Per-line direction (design mode only)"},
            "ref_b64": {"type": "string", "title": "Reference clip (base64 WAV) → clone mode"},
            "ref_text": {"type": "string", "title": "Reference transcript (required with ref_b64)"},
            "language": {"type": "string", "default": "English"},
            "voice_preset": {"type": "string", "title": "Preset label (informational)"},
            "seed": {"type": "integer", "default": 0},
        },
        "required": ["text"],
    },
    outputs=["audio"],
    volatile_keys=("voice_preset",),  # label only; the voice is in `voice`/`ref_b64`
))
provider.register(TypeSpec(
    "voice_design", _voice_design,
    params_schema={
        "type": "object",
        "properties": {
            "instruct": {"type": "string", "title": "Voice description"},
            "name": {"type": "string"},
            "sample_text": {"type": "string"},
            "language": {"type": "string", "default": "English"},
            "seed": {"type": "integer", "default": 0},
        },
        "required": ["instruct"],
    },
    outputs=["audio", "preset"],
))

app = make_app(provider)
