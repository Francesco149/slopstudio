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
-- element i (1-based) reveals after (i-1)*step seconds, over dur → 0..1 (staggered list/line reveals)
function anim.stagger(t, i, step, dur) return anim.rise(t - (i - 1) * (step or 0.08), dur or 0.4) end
-- typewriter: the leading chars of `str` shown at time t (utf8-aware; cps = chars/sec)
function anim.typewrite(t, str, cps)
  local n = math.max(0, math.floor(t * (cps or 28)))
  local off = utf8.offset(str, n + 1)
  return off and str:sub(1, off - 1) or str
end

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
-- Integrate a 2D velocity-spring FOLLOWING a piecewise-linear `path` of {t,x,y} keyframes up to time
-- t (the balatro "dangle" — it lags behind and settles). Returns x, y, and the per-step delta dx,dy
-- (a velocity proxy for lean/motion-blur). Deterministic; O(t·60) but early-outs once settled. The
-- shared core behind widgets.drag / widgets.slidein / the code slide-in.
function anim.spring_path(t, path, st, dm)
  st = st or 90; dm = dm or 12
  local function target(tt)
    if tt <= path[1].t then return path[1].x, path[1].y end
    for i = 2, #path do
      if tt <= path[i].t then local a, b = path[i - 1], path[i]
        local f = (tt - a.t) / math.max(1e-4, b.t - a.t); return a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f end
    end
    return path[#path].x, path[#path].y
  end
  if t <= 0 then local x, y = target(0); return x, y, 0, 0 end
  local settle = (path.settle or 3.0)              -- assume at rest after this long → skip integration
  if t > settle then local x, y = target(1e9); return x, y, 0, 0 end
  local dt = 1 / 60
  local px, py = target(0); local vx, vy = 0, 0; local ppx, ppy = px, py
  for i = 1, math.floor(t / dt) do
    ppx, ppy = px, py
    local tx, ty = target(i * dt)
    px, vx = anim.spring_step(px, vx, tx, dt, st, dm)
    py, vy = anim.spring_step(py, vy, ty, dt, st, dm)
  end
  return px, py, (px - ppx), (py - ppy)
end
-- a decaying screen-shake offset (dx,dy px) for an impact jolt — deterministic (summed sines, no
-- RNG): a burst at t=0 that dies out over `dur`. mag = peak px, freq = shakes/sec. Set it as the
-- returned root node's `ox`/`oy` (the whole scene shifts): `local n=...; n.ox,n.oy=anim.shake(t)`.
function anim.shake(t, mag, dur, freq)
  mag = mag or 22; dur = dur or 0.5; freq = freq or 22
  if t < 0 or t >= dur then return 0, 0 end
  local decay = (1 - t / dur) ^ 2
  local w = freq * 2 * PI
  return math.sin(t * w) * mag * decay, math.sin(t * w * 1.37 + 1.1) * mag * decay
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

-- code card palette (One Dark) — matches the native `code` clip so a scene code card looks the same.
-- cls maps the tokenize() class (0..8) → colour: default/keyword/type/string/number/comment/preproc/func/punct.
theme.code = {
  bg = { 22, 24, 32, 255 }, titlebar = { 34, 38, 50, 255 }, border = { 74, 84, 112, 255 },
  lineno = { 98, 106, 132, 255 }, dots = { { 237, 106, 94 }, { 244, 191, 79 }, { 91, 192, 99 } },
  cls = { [0] = { 208, 214, 230 }, [1] = { 198, 120, 221 }, [2] = { 86, 182, 194 }, [3] = { 152, 195, 121 },
          [4] = { 209, 154, 102 }, [5] = { 108, 118, 140 }, [6] = { 224, 108, 117 }, [7] = { 97, 175, 239 }, [8] = { 171, 178, 191 } },
}

-- node constructors ----------------------------------------------------------
local function node(k, o) o = o or {}; o.k = k; return o end
function box(o) return node("box", o) end
function row(o) return node("row", o) end
function col(o) return node("col", o) end
function text(s, o) o = o or {}; o.k = "text"; o.s = tostring(s == nil and "" or s); return o end
-- image node. { asset=uri, crop={x,y,w,h}(0..1), fit="cover|contain", tint, radius, grow/w/h/pw/ph }
-- PER-NODE TRANSFORM (offsets the drawn quad, not the layout slot — slide/spin/scale in place):
--   tx,ty=px  sc=uniform | scx,scy=per-axis  rot=degrees   (reveal/card-flip/drag animations)
-- GLOW: glow=0..1 draws enlarged copies behind, colored from glow_col={r,g,b} or a brightened mean.
function image(o) o = o or {}; o.k = "image"; return o end
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
  local trans = d.trans or 3.0            -- owner: pans default slow (heavy inertia); set d.trans lower to speed up
  local intro = d.intro or 1.3            -- the opening: a QUICK scroll over the whole article to excerpt 1 (for show)
  local function rectof(i) local e = ex[i]; return (e and e.rect) or { 0, 0, 1, 1 } end
  local function tdur(i) return (i == 1) and intro or trans end
  -- which excerpt are we in? accumulate (pan-dur + hold) per excerpt; hold on the last.
  local idx, tprev, acc = 1, 0, 0
  for i = 1, #ex do
    local seg = tdur(i) + (ex[i].hold or 2.5)
    if t < acc + seg or i == #ex then idx = i; tprev = acc; break end
    acc = acc + seg
  end
  local lt = t - tprev
  local from = (idx > 1) and rectof(idx - 1) or (d.from or { 0, 0, 1, 1 })   -- excerpt 1 opens from the WHOLE article
  local to = rectof(idx)
  local dur = tdur(idx)
  -- intro rushes in and settles (friction); later pans are slow + inertial (camera weight)
  local e = (idx == 1) and anim.friction(lt, dur) or anim.pan(lt, dur, d.damp)
  local crop = { from[1] + (to[1] - from[1]) * e, from[2] + (to[2] - from[2]) * e,
                 from[3] + (to[3] - from[3]) * e, from[4] + (to[4] - from[4]) * e }
  local cur = ex[idx] or {}
  local shown = anim.clamp((lt - dur * 0.55) / 0.4, 0, 1)     -- translation fades in as the pan settles
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

-- REVEAL — the "ta-da, here's the artifact" beat: an image slides in with inertia (friction stop),
-- straightens from a slight tilt (a hair of overshoot), and blooms a glow that fades after it settles.
-- Exercises the per-node image transform (tx/ty/rot) + glow. data:
--   { image=uri, dx=px(from), dy=px(from), rot=deg(from), glow=0..1, dur=sec, size=0..1(pw), radius=px }
function widgets.reveal(t, d)
  d = d or {}
  local dur = d.dur or 0.9
  local function slide(tt)                                          -- the slide offset at time tt
    local ee = anim.out_expo(tt / dur)                             -- fast in, long friction settle
    return (d.dx or -460) * (1 - ee), (d.dy or 40) * (1 - ee)
  end
  local dx, dy = slide(t)
  local rot = (d.rot or -7) * (1 - anim.out_back(t / (dur * 1.15)))  -- straighten past 0 then back
  local gin = anim.rise(t, dur * 0.6)
  local gout = 1 - anim.clamp((t - dur * 1.05) / 0.5, 0, 1)
  local glow = (d.glow or 0.85) * gin * gout
  local mb = nil                                                     -- motion blur from the slide velocity
  if d.blur ~= false then local px, py = slide(t - 1 / 60); mb = { dx - px, dy - py } end
  local n = center(image{ asset = d.image, pw = d.size or 0.66, aspect = true,
    tx = dx, ty = dy, rot = rot, glow = glow, glow_col = d.glow_col, radius = d.radius or 0, mb = mb })
  if d.shake then n.ox, n.oy = anim.shake(t - dur, d.shake_mag or 16, d.shake_dur or 0.45) end   -- jolt on landing
  return n
end

-- CARDFLIP — a 2D card flip: horizontal squash 1->0->1, the face swapping at the edge-on midpoint
-- (front -> back). data: { front=uri, back=uri, dur=sec, delay=sec, size=0..1(pw), glow=0..1 }
function widgets.cardflip(t, d)
  d = d or {}
  local dur = d.dur or 0.8
  local p = anim.clamp((t - (d.delay or 0.3)) / dur, 0, 1)
  local face = (p < 0.5) and (d.front or d.back) or (d.back or d.front)
  local sx = math.abs(math.cos(p * math.pi))                        -- 1 -> 0 (edge-on) -> 1
  if sx < 0.02 then sx = 0.02 end
  return center(image{ asset = face, pw = d.size or 0.5, aspect = true, scx = sx, glow = d.glow or 0 })
end

-- PERSPECTIVE — a screenshot / card shown at a 3D ANGLE (foreshortened, receding into the frame) with
-- DEPTH OF FIELD: sharp within `focus_r` of the `focus` point, blurring at rate `dof` beyond it. The
-- cinematic "hero screenshot / floating plane" look. The tilt + DoF ease in and settle. data:
--   { image=uri, rx=deg(tilt top away/toward), ry=deg(tilt left/right), persp=0..1(foreshorten;
--     higher=stronger), focus={x,y}(0..1 ON the image), focus_r=0..1(sharp radius, image-space),
--     dof=rate(how fast blur grows past the radius), dof_max=px(max blur sigma),
--     size=0..1(pw), settle=sec, hold=true(skip the entrance → straight to the final angle),
--     drift=deg(a slow living ry sway so a long hold isn't frozen), drift_speed=rad/s }
function widgets.perspective(t, d)
  d = d or {}
  local settle = d.settle or 0.9
  local e  = d.hold and 1.0 or anim.tween(t, settle, "out_back")   -- tilt eases in with a hair of overshoot
  local de = d.hold and 1.0 or anim.rise(t, settle * 1.25)         -- DoF fades in as the tilt settles
  local drift = (d.drift or 1.6) * math.sin(t * (d.drift_speed or 0.5))   -- gentle living sway on ry (deg)
  local glow = (d.glow or 0) * (d.hold and 1.0 or anim.rise(t, settle))   -- optional glow, blooms in with the tilt
  return center(image{ asset = d.image, pw = d.size or 0.72, aspect = true,
    rx = (d.rx or 16) * e, ry = (d.ry or -13) * e + drift * e, persp = d.persp or 0.55,
    focus = d.focus or { 0.5, 0.42 }, focus_r = d.focus_r or 0.22,
    dof = (d.dof or 2.2) * de, dof_max = d.dof_max or 26, glow = glow, glow_col = d.glow_col })
end

-- DRAG — a card "grabbed" and dragged along a cursor path, lagging behind with a velocity spring
-- (the Balatro dangle): it leans into the motion (rot ∝ horizontal velocity) and settles when the
-- cursor stops. Deterministic: re-integrates the spring from t=0 each frame (O(t·60), fine here).
-- data: { image, path={ {t=sec,x=0..1,y=0..1}, ... }, stiffness, damping, lean=deg-per-(frac/s),
--         size=0..1(pw), glow=0..1 }
function widgets.drag(t, d)
  d = d or {}
  local path = d.path or { { t = 0, x = 0.5, y = 0.5 } }
  local function target(tt)                                  -- piecewise-linear cursor position at tt
    if tt <= path[1].t then return path[1].x, path[1].y end
    for i = 2, #path do
      if tt <= path[i].t then
        local a, b = path[i - 1], path[i]
        local f = (tt - a.t) / math.max(1e-4, b.t - a.t)
        return a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f
      end
    end
    return path[#path].x, path[#path].y
  end
  local dt = 1 / 60
  local px, py = target(0)
  local vx, vy = 0, 0
  local ppx, ppy = px, py                                     -- position one step back (for motion blur)
  local st, dm = d.stiffness or 90, d.damping or 12
  for i = 1, math.floor(t / dt) do
    ppx, ppy = px, py
    local tx, ty = target(i * dt)
    px, vx = anim.spring_step(px, vx, tx, dt, st, dm)
    py, vy = anim.spring_step(py, vy, ty, dt, st, dm)
  end
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local rot = anim.clamp(vx * (d.lean or 26), -16, 16)       -- lean into the horizontal velocity
  local mb = (d.blur ~= false) and { (px - ppx) * W, (py - ppy) * H } or nil
  return center(image{ asset = d.image, pw = d.size or 0.28, aspect = true,
    tx = (px - 0.5) * W, ty = (py - 0.5) * H, rot = rot, glow = d.glow or 0, mb = mb })
end

-- SLIDE-IN — one or more objects each slide in from off-frame to a rest spot with the balatro
-- velocity-spring dangle (lag + settle + lean into the motion, motion-blurred), each with an optional
-- label (a year) that FOLLOWS it. The opening "cart vs wiimote" reveal. data:
--   { items = { { image=uri, from={x,y}(0..1, off-frame ok), to={x,y}(0..1 rest), delay=sec, pw=0..1,
--       label="2000", label_col={r,g,b}, label_dy=0..1(below), label_size, glow, stiffness, damping,
--       lean }, ... } }   -- a SINGLE object: pass those fields at the top level (items defaults to {d}).
function widgets.slidein(t, d)
  d = d or {}
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local kids = {}
  for _, it in ipairs(d.items or { d }) do
    local lt = t - (it.delay or 0)
    local frm, to = it.from or { -0.4, 0.5 }, it.to or { 0.5, 0.5 }
    local path = { { t = 0, x = frm[1], y = frm[2] }, { t = 0.02, x = to[1], y = to[2] }, { t = 30, x = to[1], y = to[2] } }
    local x, y, dx, dy = anim.spring_path(lt, path, it.stiffness or 80, it.damping or 11)
    local rot = anim.clamp(dx * (it.lean or 26), -14, 14)
    local mb = (it.blur ~= false) and { dx * W * 0.6, dy * H * 0.6 } or nil
    -- floating elements need a CONCRETE size (Clay % doesn't resolve for floats) → px width from pw
    kids[#kids + 1] = image{ asset = it.image, w = math.floor((it.pw or 0.3) * W), aspect = true, float = "c",
      tx = (x - 0.5) * W, ty = (y - 0.5) * H, rot = rot, glow = it.glow or 0, mb = mb }
    if it.label then
      local le = anim.rise(lt - 0.15, 0.5)
      local lw = math.floor(W * 0.3)
      kids[#kids + 1] = box{ float = "tl", fx = x * W - lw * 0.5, fy = (y + (it.label_dy or 0.3)) * H,
        w = lw, kids = { text(it.label, { size = it.label_size or 60, ta = "c",
          col = theme.fade(it.label_col or theme.acc, le) }) } }
    end
  end
  return box{ growx = true, growy = true, kids = kids }
end

-- CODE — a real vscode-style code card: monospace, SYNTAX-HIGHLIGHTED (the C `tokenize` binding →
-- One Dark colours, same as the native `code` clip), a chrome title bar + line-number gutter, and
-- the per-line reveal you liked (each line slides up + fades in, staggered via the subtree transform
-- t_y/t_op). data: { code="...\n..." | lines={...}, lang="lua|c|toml|text", title="file",
--   size=30, step=0.1, dur=0.38, nums=true, hi=<1-based line to accent> }
function widgets.code(t, d)
  d = d or {}
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local src = d.code or table.concat(d.lines or {}, "\n")
  local toklines = tokenize(src, d.lang or "lua")
  local size = d.size or 30
  local cc = theme.code
  local gdig = #tostring(#toklines)                            -- gutter digit count
  local cellw = size * 0.6                                     -- monospace advance ≈ 0.6em (Consolas)
  -- entrance: the window is DRAGGED in from the left with a spring dangle (balatro). The per-line
  -- reveal waits until it lands (rvt) so the two moves don't fight.
  local slideX, sop, rvt = 0, 1, t
  if d.slide ~= false then
    local sx = anim.spring_path(t, { { t = 0, x = -1.18, y = 0 }, { t = 0.02, x = 0, y = 0 }, { t = 30, x = 0, y = 0 } },
      d.stiffness or 78, d.damping or 13)
    slideX = sx * W; sop = anim.rise(t, 0.3); rvt = t - (d.reveal_delay or 0.5)
  end
  local rows = {}
  for li, spans in ipairs(toklines) do
    local e = anim.stagger(rvt, li, d.step or 0.1, d.dur or 0.38)
    local cells = {}
    if d.nums ~= false then
      cells[#cells + 1] = box{ w = cellw * gdig, kids = {
        text(string.format("%" .. gdig .. "d", li), { size = size, font = "mono", ta = "r",
          col = (d.hi == li) and cc.cls[7] or cc.lineno }) } }
    end
    local sp = {}
    for _, s in ipairs(spans) do
      if s.s ~= "" then sp[#sp + 1] = text(s.s, { size = size, font = "mono", wrap = "none", col = cc.cls[s.c] or cc.cls[0] }) end
    end
    if #sp == 0 then sp[1] = text(" ", { size = size, font = "mono" }) end   -- empty line keeps its height
    cells[#cells + 1] = row{ gap = 0, kids = sp }
    local hl = d.hi == li and #toklines > 1                    -- current-line highlight bar (needs a sibling to size the col)
    rows[#rows + 1] = row{ gap = cellw * 1.5, ay = "c", growx = hl or nil, bg = hl and { 255, 214, 90, 24 } or nil,
      t_y = (1 - e) * 18, t_op = e, kids = cells }
  end
  local body = box{ padl = size * 0.9, padr = size * 0.9, padt = size * 0.7, padb = size * 0.7,
    kids = { col{ gap = size * 0.24, kids = rows } } }
  local kids = {}
  if d.title then                                             -- title bar: traffic lights + filename
    local function dot(c) return box{ w = size * 0.42, h = size * 0.42, kids = { shape{ grow = true, shape = "ellipse", fill = c, color = c } } } end
    kids[#kids + 1] = row{ growx = true, h = size * 1.85, ay = "c", padl = size * 0.7, gap = size * 0.32, bg = cc.titlebar,
      kids = { dot(cc.dots[1]), dot(cc.dots[2]), dot(cc.dots[3]), spacer(size * 0.5, true),
               text(d.title, { size = size * 0.82, font = "mono", col = theme.fade(cc.cls[0], 0.9) }) } }
  end
  kids[#kids + 1] = body
  local card = col{ radius = 10, bg = cc.bg, bw = 2, bc = cc.border, clip = true, kids = kids, t_x = slideX, t_op = sop }
  local root = { center(card) }
  -- a fake mouse cursor "holding" the title bar, moving in with the window (drag feel)
  if d.cursor ~= false and d.title then
    local cw = d.cursor_size or 42
    root[#root + 1] = box{ float = "tl", fx = (d.cursor_x or 0.585) * W + slideX, fy = (d.cursor_y or 0.30) * H,
      w = cw, h = cw * 1.46, kids = { shape{ grow = true, shape = "cursor", color = { 16, 16, 22 }, fill = { 247, 248, 252 }, thickness = 3 } } }
  end
  return box{ growx = true, growy = true, kids = root }
end

-- RAYS — an anime sunburst / impact lines behind a subject. `count` wedges radiate from `at` (frame
-- fraction), fading to the tips; optionally rotate (`spin` rad/s) and burst (grow then fade over
-- `burst` sec — the impact flash). Put the subject image AFTER it so it sits on top. data:
--   { at={x,y}, count, color={r,g,b,a}, spin, burst, duty }
function widgets.rays(t, d)
  d = d or {}
  local at = d.at or { 0.5, 0.5 }
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local col = d.color or { 255, 246, 224, 210 }
  local env = 1.0
  if d.burst then                                             -- fast grow, slow fade (an impact flash)
    env = anim.out_cubic(anim.clamp(t / (d.burst * 0.28), 0, 1)) *
          (1 - anim.clamp((t - d.burst * 0.45) / (d.burst * 0.55), 0, 1))
  end
  -- a plain (non-growing) wrapper so the floating shape adds NO layout space — it overlays cleanly
  -- when stacked with other content (like widgets.callout does), rather than splitting a column.
  return box{ kids = {
    shape{ float = "tl", fx = 0, fy = 0, w = W, h = H, shape = "rays", from = at,
           count = d.count or 16, phase = (d.spin or 0.5) * t, duty = d.duty or 0.5,
           color = { col[1], col[2], col[3], (col[4] or 210) * env } } } }
end

-- WAVES — concentric rings that expand and fade OUTWARD from a point, like the ripples off a water
-- drop. Put the subject (`image`) on top so the waves radiate from behind it. data:
--   { at={x,y}(0..1 frame origin), image=uri, size=0..1(pw of the subject), count=rings,
--     period=sec(between births), maxr=0..1(reach, frame-width fraction), color, thick=px, glow }
function widgets.waves(t, d)
  d = d or {}
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local at = d.at or { 0.5, 0.5 }
  local n = d.count or 4
  local period = d.period or 1.3
  local maxr = (d.maxr or 0.42) * W
  local minr = (d.minr or 0.08) * W                    -- rings are born at the subject's edge, not a point
  local col = d.color or theme.acc
  local kids = {}
  for i = 1, n do
    local phase = ((t / period) - (i - 1) / n)
    phase = phase - math.floor(phase)                  -- 0..1 ring life (staggered)
    local r = minr + phase * (maxr - minr)             -- expands outward
    local a = (1 - phase) ^ 1.6                         -- fades as it grows
    if a > 0.01 then
      kids[#kids + 1] = shape{ float = "tl", fx = at[1] * W - r, fy = at[2] * H - r, w = 2 * r, h = 2 * r,
        shape = "ellipse", color = theme.fade(col, a), thickness = (d.thick or 6) * (1 - phase * 0.6) }
    end
  end
  if d.image then
    local pop = anim.tween(t, 0.5, "out_back")
    kids[#kids + 1] = image{ asset = d.image, w = math.floor((d.size or 0.5) * W), aspect = true, float = "c",
      sc = 0.85 + 0.15 * pop, glow = d.glow or 0 }
  end
  return box{ growx = true, growy = true, kids = kids }
end

-- format a like count YouTube-style: 1200 -> "1.2K", 3400000 -> "3.4M"
local function fmtk(n)
  n = n or 0
  if n >= 1e6 then local s = string.format("%.1f", n / 1e6):gsub("%.0$", ""); return s .. "M" end
  if n >= 1000 then local s = string.format("%.1f", n / 1000):gsub("%.0$", ""); return s .. "K" end
  return tostring(math.floor(n))
end

-- YOUTUBE COMMENT — a YouTube-style comment that slides up + fades in, with the comment text TYPING
-- in (typewriter + a blinking caret) as if written live. The phone/YouTube-comment move. data:
--   { author="@name", text="...", avatar=uri, av_color={r,g,b}, time="2 days ago", likes=1200,
--     hearted=true (creator heart), cps=34 (chars/sec; false = no typewriter), width=0..1 }
function widgets.youtube_comment(t, d)
  d = d or {}
  local W, H = (frame and frame.w) or 1920, (frame and frame.h) or 1080
  local cw = math.floor(W * (d.width or 0.6))
  local av = d.av_size or 88
  local gap = 26
  local e = anim.rise(t, 0.5)                                  -- card slide-up + fade
  local full = d.text or ""
  local shown = full
  if d.cps ~= false then                                       -- type the comment after the card lands
    shown = anim.typewrite(t - 0.35, full, d.cps or 34)
    if shown ~= full and (math.floor(t * 2) % 2 == 0) then shown = shown .. "|" end   -- blinking caret
  end
  -- avatar: an image, else a coloured circle with the author's initial. A capital glyph sits HIGH and
  -- a touch LEFT in its em-box (ascender/descender gap + side-bearing), so a plain center reads off;
  -- nudge it toward the true visual centre (down + right) with a small subtree transform on the glyph.
  local avatar
  if d.avatar then
    avatar = box{ w = av, h = av, kids = { image{ asset = d.avatar, grow = true, radius = av * 0.5 } } }
  else
    local ac = d.av_color or { 150, 90, 210 }
    local initial = tostring(d.author or "?"):gsub("^@", ""):sub(1, 1):upper()
    -- a circle = a box whose corner radius is half its size (no floating shape → stays in place)
    avatar = box{ w = av, h = av, radius = av * 0.5, bg = ac, ax = "c", ay = "c",
      kids = { text(initial, { size = av * 0.5, col = { 255, 255, 255 }, ta = "c", t_x = av * 0.02, t_y = av * 0.055 }) } }
  end
  local head = { text(d.author or "@viewer", { size = 33, col = { 236, 238, 245 } }) }
  if d.time then head[#head + 1] = text("· " .. d.time, { size = 28, col = { 140, 146, 165 } }) end
  local acts = {
    box{ w = 30, h = 30, kids = { shape{ grow = true, shape = "heart", fill = d.hearted and { 240, 90, 130 } or { 150, 156, 175 } } } },
    text(fmtk(d.likes), { size = 26, col = { 150, 156, 175 } }),
    spacer(20, true),
    text("Reply", { size = 26, col = { 150, 156, 175 } }),
  }
  -- reserve the FULL comment's wrapped height (measure_text) so the whole block can be vertically
  -- CENTRED without drifting as the typewriter reveals it: the text sits in a fixed-height box and
  -- fills from the top, so the card's total height is stable from t=0.
  local bodyW = cw - av - gap
  local _, txH = measure_text(full, 34, bodyW)
  local body = col{ growx = true, gap = 7, kids = {
    row{ gap = 12, ay = "c", kids = head },
    box{ growx = true, h = txH + 4, kids = { text(shown, { size = 34, col = { 216, 220, 232 }, wrap = "words" }) } },
    row{ gap = 14, ay = "c", padt = 6, kids = acts },
  } }
  local card = row{ w = cw, gap = gap, ay = "t", kids = { avatar, body } }
  -- vertically centred on the frame by the WHOLE comment's (reserved) height; slides up + fades in.
  return box{ growx = true, growy = true, ax = "c", ay = "c",
    kids = { box{ t_y = (1 - e) * 44, t_op = e, kids = { card } } } }
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
