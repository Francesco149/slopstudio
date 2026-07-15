-- slopstudio scene stdlib — the DRY, forkable layer of the layout engine.
-- docs/LAYOUT_ENGINE.md. Loaded once into a sandbox; scene scripts see these as globals.
--
-- A scene script is `scene(t, data, ctx) -> node`. A node is a plain table the C++ side
-- walks into Clay (flexbox) and draws with ImDrawList. You compose nodes from the
-- constructors below; every "widget" here is just Lua you can copy and fork inline.
--
-- Node schema (all fields optional unless noted):
--   containers  box{} row{} col{}:
--     kids={...} gap=px  pad=px | padl/padr/padt/padb=px
--     w=px h=px            -- FIXED size
--     pw=0..1 ph=0..1      -- PERCENT of parent
--     grow / growx / growy -- fill available space
--     ax="l|c|r" ay="t|c|b"-- align children on each axis
--     bg={r,g,b,a} radius=px  bw=px bc={r,g,b,a}  clip=true
--   text{ s=string, size=px, col={r,g,b,a}, ta="l|c|r", wrap="words|lines|none" }
-- Colors are {r,g,b,a} 0..255 (a optional, defaults 255).

-- math helpers ---------------------------------------------------------------
local function clamp(x, a, b) if x < a then return a elseif x > b then return b else return x end end
local function smooth(e) e = clamp(e, 0, 1); return e * e * (3 - 2 * e) end

anim = {}
function anim.clamp(x, a, b) return clamp(x, a, b) end
function anim.smoothstep(e) return smooth(e) end
function anim.rise(t, dur) return smooth(t / (dur or 0.45)) end                 -- 0->1 ease over the clip head
function anim.lerp(t, a, b, dur, delay) return a + (b - a) * smooth((t - (delay or 0)) / (dur or 0.5)) end
function anim.count(t, from, to, dur) return math.floor(from + (to - from) * smooth(t / (dur or 1.0)) + 0.5) end

-- Tunable power ease-in-out: x in 0..1 -> 0..1. p=1 linear, p=2 gentle, p>=3 heavy (flat
-- ends). The single knob behind the "camera inertia" feel for pans.
function anim.ease(x, p)
  x = clamp(x, 0, 1); p = p or 3
  if x < 0.5 then return 0.5 * (2 * x) ^ p else return 1 - 0.5 * (2 * (1 - x)) ^ p end
end
-- The DAMPENED CAMERA curve for pans/zooms: 0..1 progress over `dur`, with a single `damp`
-- knob (0 = linear, 1 = very heavy inertia — very slow to start AND to settle). Default is
-- weighty on purpose (owner: "most panning wants a very dampened curve, lots of inertia").
anim.damp_default = 0.78
function anim.pan(t, dur, damp)
  damp = damp or anim.damp_default
  return anim.ease(t / (dur or 0.9), 1 + damp * 5)   -- damp 0.78 -> exponent ~4.9 (strong ease-in-out)
end

-- ── golden tweening curves (Penner set + friction + spring) — x is 0..1 progress ──
-- Grounded in the godot_ui_components cards (cubic ease-in-out settles + velocity springs).
local PI = 3.14159265358979
function anim.out_cubic(x)  x = clamp(x,0,1); return 1 - (1-x)^3 end
function anim.in_cubic(x)   x = clamp(x,0,1); return x^3 end
function anim.io_cubic(x)   x = clamp(x,0,1); return x < 0.5 and 4*x*x*x or 1 - (-2*x+2)^3/2 end
function anim.out_quint(x)  x = clamp(x,0,1); return 1 - (1-x)^5 end
function anim.out_expo(x)   x = clamp(x,0,1); return x >= 1 and 1 or 1 - 2^(-10*x) end          -- FRICTION stop: fast start, long slow settle
function anim.io_expo(x)    x = clamp(x,0,1); if x==0 or x==1 then return x end
                            return x < 0.5 and 2^(20*x-10)/2 or (2 - 2^(-20*x+10))/2 end
function anim.out_back(x, s) x = clamp(x,0,1); s = s or 1.70158; local c3 = s+1        -- OVERSHOOT then settle (a satisfying pop)
                            return 1 + c3*(x-1)^3 + s*(x-1)^2 end
function anim.out_elastic(x, amp, period)                                              -- springy overshoot with ripples
  x = clamp(x,0,1); if x==0 or x==1 then return x end
  local p = period or 0.35; local a = amp or 1
  return a * 2^(-10*x) * math.sin((x - p/4) * (2*PI)/p) + 1
end
function anim.out_bounce(x)
  x = clamp(x,0,1); local n,d = 7.5625, 2.75
  if x < 1/d then return n*x*x
  elseif x < 2/d then x = x-1.5/d; return n*x*x+0.75
  elseif x < 2.5/d then x = x-2.25/d; return n*x*x+0.9375
  else x = x-2.625/d; return n*x*x+0.984375 end
end
-- friction slide (owner #2: "slides and is stopped by friction") — fast in, decelerate to rest
function anim.friction(t, dur) return anim.out_expo(t / (dur or 0.7)) end
-- named tween dispatcher: anim.tween(t, dur, "out_back") -> 0..1
local _TWEENS = { out_cubic=anim.out_cubic, in_cubic=anim.in_cubic, io_cubic=anim.io_cubic,
  out_quint=anim.out_quint, out_expo=anim.out_expo, io_expo=anim.io_expo, out_back=anim.out_back,
  out_elastic=anim.out_elastic, out_bounce=anim.out_bounce, friction=anim.friction, rise=nil }
function anim.tween(t, dur, name)
  local f = _TWEENS[name or "io_cubic"] or anim.io_cubic
  return f(t / (dur or 0.5))
end
-- a velocity-driven damped-spring step (the balatro card "dangle"): returns the new
-- {value, vel} given a target, dt, stiffness, damping. Deterministic — step it over fixed dt.
function anim.spring_step(value, vel, target, dt, stiffness, damping)
  stiffness = stiffness or 150; damping = damping or 10
  local force = -stiffness * (value - target) - damping * vel
  vel = vel + force * dt
  return value + vel * dt, vel
end

-- brand tokens (subset — the locked palette lives in gemma-brand; wire fully in P2) --
theme = {
  fg   = { 238, 240, 248, 255 },
  dim  = { 168, 176, 196, 255 },
  acc  = { 198, 130, 225, 255 },   -- brand purple-pink
  card = { 22,  22,  30,  235 },
  bg   = { 14,  14,  20,  235 },
}
function theme.accent(a) return { theme.acc[1], theme.acc[2], theme.acc[3], (a or 1) * 255 } end
function theme.fade(col, a) return { col[1], col[2], col[3], (a or 1) * (col[4] or 255) } end

-- node constructors ----------------------------------------------------------
local function node(k, o) o = o or {}; o.k = k; return o end
function box(o) return node("box", o) end
function row(o) return node("row", o) end
function col(o) return node("col", o) end
function text(s, o) o = o or {}; o.k = "text"; o.s = tostring(s == nil and "" or s); return o end
function image(o) o = o or {}; o.k = "image"; return o end   -- { asset=uri, crop={x,y,w,h}(0..1), fit, tint, radius, grow/w/h }
function shape(o) o = o or {}; o.k = "shape"; return o end   -- { shape="box|ellipse|line|arrow|underline|bracket", from={x,y}, to={x,y}, color, fill, thickness } — from/to are 0..1 of the box
function rule(o) o = o or {}; return box{ h = o.h or 3, w = o.w, growx = (o.w == nil) or nil, bg = o.col or theme.accent(1) } end
function spacer(px, horizontal) if horizontal then return box{ w = px } else return box{ h = px } end end
-- fill the frame and center a single child in it
function center(child) return box{ growx = true, growy = true, ax = "c", ay = "c", kids = { child } } end

layout = { box = box, row = row, col = col, center = center }
nodes  = { text = text, image = image, shape = shape, rule = rule, spacer = spacer }

-- widgets — composed from the above; copy + fork inline whenever you need custom ---
widgets = {}

-- a full-frame pull-quote card (the tilt-sensor / MBC7 style quote)
function widgets.quote(t, d)
  d = d or {}
  local e = anim.rise(t, 0.5)
  return center(col{
    ax = "c", gap = 20, pw = 0.66,
    kids = {
      text("\u{201C}",                 { size = 128, col = theme.accent(0.20 + 0.45 * e), ta = "c" }),
      text(d.text or "",               { size = 54,  col = theme.fg,        ta = "c", wrap = "words" }),
      rule{ w = 120 },
      text("\u{2014} " .. (d.cite or ""), { size = 30, col = theme.accent(1), ta = "c" }),
    }
  })
end

-- a big animated number + label (the count-up "receipt")
function widgets.stat(t, d)
  d = d or {}
  local n = d.to and anim.count(t, d.from or 0, d.to, d.dur or 1.0) or (d.value or 0)
  return center(col{
    ax = "c", gap = 8,
    kids = {
      text(tostring(n) .. (d.unit or ""), { size = 132, col = theme.fg,  ta = "c" }),
      text(d.label or "",                 { size = 34,  col = theme.dim, ta = "c" }),
    }
  })
end

-- the DOCUMENT widget — pan/zoom a high-DPI screenshot between excerpts, each with a
-- translation card, plus a persistent source-URL chip. The interview case. data:
--   { image=uri, source="url",
--     excerpts = { { rect={x,y,w,h}(0..1 of source), hold=sec, tr="translation" }, ... },
--     trans=sec (pan duration), from={x,y,w,h} (opening crop, default whole page) }
function widgets.document(t, d)
  d = d or {}
  local ex = d.excerpts or {}
  local trans = d.trans or 0.9
  local function rectof(i) local e = ex[i]; return (e and e.rect) or { 0, 0, 1, 1 } end
  -- which excerpt are we in? accumulate (trans + hold) per excerpt; hold on the last.
  local idx, tprev, acc = 1, 0, 0
  for i = 1, #ex do
    local seg = trans + (ex[i].hold or 2.5)
    if t < acc + seg or i == #ex then idx = i; tprev = acc; break end
    acc = acc + seg
  end
  local lt = t - tprev
  local from = (idx > 1) and rectof(idx - 1) or (d.from or { 0, 0, 1, 1 })
  local to = rectof(idx)
  local e = anim.pan(lt, trans, d.damp)                       -- dampened camera pan/zoom (inertia)
  local crop = { from[1] + (to[1] - from[1]) * e, from[2] + (to[2] - from[2]) * e,
                 from[3] + (to[3] - from[3]) * e, from[4] + (to[4] - from[4]) * e }
  local cur = ex[idx] or {}
  local shown = anim.clamp((lt - trans * 0.55) / 0.4, 0, 1)   -- translation fades in as the pan settles
  local W = (frame and frame.w) or 1920
  local kids = { image{ asset = d.image, grow = true, crop = crop, fit = d.fit } }
  if cur.tr and cur.tr ~= "" then
    kids[#kids + 1] = box{ float = "bc", fy = -70, w = math.floor(W * 0.84), pad = 24, radius = 14,
      bg = theme.fade(theme.bg, 0.92 * shown),
      kids = { text(cur.tr, { size = 40, col = theme.fade(theme.fg, shown), ta = "c", wrap = "words" }) } }
  end
  if d.source and d.source ~= "" then
    kids[#kids + 1] = box{ float = "tr", fx = -28, fy = 28, pad = 12, radius = 10,
      bg = theme.fade(theme.bg, 0.85),
      kids = { text(d.source, { size = 24, col = theme.accent(1) }) } }
  end
  return box{ growx = true, growy = true, kids = kids }
end

-- SPLIT — an image beside an explanation (the "screenshot + point" beat). data:
--   { image=uri, title="...", body="...", ratio=0.5 }
function widgets.split(t, d)
  d = d or {}
  local e = anim.rise(t, 0.4)
  local ratio = d.ratio or 0.5
  local kids = {}
  if d.image then kids[#kids + 1] = image{ asset = d.image, pw = ratio, aspect = true, radius = 12, tint = theme.fade({ 255, 255, 255 }, e) } end
  local tk = {}
  if d.title then tk[#tk + 1] = text(d.title, { size = 52, col = theme.fade(theme.acc, e) }) end
  if d.body then tk[#tk + 1] = text(d.body, { size = 36, col = theme.fade(theme.fg, e), wrap = "words" }) end
  kids[#kids + 1] = col{ growx = true, gap = 18, ay = "c", kids = tk }
  return center(row{ gap = 44, ay = "c", pw = 0.9, kids = kids })
end

-- COMPARISON — N cells (image + caption) side by side, staggered in. data:
--   { title="...", items = { { image=uri, label="..." }, ... } }
function widgets.comparison(t, d)
  d = d or {}
  local cells = {}
  for i, it in ipairs(d.items or {}) do
    local r = anim.rise(t - (i - 1) * 0.12, 0.4)
    local ck = {}
    if it.image then ck[#ck + 1] = image{ asset = it.image, growx = true, aspect = true, radius = 10, tint = theme.fade({ 255, 255, 255 }, r) } end
    if it.label then ck[#ck + 1] = text(it.label, { size = 30, col = theme.fade(theme.dim, r), ta = "c" }) end
    cells[#cells + 1] = col{ growx = true, gap = 12, ax = "c", kids = ck }
  end
  local outer = {}
  if d.title then outer[#outer + 1] = text(d.title, { size = 48, col = theme.fg, ta = "c" }) end
  outer[#outer + 1] = row{ gap = 32, ay = "t", growx = true, kids = cells }
  return center(col{ gap = 28, ax = "c", pw = 0.92, kids = outer })
end

-- LINEAGE — a horizontal chain of labeled boxes joined by → glyphs, drawn in order. data:
--   { title="...", nodes = { "A", { label="B", sub="..." }, ... } }
function widgets.lineage(t, d)
  d = d or {}
  local ns = d.nodes or {}
  local chain = {}
  for i, n in ipairs(ns) do
    local r = anim.rise(t - (i - 1) * 0.18, 0.35)
    local label = (type(n) == "table") and n.label or tostring(n)
    local sub = (type(n) == "table") and n.sub or nil
    local bk = { text(label, { size = 34, col = theme.fade(theme.fg, r), ta = "c" }) }
    if sub then bk[#bk + 1] = text(sub, { size = 22, col = theme.fade(theme.dim, r), ta = "c" }) end
    chain[#chain + 1] = box{ pad = 20, radius = 12, gap = 4, ax = "c", ay = "c", bg = theme.fade(theme.card, r), kids = bk }
    if i < #ns then
      local ar = anim.rise(t - (i - 1) * 0.18 - 0.1, 0.3)
      chain[#chain + 1] = text("\u{2192}", { size = 44, col = theme.fade(theme.acc, ar) })
    end
  end
  local outer = { row{ gap = 18, ay = "c", kids = chain } }
  if d.title then table.insert(outer, 1, text(d.title, { size = 44, col = theme.fg, ta = "c" })) end
  return center(col{ gap = 30, ax = "c", kids = outer })
end

-- TIMELINE — dated nodes on a horizontal axis, labels alternating above/below, an optional
-- span bar. Rebuilds the "Nintendo tilted first" board natively (transparent, reflowable). data:
--   { title="...", axis={r,g,b}, nodes = { { x=0..1, year="2000", name="...", sub="...",
--       color={r,g,b}, side="up"|"down" }, ... }, span = { x0, x1, label="...", color={} } }
function widgets.timeline(t, d)
  d = d or {}
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local axisY, upY, dnY = 0.50, 0.16, 0.60          -- frame fractions
  local axisCol = d.axis or { 150, 120, 210, 255 }
  local kids = {}
  -- title
  if d.title then kids[#kids+1] = box{ float="tc", fx=0, fy=0.03*H, kids={ text(d.title, { size=52, col=theme.fg, ta="c" }) } } end
  -- the axis line (full-frame floating shape; from/to are frame fractions)
  local x0 = 1.0; local x1 = 0.0
  for _,n in ipairs(d.nodes or {}) do x0 = math.min(x0, n.x); x1 = math.max(x1, n.x) end
  kids[#kids+1] = shape{ float="tl", fx=0, fy=0, w=W, h=H, shape="line",
    from={math.max(0.04,x0-0.06), axisY}, to={math.min(0.96,x1+0.06), axisY}, color=axisCol, thickness=3 }
  -- span bar
  if d.span then
    local sc = d.span.color or axisCol
    kids[#kids+1] = shape{ float="tl", fx=0, fy=0, w=W, h=H, shape="line", from={d.span.x0, 0.565}, to={d.span.x1, 0.565}, color=sc, thickness=3 }
    if d.span.label then kids[#kids+1] = box{ float="tc", fx=0, fy=0.60*H, kids={ text(d.span.label, { size=34, col=sc, ta="c" }) } } end
  end
  -- nodes
  for i,n in ipairs(d.nodes or {}) do
    local r = anim.rise(t - (i-1)*0.14, 0.4)
    local ncol = n.color or axisCol
    local dotX, side = n.x, (n.side or (i%2==1 and "up" or "down"))
    -- dot on the axis
    kids[#kids+1] = box{ float="tl", fx=dotX*W-11, fy=axisY*H-11, w=22, h=22,
      kids={ shape{ grow=true, shape="ellipse", fill=theme.fade(ncol, r), color=theme.fade(ncol, r), thickness=3 } } }
    -- connector + label band
    local lb = { }
    local nm  = n.name and text(n.name, { size=30, col=theme.fade(theme.fg, r), ta="c" })
    local sub = n.sub  and text(n.sub,  { size=22, col=theme.fade(theme.dim, r), ta="c" })
    local yr  = n.year and text(n.year, { size=40, col=theme.fade(ncol, r), ta="c" })
    if side == "up" then
      if sub then lb[#lb+1]=sub end; if nm then lb[#lb+1]=nm end; if yr then lb[#lb+1]=yr end
      kids[#kids+1] = shape{ float="tl", fx=0, fy=0, w=W, h=H, shape="line", from={dotX, 0.30}, to={dotX, axisY-0.005}, color=theme.fade(ncol, r*0.6), thickness=2 }
      kids[#kids+1] = box{ float="tc", fx=(dotX-0.5)*W, fy=upY*H, kids={ col{ ax="c", gap=3, kids=lb } } }
    else
      if yr then lb[#lb+1]=yr end; if nm then lb[#lb+1]=nm end; if sub then lb[#lb+1]=sub end
      kids[#kids+1] = shape{ float="tl", fx=0, fy=0, w=W, h=H, shape="line", from={dotX, axisY+0.005}, to={dotX, 0.585}, color=theme.fade(ncol, r*0.6), thickness=2 }
      kids[#kids+1] = box{ float="tc", fx=(dotX-0.5)*W, fy=dnY*H, kids={ col{ ax="c", gap=3, kids=lb } } }
    end
  end
  return box{ growx=true, growy=true, kids=kids }
end

-- CALLOUT — a label with an arrow that extends to point at a target on the frame. Overlay it
-- on an image/footage to annotate. data: { text="...", at={x,y}(0..1 frame target),
--   from={x,y}(0..1 label anchor), color, thickness, size }
function widgets.callout(t, d)
  d = d or {}
  local e = anim.rise(t, 0.5)
  local at = d.at or { 0.5, 0.5 }
  local from = d.from or { (at[1] < 0.5) and 0.72 or 0.28, (at[2] < 0.5) and 0.80 or 0.20 }
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local acc = d.color or theme.acc
  -- Both layers FLOAT (explicit frame-sized arrow at the root origin), so a callout overlays an
  -- image cleanly whether used standalone or composed over an in-flow image base.
  return box{ kids = {
    shape{ float = "tl", fx = 0, fy = 0, w = W, h = H, shape = "arrow", from = from,
           to = { from[1] + (at[1] - from[1]) * e, from[2] + (at[2] - from[2]) * e },
           color = acc, thickness = d.thickness or 5 },
    box{ float = "tl", fx = from[1] * W - 8, fy = from[2] * H - 62, pad = 14, radius = 10,
         bg = theme.fade(theme.card, e),
         kids = { text(d.text or "", { size = d.size or 34, col = theme.fade(theme.fg, e) }) } },
  } }
end
