# slopstudio — Layout Engine (the `scene` clip)

**Status: SCOPED + P1 KERNEL BUILT 2026-07-15, Lua-first.** This is the durable design for a
native, scriptable motion-graphics/layout system so *any* diagram, chart, callout, or
composed visual can be authored natively — transparent, **reflowable**, adjustable — with
**minimal layout baking**, instead of hand-cut opaque PNGs per situation. Build spans
several sessions (owner: "take the time to design this properly"). It lives in **one
self-contained clip type**; if it ever misbehaves, a beat falls back to a plain `image`
clip with zero blast radius. **P1 (kernel) + P2a (in-editor Lua editing · image/pan-zoom · the `document`
widget) are built + proven** — see §11; `examples/scene-demo.slop.json`.

The one-liner: **ImGui to draw · Clay to lay out · Lua to script.**

---

## 1. Why (the problem this kills)

Two recurring pains:

1. **We already reinvent flexbox, badly, in six places.** `draw_diagram_clip` has an
   auto-fit-and-rescale pass; `draw_plot_clip` auto-reflows; captions do corner-avoidance
   via `g_frameTextBoxes`; `content_centroid_span`, avatar auto-placement, and inset
   "step-aside" are each a hand-rolled fragment of a layout solver. Every new visual need
   grows another ad-hoc fragment. A real layout core **unifies all of it**.
2. **Baked opaque PNGs don't blend and don't reflow.** Diagrams/comparison boards/interview
   excerpts made in the thumbnail tool are solid rectangles that fight the checker bg and
   can't adapt when moved/resized (owner, on the kirby video). We want native, transparent,
   layout-adaptive visuals (see the `layout-adaptive-visuals` standing goal).

Plus two concrete authoring needs the engine must serve:
- Compose **any** diagram/chart/visualization natively and reflow it when nudged.
- The **document/interview case**: take a high-DPI website screenshot and pan/zoom between a
  few relevant excerpts, each with a translation beside it and a persistent source-URL chip.

## 2. The decision (and the options we rejected)

**Chosen stack:** a new `type:"scene"` clip whose `params.script` is a **Lua** function
`scene(t, data) -> tree`. The script builds a tree of plain Lua tables (containers / text /
image / shape nodes) using a **DRY stdlib** (`anim`, `layout`, `theme`, `widgets`, …). C++
walks that tree into **[Clay](https://github.com/nicbarker/clay)** (a single-header C
flexbox library) for layout, and Clay's emitted render commands are drawn with the existing
**ImGui `ImDrawList`** path in `composite_frame`. Preview==export and the param-hash cache
come for free from that shared path.

**Authoring model = Lua-first (owner pick 2026-07-15).** The script is the only tier;
common visuals are stdlib functions (`widgets.quote(t, data)`, `widgets.chart(t, data)`) —
themselves Lua you fork the moment you need something custom. A declarative JSON tier can be
layered on later (a widget name + data) but is **not** built first.

Rejected / harmonized:
- **pocketjs** — right ideas (flexbox, baked deterministic keyframes, component model),
  wrong artifact: a compiled JSX→QuickJS+Rust stack targeting PSP/Vita, **not embeddable**
  in a C++/D3D11 editor. We take its model, not its code.
- **ImGui-as-layout** — we keep ImGui's *draw + font* layer (we already use it) but its
  *layout* (windows/columns/SameLine) is immediate-**UI** layout tangled with input: no
  flexbox, no reflow-to-fit, no centering/aspect. Clay fills exactly that gap.
- **JS/QuickJS escape hatch** — best LLM-authoring ergonomics + JSON-native data, but a
  heavier VM than Lua. Lua wins on embeddability, determinism, sandbox simplicity, and
  pure-C mingw build. (Revisit only if Lua authoring proves painful.)
- **Declarative-only (no VM)** — simplest/safest but a ceiling on procedural motion; the
  owner wants full scripting power.

**Why Lua only ever touches *data*:** the script returns tables and does arithmetic; it
never calls Clay or ImGui. All layout + drawing is C++. So the Lua↔C++ binding surface is
minimal (a couple of introspection helpers), the stdlib is forkable Lua, and determinism is
easy to guarantee.

## 3. Architecture

```
per frame, given clip-local time t:

  Lua:   scene(t, data) ─────────────► tree of tables      (sandboxed, deterministic)
           └ uses stdlib: anim.* layout.* theme.* widgets.*
  C++ walk: tree ─► Clay elements ─► Clay layout           (text measured via ImGui)
  C++ draw: Clay render commands ─► ImDrawList             (in composite_frame; preview==export)
```

- **Canonical space** is project px; the existing `s = fw/p.width` scales preview (half-res)
  and export (full-res) uniformly, so a `scene` is resolution-independent for free.
- **The clip's outer `transform`** (pos/scale/rot/opacity/anchor) + `keyframes` wrap the whole
  scene like any clip (Ken Burns the whole graphic, fade it in). *Intra*-scene animation is
  script-driven off `t`.
- **Caching:** compile the Lua chunk once per `script_hash`; re-eval `scene(t,data)` per frame
  (µs–ms — Clay layout is microsecond-scale). The rendered texture caches per
  `(node, frame, param_hash)` like every node, where `param_hash` folds in
  `script_hash + data_hash + input asset hashes`. Scenes that reference generated images pull
  them through the same async content-addressed asset path (gen never enters this loop).

## 4. The `scene` clip

```json
"clips": {
  "b12_scene": {
    "row": "r_scene", "start": 40.0, "dur": 6.0,
    "params": {
      "script": "local t,d=...\nreturn widgets.quote(t,{text=d.text,cite=d.cite})",
      "data":   { "text": "A real accelerometer. In a Game Boy cart.", "cite": "HAL, 2000" },
      "notes":  "tilt-sensor pull-quote"
    },
    "transform": { "pos": [0,-40], "scale": [1,1], "rot": 0, "opacity": 1, "anchor": [0.5,0.5] },
    "keyframes": {}
  }
}
```

- **`params.script`** — inline Lua (default; keeps project dirs portable). Edited in the
  editor's inspector like a `code` card, **hot-reloaded** on change (file-watch + inspector).
- **`params.script_uri`** — optional: reference a shared `.lua` file (e.g.
  `presets/lua/scenes/lineage.lua`) instead of inlining, for big/shared scripts.
- **`params.data`** — arbitrary JSON handed to `scene(t, data)`. This is the DRY lever: **one
  script, many clips**, each with its own data (one `document` script drives every interview
  excerpt beat). Numeric fields inside `data` are **not** keyframed by the clip system — the
  script animates from `t`; keyframe the outer `transform` for whole-scene motion.
- Row type `scene` (row `r_scene`). One additive branch in `composite_frame`
  (`if (c.type=="scene") draw_scene_clip(...)`).

## 5. The Lua runtime contract

**Entry:** the script must evaluate to (or define) `scene(t, data)` returning a node (or a
list of nodes). `t` is **clip-local seconds** (`playhead - clip.start`); the script also gets
`dur`, `fps`, and the frame size in project px via a `ctx` (e.g. `select(3, ...)` or a global
`ENV`). Convention: `local t, data, ctx = ...`.

**Sandbox (determinism is non-negotiable — this runs in the instant loop AND the deterministic
export):**
- Custom `_ENV` per script exposes only: `math` (with `random` seeded per-clip, or removed in
  favor of `anim.noise`), `string`, `table`, `select`, `ipairs`, `pairs`, `tonumber`,
  `tostring`, `assert`, `error`, plus the stdlib modules. **Removed:** `os`, `io`, `require`
  (our own loader serves stdlib), `dofile`/`loadfile`, `package`, `collectgarbage`, `load`.
- **No wall-clock, no unseeded RNG, no IO.** `t` is the only time source.
- **Instruction budget:** a Lua debug hook aborts a script exceeding an instruction count per
  eval (guards a runaway loop from hanging the render thread). Abort → error card, non-fatal.
- **Errors don't crash the editor.** A syntax/runtime error draws a small red "scene error:
  <msg>" card in place (like a shader compile error) and is logged; the rest of the frame
  renders normally.

**Lua version:** **Lua 5.4**, source-vendored and compiled into the PE via mingw (same pattern
as ImGui: a `$LUA_SRC` from the flake, a Makefile compile rule → objects linked in). *Not*
LuaJIT (5.1 semantics, FFI is a sandbox hole, harder mingw cross; we don't need its speed —
the script builds one small tree per frame).

## 6. The element tree

The script returns nodes as tables. C++ (`draw_scene_clip`) walks them recursively, opening a
Clay element per node, and after `Clay.EndLayout()` translates the render commands to
`ImDrawList` calls. Node kinds:

| kind | fields (all optional unless noted) | draws as |
|---|---|---|
| `row` / `col` / `box` | `gap pad align justify w h grow wrap bg radius border clip children` | Clay container (+ optional rect/border) |
| `text` | `s` (string, req) `font size color wrap align weight` | Clay text (measured via ImGui) |
| `image` | `asset`/`uri` (req) `fit`(cover/contain) `crop`(x,y,w,h 0..1) `tint opacity radius` | Clay image → textured quad at its rect |
| `shape` | `kind`(box/line/arrow/ellipse/bracket) `color thickness fill from to` | Clay **custom** element → `draw_shape` at its rect |
| `rule` / `spacer` | `size color` | thin rect / empty gap |

- **Sizing:** `w`/`h` in project px, or `grow` (flex-grow), or omitted (fit content). Clay
  handles wrap, aspect-fit, scroll/clip, and floating/z (for callouts/badges over content).
- **Custom nodes** (shapes, the pan/zoom image transform, gradients) use Clay's custom-element
  passthrough: Clay lays out the box, hands us the rect, we draw it ourselves with the existing
  `draw_shape_clip`/image helpers — so we reuse all current native drawing.
- **Per-node transform/reveal:** a node may carry `t_pos`/`t_scale`/`opacity`/`reveal` that the
  C++ walk applies (an offset/fade/clip-in on top of layout) — but most animation is the script
  computing field values from `t` (e.g. `size = 40 * anim.rise(t, .4)`).

## 7. The stdlib (DRY, mostly Lua, forkable)

Ships as `.lua` under `presets/lua/` (hot-reloadable, human-readable, forkable — the point of
Lua-first) plus a tiny C kernel (only what Lua can't do itself: text measurement, asset
dimensions). Modules:

- **`anim`** — deterministic, all take `t`: `ease(t,a,b,dur,kind)`, `spring(t,...)`,
  `rise(t,dur)` (0→1 smoothstep), `stagger(t,i,step,dur)`, `typewrite(t,str,cps)`,
  `count(t,from,to,dur)` (number tween for stat reveals), `reveal(t,dur)`, `noise(seed,x)`.
- **`layout`** — container constructors with sane brand defaults: `row`, `col`, `box`, `grid`,
  `center`, `split(a,b,ratio)`, `stack` (z-overlay). Thin sugar over the node tables.
- **nodes** — `text`, `image`, `rule`, `shape`, `arrow`, `callout` constructors.
- **`theme`** — brand tokens from gemma-brand (palette hexes, type scale `h1/h2/body/cite`,
  spacing, accent). Sourced from `../gemma-branding/brand-package` so the look stays on-brand
  (see the `gemma-brand` skill). Overridable per clip via `data.theme`.
- **`widgets`** — the composed library (see §8).

Example widget (this is literally the shipped `widgets.quote`, forkable):
```lua
function widgets.quote(t, d)
  local e = anim.rise(t, 0.45)
  return layout.center{ opacity = e, t_pos = {0, (1-e)*24},
    layout.col{ gap = theme.sp(3), align = "center", max_w = 900,
      nodes.text('\u{201C}', { font = theme.serif, size = 120, color = theme.accent(0.5) }),
      nodes.text(d.text,     theme.h2),
      nodes.rule(theme.accent),
      nodes.text('\u{2014} '..d.cite, theme.cite) } }
end
```

## 8. Widget catalog (the build target)

Built as stdlib Lua on top of the kernel. Each is transparent, reflowable, and driven by
`data` so it's reusable across clips:

- **`quote`** — pull-quote card (replaces the `style:"quote"` caption; parity first).
- **`stat`** — a big animated number + label (uses `anim.count`).
- **`callout`** — a labeled box/arrow pointing at a point/region (over footage or a diagram).
- **`split`** — two panes (image|text, before|after) that reflow to the frame.
- **`comparison`** — an N-up board (e.g. cart-vs-wiimote, emu-comparison) that lays itself out.
- **`lineage` / `timeline`** — a chain of labeled nodes (the kirby lineage board).
- **`diagram`** — boxes + arrows / flow chains (parity with `draw_diagram_clip`, then retire it).
- **`chart`** — line/step/scatter/bar + axes/markers/regions/reveal (parity with `draw_plot_clip`).
- **`document`** — **the interview case.** `data = { image, source, excerpts:[{rect,hold,
  translation,note}] }`. Sequences pan/zoom (eased Ken-Burns between excerpt rects so the active
  excerpt fills ~60% of frame), drops each `translation` in a card on whichever side has room
  (layout-adaptive), and pins a small `source` URL chip in a corner throughout. Solves the
  "long interview text, pan to excerpts with translation + url" need directly. Flagship P2 proof.

## 9. Coordinate space & fonts

- Project px canonical; `s = fw/p.width` scales preview/export. Clay lays out in project px;
  each emitted command is drawn at `origin + rect*s`.
- **Text measurement:** Clay's `Clay_SetMeasureTextFunction` → `ImFont::CalcTextSizeA`, so Clay
  wraps/sizes using the real atlas metrics.
- **Known limitation — large text softness.** We're on **ImGui 1.91.4**, which bakes each font
  at one size (48px today) and bilinear-scales it, so very large native text goes soft. Bounded,
  not a blocker: every current native clip already ships this way, and the interview's high-DPI
  content is a *raster texture*, not native text. Upgrade path: bake a small size-ladder, or bump
  to **ImGui 1.92 dynamic fonts** (per-size rasterization) — a Phase-3 polish item.

## 10. Authoring & tooling

- **`slop.py scene`** — author/edit a scene clip: `--script '<lua>'` or `--script-file f.lua`,
  `--data '<json>'`, plus the usual `--at/--dur/--row/--pos`. Skeleton support:
  `visual: { scene: { script|widget, data } }` and a shorthand `visual: { doc: {...} }` for the
  document widget.
- **`slop.py scene-check <proj> [clip]`** — **headless script lint** (agent-loop closer): load
  the Lua under a stub kernel, run `scene(t,data)` at several `t`, report syntax/runtime errors +
  the built tree + a bbox, without opening the editor. Needs a Lua interpreter in the flake
  (already vendoring the source; expose a host `lua` too). This is how the LLM iterates.
- **`lint`** — pass-through for the opaque `script`; for `widget`+`data` clips, validate the
  known widget's `data` shape. An unknown/oversized scene never breaks structural lint.
- **In-editor:** inspector multiline Lua editor (reuse the code-card affordance) + hot-reload;
  a "scene error" surface; the outer transform/keyframe widgets as usual.

## 11. Build plan (phased, additive, PNG-fallback throughout)

**P1 — the kernel (prove `scene(t,data)->tree` renders). ✓ DONE 2026-07-15.**
- ✓ Flake exposes `$LUA_SRC` (unpacked Lua 5.4.7); Makefile compiles Lua's C core (all `src/*.c`
  minus the CLI mains) as C + `editor/src/clay_impl.c` (Clay impl TU) as C, gated `-DSLOP_SCENE`.
  Clay vendored at `editor/vendor/clay.h` (its C++ floor relaxed 202002L→201703L — local patch,
  we use the imperative API not the macros; the impl TU is C99 either way).
- ✓ `type:"scene"` dispatch in `composite_frame`; `draw_scene_clip`: Lua sandbox (allow-listed
  globals, no io/os/require, instruction-budget hook, per-chunk `_ENV` with `__index`→stdlib) +
  chunk compile/cache + eval; tree-walk → Clay → `ImDrawList` for containers (row/col/box) + text
  + scissor/clip; measure-text hook (`ImFont::CalcTextSizeA`); error card. Image + custom-shape
  render commands are stubbed → P2.
- ✓ Forkable stdlib `presets/lua/std.lua`: `anim` (rise/ease/count/smoothstep), `theme` (tokens),
  node constructors (box/row/col/text/rule/spacer/center), `widgets.quote` + `widgets.stat`.
- ✓ **Proof:** `examples/scene-demo.slop.json` — a reflowing centered quote card + a count-up stat,
  transparent on the checker, both time-driven; rendered headless + pushed to the llm-feed.
- ✓ **Spikes confirmed:** Clay drives fine imperatively from a tree-walk; Lua 5.4 cross-compiles
  into the PE clean (all 33 core objects). Node schema + stdlib API: this doc §6–§7 + std.lua.

**P2 — widgets + the interview case + tooling. (partly done 2026-07-15.)**
- ✓ **In-editor Lua editing** — the scene inspector has a `script` editor + a `data` (JSON) editor
  (persistent buffer, applies on valid parse), a **reload std.lua** button (tears down the VM →
  stdlib + scripts recompile), and inline compile/runtime errors. Script edits recompile by hash.
- ✓ **IMAGE render commands** — an `image` node (asset uri → `get_texture` → SRV) with a `crop`
  (0..1 pan/zoom window) + tint + radius; **floating** overlays (`float="tl|tr|bl|br|bc|…"` + fx/fy,
  attach to root) for cards/chips over an image. Frame size exposed to the stdlib as a global `frame`.
- ✓ **`widgets.document`** — the interview case: pan/zoom a screenshot between `excerpts` (each an
  animated crop + a translation strap that fades in on settle) + a persistent source-URL chip.
  Proof: `examples/scene-demo.slop.json` (the `sc_doc` clip pans across `docs/hero/editor.png`; feed).
- **REMAINING P2:** more widgets (`callout`/`split`/`comparison`/`lineage`); CUSTOM vector-shape
  render commands (arrows/lines); `slop.py scene`/`doc` verbs + `scene-check` headless lint; std.lua
  auto-reload on mtime. **Then** swap 2–3 kirby thumb-tool PNGs + an interview screenshot to scenes —
  what actually unblocks the kirby "solid rectangles" fix.

**P3 — parity + migration + polish.**
- `widgets.diagram`/`widgets.chart` at parity → retire the C++ `diagram`/`plot` draw paths (or
  keep as fast-path; decide on measurement).
- Port kirby fully off baked diagram/card PNGs.
- Crisp large text (size-ladder or ImGui 1.92); `frame-critic` pass over the outputs.

## 12. Risks & mitigations

- **Font softness at large sizes** — documented (§9); mitigations queued for P3; not a blocker.
- **Clay imperative use / arena sizing** — P1 spike; size the static arena for ~8k elements
  (~3.5 MB), far above any scene.
- **Lua determinism/sandbox** — standard, enforced by the custom `_ENV` + stripped stdlib +
  seeded RNG + instruction budget (§5).
- **Per-frame cost** — Lua eval + Clay layout is µs–ms; texture-cache per frame for scrub-back;
  instruction budget caps pathological scripts. Measure with `SLOP_PERF=1`.
- **Scope creep** — strict phasing; `scene` is additive; a beat can always fall back to `image`.

## 13. Fallback

`scene` is one self-contained clip type behind one dispatch branch. If the engine is disabled,
errors, or a specific scene misbehaves, that beat renders a plain `image` (a baked PNG) instead —
the pre-engine status quo. No other clip type, and no existing project, depends on it.

## 14. Open bikesheds (decide during P1)

- Clip/row name `scene` (vs `motion`/`viz`/`canvas`) — `scene` matches the `scene()` entry.
- Tree-**return** (chosen — composable, inspectable) vs an immediate-mode `ui.*` API (more
  Clay-native, marginally faster). Revisit only if the walk shows up in a profile.
- Whether `theme` is a global vs threaded through `data` (lean: global, overridable via `data.theme`).
