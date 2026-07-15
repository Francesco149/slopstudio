# Scene animation cookbook

`scene` clips are transparent, reflowable motion graphics rendered by the same editor
path in preview and export. Humans and agents should start with a named widget and edit
only its JSON data; fork the Lua only when the visual genuinely needs new behavior.

## Fast path

List the live catalogue (read from `presets/lua/std.lua`, so it cannot drift):

```sh
nix develop --command python tools/slop.py scene-widgets
```

In a skeleton beat:

```json
{"line":"The defender fights at four hundred percent.","solo":true,
 "visual":{"scene":{"widget":"stat","data":{
   "from":160,"to":400,"dur":1.1,"unit":"%","label":"defender strength"
 }}}}
```

On an existing cut:

```sh
nix develop --command python tools/slop.py scene cut.slop.json \
  --widget comparison --data '{"title":"before / after","items":[{"image":"assets/a.png","label":"before"},{"image":"assets/b.png","label":"after"}]}' \
  --at 42 --dur 6
nix develop --command python tools/slop.py scene-check cut.slop.json
```

In the editor: add a Scene clip, select it, then edit `data` JSON in the inspector. The
script for a named widget is only `return widgets.NAME(t,d)`. Duplicate a working scene
before experimenting. Invalid JSON/Lua is reported inline and does not replace the last
working render.

## Which widget should I use?

| Intent | Widget | Essential data |
|---|---|---|
| One decisive number | `stat` | `value` or `from`/`to`, `unit`, `label`, `dur` |
| Source quotation | `quote` | `text`, `cite`; optional `sub`, `wrong` |
| Before/after or A/B | `comparison` | `title`, `items:[{image,label}]` |
| Screenshot plus explanation | `split` | `image`, `title`, `body`, `ratio` |
| Article/interview receipt | `document` | `image`, `source`, `excerpts:[{rect,hold,tr}]` |
| Artifact entrance | `reveal` | `image`; optional `dx`, `dy`, `rot`, `size`, `glow` |
| Cinematic screenshot | `perspective` | `image`; optional `rx`, `ry`, `focus`, `dof`, `size` |
| Front/back card | `cardflip` | `front`, `back`, `dur`, `size` |
| Move an object along a path | `drag` | `image`, `path:[{t,x,y}]` |
| Several objects slide into place | `slidein` | `items:[{image,from,to,delay,pw,label}]` |
| Code reveal in VS Code chrome | `code` | `code`, `lang`, `title`, reveal options |
| Typed viewer comment | `youtube_comment` | `author`, `text`, `time`, `likes`, `hearted` |
| Timeline/history | `timeline` or `lineage` | dated events, or `nodes` |
| Chart/plot | `chart` | `series`, `x`, `y`; optional markers/regions/callouts |
| Annotated screenshot | `callout` | image plus label/target data |
| Pull back from a detail | `zoomout` | `image`, crop window, `dur` |
| Step through a baked strip | `filmstrip` | `image`, `panels`, `step`, `label` |
| Technical diagrams | `dcm`, `octant`, `table` | domain data; copy the matching example first |
| Accent behind a subject | `rays` or `waves` | origin plus optional subject image |

The executable examples are `examples/scene-demo.slop.json`,
`examples/scene-anim.slop.json`, and `examples/scene-skeleton.json`.

## Composition rules that prevent iteration

- A scene with its own title/labels owns the information hierarchy. Do not add a term
  plate repeating the same number or phrase; `slop.py critique` flags scene+caption
  overlaps for review.
- Use `solo:true` for stats, comparisons, charts, and dense receipts. A host can accompany
  a simple `split` or callout, but should not cover the evidence.
- Give a movement time to settle before the line ends. Entrances generally need 0.4–1.1 s;
  document pans need longer and should hold after arrival.
- Prefer one meaningful animation per claim. `rays`, shake, glow, and SFX are accents, not
  a quota.
- Long-form sound uses `soft-swish`, `soft-settle`, or `soft-tick` around -12 to -18 dB,
  normally with `sound_duck:false`. Reserve `boom`/`awkward` for actual punchlines.

