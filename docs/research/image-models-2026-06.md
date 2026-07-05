# Local open-weight image models for slopstudio — survey (2026-06-27)

**Scope.** slopstudio is MIT/open-source and **monetizes its output** (YouTube videos: anime
host "Gemma-san", reaction pictures, backgrounds, props). So **license commercial-safety is a
HARD requirement**, evaluated at the source LICENSE, not vibes. Generation runs in **ComfyUI**
on **RX 7800XT 16 GB (ROCm)** with an **RTX 3080 10 GB (CUDA)** as a second card → models must
fit ≤16 GB (ideally), native fp16 or via fp8/GGUF, and run in ComfyUI today.

**What we're trying to beat / complement:**
- **Illustrious-XL v2.0** (current anime workhorse + our Gemma-san LoRA) — `creativeml-openrail-m`.
- **SANA** (current "clean diagrams" pick) — *the CLAUDE.md note "SANA (Apache)" is WRONG; see correction below.*
- **Krea-2** = the license **BAR**: commercial use only while company revenue **< $1M/yr**,
  **revocable**, plus an AUP/content-filter obligation. Anything cleaner (Apache/MIT, no cap,
  not vendor-revocable) is strictly preferable; anything with a cap, NC clause, or geo-lock is flagged.

---

## ⚑ The single most important concept: MODEL license ≠ OUTPUT license

The thing most people (and most "AVOID FLUX/NC" takes online) get wrong: a model can be
**non-commercial for the WEIGHTS/service** while **explicitly permitting commercial use of the
generated images**. Because slopstudio monetizes *output images embedded in videos* and runs the
model *locally* (not as a hosted/paid service), an "output-commercial-OK" model is **usable for us**
even if its weights are nominally "non-commercial." Two of the strongest new models are exactly this:

- **Anima** — model weights NC, but *"You may use generated images commercially"* (selling images,
  paid commissions, **generating assets for paid products**). Hosting the model behind a paid
  API/embedding it in a paid product is what's forbidden — not putting its outputs in a monetized video.
- **FLUX.2 [dev]** — *"You may use Output for any purpose (including for commercial purposes)…"*
  while the weights are non-commercial + revocable.

So our license taxonomy below distinguishes **output-monetizable** (what we actually need) from
**model-commercial** (host/sell the model itself — we don't need this).

---

## Ranked comparison (for OUR use: anime host + reaction pics + props/backgrounds, monetized, local, ≤16 GB)

Verdict key: ✅ **CLEAN** = Apache/MIT, monetize output freely, no cap, irrevocable · 🟢 **OUTPUT-OK** =
weights NC but outputs explicitly monetizable (fits us) · 🟡 **OK\*** = commercial allowed but with
conditions (revocable/attribution/geo/cap) · 🔴 **AVOID** = NC / cap we'd hit / geo-excludes us.

| # | Model | Released | Base arch | License → commercial verdict | Anime | Reaction-pic / prompt-understanding | ≤16 GB? / ComfyUI |
|---|-------|----------|-----------|------------------------------|-------|--------------------------------------|-------------------|
| 1 | **Anima** (base-v1.0) | repo 2026-01-29, v1.0 ≈2026-05-15 | NVIDIA Cosmos-Predict2-2B | `circlestone-labs-non-commercial-license` → **🟢 OUTPUT-OK**: weights NC (no paid hosting), **outputs may be sold/used in paid products**. Inherits NVIDIA Open Model License on the Cosmos base → no cap, but **revocable** + "Built on NVIDIA Cosmos" attribution. | **Top tier.** Ground-up anime; "new Illustrious contender," cleaner linework, better anime anatomy, less accidental realism; also does realism + most art styles. Danbooru tags + NL. | Strong: expression/pose via Danbooru tags; flat-color sharp-line default suits reaction art/chibi. | **Yes — 2B, 3.89 GB ckpt**, fits 8 GB. ComfyUI **native** (Qwen-3-0.6B text enc + Qwen-Image VAE). |
| 2 | **Neta-Lumina** (v1.0) | 2025-07-26 | Lumina-Image-2.0 (2.6B DiT) | `apache-2.0` → **✅ CLEAN** (Gemma-2-2B text-encoder adds Gemma Terms at inference — commercial-OK but a footnote). **No cap, not vendor-revocable.** | **Specialist.** 13M anime images, curriculum-trained; Danbooru tags + multilingual NL; furry/guofeng coverage. The cleanest-license *true anime specialist*. | Strong prompt adherence to complex prompts; tag + NL. No dedicated "sticker" mode. | **Yes — ≥8 GB.** ComfyUI **native** (Lumina2 nodes). |
| 3 | **Qwen-Image** (20B) / **-2.0** (7B, 2026-02-10) / **-Edit-2511** | 2025-08-02 / 2026-02 | Qwen MMDiT | `apache-2.0` (confirmed on weights) → **✅ CLEAN**. (Qwen *Chat* TOS's "no commercial" applies only to Alibaba's hosted service, **not** the open weights we self-host.) | Not anime-specialist, but huge range; **Qwen-Image-Edit-2511-Anime** LoRA = flat cel-shaded anime img2img. Pair with anime LoRA for characters. | **Best-in-class prompt understanding** ("outstanding NL"); #1 on AI-Arena (blind eval) for t2i+edit per Qwen → ideal for "describe a *specific* reaction/composition." Superb text/diagram rendering. | 20B-2512 needs fp8/GGUF (~12–16 GB), ComfyUI native. ⚠️ **CORRECTION (2026-06-28): the 7B "-2.0" is API-ONLY — NO open weights (Alibaba BaiLian closed tier); ComfyUI can't run it (issue #12386 open). The 7B figure here was wrong. The *open* Qwen line = 20B-2512 + Edit-2511.** |
| 4 | **HiDream-O1-Image** (+ -Dev) | 2026-05-08 | 8B pixel-native DiT (no VAE) | `mit` → **✅ CLEAN** (the cleanest possible; no cap, irrevocable). | Generalist, not anime-specialist; very high fidelity, 2K native. Anime via LoRA. | Excellent: tops GenEval/DPG/HPSv3 for its size; #8 (highest open-weight) on Artificial Analysis arena. Great for prompt-faithful compositions, props, backgrounds, diagrams. | **Yes** — 8B; fp8/GGUF fits 16 GB. ComfyUI **native**. |
| 5 | **Z-Image-Turbo** | 2025-11-25 | Z-Image 6B (Lumina-style) | `apache-2.0` → **✅ CLEAN**. No cap, irrevocable. | Generalist; "what most people use locally now." Anime OK, needs LoRA for stronger anime/NSFW. | Good prompt following; many styles. 5–15 steps (fast). Great low-VRAM props/backgrounds engine. | **Yes — fp8 ≈6 GB**, fits the 3080. ComfyUI **supported** (ZImagePipeline). |
| 6 | **Illustrious-XL v2.0** *(current pick)* | 2025-04-18 | SDXL | `creativeml-openrail-m` → **🟡 OK\*** monetize OK; OpenRAIL **behavioral use-restrictions** + attribution; **not vendor-revocable** (irrevocable grant). | Strong; **largest LoRA ecosystem** (incl. our Gemma-san LoRA), Danbooru-tag native. | Excellent expression control via Danbooru tags (`:d`, `>:(`, `surprised`, `tears`, `pointing`…). Weak NL prompt understanding vs DiT models. | **Yes — ~3.5 GB**, trivial. ComfyUI **native**. |
| 7 | **FLUX.2-klein-4B** | 2026-01-14 | FLUX.2 rectified-flow 4B | `apache-2.0` → **✅ CLEAN** (the 4B variant only). | General/photoreal + editing; not an anime specialist. | Good prompt-following + multi-ref editing. Community verdict: "relevant, but just use Z-Image." | **Yes** — 4B fits easily. ComfyUI **native**. |
| 8 | **Chroma1-HD** | 2025–2026 | FLUX.1-schnell (8.9B) | `apache-2.0` → **✅ CLEAN**. Uncensored. | Mixed (anime/furry/art/photo in training); not a clean-line anime specialist. | Decent; uncensored breadth. | fp8 fits 16 GB. ComfyUI **supported**. |
| 9 | **Lumina-Image 2.0** | 2025-02 | 2.6B flow DiT | `apache-2.0` → **✅ CLEAN** (Gemma-2 text-encoder footnote). | Base model (use **Neta-Lumina** for anime). | Good NL + text. | **Yes**. ComfyUI **native**. |
| 10 | **HiDream-I1** | 2025-04-07 | 17B MoE DiT | `mit` → **✅ CLEAN**. | Generalist, not anime. | Strong prompt fidelity; heavy. | fp8 ≈16 GB / GGUF-Q4 ≈12 GB. ComfyUI **native**. |
| 11 | **SANA 1.6B** *(current pick)* | 2024-11-04 | NVIDIA linear-DiT | `nvidia-open-model-license` → **🟡 OK\*** (NOT Apache). Commercial allowed, **no cap, but revocable** + Trustworthy-AI/attribution; **Gemma-2-2B text-encoder** adds Gemma Terms; card says "research intended." | Weak/clean-vector; fine for diagrams, poor for character anime. | Limited. | **Yes — 1.6B**, trivial. ComfyUI **supported**. |
| 12 | **Pony Diffusion V7** | 2025-11 | AuraFlow 7B (Apache base) | **"Pony License" → 🟡 cap**: free <$1M revenue/funding; **paid inference or ≥$1M ⇒ must license**. (Same shape as Krea.) | Anime/furry specialist lineage; V7 adoption was slow/mixed. | Tag-based; expression control. | **Yes** (~7B). ComfyUI **supported**. |
| 13 | **Krea-2** *(the BAR)* | 2026-05 | Qwen-Image-based (Qwen VAE + Qwen3-VL enc) | **Krea-2 Community License → 🟡 cap**: commercial only while revenue **<$1M**, **revocable**, AUP/filter obligation. | Aesthetic-first; not anime-specialist. | Good aesthetics. | fp8 fits. ComfyUI **native** (Comfy-Org). |
| 14 | **FLUX.2 [dev]** (32B) | 2025-11-25 | FLUX.2 32B | FLUX.2-dev NC license → **🟢 OUTPUT-OK but heavy**: outputs monetizable, **weights NC, revocable, mandatory safety filter**, no-compete-training clause. | Photoreal SOTA, not anime. | Excellent prompt + 10-ref consistency. | 32B → heavy quant only. ComfyUI native. |
| 15 | **FLUX.2-klein-9B** | 2026-01-15 | FLUX.2 9B | **non-commercial** (the 9B tier) → **🔴 for model**; outputs follow FLUX-dev output terms. | "new Illustrious contender" buzz, but… | Good. | 29 GB fp16 → GGUF for 16 GB. Use the 4B Apache one instead. |
| 16 | **HunyuanImage 3.0** | 2025-09 | 80B MoE | Tencent license: commercial **but geo-EXCLUDES EU / UK / South Korea** → **🔴 geo** + far too large. | Strong but irrelevant here. | Strong. | **No — 80B.** |
| 17 | **NoobAI-XL** | 2024–25 | Illustrious/SDXL | Fair-AI-Public-License **+ explicit NC addendum** ("prohibit any form of commercialization… of model or **model-generated products**") → **🔴 AVOID**. (NC clause arguably unenforceable vs the base's share-alike, but it's a stated prohibition on *output* — do not build a monetized product on it.) | Excellent anime (v-pred), huge Civitai use. | Excellent tag control. | Yes. ComfyUI native. **License kills it for us.** |
| — | **FLUX.1 Kontext / Krea-dev** | 2025 | FLUX.1-dev | FLUX-dev NC, revocable, safety filter → **🟢 output-OK / 🔴 model**. Not anime. | n/a | edit/style ref. | fp8. ComfyUI native. |
| — | **CogView4-6B** | 2025-03 | 6B DiT | `apache-2.0` (per Zhipu release) → **✅ CLEAN**; strong Chinese text, not anime. | weak anime | good CN/EN text | Yes. ComfyUI support. |
| — | **AuraFlow / PixArt-Σ** | 2024 | DiT | AuraFlow `apache-2.0`; PixArt-Σ permissive-ish — both **superseded**; only relevant as Pony-V7's base. | dated | dated | Yes. |

---

## Per-model notes (decision detail)

### Anima — the headline new anime model (and the "newest thing you probably haven't heard of")
- **What:** 2B ground-up anime/illustration model from **CircleStone Labs × Comfy Org**, on **NVIDIA
  Cosmos-Predict2-2B-Text2Image**. Text encoder **Qwen-3-0.6B-base**, VAE **Qwen-Image VAE**.
  Res 512²–1536², sampler `er_sde`, 30–50 steps, CFG 4–5. Checkpoint **3.89 GB** — fits any GPU.
- **License (read carefully).** `circlestone-labs-non-commercial-license`. Model + derivatives are
  **non-commercial** (no paid API hosting, no embedding the *model* in a paid product). **BUT**:
  *"the non-commercial restriction applies only to the Model, and not to Outputs… You may use
  generated images commercially"* — explicitly incl. **"generating assets for paid products."**
  Putting Anima's images into a monetized YouTube video is asset-use → **permitted**. It is *also*
  a Cosmos derivative, so the **NVIDIA Open Model License** applies to the Cosmos-derived weights
  (commercial-OK, no cap, but **revocable** + must display **"Built on NVIDIA Cosmos"**).
- **Quality (corroborated, 2 independent sources):** Civitai 2026 scene guide — *"if you're really
  into anime, this is a no-brainer… does realism better than Illustrious… basically any art style."*
  Diffusion Doodles — *"cleaner linework, better anime anatomy, more consistent character rendering,
  less tendency toward accidental realism."* Treat "beats Illustrious" as strong community consensus,
  not a benchmark.
- **Verdict for us:** **#1 anime engine.** Local-only output use is exactly the carve-out granted.
  The only thing we must *not* do is offer the model itself as a paid/hosted service. Document the
  "Built on NVIDIA Cosmos" credit. *(Sources: HF model card + LICENSE-in-README, HF API, civitai 30487, diffusiondoodles.)*

### Neta-Lumina — the cleanest-license TRUE anime specialist (beats Krea-2 on license decisively)
- **Apache-2.0** anime fine-tune of Lumina-Image-2.0 (2.6B DiT), 13M anime images, **Danbooru tags
  + multilingual NL**, strong complex-prompt adherence; ≥8 GB VRAM; ComfyUI-native. Released
  **2025-07-26** by Neta.art. **No revenue cap, no vendor revocation** (Apache).
- **Footnote:** the inference pipeline uses **Gemma-2-2B** as text encoder → Google **Gemma Terms of
  Use** apply to that component (commercial-OK, but carries a Prohibited-Use policy; Google can
  require compliance changes). The DiT weights themselves are Apache. Common practice ignores this;
  technically it travels with the pipeline. Same caveat applies to Lumina-2.0 and SANA.
- **Verdict:** the **commercial-safe-no-asterisk anime base.** Quality is likely a notch below the
  best Illustrious merges / Anima for raw anime aesthetics, but the license is pristine. Excellent
  default if you want zero license risk on character art. A newer community continuation
  **NetaYume-Lumina** exists (further-trained); same Apache lineage.

### Qwen-Image family — the reaction-pic / props / text-rendering workhorse (clean Apache)
- **Apache-2.0** confirmed on the weights (HF tag). **Qwen-Image** (20B, 2025-08-02),
  **Qwen-Image-2.0** (leaner **7B**, 2026-02-10, native 2K, #1 AI-Arena blind eval per Qwen),
  **Qwen-Image-Edit-2511** (Apache) for editing + the **Qwen-Image-Edit-2511-Anime** LoRA
  (flat cel-shaded anime img2img). The **Qwen Chat ToS** "no commercial use" clause (GitHub issue #98)
  applies **only to Alibaba's hosted chat**, not to the open weights we run in ComfyUI — self-hosted
  Apache-2.0 output is fully monetizable.
- **Why it matters for reaction pics:** the task wants "describe a *specific* reaction/composition and
  get it." Qwen has the **best natural-language prompt understanding** of any option here (community
  consensus + arena), plus the best in-image **text rendering** (meme captions, signs) and clean
  diagrams (a better SANA replacement). Use Qwen for "smug girl pointing at a chart that says X,"
  then an Illustrious/Anima/Neta anime LoRA when you need on-model Gemma-san styling.
- **VRAM:** 2.0 (7B) fits 16 GB comfortably; 20B base needs fp8/GGUF (~12–16 GB).

### HiDream-O1-Image — newest MIT top-ranked open model (cleanest license + SOTA quality)
- **2026-05-08**, **MIT**, 8B **pixel-native** (drops VAE + external text encoder), 2K native; full
  (50-step CFG 5) + distilled `-Dev` (28-step CFG 0). Tops GenEval/DPG/HPSv3 for its size; **#8 and
  highest open-weight** on Artificial Analysis t2i arena (2026-05). ComfyUI-native; fp8/GGUF fits 16 GB.
- **Verdict:** not an anime specialist, but **MIT is the cleanest license available** and quality is
  SOTA-for-size. Best pick for **backgrounds, props, diagrams, and prompt-faithful reaction
  compositions** where you don't need on-model anime line art. A clean replacement for SANA.

### Z-Image-Turbo — tiny, fast, Apache generalist (the 3080 workhorse)
- **2025-11-25**, **Apache-2.0**, 6B (Tongyi-MAI / Alibaba), distilled to 5–15 steps, **fp8 ≈6 GB**.
  Photoreal-leaning but many styles; "what most people use locally now." Needs LoRAs for stronger
  anime/NSFW. Perfect for fast iteration + props/backgrounds on the 10 GB 3080, zero license risk.

### Illustrious-XL v2.0 — keep as the anime workhorse
- `creativeml-openrail-m` (HF tag confirmed, 2025-04-18). Commercial use **OK**, **irrevocable** grant
  (unlike Krea/FLUX/NVIDIA), but carries OpenRAIL **behavioral use restrictions** (Attachment A) +
  attribution. Unmatched **LoRA ecosystem** (our Gemma-san LoRA lives here) and Danbooru-tag expression
  control. Weakness: poor NL prompt understanding vs DiT models → pair with Qwen for complex scenes.
- **Civitai-merge caution:** top community anime checkpoints (WAI-Illustrious, Nova Anime XL,
  One-Obsession, etc.) are often **merged with NoobAI**, which **contaminates the license with
  NoobAI's NC addendum**. For a monetized product, prefer **pure-Illustrious-lineage** checkpoints
  or our own LoRA on the clean Illustrious base — do **not** ship output from a NoobAI-tainted merge.

### Krea-2 — the bar (re-confirmed at source)
- Krea-2 Community License: *"limited… revocable, royalty-free"*; commercial use **only if total
  company-wide annual revenue < $1,000,000 USD**, *"must immediately cease Commercial Use"* if you
  cross it; plus AUP/content obligations. Released 2026-05 (Qwen-Image-based, Qwen VAE + Qwen3-VL
  encoder, Comfy-Org checkpoints). **Every Apache/MIT option above beats it on license cleanliness.**

---

## Corrections to the current stack (CLAUDE.md / docs)

1. **"SANA (Apache)" is INCORRECT.** SANA's weights are `nvidia-open-model-license`
   (`license_name: nvidia-open-model-license`, HF tag `other`), **not Apache**. That license **does
   permit commercial use with no revenue cap**, but it is **revocable**, requires Trustworthy-AI
   compliance + attribution, and SANA's **Gemma-2-2B text encoder** adds Google **Gemma Terms**; the
   model card states research-intended use. → **Either** keep SANA but document it accurately as
   "NVIDIA Open Model License, revocable, +Gemma terms" (it's still cleaner than Krea — no cap),
   **or** (recommended) swap clean-diagram/vector duty to **Qwen-Image** (Apache, best text/diagram
   rendering) or **HiDream-O1** (MIT) for a no-asterisk license.

2. **The "AVOID FLUX (non-commercial)" instinct is half-right.** FLUX `[dev]`/`[klein-9B]`/Kontext
   weights are NC + revocable, **but their OUTPUTS are explicitly monetizable**. We don't need them
   (not anime; heavy), but the blanket "AVOID" reasoning should be "avoid because not-anime + heavy +
   revocable-model," not "can't monetize output." The same output-vs-model distinction is what makes
   **Anima usable**.

3. **NoobAI / NoobAI-tainted Civitai merges = AVOID** for monetized output (explicit NC-on-products
   addendum), even though it's the strongest raw-anime community lineage and the NC clause is
   *arguably* unenforceable against the base's share-alike. Not worth the risk on monetized work.

---

## Reaction-picture / meme strategy (expressive faces, smug/surprised/pointing/crying-laughing/chibi)

No single model is a dedicated "reaction/sticker" engine; the winning recipe is two-pronged:
- **Expression & pose control:** **Danbooru-tag** models — **Anima**, **Illustrious/Neta-Lumina** —
  give fine-grained face tags (`smug`, `:d`, `>:(`, `surprised`, `tears`, `pointing`, `chibi`,
  `sweatdrop`, `tv-glitch`) → the proven way to nail anime reaction faces. Anima's flat-color,
  sharp-line default is ideal for sticker/emote/chibi art.
- **"Describe a specific composition" + in-image text:** **Qwen-Image** (Apache) — best NL prompt
  understanding + best caption/text rendering — for "a smug purple-haired chibi pointing at a graph
  reading 'SLOP UP 300%'." **HiDream-O1** (MIT) is the runner-up for prompt-faithful comp.
- For an actual **chibi-emote sheet** (the pngtuber expression set), an Illustrious/Anima base + a
  small expression LoRA + a fixed seed/character LoRA is the standard pipeline (mirrors the existing
  Gemma-san expression set → pngtuber states).

---

## Bottom line (top picks for slopstudio)

1. **Anima** — best new anime quality, tiny, ComfyUI-native; **outputs are explicitly commercial**
   (NC only restricts hosting the model). The new anime workhorse; likely beats Illustrious on
   linework/versatility per community.
2. **Neta-Lumina** — **Apache-2.0** true anime specialist; the zero-asterisk commercial-safe base.
3. **Qwen-Image (+2.0 / Edit-2511-Anime)** — Apache; best prompt understanding + text → reaction-pic
   composer + props/backgrounds/diagrams (better SANA replacement).
4. **HiDream-O1-Image** — newest **MIT** SOTA-for-size; backgrounds/props/diagrams, cleanest license.
5. **Z-Image-Turbo** — Apache, 6 GB, fast; low-VRAM generalist for the 3080.
- **Keep** Illustrious-XL v2.0 for the LoRA ecosystem + Gemma-san LoRA (clean-ish OpenRAIL).
- **Fix** the SANA="Apache" note (it's NVIDIA Open Model License, revocable, +Gemma).

---

## Sources

- Anima — HF model card / README+LICENSE: https://huggingface.co/circlestone-labs/Anima · HF API: https://huggingface.co/api/models/circlestone-labs/Anima · Civitai: https://civitai.com/models/2458426/anima-official · review: https://diffusiondoodles.substack.com/p/anima-light-fast-and-slightly-unruly
- Neta-Lumina — HF: https://huggingface.co/neta-art/Neta-Lumina · blog: https://www.neta.art/blog/neta_lumina/ · site: https://www.netalumina.org/
- Lumina-Image 2.0 — https://github.com/Alpha-VLLM/Lumina-Image-2.0 · ComfyUI: https://comfyanonymous.github.io/ComfyUI_examples/lumina2/
- Qwen-Image — HF: https://huggingface.co/Qwen/Qwen-Image · HF API (license=apache-2.0): https://huggingface.co/api/models/Qwen/Qwen-Image · LICENSE: https://github.com/QwenLM/Qwen-Image/blob/main/LICENSE · output/commercial issue #98: https://github.com/QwenLM/Qwen-Image/issues/98 · Edit-2511: https://huggingface.co/Qwen/Qwen-Image-Edit-2511 · Anime LoRA: https://huggingface.co/prithivMLmods/Qwen-Image-Edit-2511-Anime · Qwen-Image-2.0: https://qwenimages.com/blog/qwen-image-2-release
- HiDream-O1-Image — HF: https://huggingface.co/HiDream-ai/HiDream-O1-Image · -Dev: https://huggingface.co/HiDream-ai/HiDream-O1-Image-Dev · blog: https://wavespeed.ai/blog/posts/hidream-o1-image-dev-pixel-unified-transformer/ · ComfyUI: https://docs.comfy.org/tutorials/image/hidream/hidream-o1
- HiDream-I1 — https://comfyui-wiki.com/en/news/2025-04-08-hidream-i1-open-source-release · https://blog.comfy.org/p/hidream-i1-native-support-in-comfyui
- Z-Image-Turbo — HF: https://huggingface.co/Tongyi-MAI/Z-Image-Turbo · HF API (apache-2.0, 2025-11-25): https://huggingface.co/api/models/Tongyi-MAI/Z-Image-Turbo · low-VRAM quant: https://civitai.com/models/2169712
- Illustrious-XL v2.0 — HF: https://huggingface.co/OnomaAIResearch/Illustrious-XL-v2.0 · HF API (creativeml-openrail-m, 2025-04-18): https://huggingface.co/api/models/OnomaAIResearch/Illustrious-XL-v2.0 · license discussion: https://huggingface.co/OnomaAIResearch/Illustrious-XL-v2.0/discussions/1
- SANA — HF: https://huggingface.co/Efficient-Large-Model/Sana_1600M_1024px · HF API (nvidia-open-model-license, 2024-11-04): https://huggingface.co/api/models/Efficient-Large-Model/Sana_1600M_1024px · NVIDIA Open Model License: https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license/
- FLUX.2-klein-4B (apache-2.0) — https://huggingface.co/black-forest-labs/FLUX.2-klein-4B · HF API: https://huggingface.co/api/models/black-forest-labs/FLUX.2-klein-4B · klein guide: https://docs.comfy.org/tutorials/flux/flux-2-klein
- FLUX.2 [dev] + licensing — https://huggingface.co/black-forest-labs/FLUX.2-dev · BFL licensing: https://bfl.ai/licensing · FLUX [dev] NC license terms: https://bfl.ai/legal/non-commercial-license-terms
- Chroma1-HD — https://huggingface.co/lodestones/Chroma1-HD
- Krea-2 (the bar) — license: https://www.krea.ai/krea-2-licensing · HF: https://huggingface.co/Comfy-Org/Krea-2 · ComfyUI: https://docs.comfy.org/tutorials/image/krea/krea-2
- Pony Diffusion V7 — https://civitai.com/models/1901521/pony-v7-base · https://civitai.com/articles/6309 · guide: https://apatero.com/blog/pony-diffusion-v7-auraflow-complete-guide-2025
- NoobAI-XL (AVOID) — HF: https://huggingface.co/Laxhar/noobai-XL-1.1 · NC addendum note: https://x.com/satos73/status/1899426295492309103 · license analysis: https://civitai.com/articles/18619/what-the-license
- HunyuanImage 3.0 (geo-locked) — LICENSE: https://huggingface.co/tencent/HunyuanImage-3.0/blob/main/LICENSE · https://comfyui-wiki.com/en/news/2025-09-27-tencent-open-source-hunyuan-image-3-0
- Civitai 2026 scene guide — https://civitai.com/articles/30487/guide-to-the-general-ai-scene-in-2026 · anime model comparison: https://anifusion.ai/models/
