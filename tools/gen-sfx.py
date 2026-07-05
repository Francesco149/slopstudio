#!/usr/bin/env python3
"""Generate the built-in sound effects into library/sfx/.

Synthesized from scratch (sine/noise + envelopes, stdlib only) so they are
commercial-safe BY CONSTRUCTION — no sourced samples, no license, regenerable
and deterministic (seeded PRNG). Tuned against the user's meme-SFX style
references (2026-07-03: "Top 60 Sound Effects For Editing #1" 0:08/0:38 + the
pop/swipe pair) — spectrally matched, not sampled.

    nix develop --command python tools/gen-sfx.py

Transition one-shots (gated by the project toggle `meta.sfx` — Project panel):
  pop.wav        — the spring pop-in = a low hollow MOUTH-POP. A user RECORDING
                   (tools/sfx-src/pop.wav), trimmed to its transient + de-clicked;
                   synthesized fallback (gen_pop) if that source is missing.
  pop-blip.wav   — alt: a Minecraft-style ~1 kHz pickup blip (params.sfx_cue).
  whoosh.wav     — slide / pose-swap swipe = a soft LOW stable-pitch swing (~380 Hz).
  whoosh-sharp.wav — alt: the sharper 700→2800→700 Hz arc swish (params.sfx_cue).

Authored gag cues (`params.sfx_cue` on any clip — NOT gated by meta.sfx, so
full-length videos get them too; the editor ducks the music around them):
  awkward.wav — "Awkward Moment": a muffled cough-thud, then crickets
                (~4.4 kHz chirp bursts) over a faint 750 Hz room tone, ~2.2 s.
                Auto-attached to the skeleton's clueless gag.
  boom.wav    — "Vine Boom (Slowed)": ~45 Hz sub thump, instant attack, soft-
                clipped harmonics, ~2.4 s reverb-ish tail. For ominous outros.

Ships in the stock pack (tools/pack-stock-assets.py); *.wav stays gitignored.
"""
import math, os, random, struct, wave

SR = 48000
OUT_DIR = "library/sfx"
SFX_SRC = "tools/sfx-src"     # committed recorded SFX sources (see recorded_pop)
rng = random.Random(0x510B)  # deterministic across runs


def write_wav(name, samples, peak=0.8):
    mx = max(1e-9, max(abs(s) for s in samples))
    k = peak / mx
    path = os.path.join(OUT_DIR, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, s * k)) * 32767)) for s in samples))
    print(f"  {path}  {len(samples)/SR:.3f}s")


def _read_wav_mono(path):
    """Read a 16-bit PCM wav → mono float list at SR (linear-resampled if needed)."""
    with wave.open(path, "rb") as w:
        n, sr, ch, sw = w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError(f"{path}: expected 16-bit PCM, got sampwidth={sw}")
    a = struct.unpack("<%dh" % (len(raw) // 2), raw)
    mono = [a[i * ch] / 32768.0 for i in range(n)]        # take channel 0 if stereo
    if sr != SR:
        m = int(len(mono) * SR / sr); res = []
        for i in range(m):
            x = i * sr / SR; i0 = int(x); fr = x - i0
            s0 = mono[i0] if i0 < len(mono) else 0.0
            s1 = mono[i0 + 1] if i0 + 1 < len(mono) else s0
            res.append(s0 + (s1 - s0) * fr)
        mono = res
    return mono


def recorded_pop():
    """The recorded mouth-pop (tools/sfx-src/pop.wav) TRIMMED to its transient +
    de-clicked, so it lands ON the cut (a raw take is quiet + fronted by silence, so
    the pop would be inaudible AND late). write_wav re-normalizes to the calibrated
    0.7 peak (the pop event plays at -8 dB). Returns None if the source is absent →
    main() falls back to the synthesized gen_pop()."""
    path = os.path.join(SFX_SRC, "pop.wav")
    if not os.path.exists(path):
        return None
    s = _read_wav_mono(path)
    pk = max((abs(x) for x in s), default=0.0)
    if pk < 1e-4:
        return None
    # thresholds are a FRACTION OF PEAK, kept well above a quiet take's noise floor (a raw
    # mouth-pop can sit ~0.08 peak over a ~-70 dB floor, so a 0.5%-of-peak gate would trigger on
    # hiss and never trim the ~0.14 s of leading silence → the pop lands late). 4–6% is robust.
    on, off = pk * 0.06, pk * 0.04
    a = next((i for i in range(len(s)) if abs(s[i]) > on), 0)
    b = next((i for i in range(len(s) - 1, -1, -1) if abs(s[i]) > off), len(s) - 1)
    a = max(0, a - int(0.004 * SR)); b = min(len(s), b + int(0.010 * SR))    # 4 ms pre-roll (attack), 10 ms tail
    core = s[a:b]; M = len(core)
    fi, fo = int(0.0015 * SR), int(0.005 * SR)            # de-click the hard-cut ends
    for i in range(M):
        if i < fi:      core[i] *= i / fi
        if i > M - fo:  core[i] *= max(0.0, (M - i) / fo)
    return core


def gen_pop(dur=0.09):
    """Mouth-pop (user tweak, matched to a real lip-pop ref: ~250 Hz dominant,
    ~40 ms): a LOW hollow 'bock' — a fast downward pitch glide (560→170 Hz) with
    a resonant low body and a snappy decay. Reads percussive, not a tonal beep."""
    n = int(dur * SR); out = []; phase = 0.0
    lp = band = 0.0                                       # a resonant low-pass 'cavity' body
    for i in range(n):
        t = i / SR
        f = 170.0 + 390.0 * math.exp(-t * 90.0)          # 560 → 170 Hz, very fast drop = the pop
        phase += 2 * math.pi * f / SR
        amp = math.exp(-t * 42.0) * (1.0 - math.exp(-t / 0.0025))  # snappy, soft attack
        s = math.sin(phase) * amp
        # resonant cavity: excite a ~300 Hz band-pass with the onset → a hollow 'ock'
        fc = 2.0 * math.sin(math.pi * 300.0 / SR)
        exc = (rng.random() * 2 - 1) * (1.0 if t < 0.003 else 0.0)
        lp += fc * band
        band += fc * (exc - lp - 0.6 * band)
        out.append(s * 0.85 + band * 0.5 * math.exp(-t * 46.0))
    return out


def gen_pop_blip(dur=0.06):
    """Alternate pop — Minecraft-style item-pickup blip (ref: clean ~1 kHz tone,
    ~25 ms). A short soft sine with a tiny UP-glide + fast decay. Selectable via
    params.sfx_cue:"pop-blip" if the mouth-pop isn't the vibe."""
    n = int(dur * SR); out = []; phase = 0.0
    for i in range(n):
        t = i / SR
        f = 900.0 + 260.0 * (t / dur)                    # slight rise 900→1160 Hz
        phase += 2 * math.pi * f / SR
        amp = math.exp(-t * 70.0) * (1.0 - math.exp(-t / 0.002))
        out.append(math.sin(phase) * amp)
    return out


def gen_whoosh(dur=0.44):
    """Swipe DEFAULT — a soft, LOW, stable-pitch swing (user tweak): filtered
    noise held around a roughly CONSTANT low band (~380 Hz, only a gentle ±90 Hz
    breath) with a smooth bell envelope. Softer + lower than the arc swish."""
    n = int(dur * SR); out = []
    low = [0.0, 0.0]; band = [0.0, 0.0]; q = 0.6         # Q ≈ 1.7 each — airy, not whistly
    for i in range(n):
        t = i / SR; u = t / dur
        arc = math.sin(math.pi * u)
        fc = 380.0 + 90.0 * arc                           # nearly stable, low → a calm swing
        f = 2.0 * math.sin(math.pi * min(fc, SR * 0.24) / SR)
        x = rng.random() * 2 - 1
        for s in range(2):
            low[s] += f * band[s]
            high = x - low[s] - q * band[s]
            band[s] += f * high
            x = band[s]
        env = arc ** 1.3
        out.append(x * env)
    return out


def gen_whoosh_sharp(dur=0.42):
    """Swipe ALT — the sharper arc swish the user liked (kept). Centre ARCS up
    then back down 700→2800→700 Hz. Selectable via params.sfx_cue:"whoosh-sharp"."""
    n = int(dur * SR); out = []
    low = [0.0, 0.0]; band = [0.0, 0.0]; q = 0.5
    for i in range(n):
        t = i / SR; u = t / dur
        arc = math.sin(math.pi * u)
        fc = 700.0 + 2100.0 * arc
        f = 2.0 * math.sin(math.pi * min(fc, SR * 0.24) / SR)
        x = rng.random() * 2 - 1
        for s in range(2):
            low[s] += f * band[s]
            high = x - low[s] - q * band[s]
            band[s] += f * high
            x = band[s]
        env = arc ** 1.4
        out.append(x * env)
    return out


def gen_awkward(dur=2.2):
    """'Awkward Moment': a muffled cough-thud up front, then an awkward-silence
    bed — faint 750 Hz room tone + cricket chirp bursts (~4.4 kHz carrier gated
    at ~35 Hz, in ~0.45 s bursts), fading out."""
    n = int(dur * SR); out = [0.0] * n
    # muffled cough: two quick low-passed noise thumps (a real cough is a double)
    lp = 0.0
    for (t0, dur_c, g) in ((0.02, 0.09, 1.0), (0.13, 0.07, 0.6)):
        i0 = int(t0 * SR)
        for i in range(int(dur_c * SR)):
            t = i / SR
            x = (rng.random() * 2 - 1)
            lp += 0.06 * (x - lp)                         # dark, muffled
            env = math.sin(math.pi * min(1.0, t / dur_c)) ** 2
            if i0 + i < n: out[i0 + i] += lp * env * 2.6 * g
    # faint room tone (the "silence"): quiet 750 Hz hum + air noise, slow fade
    ph = 0.0; air = 0.0
    for i in range(n):
        t = i / SR
        ph += 2 * math.pi * 750.0 / SR
        x = rng.random() * 2 - 1
        air += 0.02 * (x - air)
        fade = max(0.0, 1.0 - t / dur)
        out[i] += (math.sin(ph) * 0.018 + air * 0.35) * fade
    # crickets: chirp bursts — 4.4 kHz carrier, 35 Hz gate, bursts every ~0.62 s
    for b, t0 in enumerate((0.42, 1.04, 1.66)):
        blen = 0.45
        i0 = int(t0 * SR); ph = 0.0
        detune = 1.0 + 0.004 * b                          # each burst slightly different
        for i in range(int(blen * SR)):
            t = i / SR
            ph += 2 * math.pi * 4400.0 * detune / SR
            gate = max(0.0, math.sin(2 * math.pi * 35.0 * t)) ** 3   # chirp pulses
            benv = math.sin(math.pi * min(1.0, t / blen)) ** 0.7
            if i0 + i < n: out[i0 + i] += math.sin(ph) * gate * benv * 0.16
    return out


def gen_boom(dur=3.0):
    """'Vine Boom (Slowed)', RUMBLIER (user tweak): a bright transient into a
    sub thump with a heavier, longer-sustaining low end — a ~40 Hz fundamental
    + a ~26 Hz sub-octave layer, slow body decay, and a fuller low-passed rumble
    bed that lingers under it."""
    n = int(dur * SR); out = []; phase = 0.0; sub = 0.0; lp = 0.0; lp2 = 0.0
    for i in range(n):
        t = i / SR
        f = 40.0 + 42.0 * math.exp(-t * 26.0)             # 82 → 40 Hz glide
        phase += 2 * math.pi * f / SR
        sub += 2 * math.pi * (f * 0.5) / SR               # sub-octave (~20 Hz) for weight
        body = math.tanh(math.sin(phase) * 2.4) * math.exp(-t * 0.8)   # slower decay = more sustain
        body += math.sin(sub) * 0.6 * math.exp(-t * 0.7)  # the added rumble layer
        x = rng.random() * 2 - 1
        lp += 0.03 * (x - lp)                             # dark room rumble
        lp2 += 0.012 * (lp - lp2)                         # a second, deeper pole → heavier low bed
        tail = (lp * 1.2 + lp2 * 3.2) * math.exp(-t * 1.0)
        s = body + tail
        if t < 0.012:                                     # the bright attack transient
            s += (rng.random() * 2 - 1) * 0.9 * (1.0 - t / 0.012)
        out.append(s * min(1.0, t / 0.004))
    return out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    write_wav("pop.wav", recorded_pop() or gen_pop(), peak=0.7)  # recorded mouth-pop (tools/sfx-src), synth fallback
    write_wav("pop-blip.wav", gen_pop_blip(), peak=0.6)    # minecraft-style alt
    write_wav("whoosh.wav", gen_whoosh(), peak=0.7)        # soft low swing (default)
    write_wav("whoosh-sharp.wav", gen_whoosh_sharp())      # the sharper arc swish (alt)
    write_wav("awkward.wav", gen_awkward(), peak=0.6)
    write_wav("boom.wav", gen_boom(), peak=0.95)
    print("done.")


if __name__ == "__main__":
    main()
