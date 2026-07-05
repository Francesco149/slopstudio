"""Model-free reference provider.

Implements the `speech` capability without any model: it writes a short, deterministic
placeholder WAV plus naive word timings. Its purpose is to let the editor (and the
protocol itself) be developed and tested against a live endpoint before the GPU-backed
providers exist. Same protocol, same job lifecycle, same cache — just instant + fake.
"""
from __future__ import annotations

import os

import numpy as np
import soundfile as sf

from ..base import Asset, Provider, Result, TypeSpec, make_app


async def _speech(params: dict, inputs: list, ctx) -> Result:
    text = str(params.get("text", ""))
    seed = int(params.get("seed", 0))
    sr = 48000
    dur = max(0.4, 0.06 * len(text))  # ~natural-ish reading length placeholder

    await ctx.report(0.3, "synthesizing")
    n = int(sr * dur)
    t = np.arange(n) / sr
    freq = 180 + (seed % 5) * 20  # faint deterministic tone, non-empty + audible
    wav = (0.02 * np.sin(2 * np.pi * freq * t)).astype("float32")

    h = ctx.job.hash
    sf.write(ctx.cache.asset_path(h, "wav"), wav, sr)

    await ctx.report(0.9, "timing")
    words = text.split()
    timings = []
    if words:
        per = dur / len(words)
        timings = [
            {"w": w, "t0": round(i * per, 3), "t1": round((i + 1) * per, 3)}
            for i, w in enumerate(words)
        ]

    return Result(
        assets=[
            Asset(
                "audio",
                ctx.cache.uri(h, "wav"),
                {"duration": round(dur, 3), "sample_rate": sr, "word_timings": timings},
            )
        ],
        provenance=None,
    )


provider = Provider(
    "mock-tts",
    "mock@0.1",
    cache_dir=os.environ.get("SLOP_CACHE", "cache/mock"),
)
provider.register(
    TypeSpec(
        "speech",
        _speech,
        params_schema={
            "type": "object",
            "properties": {
                "text": {"type": "string"},
                "voice_preset": {"type": "string"},
                "instruct": {"type": "string", "title": "Direction (emotion/intonation)"},
                "seed": {"type": "integer", "default": 0},
            },
            "required": ["text"],
        },
        outputs=["audio"],
        returns_timings=True,
    )
)

app = make_app(provider)
