"""rembg provider — background removal / alpha matting for images.

One capability, ``remove_bg``: take an image and return an **RGBA cutout** with the
background keyed to alpha. Where the editor's built-in colour-key (`removebg`
`method:"colorkey"`) only handles a FLAT background, this segments the subject by SHAPE —
so it cuts purple-hair-on-purple and soft/gradient backgrounds the key can't (the same
``isnet-anime`` matte the avatar sprite pipeline uses, ``tools/cutout-sprites.sh``).

Engine: **rembg** (MIT) over ONNX models (downloaded once to ``U2NET_HOME``). Models:
``isnet-anime`` (default — the anime host + stylised art), ``isnet-general-use`` /
``birefnet-general`` (photos), ``u2net`` (classic), ``u2net_human_seg`` (people). CPU by
default — a single ~1k image is a few seconds and it dodges the 3080/7800XT gen contention;
``onnxruntime-gpu`` can slot in later via the same handler.

The image arrives **base64 in params** (``image_b64``) — the editor↔provider transport for
editor-local media on this NAT'd topology (same as the image provider's ``init_b64``), since
a library item on wslop isn't in lame's content-addressed cache. The bytes fold into the
job hash, so re-cutting the same image+model is a cache hit.

Heavy deps (``rembg``/``onnxruntime``) are **lazy-imported**, so this module imports fine on
the editor host. Set ``SLOP_REMBG_FAKE=1`` for a deterministic, model-free corner-key path —
used by the unit tests and as graceful degradation when the model/runtime is absent.
"""
from __future__ import annotations

import asyncio
import base64
import io
import os
import threading

from ..base import Asset, Provider, Result, TypeSpec, make_app

# Models we advertise. rembg ships more; these are the commercial-safe, generally-useful set.
_MODELS = ["isnet-anime", "isnet-general-use", "birefnet-general", "u2net", "u2net_human_seg", "silueta"]
_DEFAULT_MODEL = os.environ.get("REMBG_MODEL", "isnet-anime")

_sessions: dict = {}  # model name → rembg session (each loads one ONNX graph; cached resident)
_load_lock = threading.Lock()


def _fake() -> bool:
    return os.environ.get("SLOP_REMBG_FAKE", "") not in ("", "0", "false", "False")


def _session(model: str):
    """Lazily build + cache a rembg session for `model` (downloads the ONNX once)."""
    from rembg import new_session

    with _load_lock:
        if model not in _sessions:
            _sessions[model] = new_session(model)
        return _sessions[model]


# ── fake / model-free matte (tests + no-runtime degradation) ─────────────────
def _fake_cutout(png: bytes, fuzz: int = 32) -> tuple[bytes, int, int]:
    """Deterministic stand-in for the segmenter: key out pixels near the 4-corner mean
    colour (the flat-bg case). Exercises the asset write + alpha + dims without ONNX."""
    from PIL import Image
    import numpy as np

    im = Image.open(io.BytesIO(png)).convert("RGBA")
    a = np.asarray(im).astype(np.int16)
    h, w = a.shape[:2]
    corners = np.stack([a[0, 0, :3], a[0, w - 1, :3], a[h - 1, 0, :3], a[h - 1, w - 1, :3]])
    key = corners.mean(axis=0)
    dist2 = ((a[:, :, :3] - key) ** 2).sum(axis=2)
    out = a.copy()
    out[:, :, 3] = np.where(dist2 <= fuzz * fuzz, 0, out[:, :, 3])
    buf = io.BytesIO()
    Image.fromarray(out.astype("uint8"), "RGBA").save(buf, format="PNG")
    return buf.getvalue(), w, h


# ── handler ───────────────────────────────────────────────────────────────
def _decode_input(params: dict) -> bytes:
    b64 = params.get("image_b64")
    if not b64:
        raise ValueError("remove_bg requires `image_b64` (base64-encoded source image)")
    return base64.b64decode(b64)


def _do_remove_bg(params, cache, h) -> Result:
    src = _decode_input(params)
    model = params.get("model", _DEFAULT_MODEL)
    if _fake():
        out, w, ht = _fake_cutout(src)
        engine = "fake"
    else:
        from rembg import remove
        from PIL import Image

        out = remove(
            src,
            session=_session(model),
            alpha_matting=bool(params.get("alpha_matting", False)),
            post_process_mask=bool(params.get("post_process", True)),
            only_mask=bool(params.get("only_mask", False)),
        )
        engine = model
        im = Image.open(io.BytesIO(out))
        w, ht = im.size
    path = cache.asset_path(h, "png")
    with open(path, "wb") as f:
        f.write(out)
    return Result(assets=[Asset("image", cache.uri(h, "png"),
                                {"model": model, "engine": engine, "w": w, "h": ht,
                                 "alpha_matting": bool(params.get("alpha_matting", False)),
                                 "only_mask": bool(params.get("only_mask", False))})])


async def _remove_bg(params, inputs, ctx) -> Result:
    await ctx.report(0.2, "segmenting subject")
    # Blocking ONNX inference + file write run in the executor, never on the event loop
    # (a slow matte must not freeze GET /jobs / the WS progress stream — the align lesson).
    return await asyncio.get_event_loop().run_in_executor(
        None, _do_remove_bg, params, ctx.cache, ctx.job.hash)


# ── provider wiring ─────────────────────────────────────────────────────────
provider = Provider(
    "rembg",
    os.environ.get("REMBG_VERSION", "rembg-2.0+isnet-anime@2026.01"),
    cache_dir=os.environ.get("SLOP_CACHE", "cache/rembg"),
    concurrency=1,
)

provider.register(TypeSpec(
    "remove_bg", _remove_bg,
    params_schema={
        "type": "object",
        "properties": {
            "image_b64": {"type": "string", "title": "Source image (base64 PNG)"},
            "model": {"type": "string", "enum": _MODELS, "default": _DEFAULT_MODEL,
                      "title": "Segmentation model (isnet-anime = the host/stylised art)"},
            "alpha_matting": {"type": "boolean", "default": False,
                              "title": "Alpha matting (finer hair/edges; slower)"},
            "post_process": {"type": "boolean", "default": True, "title": "Post-process the mask"},
            "only_mask": {"type": "boolean", "default": False, "title": "Return the matte (grayscale) only"},
        },
        "required": ["image_b64"],
    },
    outputs=["image"],
    presets=_MODELS,
    returns_timings=True,
))

app = make_app(provider)
