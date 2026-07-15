# slopstudio — Project File Format

A slopstudio project is **one JSON document** and it is the **source of truth**. The
editor reads/writes it (pretty-printed, stable key order → clean git diffs), watches it
for external edits (so an LLM or a human can edit the file and the editor live-reloads),
and exposes the same edits via the control API (`docs/PROVIDER_PROTOCOL.md` covers
providers; the control API mirrors these structures). Human/LLM annotations go in
first-class `notes` fields, never JSON comments, so editor round-trips never lose them.

Filename: `<name>.slop.json`. Top-level `schema` is versioned; the editor migrates older
schemas forward.

## Top-level shape

```json
{
  "schema": "slopstudio.project/1",
  "meta":   { "...": "see §meta" },
  "assets": { "<hash>": { "...": "generated-asset cache index — §assets" } },
  "tracks": [ { "...": "ordered display + z-order — §tracks" } ],
  "rows":   { "<row_id>": { "...": "a pipeline instance — §rows" } },
  "clips":  { "<clip_id>": { "...": "a time-ranged unit — §clips" } },
  "variants": [ { "...": "non-destructive cuts — §variants" } ],
  "exports":  [ { "...": "staging queue entries — §exports" } ]
}
```

Rows, clips, and assets are **maps keyed by id** (stable, easy for an LLM to address and
patch). Ordering is explicit: a track lists its rows in z-order; a row's clips are ordered
by `start`.

## §meta
```json
"meta": {
  "title": "Recettear's item-pricing bug, explained",
  "fps": 60,
  "resolution": [1920, 1080],
  "sample_rate": 48000,
  "format": "1080p",
  "sfx": false,
  "gain_db": 0,
  "speech_gain_db": 12,
  "speech_rate": 1.0,
  "song_credits": true,
  "song_credit_corner": "tl",
  "song_credit_secs": 10,
  "bg": "checker",
  "letterbox": 0,
  "anchors": { "bust": [0, -104], "code_host": [660, 194], "tr_room": [0, -673] },
  "notes": "video essay; dense reaction pics; Gemma host"
}
```

**Project-global settings** (all optional; edited in the editor's docked **Project** tab,
persisted here). **`format`** picks the default bundle — `"1080p"` (the default; the locked
full-length-video conventions: built-in SFX off, speech 1.0x) or `"portrait"` (shorts:
1080x1920 canvas when no explicit `resolution`, built-in SFX on, speech 1.0x, faster
pacing in `slop.py skeleton`). Explicit keys always win over the format's defaults:
- **`sfx`** (bool) — the BUILT-IN transition one-shots (`library/sfx/`, regenerate with
  `tools/gen-sfx.py`): `pop.wav` (a low mouth-pop) on the default pop entrance (incl.
  pop-through), `whoosh.wav` (a soft low swing) on slide_*/rise + the avatar pose-swap
  slide. Identical in preview + export; audio clips you add yourself are never gated.
  Per-clip opt-out: `params.sfx: false`. Alternates ship too, reachable as authored cues
  (`params.sfx_cue`): `pop-blip` (a Minecraft-style ~1 kHz pickup) and `whoosh-sharp`
  (a sharper arcing swish).
  **Authored gag cues** are separate and NOT gated by this toggle (full videos keep their
  punchline sounds): `params.sfx_cue: "awkward"` (the cough-and-crickets awkward-moment
  sting — the skeleton's clueless gag sets it automatically) or `"boom"` (the slowed
  vine-boom, for ominous outros) on any clip plays `library/sfx/<cue>.wav` at the clip's
  start (+`sfx_at` s, `sfx_gain_db` trims), and the MUSIC DUCKS around it (fade out 0.3s
  → floor → back over 0.6s; `sfx_duck: false` opts out). In a skeleton: `"sound": "boom"`
  on a beat.
- **`gain_db`** — master gain on the FINAL mix, applied after per-clip loudness
  normalization (preview mixer + export.sh post-amix `volume=`, identically).
- **`speech_gain_db`** (default **+12**) — a GLOBAL boost added to every **tts** (speech) clip,
  ON TOP of its loudness normalization + per-clip/lane `gain_db`. Because it's the same fixed dB
  on every line, the relative dynamics between lines are unchanged — the whole speech track just
  sits louder (e.g. over music) without re-normalizing. Preview mixer + export identically; does
  NOT touch music/video/SFX. (Distinct from `gain_db`, which is the whole-mix master.)
- **`speech_rate`** — the default playback `rate` for tts clips that carry none of their
  own (pitch-preserving). Changing it does NOT rescale existing clip durations — recompile
  or `slop.py retime` a composed cut.
- **`song_credits`** (default **true**) — the automatic on-screen **"now playing" chip**:
  when a song STARTS, a `♪ Title — Artist` pill fades into a corner for `song_credit_secs`
  (default **10**), then fades out. Reads the music **asset's** `meta.title`/`meta.artist`
  (the same metadata that feeds the export description credits — one source of truth). A song
  INSTANCE = a contiguous run of same-asset music clips: a looped bed triggers ONCE; the same
  song re-used later triggers again. **`song_credit_corner`** = `tl`/`tr`/`bl`/`br` (default `tl`)
  is the STARTING corner; the chip then **shifts vertically to dodge on-screen text** — it measures
  the ACTUAL bounding boxes of every caption/plate/transcript drawn that frame (so a hand-moved
  plate is respected, not guessed) and slides down/up its column until clear (column full → shifts
  up above the text). When that target moves (a plate appears/vanishes) it **eases there with a
  spring + motion-blur trail**, like the host pose-swap slide — deterministic in export. Per-song
  opt-out: the start clip's `params.credit: false`. Drawn in BOTH the live preview and the export.
  Backfill/edit the metadata with `slop.py musicmeta` (ID3 auto-detect) or the Inspector fields.
- **`bg`** (default **`"checker"`**) — the frame background BEHIND content (the dead space behind an
  inset / host / letterboxed media). `"checker"` = a subtle diagonal-scrolling soft checkerboard in the
  brand purples (gives that space a branded texture, not a black void); `"black"`/`"none"` = the old flat
  base. A cover backdrop or an explicit `filler` blur clip still hides it. (Project ▸ background.)
- **`checker_scroll`** (default **2.0**) — the checkerboard's diagonal drift speed (`1.0` = the original
  slow drift; `0` = hold still). (Project ▸ background ▸ checker scroll speed.)
- **`letterbox`** (default **0**) — cinematic black bars top+bottom, as a fraction of frame HEIGHT per
  bar (~`0.11` ≈ 2.35:1). Composes with any clip's `filter`. (Project ▸ background ▸ letterbox bars.)

## §assets — the generation cache index
One entry per generated asset, keyed by its content hash (see protocol §param-hash). The
editor never blocks on these; an entry is just a reference + status.
```json
"assets": {
  "a_3f9c…": {
    "provider": "tts", "type": "speech",
    "params": { "text": "Fufu~ welcome back.", "voice_preset": "gemma-san", "instruct": "smug, teasing", "seed": 7 },
    "inputs": [],
    "status": "ready",                       // queued | running | ready | error
    "uri": "cache://tts/a_3f9c.wav",
    "meta": {
      "duration": 1.84,
      "word_timings": [ { "w": "Fufu", "t0": 0.06, "t1": 0.55 }, "…" ],
      "title": "Deadly Roulette", "artist": "Kevin MacLeod",   // MUSIC: drive the on-screen now-playing chip
      "attribution": { "attribution_text": "\"…\" — CC BY 4.0" } // present for sourced music/art → export credits
    }
  }
}
```
Editing a clip's generation params changes the computed hash → a new (initially `queued`)
asset entry; the old asset stays cached so the preview keeps showing it until the new one
is `ready`.

A **`type:"video"`** asset's `uri` (or an explicit `src`) is the **source media** the editor decodes
**in-process** (libav: seek + decode + swscale→RGBA→texture); `fps`/`frames`/`w`/`h` are filled from the
file on first open, so a video asset can be as small as `{ "type":"video", "uri":"…mp4" }`. An optional
decoded **`proxy`** (`cache://video/<name>`, a dir of decimated JPEG frames from `tools/video-proxy.py`,
gitignored) is a FALLBACK — used when the editor is built without libav, run with `--no-video-decode`, or
the source won't open. See `video` under §clip params.

## §tracks — display + compositing order
```json
"tracks": [
  { "id": "tk_fg",   "name": "Foreground", "kind": "video", "rows": ["r_avatar", "r_react"] },
  { "id": "tk_bg",   "name": "Background", "kind": "video", "rows": ["r_diagram"] },
  { "id": "tk_vo",   "name": "Narration",  "kind": "audio", "rows": ["r_vo"] },
  { "id": "tk_mus",  "name": "Music",      "kind": "audio", "rows": ["r_music"] }
]
```
Video tracks composite top-of-list over bottom-of-list; audio tracks mix.

## §rows — one generation pipeline each
```json
"rows": {
  "r_vo":     { "type": "tts",     "name": "Gemma VO", "params": { "voice_preset": "gemma-san" }, "clips": ["c1","c2"] },
  "r_avatar": { "type": "avatar",  "name": "Gemma",    "params": { "rig": "gemma-pngtuber", "driven_by": "r_vo" }, "clips": ["c_av"] },
  "r_react":  { "type": "image",   "name": "Reactions","params": { "model": "illustrious-xl-2.0", "lora": "gemma-san" }, "clips": ["c_r1"] },
  "r_music":  { "type": "music",   "name": "BGM",      "params": { "source": "jamendo", "mood": "calm lo-fi, no vocals" }, "clips": ["c_m1"] }
}
```
`type` selects the pipeline; `params` are row-level defaults (e.g. the voice preset).
Pipeline types: `tts · avatar · image · video · music · caption · code · shape · diagram ·
plot · scene · blur · filter · filler · anchor · effect · capture · group`. New
generation-provider types/params are discovered from provider `/capabilities` — the editor builds
the param UI from the schema, so adding a model needs no editor change.

## §clips — time-ranged units
```json
"clips": {
  "c1": {
    "row": "r_vo", "start": 0.0, "dur": 1.84,
    "asset": "a_3f9c…",
    "params": { "text": "Fufu~ welcome back.", "instruct": "smug, teasing", "emotion": "smug" },
    "effects": [],
    "transform": { "pos": [0,0], "scale": [1,1], "rot": 0, "opacity": 1, "anchor": [0.5,0.5] },
    "keyframes": {},
    "notes": ""
  },
  "c_r1": {
    "row": "r_react", "start": 1.9, "dur": 2.5,
    "asset": "a_77b1…",
    "params": { "prompt": "gemma-san pointing smugly, comedic", "seed": 12 },
    "effects": [ { "id": "fx_glow", "shader": "bloom", "params": { "threshold": 0.8, "radius": 4 } } ],
    "transform": { "pos": [420,-180], "scale": [0.6,0.6], "rot": 0, "opacity": 1, "anchor": [0.5,0.5] },
    "keyframes": {
      "transform.scale": [
        { "t": 1.9, "v": [0.0,0.0], "interp": "spring", "spring": { "stiffness": 380, "damping": 26 } },
        { "t": 2.1, "v": [0.6,0.6] }
      ]
    },
    "notes": "pop-in reaction"
  }
}
```
- `asset` references the generated content; `params` are this clip's generation inputs
  (the things that change the asset hash) plus presentation-only fields.
- `effects` is an ordered stack (each reads the previous output as `src`).
- `transform` is the clip's placement in the composite.
- **`params.anchor`** (optional) names a key in **`meta.anchors`** (category → base `[x,y]`;
  seeded by `slop.py skeleton`/`transcript`, tuned in the Project panel). The clip renders at
  **anchor + transform.pos**, so its pos is an OFFSET and one project-level knob nudges every
  clip of that category — in that project only. No `anchor` param → absolute pos (back-compat).
  Current categories: `bust` (host bust shots), `code_host` (the host while a code card is up),
  `tr_room` / `tr_scene` / `tr_content` / `tr_code` (the shorts transcript bands: solo-room ·
  room-with-scene-backdrop · content beat · code beat).
- **caption-anchor clips** (row type **`anchor`**, conventional row `r_capanchor`): a clip that is
  a **move handle for a TIME RANGE of captions** — every caption/text clip whose *start* falls
  inside the anchor clip's span renders shifted by the anchor's `transform.pos`. Because
  `slop.py transcript` rewrites only the transcript chunks, the offset **survives a caption
  regen** — author time-range caption moves as an anchor clip, never as per-chunk pos nudges
  (those are wiped on regen). Overlapping anchors sum; `params.rows` (array of row ids)
  narrows the targets (default: every caption/text row). The clip draws nothing itself.
  Author via **`slop.py anchor <proj> --beat b04[..b06] | --t0 T --t1 T --pos X,Y`** (no
  `--pos` lists; `--rm <id>` removes), or in-editor: Edit ▸ Add special clip ▸ Caption anchor.
- `keyframes` maps a dotted param path → an ordered list of keyframes. Each keyframe has
  `t`, `v`, and an `interp` (`linear · bezier · constant · spring`); `bezier` adds
  `in`/`out` handle vectors, `spring` adds `{stiffness,damping}`. **Any** numeric param is
  animatable this way: `transform.pos/scale/opacity/anchor` (Ken Burns = animated
  pos+scale; a pop-in = a `spring` on scale; a fade = `opacity`) and clip params like
  `params.typewrite` / `params.scroll` / `params.grow`.
  - **`t` is TIMELINE seconds (absolute)**, sampled at the playhead — *not* clip-local. (So
    keyframes don't yet move with a dragged clip; author them at the clip's actual time.)
  - The **left** keyframe's `interp` drives each segment. A trailing `spring` keeps settling
    past its target (overshoot → rest); `v` is a scalar or a vec2 matching the param.

## §native compositing clips (code · caption · shape · diagram · plot · scene)

These clip types render **live in the compositor** (no provider, no generation) — instant,
keyframe-animatable overlays built for technical videos.
The row `type` selects the renderer; the params live on the clip. Place/scale with the
`transform`; animate any numeric param via `keyframes`.

**`scene`** — a transparent reflowable motion-graphics tree. `params.script` is Lua that
receives `(t, data, frame)` and returns a node tree; `params.data` is JSON content. Prefer
the reusable widgets in `presets/lua/std.lua` so content remains editable without touching
Lua. `slop.py scene-widgets` lists the live catalogue, `slop.py scene-check` validates every
scene headlessly, and `docs/SCENE_COOKBOOK.md` contains copyable recipes.

```json
"params": {
  "script": "local t,d=...\nreturn widgets.stat(t,d)",
  "data": {"from":160,"to":400,"unit":"%","label":"defender strength"}
}
```

**`code`** — a syntax-highlighted source/decompilation card. Self-sizes to its content.
```json
"params": {
  "code": "undefined4 SetHook(void)\n{\n  DAT = SetWindowsHookExA(4,...);\n}",
  "lang": "c",            // c (default) · lua · toml · text
  "title": "MinkIt.dll · SetHook",  // optional title bar (traffic-light dots)
  "line_numbers": true, "first_line": 885,   // gutter; first_line = real decompile ref
  "highlight": [5],        // 1-based lines to emphasise (others dim)
  "font_px": 30, "pad": 26,
  "typewrite": 1.0,        // 0..1 chars revealed (animate for a typewriter + caret)
  "scroll": 0, "max_lines": 0   // viewport scroll (lines) / height (0 = all)
}
```

**`caption`** (alias `text`) — styled on-screen text: up to three stacked lines.
```json
"params": {
  "text": "らき☆マス", "sub": "Raki☆Masu", "gloss": "the 2007 desktop set",
  "style": "jp_lesson",   // plain · lower_third · term (pill) · jp_lesson · quote
  "align": "center",       // left · center · right
  "font_px": 56, "box": true, "wrap_px": 0,
  "color": [245,247,252], "accent": [220,120,200],   // [r,g,b] or [r,g,b,a] 0..255
  "box_color": [16,16,22,214], "sub_color": [...], "gloss_color": [...]
}
```
- **`style: "quote"`** — a full-screen CENTRED pull-quote card (a decorative accent quote-mark, the
  quote, an accent rule, the attribution) with a built-in settle entrance (a rise + the mark scales in
  over the clip head, on top of the fade). `text` = the quote, `sub` = the attribution; `place` is
  ignored (it centres); `box: true` adds a scrim for legibility over footage. Meant to sit on the checker
  bg between footage beats — the "text card like the quotes in the recettear videos" preset.

**`shape`** — a vector callout.
```json
"params": {
  "shape": "box",          // box · ellipse · line · arrow · bracket
  "w": 560, "h": 44, "round": 6,            // box/ellipse size (project px)
  "from": [430,360], "to": [60,70], "grow": 1.0,  // line/arrow endpoints (centre-rel px); grow 0..1 animates
  "color": [255,214,90], "thickness": 4, "fill": [255,214,90,30]   // fill optional
}
```

**`diagram`** — reusable boxes + arrows (flow chains / labeled graphs) with a staged reveal. For
explainer flows (the gcalsrv path, Act 7) and relationships (the font→window-size finding, Act 9).
```json
"params": {
  "flow": ["XP app", "WinINet", {"label":"Schannel","sub":"@ localhost"}, "Lua EVENTS/MAIL"],
  "dir": "h",              // flow layout: h (default) · v   (ignored when `nodes` is set)
  "reveal": 1.0,           // 0..1 stages it in (node,arrow,node…); animate for a draw-in
  "title": "the fake Google", "font_px": 30, "gap": 56,
  "accent": [120,205,235], "fill": [26,30,40,235], "color": [238,244,252]
  // OR an explicit graph instead of `flow`:
  // "nodes": [{"id":"a","label":"XP app","x":-400,"y":0},{"id":"b","label":"server","x":300,"y":0}],
  // "edges": [{"from":"a","to":"b","label":"localhost"}]   // from/to = node id or array index
}
```

## §clip params by type — the full knob list (everything is hand-editable + live-reloaded)

Every param below is plain JSON you can edit by hand — the editor **watches the file and
auto-reloads** on external/LLM edits — and every numeric one is **keyframeable** (§clips
`keyframes`, dotted path `params.<name>`). Nothing is hard-coded; tweak + see it live.

**`image` / footage** — a generated or external still, drawn with the `transform`:
- generation (changes the asset hash): `prompt`, `seed`, `lora`, `lora_strength`, `size`,
  `steps`, `cfg`; img2img/inpaint: `init_b64`, `mask_b64`, `denoise`.
- presentation (live, no regen): **`blur`** (gaussian sigma in *source* px — a cached low-res
  defocus for "text over footage"; keyframe 0→N for a smooth blur-in), **`dim`** (RGB-multiply
  brightness 0..1, 1 = full bright; keyframe down *with* `blur` so white titles stay legible).
- color grade (live, no regen): **`saturation`** (chroma scale, 1=neutral; <1 tames oversaturated
  gens), **`contrast`** (around mid-gray, 1=neutral; <1 flattens) — both baked into the same cached
  processed copy as `blur` — plus **`temperature`** (warm +/cool −) and **`tint`** (green −/magenta +),
  a cheap per-channel multiply at draw time. All keyframeable + live-reloaded; the lever for matching
  a background's tone to the host (the purple host pops on a brighter/warmer, less-saturated plate).
- **cinematic `filter`** (image *and* video) — a named "look" = a grade + a post pass (edge vignette +
  animated film grain): `cinematic` (warm filmic) · `noir` (high-contrast B&W — full desat on stills;
  moving video keeps colour, since video skips the sat texture pass) · `vintage` (faded 70s) · `cyber`
  (cold neon) · `dream` (bright airy). The grade values are DEFAULTS — an explicit `saturation`/`contrast`/
  `temperature`/`tint`/`dim` still wins. Grain is deterministic per frame time. (Inspector Look ▸ *cinematic
  filter*.) Scene-wide **`meta.letterbox`** (fraction of frame height per bar; ~0.11 ≈ 2.35:1) adds cinematic
  bars top+bottom. Composes with everything below.
  - **Whole-frame `filter` CLIP** (a clip of `type:"filter"`, the colour sibling of the `blur` transition
    clip) — grades the ENTIRE composite for its span (host, captions, footage — every layer), not one clip.
    `params.filter` picks the preset (same five), `params.strength` (0..1) blends it, and it fades in/out at
    the clip's edges. Because it grades the read-back frame buffer (not a per-clip texture pass), **full noir
    desaturation works on the whole frame incl. moving video.** Add via Edit ▸ Add special clip ▸ Cinematic
    filter (or a `filter` row); preview == export.
- **`inset_style`** (image *and* video) — a fancy frame treatment for inset footage that composes with the
  `filter` + `glow`: `device` (dark rounded bezel) · `polaroid` (thick photo border) · `card` (thin, round) ·
  `clean` (the default pro border forced on). Each carries its drop shadow. (Inspector Look ▸ *inset style*.)
  The base **`frame`** (bool / `{color,thickness,radius,shadow}`; default-ON for a non-fullscreen inset) and
  **`glow`** (`{color,size,strength}`) knobs still apply — `inset_style` just names a common bundle.

**`video` / footage-in-motion** — a B-roll clip backed by its **source mp4**, decoded **in-process** by
libav. The compositor maps the playhead → the clip's local time → a frame index → a decoded texture
(LRU-bounded, so a long scrub never pins VRAM); identical in preview + export. Point the asset at the
source (`uri`/`src`) and the decoder fills `fps`/`frames`/`w`/`h`. **Fallback proxy** (no libav /
`--no-video-decode`): `tools/video-proxy.py <src> [--fps 15] [--max-dim 1280] [--ss S --t D] [--into P
--key K]` extracts a decimated-JPEG `proxy` dir → `cache://video/<name>`. **A video clip needs a
`type:"video"` ROW** (a clip's type comes from its row); dropping a video file/library item auto-creates one.
- retime (live): **`in`** (source in-point seconds; also the loop IN / **A** point), **`speed`** (×, e.g.
  0.5 slow-mo / 2 fast), **`loop`** (`true` wrap · `false` hold the last frame · `"pingpong"` bounce
  forward/backward — moving b-roll loops without the hard rewind seam), and **`loop_out`** (source seconds;
  the loop OUT / **B** point — absent = source EOF). `in`/`loop_out` are the **A-B loop segment**: the clip
  plays + loops `[in, loop_out]`, so you can pick a clean loop inside a long source without hand-cutting it
  (editor: Inspector ▸ playback ▸ loop segment; the "set A/B" buttons capture the frame under the playhead).
  A retimed clip (`speed`≠1 or pingpong) OR an A-B sub-loop (`loop_out` set) plays its own audio MUTED (it
  would desync). Loop wrapping uses the REAL decoded stream end — containers that overpromise
  `nb_frames`/duration are learned + corrected at the first tail decode (no "can't decode source" gap).
- presentation (live): **`dim`** + **`temperature`**/**`tint`** (same draw-time tint as image) + the full
  `transform` (pos/scale/anchor/opacity, all keyframeable → Ken Burns over moving footage). *Not* supported
  on video: `blur`/`saturation`/`contrast` (those are texture-PROCESSED + path-keyed; skipped so a scrub
  can't grow an unbounded per-frame cache — keep a still for a blurred backdrop, or add a bounded cache later).
- in-process decode is full-rate from the source (no proxy-fps cap); NVDEC/d3d11va hardware decode can later
  swap the decode source inside `VideoDecoder` behind the same clip+time→SRV contract. (If you fall back to a
  proxy, its `--fps` caps B-roll smoothness — re-extract higher for the master.) Demo: `../slopstudio-projects/demos/video-broll.slop.json`.

**`layout` + `crop` (image AND video) — framing without a derived asset:**
- **`layout`** (render-time adaptive placement): `inset`/`inset-left`/`inset-right` (fit ~half-frame beside
  the host — portrait: the top band), `fullscreen` (cover; degrades to contain on an extreme aspect so a
  banner isn't cropped to mush), `fit` (contain, letterbox over the filler), `cover` (ALWAYS cover — never
  degrades; for backdrops + montage flashes that must fill). `transform` layers ON TOP (pos nudge, scale ×).
- **`crop`**: `[x, y, w, h]` as **fractions of the source** (0..1) — zoom a beat into part of a grab (the
  copy dialog in a full XP desktop) with **no pre-cropped derived asset**. Composes with `layout` (which
  fits the *cropped* region), `transform`, mirror, and Ken Burns. `slop.py` visual spec passes `crop`
  through for image/video beats.

**`tts`** — speech, resolved from row+clip+`voice_preset` (`presets/voices/<name>.json`):
- `text`, `emotion` (per-line delivery — design mode only), `voice` (the design *instruct*),
  `voice_preset`, `seed`, `language`, `ref_text`.
- **`transcript`** — optional DISPLAY text for the animated on-screen transcript
  (`slop.py transcript` / portrait retime → caption chunks on `r_transcript`): what the
  viewer reads when `text` is written weird for the TTS ("Heh~", phonetic spellings).
  Defaults to `text`.
- **clone vs design:** a preset shipping a golden `ref` clip → CLONE (identical timbre across
  lines, but drops `emotion`/instruct → flat). No ref → DESIGN (VoiceDesign derives the voice
  from `voice`+`emotion`; drifts per call/seed). A smug catchphrase ("fufu~") wants a design
  clip — see `../slopstudio-projects/demos/fufu-lab.slop.json` (the hiragana ふふ reads as a laugh; use romaji).
- **loudness** (any audio clip — `tts`/`music`): `gain_db` (manual trim) + **`normalize`** (bool) /
  **`normalize_db`** (target dBFS RMS, default −20). Normalize brings a quiet/whispered clip up to a
  consistent level vs the rest (clamped −12..+24 dB); editor-measured (RMS), applied identically in
  the preview mixer + the export plan. Set per clip or on the row (clip overrides). WAV only in-editor;
  mp3 keeps its `gain_db` (ffmpeg mixes it).
- **volume automation (ramp):** `gain_db` is **keyframeable** (`keyframes["params.gain_db"]`, dB over
  project time, linear interp) — a music-lane volume ramp (title-drop swell, taper-into-bed). The
  preview mixer evaluates the envelope per-sample; the export emits a piecewise-linear ffmpeg `volume`
  expression (clip-local, composed with the gag-cue duck). `slop.py bed --ramp "t:dB,…"` authors it
  across a looping bed. Lane gain + normalize still add on top of the keyframed clip gain.

**`avatar`** — pngtuber (static pose per expression + audio-reactive bob/light-up). Row params:
`rig`, `driven_by` (the VO row it lip-syncs + emotes from). `rig` resolves in two places (presets
first): a baked **`presets/avatars/<rig>/manifest.json`** (e.g. `gemma-pngtuber`, `gemma-chibi`),
or a **library-authored rig** `library/avatars/<rig>.avatar.json` = `{ "prefix": "gemma-",
"fallback": "neutral", "emotions": { "<emotion>": "<library-image.png>" } }` — emotion E resolves to
`library/images/<prefix>E.png`, with `emotions` overrides winning (a manually-named emotion → a
specific library image). Author/edit rigs in the editor's **Library ("+ avatar") + Viewer** (prefix
+ per-emotion override pickers); **drop a rig onto the timeline** → an avatar clip from scratch (the
row gets the `rig`; all its clips share the emotion frames). The canonical emotion set is
neutral/happy/smug/confused/annoyed/surprised/sad.
Clip params (all live): `emotion` (pose tag, or `"auto"` → the driven line's emotion),
`bob`/`bob_speed` (idle breathing), `talk_bob`/`talk_scale` (audio-reactive up-down / scale),
`talk_attack`/`talk_decay` (1/s envelope on the talk level), `lightup` (talk brighten), `dim`
(silent brightness, <1 dims when quiet). The current pngtuber rig is static-pose: audio
drives bob/light-up, not mouth/blink frames. Mouth/blink/deformation belongs to a future
Inochi2D tier if that path is revived.
- **integrate into a plate — AUTO by default:** an avatar clip samples the bg plate behind it and grades
  the host toward it (gentle desaturate + match warm/cool) + adds a contact shadow automatically, no params
  needed. `auto_grade: false` opts out. Override any piece explicitly: `saturation`/`contrast` (grade the
  host, same cached path as image clips), `temperature`/`tint` (white-balance), `shadow` (0..1 contact-shadow
  pool at her feet). The auto `shadow` only draws on NON-floating full-body shots (a float pose hovers;
  bust/closeup have feet off-frame); **`foot_shadow`** (bool or 0..1 intensity) FORCES the ground shadow on
  for ANY pose/framing — e.g. a floating pose grounded by a contact shadow (inspector "foot contact-shadow").
  Project-level **`meta.vignette`** (0..1) darkens frame edges to unify bg + host. The "blend the character
  into the background" combo = a little grade + a contact shadow + a touch of vignette.
- **face anchor — per-sprite override sidecar:** `framing` (`bust`/`closeup`/`full`) anchors the WHOLE sprite
  on the DETECTED face (pale-skin heuristic). The detector over/under-reaches on some poses (bares more skin ⇒
  wider "face" ⇒ renders smaller). Fine-tune per sprite FILE via the inspector **"Tune face box" gizmo** →
  a `"face":{cx,eyeY,w}` (source px) block in the sprite's `<sprite>.png.meta.json` sidecar that WINS over the
  detector, so a rig's poses hold a consistent apparent size. `tools/gen-face-boxes.py <rigdir>` seeds them.

**`music`** — `source` (jamendo | ace-step), `mood`, `seed`. The asset's `meta.title`/`meta.artist`/
`meta.attribution.attribution_text` drive BOTH the on-screen **now-playing chip** (meta.song_credits)
and the **export description credits** (only songs actually placed on a clip are credited, deduped).
A dragged-in song auto-fills this metadata from its **ID3 tags** on import; `slop.py musicmeta <proj>`
backfills a whole cut; edit it in the Inspector's *song metadata* fields (`params.credit: false` on a
clip opts that instance out of the chip).

(`code` · `caption` · `shape` knobs are in §native compositing clips above; `transform`
pos/scale/rot/opacity/anchor animates via §clips `keyframes` like everything else.)

## §variants — non-destructive versions
```json
"variants": [
  { "id": "youtube", "base": true },
  { "id": "shorts", "from": "youtube",
    "resolution": [1080,1920], "fps": 60,
    "overrides": [
      { "op": "hide_track", "track": "tk_diagram" },
      { "op": "retime", "scope": "all", "speed": 1.15 },
      { "op": "keep_clips", "clips": ["c1","c_r1","…"] }
    ] }
]
```
A variant overlays overrides onto a base at render time without mutating it. `op`s:
`hide_track · show_only · retime · reorder · keep_clips · set_resolution · set_param`.
Render/export targets a variant by id.

## §exports — staging queue
```json
"exports": [
  { "id": "x1", "variant": "youtube", "status": "rendered",
    "file": "exports/recettear-pricing.youtube.mp4",
    "title": "Recettear's item-pricing bug, explained",
    "description_scaffold": "…",
    "credits": [ { "kind": "music", "text": "\"Calm\" by Artist — Jamendo (url) — CC BY 4.0" } ],
    "overlays": [ { "at": 42.0, "text": "♪ Calm — Artist (CC BY 4.0)" } ] }
]
```
Credits + overlays are auto-assembled from the tracked `attribution` of every sourced
asset. The human reviews the queue and uploads manually.

## Worked example
A minimal end-to-end project — the host's signature opener (a giggle zoomed close, then
"Welcome back, mortals." on the canon day room, integrated) — ships at
`examples/signature-opener.slop.json` and doubles as a parser test fixture + the reusable opener recipe.
