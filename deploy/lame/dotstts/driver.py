#!/usr/bin/env python3
"""dots.tts A/B driver — runs inside slop-dotstts on lame's 3080.
Reads a JSON manifest [{name,text,prompt_audio,prompt_text,seed,num_steps}], clones each,
writes /out/<name>.wav, prints timing + VRAM. Loads the model ONCE via the runtime API;
falls back to the dots.tts CLI per-line if the API import/signature differs.
Usage: python driver.py /work/manifest.json <model> /out
"""
import json, sys, time, subprocess, os

MANIFEST = sys.argv[1]
MODEL = sys.argv[2] if len(sys.argv) > 2 else "rednote-hilab/dots.tts-soar"
OUT = sys.argv[3] if len(sys.argv) > 3 else "/out"
items = json.load(open(MANIFEST))

def vram():
    try:
        import torch
        return f"{torch.cuda.memory_allocated()/1e9:.2f}/{torch.cuda.max_memory_allocated()/1e9:.2f}GB alloc/peak"
    except Exception:
        return "?"

api_ok = False
rt = None
try:
    import torch
    from dots_tts.runtime import DotsTtsRuntime
    t0 = time.time()
    rt = DotsTtsRuntime.from_pretrained(MODEL, precision="bfloat16", optimize=True)
    print(f"[api] model loaded in {time.time()-t0:.1f}s  vram={vram()}", flush=True)
    api_ok = True
except Exception as e:
    print(f"[api] runtime API unavailable ({type(e).__name__}: {e}) -> CLI fallback", flush=True)

import soundfile as sf

for it in items:
    name = it["name"]; out = os.path.join(OUT, name + ".wav")
    steps = int(it.get("num_steps", 10)); seed = int(it.get("seed", 42))
    t0 = time.time()
    if api_ok:
        try:
            import torch
            torch.manual_seed(seed)
        except Exception:
            pass
        kw = dict(text=it["text"], prompt_audio_path=it["prompt_audio"],
                  prompt_text=it["prompt_text"], num_steps=steps, guidance_scale=1.2)
        try:
            res = rt.generate(seed=seed, **kw)
        except TypeError:
            res = rt.generate(**kw)
        audio = res["audio"]
        try:
            audio = audio.float().cpu().squeeze().numpy()
        except Exception:
            import numpy as np; audio = np.asarray(audio).squeeze()
        sr = int(res.get("sample_rate", 48000))
        sf.write(out, audio, sr)
        dur = len(audio) / sr
    else:
        cmd = ["dots.tts", "--model-name-or-path", MODEL, "--num-steps", str(steps),
               "--seed", str(seed), "--language", "EN", "--text", it["text"],
               "--prompt-audio", it["prompt_audio"], "--prompt-text", it["prompt_text"],
               "--output", out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[cli] {name} FAILED rc={r.returncode}\nSTDERR:\n{r.stderr[-2000:]}", flush=True)
            continue
        info = sf.info(out); sr = info.samplerate; dur = info.duration
    print(f"[ok] {name:22s} steps={steps} seed={seed} synth={time.time()-t0:5.1f}s "
          f"dur={dur:5.2f}s sr={sr} vram={vram()} -> {out}", flush=True)

print("ALL DONE", flush=True)
