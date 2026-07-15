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
  for i = 1, math.min(math.floor(t / dt), 240) do   -- hard cap (belt-and-suspenders past the settle early-out)
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

-- a soft handheld CAMERA BOB — gentle looping (tx,ty) drift + a hair of rotation, so an image is
-- never truly static (owner: "we almost never want truly static images"). Deterministic (summed
-- sines). Returns dx, dy (px), drot (deg). Add to any image's tx/ty/rot.
function anim.bob(t, amp, speed)
  amp = amp or 12; speed = speed or 1.0
  return amp * math.sin(t * speed * 0.8 + 0.5), amp * 0.85 * math.sin(t * speed * 1.13),
         0.5 * math.sin(t * speed * 0.55)
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
  local kids = {
    text("\u{201C}",                 { size = 128, col = theme.accent(0.20 + 0.45 * e), ta = "c" }),
    text(d.text or "",               { size = 54,  col = theme.fg,        ta = "c", wrap = "words" }),
    rule{ w = 120 },
    text("\u{2014} " .. (d.cite or ""), { size = 30, col = theme.accent(1), ta = "c" }),
  }
  -- optional corrective SUBTEXT (d.wrong = red → "the source is mistaken"), fades in after the quote
  if d.sub and d.sub ~= "" then
    local se = anim.rise(t - 0.4, 0.5)
    kids[#kids + 1] = spacer(8)
    kids[#kids + 1] = text(d.sub, { size = 36, ta = "c", wrap = "words",
      col = theme.fade(d.wrong and { 236, 98, 112 } or theme.dim, se) })
  end
  return center(col{ ax = "c", gap = 20, pw = 0.7, kids = kids })
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
  return center(image{ asset = d.image, pw = d.size or 0.72, aspect = true, crop = d.crop,
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
      -- label follows the object's x, but its y is `label_y` (absolute frame fraction) when set, so a
      -- row of labels aligns on one baseline; else it trails below the object (label_dy).
      local ly = it.label_y and (it.label_y * H) or ((y + (it.label_dy or 0.3)) * H)
      kids[#kids + 1] = box{ float = "tl", fx = x * W - lw * 0.5, fy = ly,
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
  local slideX, sop, rvt, tilt = 0, 1, t, 0
  if d.slide ~= false then
    local sx, _, dx = anim.spring_path(t, { { t = 0, x = -1.18, y = 0 }, { t = 0.02, x = 0, y = 0 }, { t = 30, x = 0, y = 0 } },
      d.stiffness or 78, d.damping or 13)
    slideX = sx * W; sop = anim.rise(t, 0.3); rvt = t - (d.reveal_delay or 0.5)
    tilt = anim.clamp(dx * (d.lean or 190), -13, 13)   -- balatro lean: tilt into the drag velocity, settles to 0
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
  local card = col{ radius = 10, bg = cc.bg, bw = 2, bc = cc.border, clip = true, kids = kids, t_x = slideX, t_op = sop, t_rot = tilt }
  local root = { center(card) }
  -- a fake mouse cursor "holding" the title bar. It's positioned from the PREDICTED (centred) card
  -- layout — measured with the same mono metrics Clay uses — so it stays ON the title bar as the code
  -- (and thus the card) changes size. Upright; shifts with the slide (but doesn't tilt with the card).
  if d.cursor ~= false and d.title and d.cursor_img then
    local gw = (d.nums ~= false) and measure_text(string.rep("0", gdig), size, 0, true) or 0
    local maxLineW = 0
    for _, spans in ipairs(toklines) do
      local ln = ""; for _, sp in ipairs(spans) do ln = ln .. sp.s end
      local lw = measure_text(ln, size, 0, true); if lw > maxLineW then maxLineW = lw end
    end
    local bodyW = size * 1.8 + gw + cellw * 1.5 + maxLineW
    local tlW = size * 3.74 + measure_text(d.title, size * 0.82, 0, true)
    local cardW = math.max(bodyW, tlW)
    local titleH = size * 1.85
    local cardH = titleH + size * 1.4 + #toklines * size * 1.05 + (#toklines - 1) * size * 0.24
    local cardLeft, cardTop = (W - cardW) * 0.5, (H - cardH) * 0.5
    root[#root + 1] = image{ asset = d.cursor_img, w = d.cursor_size or 46, aspect = true, float = "tl",
      fx = cardLeft + cardW * (d.cursor_x or 0.66) + slideX, fy = cardTop + titleH * (d.cursor_y or 0.42) }
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
    local bx, by, br = anim.bob(t, d.bob or 15, d.bob_speed or 0.9)   -- soft camera bob (never static)
    kids[#kids + 1] = image{ asset = d.image, w = math.floor((d.size or 0.5) * W), aspect = true, float = "c",
      sc = 0.85 + 0.15 * pop, tx = bx, ty = by, rot = br, glow = d.glow or 0 }
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

-- ── NATIVE FIGURE PRIMITIVES (charts / diagrams) ────────────────────────────
-- Faithful, transparent, reflowable replacements for the baked figure PNGs (the kirby charts,
-- the DCM square-wave, the octant dial). Everything is drawn in FRAME-FRACTION space onto full-
-- frame floating shapes (like widgets.timeline), so it overlays the checker with no baked bg.
-- Built on the `polyline` shape primitive (a list of points, optional closed + fill = a pie wedge).
local function _fw() return (frame and frame.w) or 1920 end
local function _fh() return (frame and frame.h) or 1080 end
-- accept a colour as {r,g,b,a} OR a "#RRGGBB"/"#RRGGBBAA" hex string (the scene shape reader only
-- takes tables, so normalise hex → rgb here — lets figure data use either form).
local function _col(c)
  if type(c) == "string" then
    local h = c:gsub("#", "")
    return { tonumber(h:sub(1, 2), 16) or 255, tonumber(h:sub(3, 4), 16) or 255,
             tonumber(h:sub(5, 6), 16) or 255, (#h >= 8 and tonumber(h:sub(7, 8), 16)) or 255 }
  end
  return c
end
-- palette shared by the figures (matches the native draw_plot_clip look)
local FIG = { acc = { 120, 205, 235, 255 }, axis = { 150, 162, 186, 255 }, grid = { 72, 80, 102, 110 },
  txt = { 238, 244, 252, 255 }, sub = { 162, 174, 198, 255 }, card = { 15, 17, 26, 232 },
  cardb = { 88, 96, 120, 140 }, yellow = { 255, 213, 74, 255 }, pill = { 18, 20, 30, 240 } }
-- a straight line between two PX points (converted to fractions of a full-frame float box)
local function pxline(x0, y0, x1, y1, col, th)
  local W, H = _fw(), _fh()
  return shape{ float = "tl", fx = 0, fy = 0, w = W, h = H, shape = "line",
    from = { x0 / W, y0 / H }, to = { x1 / W, y1 / H }, color = _col(col), thickness = th or 2 }
end
local function pxarrow(x0, y0, x1, y1, col, th)
  local W, H = _fw(), _fh()
  return shape{ float = "tl", fx = 0, fy = 0, w = W, h = H, shape = "arrow",
    from = { x0 / W, y0 / H }, to = { x1 / W, y1 / H }, color = _col(col), thickness = th or 4 }
end
-- a polyline through PX points; closed+fill => a filled polygon (pie wedge / area)
local function pxpoly(pts, col, th, closed, fill)
  local W, H = _fw(), _fh(); local fp = {}
  for i, p in ipairs(pts) do fp[i] = { p[1] / W, p[2] / H } end
  return shape{ float = "tl", fx = 0, fy = 0, w = W, h = H, shape = "polyline",
    points = fp, color = _col(col), thickness = th or 2, closed = closed or false, fill = _col(fill) }
end
-- a circle centred at PX (x,y), radius r px. fill=nil => outline only (color=line).
local function pxdot(x, y, r, fill, line, th)
  return shape{ float = "tl", fx = x - r, fy = y - r, w = 2 * r, h = 2 * r, shape = "ellipse",
    fill = _col(fill), color = _col(line or fill) or { 0, 0, 0, 0 }, thickness = th or 2 }
end
local function pxrect(x, y, w, h, bg, rad, bw, bc)
  return box{ float = "tl", fx = x, fy = y, w = w, h = h, bg = _col(bg), radius = rad, bw = bw, bc = _col(bc) }
end
-- a text label whose (ax,ay) anchor (0=left/top .5=centre 1=right/bottom) sits at PX (x,y)
local function pxtext(s, x, y, size, col, ax, ay, mono)
  ax = ax or 0; ay = ay or 0
  local tw, th = measure_text(s, size, 0, mono or false)
  return box{ float = "tl", fx = x - ax * tw, fy = y - ay * th,
    kids = { text(s, { size = size, col = _col(col), font = mono and "mono" or nil }) } }
end

-- ZOOMOUT — the pan-zoom-out reveal: start tight on a crop window (a few cells) and pull back to
-- the full image, easing out, so the whole thing (e.g. all 64 sprite frames) resolves into view.
-- Keep `from`/`to` at the frame aspect (for a 16:9 image, w==h in fraction) so nothing stretches.
-- data: { image, from={x,y,w,h}(0..1 of the image), to={x,y,w,h}, dur, ease, fit, glow, hold }
function widgets.zoomout(t, d)
  d = d or {}
  local dur = d.dur or 1.7
  local e = anim.tween(t, dur, d.ease or "out_expo")
  local f = d.from or { 0.40, 0.15, 0.30, 0.30 }
  local to = d.to or { 0, 0, 1, 1 }
  local crop = {}; for i = 1, 4 do crop[i] = f[i] + (to[i] - f[i]) * e end
  local bx, by = anim.bob(t, 6, 0.5)                                 -- a hair of life once settled
  local glow = (d.glow or 0) * anim.rise(t, dur * 0.7) * (1 - anim.clamp((t - dur * 0.9) / 0.6, 0, 1))
  return center(image{ asset = d.image, grow = true, crop = crop, fit = d.fit or "contain",
    tx = bx * e, ty = by * e, glow = glow, glow_col = d.glow_col })
end

-- FILMSTRIP — highlight each panel of a baked sequence strip in turn (a slow "watch it advance"
-- sweep — the tilt→roll odometer feel). The base strip fills the frame; an accent box steps across.
-- data: { image, panels=[{x,y,w,h}(0..1 frame)], step=sec, color, dim=0..1, label }
function widgets.filmstrip(t, d)
  d = d or {}
  local W, H = _fw(), _fh()
  local ac = d.color or { 120, 205, 235, 255 }
  local panels = d.panels or {}
  local step = d.step or 1.0
  local cur = math.max(1, math.min(#panels, math.floor(t / step) + 1))
  local kids = { image{ asset = d.image, grow = true, fit = "contain", tint = { 255, 255, 255, math.floor(255 * (1 - (d.dim or 0.0))) } } }
  for i, p in ipairs(panels) do
    local shown = t >= (i - 1) * step
    if shown then
      local on = i == cur
      local e = anim.rise(t - (i - 1) * step, 0.28)
      kids[#kids + 1] = box{ float = "tl", fx = p[1] * W, fy = p[2] * H, w = p[3] * W, h = p[4] * H,
        radius = 10, bw = on and 6 or 3, bc = theme.fade(ac, (on and 1.0 or 0.35) * e) }
      if on and d.label then
        kids[#kids + 1] = box{ float = "tl", fx = p[1] * W, fy = (p[2] + p[4]) * H + 8, pad = 8, radius = 8,
          bg = theme.fade({ 18, 20, 30 }, e), kids = { text(string.format(d.label, i), { size = 26, col = ac }) } }
      end
    end
  end
  return box{ growx = true, growy = true, kids = kids }
end

-- CHART — line/step/scatter/bar with axes, ticks (int|hex|f1|f2|pct), gridlines, shaded regions,
-- dashed vlines, annotated markers (dot + callout pill), arrow callouts, legend, title/subtitle,
-- and a left-to-right `reveal`. Parity with the native `plot` clip, now a forkable scene widget.
-- data: { title, sub, series=[{points=[[x,y]], kind="line|step|scatter|bar", color, width, dots}],
--   x/y={min,max,label,ticks,fmt}, regions=[{x0,x1|y0,y1,color,label}], vlines=[{x,color,label}],
--   markers=[{x,y,label,sub,color,dy}], callouts=[{x,y,text,from={xf,yf},color}], legend, reveal,
--   source, top,bottom,x0,x1 (card bounds as frame fractions), font_px }
function widgets.chart(t, d)
  d = d or {}
  local W, H, co = _fw(), _fh(), FIG
  local kids = {}
  local cx0, cy0 = (d.x0 or 0.045) * W, (d.top or 0.115) * H
  local cx1, cy1 = (d.x1 or 0.955) * W, (d.bottom or 0.915) * H
  local title, sub = d.title, d.sub
  local fp = d.font_px or 30
  local mL = fp * (d.mL or 4.4); local mR = fp * (d.mR or 1.2)
  local mT = (title and fp * 2.1 or fp * 0.5) + (sub and fp * 1.7 or 0)
  local mB = fp * (d.mB or 3.0)
  local pX0, pY0 = cx0 + mL, cy0 + mT
  local pX1, pY1 = cx1 - mR, cy1 - mB
  -- data ranges (auto unless x/y.min/max given)
  local xmin, xmax, ymin, ymax = 1e300, -1e300, 1e300, -1e300
  for _, sr in ipairs(d.series or {}) do for _, p in ipairs(sr.points or {}) do
    xmin = math.min(xmin, p[1]); xmax = math.max(xmax, p[1])
    ymin = math.min(ymin, p[2]); ymax = math.max(ymax, p[2])
  end end
  local function rng(ax, lo, hi) local a = d[ax]
    if a then if a.min then lo = a.min end; if a.max then hi = a.max end end; return lo, hi end
  xmin, xmax = rng("x", xmin, xmax); ymin, ymax = rng("y", ymin, ymax)
  if xmax - xmin < 1e-9 then xmax = xmin + 1 end
  if ymax - ymin < 1e-9 then ymax = ymin + 1 end
  local function sx(x) return pX0 + (x - xmin) / (xmax - xmin) * (pX1 - pX0) end
  local function sy(y) return pY1 - (y - ymin) / (ymax - ymin) * (pY1 - pY0) end
  local function fmt(v, f)
    if f == "hex" then return string.format("0x%X", math.floor(v + 0.5))
    elseif f == "f1" then return string.format("%.1f", v)
    elseif f == "f2" then return string.format("%.2f", v)
    elseif f == "pct" then return string.format("%d%%", math.floor(v + 0.5))
    else return string.format("%d", math.floor(v + 0.5)) end
  end
  local function ticks(ax, lo, hi) local a = d[ax]
    if a and a.ticks then return a.ticks end
    local tk = {}; for i = 0, 4 do tk[#tk + 1] = lo + (hi - lo) * i / 4 end; return tk end
  -- CARD (behind everything)
  local pad = fp * 0.7
  kids[#kids + 1] = pxrect(cx0 - pad, cy0 - pad, (cx1 - cx0) + 2 * pad, (cy1 - cy0) + 2 * pad,
    co.card, fp * 0.4, 1.2, co.cardb)
  if title then kids[#kids + 1] = pxtext(title, (cx0 + cx1) / 2, cy0 + fp * 0.05, fp * 1.15, d.title_col or co.txt, 0.5, 0) end
  if sub then kids[#kids + 1] = pxtext(sub, (cx0 + cx1) / 2, cy0 + fp * 1.5, fp * 0.72, d.sub_col or co.acc, 0.5, 0) end
  -- shaded regions
  for _, r in ipairs(d.regions or {}) do
    local rc = r.color or { 210, 90, 90, 40 }
    if r.x0 then local a, b = sx(r.x0), sx(r.x1)
      kids[#kids + 1] = pxrect(math.min(a, b), pY0, math.abs(b - a), pY1 - pY0, rc, 0)
      if r.label then kids[#kids + 1] = pxtext(r.label, math.min(a, b) + 7, pY0 + 4, fp * 0.62, co.sub, 0, 0) end
    else local a, b = sy(r.y1 or ymax), sy(r.y0 or ymin)
      kids[#kids + 1] = pxrect(pX0, math.min(a, b), pX1 - pX0, math.abs(b - a), rc, 0)
      if r.label then kids[#kids + 1] = pxtext(r.label, pX0 + 7, math.min(a, b) + 4, fp * 0.62, co.sub, 0, 0) end
    end
  end
  -- grid + tick labels
  for _, gx in ipairs(ticks("x", xmin, xmax)) do local X = sx(gx)
    if d.grid ~= false then kids[#kids + 1] = pxline(X, pY0, X, pY1, co.grid, 1) end
    kids[#kids + 1] = pxtext(fmt(gx, d.x and d.x.fmt or "int"), X, pY1 + fp * 0.35, fp * 0.6, co.sub, 0.5, 0)
  end
  for _, gy in ipairs(ticks("y", ymin, ymax)) do local Y = sy(gy)
    if d.grid ~= false then kids[#kids + 1] = pxline(pX0, Y, pX1, Y, co.grid, 1) end
    kids[#kids + 1] = pxtext(fmt(gy, d.y and d.y.fmt or "int"), pX0 - fp * 0.45, Y, fp * 0.6, co.sub, 1, 0.5)
  end
  -- axes
  kids[#kids + 1] = pxline(pX0, pY1, pX1, pY1, co.axis, 1.8)
  kids[#kids + 1] = pxline(pX0, pY0, pX0, pY1, co.axis, 1.8)
  if d.x and d.x.label then kids[#kids + 1] = pxtext(d.x.label, (pX0 + pX1) / 2, cy1 - fp * 1.0, fp * 0.7, co.axis, 0.5, 0) end
  if d.y and d.y.label then                                  -- rotated (subtree t_rot) vertical axis label
    local s = fp * 0.66; local tw, th2 = measure_text(d.y.label, s, 0, false)
    local ccx, ccy = cx0 + fp * 1.0, (pY0 + pY1) / 2
    kids[#kids + 1] = box{ float = "tl", fx = ccx - tw / 2, fy = ccy - th2 / 2, t_rot = -90,
      kids = { text(d.y.label, { size = s, col = co.axis }) } }
  end
  -- dashed vertical markers (e.g. "frame 0" on the flick trace)
  for _, v in ipairs(d.vlines or {}) do
    local X = sx(v.x); local vc = v.color or co.acc; local dash, gap = v.dash or 9, v.gap or 7
    local y = pY0
    while y < pY1 do kids[#kids + 1] = pxline(X, y, X, math.min(y + dash, pY1), vc, v.thickness or 1.5); y = y + dash + gap end
    if v.label then kids[#kids + 1] = pxtext(v.label, X + 5, pY0 + 2, fp * 0.58, vc, 0, 0) end
  end
  -- series (reveal = left-to-right plotter draw)
  local reveal = d.reveal; if reveal == nil then reveal = anim.rise(t - 0.15, d.reveal_dur or 1.1) end
  for _, sr in ipairs(d.series or {}) do
    local pts = sr.points or {}; local N = #pts
    if N > 0 then
      local col = sr.color or co.acc; local th = sr.width or 3.0
      local kfull = reveal >= 1 and N or math.max(1, math.floor(reveal * N))
      local rp = {}
      for i = 1, math.min(kfull, N) do rp[#rp + 1] = { sx(pts[i][1]), sy(pts[i][2]) } end
      if kfull < N then                                     -- partial last segment
        local fpts = reveal * (N - 1) + 1; local k = math.floor(fpts)
        if k >= 1 and k < N then local fr = fpts - k
          rp[#rp + 1] = { sx(pts[k][1] + (pts[k + 1][1] - pts[k][1]) * fr),
                          sy(pts[k][2] + (pts[k + 1][2] - pts[k][2]) * fr) }
        end
      end
      if sr.kind == "scatter" then
        for _, p in ipairs(rp) do kids[#kids + 1] = pxdot(p[1], p[2], th * 1.7, col, col, 0) end
      elseif sr.kind == "bar" then
        local base = sy(math.max(ymin, 0)); local bw = (pX1 - pX0) / math.max(1, N) * 0.6
        for i = 1, math.min(kfull, N) do local X, Y = sx(pts[i][1]), sy(pts[i][2])
          kids[#kids + 1] = pxrect(X - bw / 2, math.min(Y, base), bw, math.abs(Y - base), col, 0) end
      else
        if sr.kind == "step" then local st = {}
          for i = 1, #rp do st[#st + 1] = rp[i]; if rp[i + 1] then st[#st + 1] = { rp[i + 1][1], rp[i][2] } end end
          rp = st
        end
        if #rp >= 2 then kids[#kids + 1] = pxpoly(rp, col, th) end
        if sr.dots then for i = 1, math.min(kfull, N) do kids[#kids + 1] = pxdot(sx(pts[i][1]), sy(pts[i][2]), th * 1.5, col, col, 0) end end
      end
    end
  end
  -- annotated markers: dot + ring + callout pill above (connector line), kept inside the plot
  for _, m in ipairs(d.markers or {}) do
    local X, Y = sx(m.x), sy(m.y); local mc = m.color or co.yellow
    kids[#kids + 1] = pxdot(X, Y, 5, mc, mc, 0)
    kids[#kids + 1] = pxdot(X, Y, 9, nil, mc, 1.6)
    if m.label and m.label ~= "" then
      local s1, s2 = fp * 0.72, fp * 0.56
      local lw = measure_text(m.label, s1, 0, false)
      local sw = m.sub and measure_text(m.sub, s2, 0, false) or 0
      local lh = fp * 0.9
      local bw = math.max(lw, sw) + fp * 0.7
      local bh = lh + (m.sub and (fp * 0.72 + 2) or 0) + fp * 0.4
      local lx = math.max(pX0, math.min(pX1 - bw, X - bw / 2))
      local ly = Y + (m.dy or -1) * fp * 1.4 - bh
      kids[#kids + 1] = pxline(X, Y, X, ly + bh, mc, 1.3)
      kids[#kids + 1] = pxrect(lx, ly, bw, bh, co.pill, fp * 0.25, 1.2, mc)
      kids[#kids + 1] = pxtext(m.label, lx + bw / 2, ly + fp * 0.2, s1, co.txt, 0.5, 0)
      if m.sub then kids[#kids + 1] = pxtext(m.sub, lx + bw / 2, ly + fp * 0.2 + lh, s2, co.sub, 0.5, 0) end
    end
  end
  -- arrow callouts (a text block with an arrow that extends to a data point)
  for _, c in ipairs(d.callouts or {}) do
    local X, Y = sx(c.x), sy(c.y); local cc = _col(c.color or co.acc)
    local ax0 = (c.from and c.from[1] or 0.62) * W; local ay0 = (c.from and c.from[2] or 0.66) * H
    local e = anim.rise(t - (c.delay or 0.9), 0.5)
    if e > 0.01 then kids[#kids + 1] = pxarrow(ax0, ay0, ax0 + (X - ax0) * e, ay0 + (Y - ay0) * e, cc, c.thickness or 4) end
    local lines = c.lines or {}
    if c.text then for ln in tostring(c.text):gmatch("[^\n]+") do lines[#lines + 1] = ln end end
    local ly = ay0; local ls = c.size or fp * 0.82
    for _, ln in ipairs(lines) do
      kids[#kids + 1] = pxtext(ln, ax0, ly - #lines * ls * 1.15, ls, theme.fade(cc, e), 0, 0)
      ly = ly + ls * 1.15
    end
  end
  -- legend (top-right inside the plot)
  if d.legend then
    local maxw = 0; local nrow = 0
    for _, sr in ipairs(d.series or {}) do if sr.label then maxw = math.max(maxw, (measure_text(sr.label, fp * 0.62, 0, false))); nrow = nrow + 1 end end
    if nrow > 0 then
      local sw = fp * 0.9; local boxw = maxw + sw + fp * 1.1; local boxh = nrow * fp * 0.95 + fp * 0.5
      local lx = pX1 - boxw - fp * 0.4; local ty = pY0 + fp * 0.35
      kids[#kids + 1] = pxrect(lx, ty, boxw, boxh, { 20, 22, 34, 225 }, fp * 0.2, 1, { 90, 100, 130, 160 })
      local yy = ty + fp * 0.28
      for _, sr in ipairs(d.series or {}) do if sr.label then
        kids[#kids + 1] = pxline(lx + fp * 0.4, yy + fp * 0.32, lx + fp * 0.4 + sw, yy + fp * 0.32, sr.color or co.acc, sr.width or 3)
        kids[#kids + 1] = pxtext(sr.label, lx + fp * 0.4 + sw + fp * 0.3, yy, fp * 0.62, co.txt, 0, 0)
        yy = yy + fp * 0.95
      end end
    end
  end
  if d.source then kids[#kids + 1] = pxtext(d.source, cx1 - 6, cy1 - fp * 0.85, fp * 0.54, { 150, 140, 172, 200 }, 1, 0) end
  return box{ growx = true, growy = true, t_op = anim.rise(t, 0.3), kids = kids }
end

-- DCM — the ADXL202 duty-cycle diagram: two labeled square waves (0 g = 50% duty, +1 g = wider
-- high-time) with dimension brackets (T1/T2), side annotations, legend, and a running-in reveal.
-- The pulses are `polyline` step waves. data: { title, sub, waves=[{label,color,duty,cy(frac)}],
--   cycles, source }.
function widgets.dcm(t, d)
  d = d or {}
  local W, H, co = _fw(), _fh(), FIG
  local kids = {}
  local cx0, cy0 = 0.045 * W, 0.11 * H
  local cx1, cy1 = 0.955 * W, 0.915 * H
  local fp = d.font_px or 30
  local pX0, pX1 = cx0 + fp * 1.0, cx1 - fp * 8.5           -- leave a right gutter for side labels
  local pTop, pBot = cy0 + fp * 2.9, cy1 - fp * 2.2
  local cycles = d.cycles or 4.0
  local amp = fp * 1.5                                       -- wave half-height
  local P = (pX1 - pX0) / (cycles + 0.15)                   -- one period width
  -- card + title/sub
  local pad = fp * 0.7
  kids[#kids + 1] = pxrect(cx0 - pad, cy0 - pad, (cx1 - cx0) + 2 * pad, (cy1 - cy0) + 2 * pad, co.card, fp * 0.4, 1.2, co.cardb)
  if d.title then kids[#kids + 1] = pxtext(d.title, (cx0 + cx1) / 2, cy0 - fp * 0.2, fp * 1.2, co.txt, 0.5, 0) end
  if d.sub then kids[#kids + 1] = pxtext(d.sub, (cx0 + cx1) / 2, cy0 + fp * 1.35, fp * 0.72, co.acc, 0.5, 0) end
  local reveal = d.reveal; if reveal == nil then reveal = anim.rise(t - 0.1, 1.2) end
  local waves = d.waves or {}
  for wi, wv in ipairs(waves) do
    local cyy = (wv.cy or (wi == 1 and 0.40 or 0.66)) * H
    local yHi, yLo = cyy - amp, cyy + amp
    local duty = wv.duty or 0.5
    -- build the square wave as a step polyline, revealed left→right
    local pts = {}; local x = pX0
    local xend = pX0 + reveal * (pX1 - pX0)
    local function push(px, py) pts[#pts + 1] = { math.min(px, xend), py } end
    for c = 0, math.ceil(cycles) - 1 do
      local x0 = pX0 + c * P
      if x0 > xend then break end
      push(x0, yHi); push(x0 + duty * P, yHi); push(x0 + duty * P, yLo); push(x0 + P, yLo)
      x = x0 + P
    end
    if #pts >= 2 then kids[#kids + 1] = pxpoly(pts, wv.color or co.acc, wv.width or 4) end
    -- side annotation to the right of the wave
    if wv.side then
      local sc = (d.highlight == "duty") and co.txt or (wv.color or co.acc)
      kids[#kids + 1] = pxtext(wv.side, pX1 + fp * 0.5, cyy - fp * 0.5, fp * 0.82, sc, 0, 0.5)
    end
  end
  -- highlight the 50%-duty reference (the "flat" beat): a dashed midline across the flat wave
  if d.highlight == "duty" and #waves >= 1 then
    local w1 = waves[1]; local cyy = (w1.cy or 0.40) * H
    local x = pX0
    while x < pX1 do kids[#kids + 1] = pxline(x, cyy, math.min(x + 12, pX1), cyy, { 120, 205, 235, 150 }, 1.5); x = x + 22 end
    kids[#kids + 1] = pxtext("50% = flat", pX0 + fp * 0.3, cyy - fp * 1.0, fp * 0.6, co.acc, 0, 0)
  end
  -- dimension brackets on the first wave: T1 (high-time, above) and T2 (period, below)
  if #waves >= 1 then
    local w1 = waves[1]; local cyy = (w1.cy or 0.40) * H; local yHi = cyy - amp
    local duty = w1.duty or 0.5
    local function dim(x0, x1, y, label, above)
      local cap = fp * 0.4
      kids[#kids + 1] = pxline(x0, y, x1, y, co.sub, 2)
      kids[#kids + 1] = pxline(x0, y - cap, x0, y + cap, co.sub, 2)
      kids[#kids + 1] = pxline(x1, y - cap, x1, y + cap, co.sub, 2)
      kids[#kids + 1] = pxtext(label, (x0 + x1) / 2, above and (y - fp * 1.05) or (y + fp * 0.25), fp * 0.64, co.txt, 0.5, 0)
    end
    dim(pX0, pX0 + duty * P, yHi - fp * 0.9, "T1 (high)", true)
    dim(pX0, pX0 + P, (cyy + amp) + fp * 1.2, "T2 (period, fixed by RSET)", false)
    if #waves >= 2 then
      local w2 = waves[2]; local cyy2 = (w2.cy or 0.66) * H; local d2 = w2.duty or 0.625
      kids[#kids + 1] = pxtext("T1 grows with tilt", pX0, (cyy2 - amp) - fp * 1.15, fp * 0.62, w2.color or co.yellow, 0, 0)
      local cap = fp * 0.4; local y = (cyy2 - amp) - fp * 0.55
      kids[#kids + 1] = pxline(pX0, y, pX0 + d2 * P, y, w2.color or co.yellow, 2)
      kids[#kids + 1] = pxline(pX0, y - cap, pX0, y + cap, w2.color or co.yellow, 2)
      kids[#kids + 1] = pxline(pX0 + d2 * P, y - cap, pX0 + d2 * P, y + cap, w2.color or co.yellow, 2)
    end
  end
  -- x axis with ticks 0..cycles
  kids[#kids + 1] = pxline(pX0, pBot, pX1 + P * 0.15, pBot, co.axis, 1.8)
  for i = 0, math.floor(cycles) do local X = pX0 + i * P
    kids[#kids + 1] = pxtext(tostring(i), X, pBot + fp * 0.3, fp * 0.62, co.sub, 0.5, 0) end
  if d.xlabel then kids[#kids + 1] = pxtext(d.xlabel, (pX0 + pX1) / 2, cy1 - fp * 1.0, fp * 0.68, co.acc, 0.5, 0) end
  -- legend (top-right)
  local nrow = #waves
  if nrow > 0 then
    local maxw = 0; for _, wv in ipairs(waves) do maxw = math.max(maxw, (measure_text(wv.label or "", fp * 0.62, 0, false))) end
    local sw = fp * 0.9; local boxw = maxw + sw + fp * 1.1; local boxh = nrow * fp * 0.95 + fp * 0.5
    local lx = cx1 - boxw - fp * 0.8; local ty = cy0 + fp * 2.3
    kids[#kids + 1] = pxrect(lx, ty, boxw, boxh, { 20, 22, 34, 230 }, fp * 0.2, 1, { 90, 100, 130, 170 })
    local yy = ty + fp * 0.28
    for _, wv in ipairs(waves) do
      kids[#kids + 1] = pxline(lx + fp * 0.4, yy + fp * 0.32, lx + fp * 0.4 + sw, yy + fp * 0.32, wv.color or co.acc, wv.width or 4)
      kids[#kids + 1] = pxtext(wv.label or "", lx + fp * 0.4 + sw + fp * 0.3, yy, fp * 0.62, co.txt, 0, 0)
      yy = yy + fp * 0.95
    end
  end
  if d.source then kids[#kids + 1] = pxtext(d.source, cx1 - 6, cy1 - fp * 0.8, fp * 0.54, { 150, 140, 172, 200 }, 1, 0) end
  return box{ growx = true, growy = true, t_op = anim.rise(t, 0.3), kids = kids }
end

-- OCTANT — the "menus quantize to 8 buckets" radial dial: 8 filled pie wedges + labels, a live
-- cyan analog tilt vector that sweeps continuously (the point: the raw tilt is analog, the menu
-- SNAPS it to whichever octant it lands in — highlighted). data: { title, sub, footer, note,
--   cx,cy,r (frame fractions), vector_deg (screen deg, 0=E +cw, -90=up), sweep, speed, source }.
function widgets.octant(t, d)
  d = d or {}
  local W, H, co = _fw(), _fh(), FIG
  local kids = {}
  local cxp, cyp = (d.cx or 0.5) * W, (d.cy or 0.53) * H
  local r = (d.r or 0.235) * H
  local OCT = { { a = 0, l = "E" }, { a = 45, l = "SE" }, { a = 90, l = "S" }, { a = 135, l = "SW" },
                { a = 180, l = "W" }, { a = 225, l = "NW" }, { a = 270, l = "N" }, { a = 315, l = "NE" } }
  local function rim(ang, rr) local a = ang * math.pi / 180; return { cxp + math.cos(a) * rr, cyp + math.sin(a) * rr } end
  -- the live analog vector sweeps across the N/NE boundary, gliding (never snapping) — and it stays
  -- off the octant labels (which sit on the spoke centres) so the arrow doesn't cross the text
  local va = (d.vector_deg or -62) + (d.sweep or 20) * math.sin(t * (d.speed or 0.7) + 0.5)
  local function norm(a) a = a % 360; if a < 0 then a = a + 360 end; return a end
  local active, best = 1, 1e9
  for i, o in ipairs(OCT) do local dd = math.abs(((norm(va) - o.a + 180) % 360) - 180)
    if dd < best then best = dd; active = i end end
  -- title / sub (above the dial)
  if d.title then kids[#kids + 1] = pxtext(d.title, 0.06 * W, 0.06 * H, 44, co.txt, 0, 0) end
  if d.sub then kids[#kids + 1] = pxtext(d.sub, 0.06 * W, 0.06 * H + 52, 26, co.acc, 0, 0) end
  -- filled pie wedges (alternating purple), outlined so the radial separators come for free
  local shades = { { 104, 74, 158, 255 }, { 132, 92, 190, 255 } }
  local diag = { [2] = true, [4] = true, [6] = true, [8] = true }   -- SE/SW/NW/NE = the diagonals
  local appear = anim.rise(t, 0.5)
  for i, o in ipairs(OCT) do
    local a0, a1 = o.a - 22.5, o.a + 22.5
    local pts = { { cxp, cyp } }
    for k = 0, 9 do pts[#pts + 1] = rim(a0 + (a1 - a0) * k / 9, r * appear) end
    local fillc = shades[(i % 2) + 1]
    if d.highlight == "diagonals" and diag[i] then fillc = { 152, 108, 214, 255 } end   -- brighten the 4 diagonals
    kids[#kids + 1] = pxpoly(pts, { 38, 28, 52, 255 }, 1.5, true, fillc)
  end
  -- active-octant accent rim arc
  do local o = OCT[active]; local a0, a1 = o.a - 22.5, o.a + 22.5; local arc = {}
    for k = 0, 12 do arc[#arc + 1] = rim(a0 + (a1 - a0) * k / 12, r) end
    kids[#kids + 1] = pxpoly(arc, { 64, 224, 208, 255 }, 5, false, nil)
  end
  kids[#kids + 1] = pxdot(cxp, cyp, r, nil, { 158, 128, 208, 170 }, 2)   -- clean outer ring
  -- octant labels (active one — or all diagonals when highlighted — brightened cyan + bigger)
  for i, o in ipairs(OCT) do local lp = rim(o.a, r * 0.60); local act = i == active
    local hot = act or (d.highlight == "diagonals" and diag[i])
    kids[#kids + 1] = pxtext(o.l, lp[1], lp[2], hot and 42 or 34, hot and { 64, 224, 208, 255 } or { 245, 245, 250, 255 }, 0.5, 0.5)
  end
  -- the analog tilt vector
  local vp = rim(va, r * 0.80)
  kids[#kids + 1] = pxarrow(cxp, cyp, vp[1], vp[2], { 64, 224, 208, 255 }, 6)
  local tipx = vp[1] > cxp and (vp[1] + 18) or (vp[1] - 18)
  local aticha = vp[1] > cxp and 0 or 1
  kids[#kids + 1] = pxtext("actual analog", tipx, vp[2] - 24, 26, { 64, 224, 208, 255 }, aticha, 0)
  kids[#kids + 1] = pxtext("tilt vector", tipx, vp[2] + 2, 26, { 64, 224, 208, 255 }, aticha, 0)
  -- footer + note (below the dial)
  if d.footer then kids[#kids + 1] = pxtext(d.footer, 0.5 * W, cyp + r + 0.03 * H, 34, co.acc, 0.5, 0) end
  if d.note then kids[#kids + 1] = pxtext(d.note, 0.5 * W, cyp + r + 0.03 * H + 44, 26, co.sub, 0.5, 0) end
  if d.source then kids[#kids + 1] = pxtext(d.source, 0.945 * W, 0.945 * H, 24, { 150, 140, 172, 200 }, 1, 0) end
  return box{ growx = true, growy = true, t_op = anim.rise(t, 0.3), kids = kids }
end
