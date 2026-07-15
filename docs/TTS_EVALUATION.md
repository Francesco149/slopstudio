# TTS stability evaluation

The production problem is not “best demo voice.” It is editing minutes per finished
minute: pronunciation failures, added/missing words, tonal misses, artifacts, and how many
takes a human must audition. Qwen3 VoiceDesign remains useful for designing an original
voice, but production narration should be A/B tested through clone-oriented engines.

## Trial order

1. **Chatterbox**: Turbo 350M for fast English A/B and Multilingual V3 500M for English/
   Japanese. The official project is MIT and describes V3 as improving speaker similarity,
   content consistency, and repetition/hallucination behavior. Deploy on the RTX 3080;
   upstream documents CUDA/CPU/MPS, not ROCm. <https://github.com/resemble-ai/chatterbox>
2. **CosyVoice 3 0.5B-2512**: Apache-2.0, cross-lingual cloning and instruction control,
   with official CUDA/Docker paths. <https://github.com/FunAudioLLM/CosyVoice>
3. **dots.tts bounded retry only**: the repo's existing 10-step/up-sampled-reference test
   lost to Qwen. Retry once with a native 48 kHz reference and 25–32 steps; do not expand
   the trial if it remains robotic/artifact-prone. <https://github.com/rednote-hilab/dots.tts>
4. **VoxCPM2 exploratory only**: technically attractive and Apache-licensed, but the
   maintainers state the training set is undisclosed and users must assess local-law risk;
   that provenance uncertainty makes it a weaker monetized default.
   <https://github.com/OpenBMB/VoxCPM>

Cloud escape hatches:

- Azure Neural TTS is the strongest deterministic-authoring option because SSML exposes
  phonemes/lexicons, breaks, rate, pitch, styles/roles, and visemes.
  <https://learn.microsoft.com/en-us/azure/ai-services/speech-service/speech-synthesis-markup-voice>
- ElevenLabs Multilingual v2 deserves a blind quality test and supports pronunciation
  dictionaries, but its documentation says seeded synthesis remains nondeterministic; it
  is a quality/pronunciation option, not automatically a stability fix.
  <https://elevenlabs.io/docs/eleven-api/guides/how-to/text-to-speech/pronunciation-dictionaries>

## Fixed A/B suite

Freeze two licensed 10–15 s references per host (neutral and expressive), at their native
sample rate with exact transcripts. Use 30 lines:

- 10 ordinary narration;
- 5 proper names, acronyms, numbers, and code terms (include Kirby and LotR II);
- 5 punctuation/quotation stress cases;
- 4 intended-emotion lines;
- 3 very short lines;
- 3 long lines (250–350 characters), including English/Japanese switching.

Generate five takes per engine/line with recorded model commit, weights hash, seed,
settings, real-time factor, peak VRAM, and failure status. Loudness-normalize playback and
randomize it double-blind.

Automated gates per take: normalized ASR WER/CER; missing/extra words and hallucinated
tail; WPM; speaker-embedding similarity; clipping/DC/silence tail; repetition. Keep the
best two conforming takes for human tone choice—WER alone must never choose the winner.

Human scores (1–5): intelligibility, pronunciation, identity, naturalness, intended tone,
and artifacts, plus pairwise preference. A production candidate passes only if it achieves:

- zero added/missing words on critical lines;
- at least 95% line success without manual regeneration;
- median pronunciation score at least 4;
- at least 90% artifact-free takes;
- identity variance no worse than current Qwen.

Report p50/p95 regeneration count and editing minutes per finished minute.

## Authoring changes required regardless of engine

- Make a pronunciation lexicon first-class project data: display spelling → spoken alias
  or IPA plus language. Stop hiding permanent pronunciation knowledge in one-off line text.
- Expose a backend capability map: deterministic seed, phonemes/lexicon, style, speed,
  language, clone/design, and native sample rate.
- Use VoiceDesign to bake a stable licensed reference, then use clone engines for lines.
- Preserve losing takes in cache and make A/B audition a single human action.

## Shorts rate result

A same-method sample of six Veritasium Shorts and six long-form uploads measured 166.4 vs
164.9 WPM—a 1.5 WPM (0.9%) difference. Veritasium is the slower comparison: the existing
creator corpus ranges from 160–204 WPM, with Juniper/Newbie at 196/191 and 100thCoin at
204. The Veritasium result supports equal rates across formats, not its absolute pace or a blanket
1.3× speed-up. Slopstudio now gives portrait and landscape the same default rate and uses
about 180 measured WPM (roughly 175–190) as the house target. A voice preset may still use
1.15× to reach that pace; Shorts simply do not add another multiplier. Existing projects
with explicit `meta.speech_rate` remain unchanged.
