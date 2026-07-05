# presets/voice-snips — reusable Gemma vocal snippets

Short, **recorded (non-regenerable) Gemma-san vocalizations** cut from known-good VO takes and reused
verbatim across videos — for sounds the TTS renders **inconsistently** (giggles, sighs, hums, "heh~").
Committed here (the `!presets/**` allow-list survives the global `*.wav` ignore) because they are *cut
from a specific recording*, not synthesizable — unlike `library/sfx/` (synthesized, regenerable).

Use a snippet as a normal VO/asset clip (drop it on `r_vo` or an overlay audio row), or wire it as a cue.
Keep them at a natural voice level (peak ≈0.55); the mixer's `meta.speech_gain_db` applies on top.

| file | source | notes |
|---|---|---|
| `gemma-heh.wav` | luckymas3 `b01` ("Heh~ Welcome back, mortals.", `a_fkd464dl3yyhscxg`), cut 0.238–0.530s | the signature spoken giggle. **The TTS can't say "fufu" and stumbles on "heh"** ([[gemma-fufu-signature]]), so reuse THIS instead of regenerating. 0.292s, 24kHz mono, de-clicked (8ms/50ms fades). |

**TODO (owner ask):** grow this into a small library of Gemma vocal sounds for VO variety (more giggles,
sighs, "fufu"-adjacent hums, reactions) — cut from the best takes as they appear. See [[gemma-voice-workflow]].
