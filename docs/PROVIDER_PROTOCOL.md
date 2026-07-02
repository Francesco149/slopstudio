# slopstudio — Provider Protocol

A **provider** is a small out-of-process HTTP/WS service that wraps one or more generation
capabilities (a model). The editor talks to providers only through this protocol, so:
- a provider can run anywhere (default `lame`, or locally) — only its URL in `config.toml`
  changes ⇒ redeployable;
- a provider crash/OOM is isolated and never takes down the editor;
- a new model exposes its parameters via a schema and the editor builds the UI
  automatically ⇒ extensible with no editor change.

All bodies are JSON unless noted. Conventional base path per provider; the editor knows
each provider's base URL from config.

## Endpoints

### `GET /healthz` → `200 "ok"`
Liveness. The editor polls this to show provider status.

### `GET /capabilities`
What this provider can do. Drives the editor's parameter UI.
```json
{
  "provider": "tts",
  "version": "qwen3-tts-1.7b-voicedesign@2026.01",
  "types": [
    {
      "type": "speech",
      "outputs": ["audio"],
      "params_schema": { "$schema": "…", "type": "object",
        "properties": {
          "text":    { "type": "string" },
          "voice_preset": { "type": "string" },
          "instruct":{ "type": "string", "title": "Direction (emotion/intonation)" },
          "seed":    { "type": "integer", "default": 0 }
        }, "required": ["text"] },
      "presets": ["gemma-san"],
      "deterministic": true,
      "returns_timings": true
    },
    { "type": "voice_design", "outputs": ["preset"], "params_schema": { "…": "…" } }
  ]
}
```

### `POST /jobs`
Submit a generation. Idempotent by content hash.
```json
// request
{ "type": "speech",
  "params": { "text": "Fufu~ welcome back.", "voice_preset": "gemma-san", "instruct": "smug, teasing", "seed": 7 },
  "inputs": [],                       // asset refs this job consumes (hashes/uris)
  "preset": null }
// response (cache miss → queued)
{ "job_id": "j_8a1", "hash": "a_3f9c…", "status": "queued", "cached": false }
// response (cache hit → immediate result)
{ "job_id": "j_8a1", "hash": "a_3f9c…", "status": "done", "cached": true, "result": { "…": "see §result" } }
```
The `hash` is the asset key the editor stores in the project's `assets` map.

### `GET /jobs/{id}`
Poll a job.
```json
{ "job_id": "j_8a1", "hash": "a_3f9c…",
  "status": "running",                // queued | running | done | error
  "progress": 0.42, "message": "synthesizing",
  "result": null, "error": null }
```

### `WS /jobs/{id}/events`
Progress stream; one JSON frame per update until a terminal status, e.g.
`{ "status": "running", "progress": 0.42, "message": "step 12/30" }` …
`{ "status": "done", "result": { … } }`. The editor prefers WS and falls back to polling.

### Asset fetch
A `result.assets[].uri` is one of:
- `file://…` / a shared-mount path — used when editor + provider share storage (preferred
  for large media; zero-copy);
- `http(s)://<provider>/assets/{hash}` — `GET /assets/{hash}` streams the bytes when no
  shared storage exists.

`cache://<provider>/<hash>.<ext>` is the editor-side canonical form once cached locally.

## §result
```json
{
  "assets": [
    { "kind": "audio", "uri": "file:///opt/ai-lab/cache/tts/a_3f9c.wav",
      "meta": { "duration": 1.84, "sample_rate": 48000,
                "word_timings": [ { "w": "Fufu", "t0": 0.06, "t1": 0.55 } ] } }
  ],
  "timings": { "queued_ms": 3, "run_ms": 920 },
  "provenance": null                  // for sourced media: the attribution record
}
```
`kind` ∈ `image · audio · video · data · preset`. For sourced music/art, `provenance`
carries the attribution schema (title, artist, source_url, license_name, license_url,
attribution_required, attribution_text, content_id_risk) which flows into auto-credits.

## §param-hash (the cache key)
```
hash = "a_" + base32( sha256( canonical_json({
  "provider_type": <type>,
  "params":        <params with provider-declared volatile keys stripped>,
  "inputs":        <sorted input hashes>,
  "model_version": <capabilities.version>
}) ) )[0:16]
```
- Canonical JSON = sorted keys, no insignificant whitespace, normalized numbers.
- Providers must be **deterministic given params** (expose a `seed` for stochastic models)
  so the hash is meaningful; identical hash ⇒ identical output ⇒ cache hit ⇒ no rerun.
- `model_version` in the hash means upgrading a model invalidates stale assets cleanly.

## Errors
```json
{ "job_id": "j_8a1", "status": "error",
  "error": { "code": "oom", "message": "CUDA OOM at 768x768; retry lower res", "retriable": true } }
```
Codes: `bad_params · oom · model_unavailable · upstream · timeout · internal`. The editor
surfaces the message on the clip and keeps showing the last good asset.

## Provider catalogue (initial)
| Provider | Types | Engine |
|---|---|---|
| `tts`   | `speech`, `voice_design` | Qwen3-TTS 1.7B VoiceDesign |
| `image` | `text2image`, `image2image`, `inpaint`, `controlnet` | ComfyUI (Illustrious-XL 2.0 + LoRA; SANA) |
| `video` | `motion` (procedural), `image2video` (Wan 2.2) | ffmpeg/depth/RIFE; ComfyUI |
| `music` | `search` (Jamendo), `generate` (ACE-Step) | Jamendo API; ACE-Step 1.5 |
| `align` | `word_timing` (WhisperX), `visemes` (Rhubarb) | WhisperX; Rhubarb Lip Sync |
| `rembg` | `remove_bg` (image → RGBA cutout) | rembg isnet-anime/u2net (ONNX, CPU) |

A reference provider skeleton (FastAPI: capabilities, job queue, WS progress, content-hash
cache) lives in `providers/` and is the base every concrete provider subclasses.
