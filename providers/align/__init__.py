"""align provider — word-level timing (WhisperX) + mouth visemes (Rhubarb).

Two capabilities feed the Phase-1 avatar spine (docs/ARCHITECTURE.md §7):

- **word_timing** — transcribe + force-align an audio asset → per-word ``[t0,t1]``.
  Drives captions and is a fallback lip-sync source. Engine: **WhisperX** (CUDA, on
  lame's 3080). Our TTS provider doesn't emit word timings, and imported audio has none,
  so this is the canonical source.
- **visemes** — phoneme→mouth-shape cues for the **pngtuber** state machine. Engine:
  **Rhubarb Lip Sync** (MIT, CPU), optionally guided by the line's ``dialog`` text for
  accuracy. Rhubarb's Preston-Blair shapes (A–H, X) map 1:1 to Gemma-san's mouth sprites;
  we also emit a normalized ``openness`` 0..1 so an editor with a smaller sprite set can
  still pick a mouth.

Heavy deps (``whisperx``/``torch``) are **lazy-imported** and Rhubarb is invoked as a
subprocess, so this module imports fine on the editor host; models/binaries load on lame.
Set ``SLOP_ALIGN_FAKE=1`` to force a deterministic, model-free path — used by the unit
tests and as graceful degradation when no GPU/binary is present.

Audio arrives via the protocol's ``inputs`` (a hash and/or uri). We resolve ``file://``
paths directly (shared storage), fetch ``http(s)://`` uris to a temp file (no shared
storage), and fall back to looking up ``<hash>.<ext>`` in our own cache (shared-cache
case). The input hash folds into the job hash, so re-aligning the same audio is a cache hit.
"""
from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
import tempfile
import threading
import urllib.request

from ..base import Asset, Provider, Result, TypeSpec, make_app

# Rhubarb Preston-Blair mouth shape → normalized openness (0 closed … 1 wide open).
# A=closed(PBM) B=slightly-open C=open(E/AE) D=wide(AA) E=rounded(O) F=puckered(U/W)
# G=F/V(teeth-on-lip) H=L(tongue) X=idle/rest.
_OPENNESS = {"X": 0.0, "A": 0.0, "B": 0.25, "C": 0.6, "D": 1.0, "E": 0.5, "F": 0.3, "G": 0.35, "H": 0.5}

_RHUBARB_BIN = os.environ.get("RHUBARB_BIN", "rhubarb")
_WHISPERX_MODEL = os.environ.get("WHISPERX_MODEL", "small")

_wx_model = None
_wx_align: dict = {}  # language_code → (model, metadata)
_load_lock = threading.Lock()


def _fake() -> bool:
    return os.environ.get("SLOP_ALIGN_FAKE", "") not in ("", "0", "false", "False")


def _rhubarb_available() -> bool:
    return os.path.isfile(_RHUBARB_BIN) or shutil.which(_RHUBARB_BIN) is not None


# ── helpers (pure / model-free) ─────────────────────────────────────────────
def _wav_duration(path: str) -> float:
    import soundfile as sf

    info = sf.info(path)
    return float(info.frames) / float(info.samplerate)


def _even_word_timings(text: str, dur: float) -> list:
    """Distribute words evenly across [0,dur] — the fake/no-model word source."""
    words = (text or "").split()
    if not words:
        return []
    per = dur / len(words)
    return [{"w": w, "t0": round(i * per, 3), "t1": round((i + 1) * per, 3)} for i, w in enumerate(words)]


def _fake_mouth_cues(dur: float, text: str) -> list:
    """Deterministic mouth cues covering [0,dur]: an open shape per word, X in the gaps."""
    wt = _even_word_timings(text, dur)
    if not wt:  # no transcript → coarse alternation so lip-sync still has something
        n = max(1, int(dur * 3))
        wt = [{"w": "x", "t0": round(i * dur / n, 3), "t1": round((i + 1) * dur / n, 3)} for i in range(n)]
    shapes = ["C", "D", "E", "B"]
    cues, prev = [], 0.0
    for i, w in enumerate(wt):
        if w["t0"] > prev:
            cues.append({"start": round(prev, 3), "end": round(w["t0"], 3), "value": "X"})
        cues.append({"start": round(w["t0"], 3), "end": round(w["t1"], 3), "value": shapes[i % len(shapes)]})
        prev = w["t1"]
    if prev < dur:
        cues.append({"start": round(prev, 3), "end": round(dur, 3), "value": "X"})
    return cues


def _asset_roots() -> list:
    """Content-addressed stores to search for input assets by hash. SLOP_ASSET_ROOTS is a
    ``:``-separated list (e.g. our own cache + sibling providers' caches mounted read-only),
    so co-located providers exchange large media zero-copy by hash — the architecture's
    'shared filesystem → path' transport — with no provider-to-provider HTTP."""
    roots = os.environ.get("SLOP_ASSET_ROOTS") or os.environ.get("SLOP_CACHE", "cache/align")
    return [r for r in roots.split(":") if r]


def _find_by_hash(h: str):
    for root in _asset_roots():
        for ext in ("wav", "mp3", "flac", "ogg", "m4a"):
            p = os.path.join(root, f"{h}.{ext}")
            if os.path.exists(p):
                return p
    return None


def _resolve_audio(inputs: list):
    """Resolve the first audio input → (local_path, cleanup_fn). Prefers local resolution
    (shared/mounted store) over HTTP, so it works behind a firewall that only exposes
    published ports. Raises if unresolvable."""
    ref = inputs[0] if inputs else None
    if ref is None:
        raise ValueError("align requires one audio input (the TTS asset)")
    uri = ref.get("uri") if isinstance(ref, dict) else (ref if isinstance(ref, str) else None)
    h = ref if isinstance(ref, str) else (ref.get("hash") if isinstance(ref, dict) else None)

    # 1) explicit local file (shared mount)
    if uri and uri.startswith("file://") and os.path.exists(uri[7:]):
        return uri[7:], (lambda: None)
    # 2) content-addressed local store — zero-copy, the preferred co-located transport
    if h:
        p = _find_by_hash(h)
        if p:
            return p, (lambda: None)
    # 3) HTTP fetch — no shared storage between editor/provider
    if uri and uri.startswith(("http://", "https://")):
        fd, tmp = tempfile.mkstemp(suffix=".audio")
        os.close(fd)
        urllib.request.urlretrieve(uri, tmp)
        return tmp, (lambda: os.path.exists(tmp) and os.remove(tmp))
    # 4) plain path
    if uri and os.path.exists(uri):
        return uri, (lambda: None)
    raise ValueError(f"could not resolve audio input: {ref!r}")


# ── engines (lazy, GPU/binary) ──────────────────────────────────────────────
def _whisperx_align(path: str, params: dict) -> dict:
    import torch
    import whisperx

    global _wx_model
    device = "cuda" if torch.cuda.is_available() else "cpu"
    compute = os.environ.get("WHISPERX_COMPUTE", "float16" if device == "cuda" else "int8")
    with _load_lock:
        if _wx_model is None:
            _wx_model = whisperx.load_model(_WHISPERX_MODEL, device, compute_type=compute)
    audio = whisperx.load_audio(path)
    tr = _wx_model.transcribe(audio, batch_size=int(os.environ.get("WHISPERX_BATCH", "8")),
                              language=params.get("language"))
    lang = tr.get("language", params.get("language", "en"))
    with _load_lock:
        if lang not in _wx_align:
            _wx_align[lang] = whisperx.load_align_model(language_code=lang, device=device)
    amodel, meta = _wx_align[lang]
    aligned = whisperx.align(tr["segments"], amodel, meta, audio, device, return_char_alignments=False)
    words = [
        {"w": w["word"], "t0": round(float(w["start"]), 3), "t1": round(float(w["end"]), 3),
         "score": round(float(w.get("score", 0.0)), 3)}
        for seg in aligned["segments"] for w in seg.get("words", []) if "start" in w and "end" in w
    ]
    return {"language": lang, "words": words}


def _rhubarb_cues(path: str, dialog: str, recognizer: str) -> list:
    args = [_RHUBARB_BIN, "-f", "json", "-r", recognizer, "--machineReadable"]
    dlg = None
    if dialog:
        fd, dlg = tempfile.mkstemp(suffix=".txt")
        with os.fdopen(fd, "w") as f:
            f.write(dialog)
        args += ["--dialogFile", dlg]
    args.append(path)
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=600)
    finally:
        if dlg and os.path.exists(dlg):
            os.remove(dlg)
    if out.returncode != 0:
        raise RuntimeError(f"rhubarb failed: {out.stderr.strip()[:200]}")
    return json.loads(out.stdout).get("mouthCues", [])


# ── handlers ────────────────────────────────────────────────────────────────
# All blocking work (audio fetch, model inference, file write) runs in the executor,
# NEVER on the event loop — a slow/hanging input fetch must not freeze the provider (so
# GET /jobs and WS progress keep responding). The async wrappers just report coarse
# progress and hand off. The base queue serializes jobs (concurrency=1).
def _do_word_timing(params, inputs, cache, h) -> Result:
    path, cleanup = _resolve_audio(inputs)
    try:
        if _fake():
            data = {"language": params.get("language", "en"),
                    "words": _even_word_timings(params.get("text", ""), _wav_duration(path))}
        else:
            data = _whisperx_align(path, params)
    finally:
        cleanup()
    with open(cache.asset_path(h, "json"), "w") as f:
        json.dump(data, f)
    return Result(assets=[Asset("data", cache.uri(h, "json"),
                                {"word_timings": data["words"], "language": data["language"],
                                 "n_words": len(data["words"])})])


def _do_visemes(params, inputs, cache, h) -> Result:
    path, cleanup = _resolve_audio(inputs)
    dialog = params.get("dialog", "") or params.get("text", "")
    recognizer = params.get("recognizer", "pocketSphinx" if dialog else "phonetic")
    try:
        if _fake() or not _rhubarb_available():
            engine = "fake"
            cues = _fake_mouth_cues(_wav_duration(path), dialog)
        else:
            engine = "rhubarb"
            cues = _rhubarb_cues(path, dialog, recognizer)
    finally:
        cleanup()
    visemes = [
        {"viseme": c["value"], "t0": round(float(c["start"]), 3), "t1": round(float(c["end"]), 3),
         "openness": _OPENNESS.get(c["value"], 0.3)}
        for c in cues
    ]
    with open(cache.asset_path(h, "json"), "w") as f:
        json.dump({"visemes": visemes, "engine": engine, "shape_set": "preston-blair"}, f)
    # Light meta only: the (potentially many) per-phoneme cues live in the json file, which
    # the editor downloads + parses — keeping them out of meta avoids bloating the project doc.
    return Result(assets=[Asset("data", cache.uri(h, "json"),
                                {"n": len(visemes), "engine": engine, "shape_set": "preston-blair",
                                 "duration": round(visemes[-1]["t1"], 3) if visemes else 0.0})])


async def _word_timing(params, inputs, ctx) -> Result:
    await ctx.report(0.2, "transcribing + aligning")
    return await asyncio.get_event_loop().run_in_executor(
        None, _do_word_timing, params, inputs, ctx.cache, ctx.job.hash)


async def _visemes(params, inputs, ctx) -> Result:
    await ctx.report(0.2, "analyzing mouth shapes")
    return await asyncio.get_event_loop().run_in_executor(
        None, _do_visemes, params, inputs, ctx.cache, ctx.job.hash)


# ── provider wiring ─────────────────────────────────────────────────────────
provider = Provider(
    "align",
    os.environ.get("ALIGN_VERSION", "whisperx-small+rhubarb-1.14@2026.01"),
    cache_dir=os.environ.get("SLOP_CACHE", "cache/align"),
    concurrency=1,
)

provider.register(TypeSpec(
    "word_timing", _word_timing,
    params_schema={
        "type": "object",
        "properties": {
            "language": {"type": "string", "title": "Language (auto-detected if omitted)"},
            "model": {"type": "string", "title": "Whisper model size", "default": _WHISPERX_MODEL},
            "text": {"type": "string", "title": "Known transcript (fake-path fallback)"},
        },
        "required": [],
    },
    outputs=["data"],
    returns_timings=True,
))
provider.register(TypeSpec(
    "visemes", _visemes,
    params_schema={
        "type": "object",
        "properties": {
            "dialog": {"type": "string", "title": "Line text (improves accuracy)"},
            "recognizer": {"type": "string", "enum": ["pocketSphinx", "phonetic"],
                           "title": "Rhubarb recognizer (pocketSphinx=English+dialog, phonetic=any)"},
        },
        "required": [],
    },
    outputs=["data"],
))

app = make_app(provider)
