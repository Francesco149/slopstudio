# slopthumb — the thumbnail tool

A **separate app from the editor** (`build/slopthumb.exe`, sources in `thumbtool/`) for
authoring, iterating and A/B-managing video thumbnails. Cross-compiled like the editor:
`nix develop --command make -C thumbtool`.

**Rendering is two-layer (the slopstudio idea in miniature):** every layer rasterizes
into a padded RGBA *block* (CPU/stb — resize, distance-transform outlines, text) that is
**cached by a param-hash excluding position/rotation/opacity**, so moving a layer never
re-rasterizes anything. The GUI then composites blocks **on the GPU** (D3D11: premultiplied
rotated quads + a quantize shader for mosaic) into an offscreen RT — dragging is lag-free
(cache hits + a few quads; the toolbar shows live `build/gpu` ms). PNG **export stays on
the CPU compose path** — the deterministic reference, headless, no GPU needed — and is
byte-identical between GUI export buttons and the CLI.

**The one design rule: the tool is brand-agnostic.** Everything channel-specific — palette,
fonts, text styles, sticker defaults, sprite library roots, watermark, layout templates,
lint config — lives in a **brand package** directory the document points at. Swap the
package, retheme the doc. The @GemmaExplains package lives at
`../gemma-branding/brand-package/` (palette hexes LOCKED there, sampled from the rig
art); a generic demo package is committed at `examples/thumb-demo/brand-demo/`.

## Quick start
```
make -C thumbtool                                  # build (inside nix develop)
./build/slopthumb.exe examples/thumb-demo/demo.thumb.json          # GUI
./build/slopthumb.exe doc.thumb.json --export out.png --proof p.png --info i.json  # headless
python tools/thumb.py new t.thumb.json --brand ../gemma-branding/brand-package --template reaction-right
python tools/thumb.py render t.thumb.json --proof  # render + push to llm-feed
```

## Document format (`*.thumb.json`)
```jsonc
{
  "format": "thumb-1",
  "canvas": [1280, 720],            // or [1080, 1920] Shorts cover
  "brand": "../../branding/gemma",  // brand package dir, doc-relative
  "title": "the paired video title",// lint context (curiosity-gap check)
  "layers": [ /* bottom → top */ ]
}
```
Coordinates are **logical canvas px**; `x`/`y` are the layer **center**. Colors are
`#rrggbb[aa]` or `$name` (resolved via the brand palette). Any layer: `hidden`, `opacity`,
`rot` (deg).

**New docs auto-brand**: `thumb.py new` (and the GUI/export) walk up for the gemma package
(`gemma-branding/brand-package`) and load it with no `--brand` flag; a new branded doc defaults to
the package's `default_template` (the `reaction-marks` a2/b2/thirst layout — artifact+ring+arrow,
Gemma reacting right, headline, "?!"), so authoring is "fill the text + pick the 2 images + nudge
the marks". `--blank` opts out to a bare bg.

| type | fields |
|---|---|
| `bg` | `fill`, `grad_to` + `grad_angle`, `image` (cover-fit) + `blur`/`darken`/`opacity`, `vignette`; **`pattern`:`diamond`/`argyle`** quilt — `cell`, `pattern_fill` (alternate-diamond color), `pattern_line`+`pattern_line_px`+`pattern_line_alpha` (lattice), `pattern_motif` (`diamond`/`dot`/`plus`/`ring`/`box` accent at the corners) + `pattern_motif_fill`/`_size`/`_alpha`/`_every`/`_phase`; `pattern_ox`/`pattern_oy` slide the whole quilt (e.g. to center a motif group in a crop). Procedural → stays crisp at any scale (banners) |
| `image` | `src`, `x`,`y`, `scale` (1.0 = fit canvas height), `rot`, `flip`, `outline_px`+`outline` (sticker), `shadow{dx,dy,blur,alpha,color,off}`, `glow{px,color,alpha}` — outline/shadow default from the package's `sticker` |
| `text` | `text` (\n = line break), `style` (package style name; every field overridable), `x`,`y`, `px`, `max_w` (auto-shrink), `fill`+`grad_to` (vertical gradient), `stroke_px`+`stroke`, `tracking` (letter spacing), `line_height` (×natural leading, default 1.02; <1 packs multi-line text tighter), `align`, `plate{pad_x,pad_y,radius,fill,alpha}`, `shadow`, `glow` |
| `shape` | `shape`: `arrow` (`x1,y1,x2,y2,width`) · `circle` (`x,y,r,thick` — 0=disc) · `rect` (`x,y,w,h,radius,thick` — 0=filled); `fill`, `outline_px`/`outline`, `shadow`, `glow` |
| `mosaic` | `x`,`y`,`w`,`h`,`cell` — pixelates whatever is already on the canvas under the rect (the anime censor-gag primitive) |
| `watermark` | no fields — draws the package's `watermark` spec |

Shapes also take `grad_to` + `grad_angle` (linear gradient across the shape bbox — panel backdrops).
A doc-level `"lint": {…}` object overrides the brand package's lint config for deliberate exceptions
(e.g. a 4-word meme line).

## Brand package (`<dir>/brand.json`)
`palette` (name→hex) · `fonts` (name→ttf, package-relative) · `styles` (named text-style
defaults) · `sticker` (image-layer outline/shadow defaults) · `sprite_roots` (dirs the GUI
sprite browser lists) · `watermark` · `templates` (named ready-to-fill layer stacks with a
`hint`) · `lint` (`max_words`, `keep_clear_br`).

## GUI
Layers panel (reorder/dup/hide) · Inspector with palette swatches · brand template buttons ·
sprite browser (from `sprite_roots`, click = add layer) · canvas with drag-move,
ctrl+wheel scale, and a live **168px squint inset** ("feed size") · **history panel**
(sibling `*.thumb.json` = A/B variants; `history/` snapshots with PNG previews, click to
restore — undoable) · Snapshot / Export PNG buttons · 16:9↔9:16 toggle.

- **Undo**: editor-pattern doc snapshots at gesture settle; Ctrl+Z/Y. Snapshots append to
  `<doc>.undo.jsonl` so the **full undo history persists across sessions** (delete the file
  to reset). External (agent) edits hot-reload as a normal undo step.
- **Hot-reload**: the GUI polls the doc mtime (~0.5 s). If the file changes on disk with no
  unsaved local edits it reloads silently — this is the **LLM-authoring loop**: the agent
  edits the doc (thumb.py or raw JSON), the human watches it change live. With unsaved local
  edits it shows a reload-theirs / keep-mine conflict bar instead.

## Agent workflow (`tools/thumb.py`)
`new` (--brand/--template/--portrait/--title) · `overview` · `set <layer> k=v…` (dotted
keys descend, `\n` ok in text) · `docset` · `add <type>` · `rm` · `order` · `render`
(exports PNG + `--proof` 168px + info sidecar, pushes both to the llm-feed) · `lint` ·
`snapshot` (save doc+PNG into `history/` — the A/B bank) · `variants`.

**Hard lint gates** (from `../gemma-branding/research/thumbnail-pipeline-2026-07.md` +
`vtuber-branding-2026-07.md`): total words ≤ `lint.max_words` (default 3) · headline must
NOT repeat title words (stopwords excluded — thumb+title form a curiosity gap, not an echo)
· no text in the bottom-right duration-stamp zone (20%×16%) · warn if the largest subject
is <12% of frame or there's no image layer at all. The fuzzy rest (emotion read, focal
clarity, in-feed pop) goes through the **frame-critic** skill on the rendered PNG + proof.

## The @GemmaExplains thumbnail format (locked defaults)
VTuber-meta geometry (research: `vtuber-branding-2026-07.md` §2, §7): bg + Gemma cutout +
text, **Gemma at 30–50% of frame, bust-up default**, sticker outline + one accent glow,
**exaggerated expression** (the chibi is a legibility *advantage* at 168px — never nudge
her toward realism), **the retro artifact co-dominant** (differentiates video-to-video;
Gemma herself never changes), EN text discipline: ≤3 Anton/Bebas words off the face.
Package templates = the genre color-code: `reaction-right` (The Moment) ·
`artifact-hero` (The Result/Forbidden) · `deadpan-verdict` (punchline packaging) ·
`shorts-cover` (9:16). Ship 2–3 **structurally distinct** variants per video (different
archetype, not recolors) → `snapshot` each → YouTube Test & Compare picks the watch-time
winner → bank the winning pattern.

Reference set: `../slopstudio-projects/luckymas/thumbs/` (a/b/c variants of video-001).
