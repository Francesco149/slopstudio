#!/usr/bin/env python3
"""dashboard.py — the @GemmaExplains control panel (local web UI).

A single-file, dependency-free (stdlib http.server) dashboard the owner opens in a
browser to run the whole video/social operation at a glance:

  • SOCIAL   — the queue + per-platform cadence (DUE/ok, last posted, depth), the
               "post today" suggestions, a gallery of GPT gens with which post each is
               paired to; mark-posted / pair-image / copy-body without touching a file.
  • IMAGE QUEUE — GPT-image briefs (social/image-requests/) to stock up on gens; drag the finished
               image onto a brief to fulfil it — it saves into the gens folder, marks the request
               done, and auto-pairs to its for-post when one is linked.
  • LAUNCHER — one-click the VIDEO_RUNBOOK actions (build · voice · lint · look · export ·
               transcript · status) with their options, open the editor / thumbnail editor
               (Gemma brand) on a project, and the machine checks (doctor · wake).
  • PROJECTS — the per-project pipeline checklist (skeleton · cut · thumbs · packaging · export).

It never reimplements the tools — it drives tools/video.py, tools/social.py and the built
editors, streaming their output into a live log. Whitelisted commands only; binds to
localhost. Run it inside the dev shell:

  nix develop --command python tools/dashboard.py [--port 8080] [--open]
  # or:  nix develop --command python tools/video.py dashboard

Social content lives in ../gemma-branding/social/ (see ../gemma-branding/SOCIAL.md); GPT gens
land in /mnt/f/Pictures/oc/gemma-san/social/. Nothing Gemma-specific is hardcoded here — it is
all read at runtime, same as tools/social.py.
"""
import argparse, datetime, glob, hashlib, io, json, mimetypes, os, re, shutil, socket
import subprocess, sys, threading, time, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
PROJECTS = os.path.normpath(os.path.join(ROOT, "..", "slopstudio-projects"))
GEMMA = os.path.normpath(os.path.join(ROOT, "..", "gemma-branding"))
SOCIAL_DIR = os.path.join(GEMMA, "social")
SOCIAL_IMG = "/mnt/f/Pictures/oc/gemma-san/social"
EXPORTS = os.path.join(ROOT, "exports")
THUMB_CACHE = os.path.join(ROOT, "cache", "dashboard-thumbs")
FEED_HEALTH = "http://localhost:8777/healthz"
LAME_HOST = "10.0.10.56"
EDITOR_EXE = os.path.join(ROOT, "build", "slopstudio.exe")
THUMB_EXE = os.path.join(ROOT, "build", "slopthumb.exe")
BRAND_PKG = os.path.join(GEMMA, "brand-package")
YUTU_BIN = shutil.which("yutu") or os.path.expanduser("~/.local/bin/yutu")
YUTU_DIR = os.path.expanduser("~/.config/yutu")           # yutu reads creds from its CWD
YUTU_TOKEN = os.path.join(YUTU_DIR, "youtube.token.json")  # exists ⇒ OAuth done

sys.path.insert(0, TOOLS)
import social  # reuse the queue parser / window logic — one source of truth
import engage  # same for the engagement cards
ENGAGE_DIR = os.path.join(SOCIAL_DIR, "engagement")

IMG_EXT = (".png", ".jpg", ".jpeg", ".webp", ".gif")
VID_EXT = (".mp4", ".mov", ".webm", ".mkv")

# roots a thumbnail/file request is allowed to read from (no arbitrary FS access)
SAFE_ROOTS = [SOCIAL_IMG, EXPORTS, PROJECTS, GEMMA, os.path.join(ROOT, "build"),
              os.path.join(ROOT, "library"), os.path.join(ROOT, "presets")]


# ── small helpers ─────────────────────────────────────────────────────────────
def tcp_ok(host, port, t=1.0):
    try:
        with socket.create_connection((host, port), timeout=t):
            return True
    except OSError:
        return False


def http_ok(url, t=1.0):
    try:
        with urllib.request.urlopen(url, timeout=t) as r:
            return r.status == 200
    except Exception:
        return False


def under_safe_root(path):
    ap = os.path.realpath(path)
    return any(ap == os.path.realpath(r) or ap.startswith(os.path.realpath(r) + os.sep) for r in SAFE_ROOTS)


def kind_of(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in IMG_EXT:
        return "image"
    if ext in VID_EXT:
        return "video"
    return "other"


def resolve_ref(ref):
    """A post's image: ref — absolute, or relative to the gemma-branding repo root."""
    if not ref or ref in ("none", "wanted"):
        return None
    return ref if ref.startswith("/") else os.path.normpath(os.path.join(GEMMA, ref))


# ── state: social ─────────────────────────────────────────────────────────────
def social_state(today):
    try:
        cfg = social.tomllib.load(open(os.path.join(SOCIAL_DIR, "platforms.toml"), "rb"))
    except Exception as e:
        return {"error": f"platforms.toml: {e}", "platforms": [], "queue": [], "posted": [], "images": []}
    posts = social.load_all(SOCIAL_DIR)

    platforms = []
    due_names = set()
    for name, pc in cfg.items():
        lp = social.last_posted(posts, name)
        cad = pc.get("cadence_days", 3)
        since = (today - lp).days if lp else None
        due = since is None or since >= cad
        if due:
            due_names.add(name)
        depth = sum(1 for p in posts["queue"] if name in p["platforms"]
                    and social.window_state(p["meta"].get("window"), today)[0] in ("open", "upcoming"))
        platforms.append({"name": name, "handle": pc.get("handle", ""), "cadence_days": cad,
                          "last_posted": str(lp) if lp else None, "days_since": since,
                          "due": due, "queue_depth": depth, "notes": pc.get("notes", "")})

    def post_json(p, posted=False):
        ref = p["meta"].get("image", "none")
        resolved = resolve_ref(ref)
        st, _, human = social.window_state(p["meta"].get("window"), today)
        return {"id": p["id"], "platforms": p["platforms"], "occasion": p["meta"].get("occasion", ""),
                "window": p["meta"].get("window", "anytime"), "window_state": st, "window_human": human,
                "priority": p["meta"].get("priority", "normal"), "author": p["meta"].get("author", ""),
                "image_ref": ref, "image_abs": resolved or "",
                "image_ok": bool(resolved and os.path.exists(resolved)),
                "image_kind": ("none" if ref in ("none", "") else "wanted" if ref == "wanted"
                               else kind_of(ref)),
                "image_prompt": p["meta"].get("image-prompt", ""), "note": p["meta"].get("image-note", ""),
                "body": p["body"], "posted": p["meta"].get("posted"), "posted_url": p["meta"].get("posted-url")}

    queue = [post_json(p) for p in posts["queue"]]
    posted = [post_json(p, True) for p in posts["posted"]]

    # "post today": window-open posts for due platforms — dated windows always surface
    # (mirror social.py status ordering)
    sug = []
    for p in queue:
        dated = p["window_human"] != "anytime"
        if p["window_state"] == "open" and (dated or any(pl in due_names for pl in p["platforms"])):
            sug.append((not dated, p["priority"] != "high", p["id"]))
    sug.sort()
    suggestions = [s[2] for s in sug]

    # image gallery: every gen in the social dir + which posts reference it
    used = {}
    for p in queue + posted:
        r = resolve_ref(p["image_ref"])
        if r:
            used.setdefault(os.path.realpath(r), []).append(p["id"])
    images = []
    if os.path.isdir(SOCIAL_IMG):
        for f in sorted(os.listdir(SOCIAL_IMG)):
            fp = os.path.join(SOCIAL_IMG, f)
            if kind_of(fp) != "image" or not os.path.isfile(fp):
                continue
            images.append({"name": f, "path": fp, "paired": used.get(os.path.realpath(fp), [])})
    return {"platforms": platforms, "queue": queue, "posted": posted, "images": images,
            "suggestions": suggestions,
            "stale": [p["id"] for p in queue if p["image_kind"] in ("image", "video") and not p["image_ok"]]}


# ── state: engagement (cards live in social/engagement/, see tools/engage.py) ──
def engage_state():
    try:
        cards = engage.load_all(ENGAGE_DIR)
        engage.auto_prune(ENGAGE_DIR, cards, quiet=True)  # stale pending auto-skip (x/bsky >24h, yt >7d)
    except Exception as e:
        return {"error": str(e), "pending": [], "engaged": [], "skipped": 0}

    def cj(c):
        m = c["meta"]
        sug = []
        if "## suggested replies" in c["body"]:
            for ln in c["body"].split("## suggested replies", 1)[1].splitlines():
                if not ln.startswith("- "):
                    continue
                txt, reply_to = ln[2:].strip(), None
                mt = re.match(r"\[reply to ([^\]]+)\]\s*(.*)$", txt)  # cmd_draft --reply-to marker
                if mt:
                    reply_to, txt = mt.group(1), mt.group(2)
                sug.append({"text": txt, "reply_to": reply_to})
        return {"id": c["id"], "platform": m.get("platform", "?"), "url": m.get("url", ""),
                "suggestions": sug,
                "author": m.get("author", ""), "captured": m.get("captured", ""),
                "session": m.get("session", ""), "stats": m.get("stats", ""),
                "flagged": m.get("safety", "").startswith("flagged"), "safety": m.get("safety", ""),
                "responded": m.get("responded", ""), "response_url": m.get("response-url", ""),
                "likes": m.get("response-likes", ""), "replies": m.get("response-replies", ""),
                "body": c["body"]}

    # cards with suggested replies float to the top — they're the ones waiting on the owner
    return {"pending": [cj(c) for c in sorted(cards["cards"],
                                              key=lambda c: ("## suggested replies" not in c["body"],
                                                             c["meta"].get("captured", "")))],
            "engaged": [cj(c) for c in sorted(cards["engaged"],
                                              key=lambda c: c["meta"].get("responded", ""), reverse=True)],
            "skipped": len(cards["skipped"])}


# ── state: projects ───────────────────────────────────────────────────────────
def projects_state():
    out = []
    for d in sorted(glob.glob(os.path.join(PROJECTS, "*/"))):
        base = os.path.basename(d.rstrip("/"))
        if base in ("demos", "branding"):  # not videos
            pass
        cuts = [x for x in sorted(glob.glob(os.path.join(d, "*.slop.json")))
                if os.sep + "history" + os.sep not in x]
        # LIVE cut first = most recently edited. A plain alphabetical sort put
        # "recettear-v1.slop.json" ('-' < '.') ahead of the live "recettear.slop.json" —
        # the Projects tab's "open editor" silently opened the stale v1 cut (the owner's
        # "no music lane" bug). Canonical-name-first is wrong too (luckymas' live cut is
        # luckymas3, not luckymas.slop.json); the cut being worked on is the newest one.
        cuts.sort(key=lambda x: -os.path.getmtime(x))
        # The one-click "open editor" uses cuts[0]; it must be the MAIN long-form cut, not whichever
        # Short was touched most recently (a Short has "short" in its name). Stable-sort non-shorts
        # ahead of shorts (mtime order kept within each group) so cuts[0] = newest non-short cut.
        # Every cut stays reachable via the Projects-tab cut picker.
        cuts.sort(key=lambda x: "short" in os.path.basename(x).lower())
        sk = sorted(glob.glob(os.path.join(d, "*.skeleton.json")))
        thumbs = sorted(glob.glob(os.path.join(d, "thumbs", "*.thumb.json")))
        pkg = sorted(glob.glob(os.path.join(d, "docs", "*packaging*.md")))
        exports = sorted(glob.glob(os.path.join(EXPORTS, base + "*.mp4")))
        out.append({
            "name": base,
            "skeletons": [os.path.basename(x) for x in sk],
            "cuts": [{"name": os.path.basename(x), "path": x} for x in cuts],
            "thumbs": [{"name": os.path.basename(x).replace(".thumb.json", ""), "path": x} for x in thumbs],
            "packaging": [os.path.basename(x) for x in pkg],
            "exports": [{"name": os.path.basename(x), "mb": round(os.path.getsize(x) / 1e6)} for x in exports],
        })
    return out


# ── state: YouTube channel (read-only, via yutu; cached, degrades gracefully) ──
_CHAN = {"data": None, "ts": 0.0}


def _first_channel(data):
    if isinstance(data, dict):
        return (data.get("items") or [data])[0] if "items" in data else data
    if isinstance(data, list):
        return data[0] if data else {}
    return {}


def channel_state(force=False):
    if not os.path.exists(YUTU_BIN):
        return {"ok": False, "need_setup": True, "hint": "yutu not installed — see docs/INFRA.md § yutu"}
    if not os.path.exists(YUTU_TOKEN):
        return {"ok": False, "need_auth": True,
                "hint": "one-time OAuth needed — see ~/.config/yutu/README.md (docs/INFRA.md § yutu)"}
    now = time.time()
    if not force and _CHAN["data"] and now - _CHAN["ts"] < 300:
        return _CHAN["data"]
    try:
        # yutu reads client_secret.json / youtube.token.json from its CWD (default filenames);
        # absolute path/env forms get mis-parsed as base64, so run from the creds dir.
        r = subprocess.run([YUTU_BIN, "channel", "list", "--mine", "-o", "json",
                            "-p", "id,snippet,statistics"], cwd=YUTU_DIR,
                           capture_output=True, text=True, timeout=25)
        if r.returncode != 0:
            return {"ok": False, "error": (r.stderr or "yutu channel list failed").strip()[:400]}
        ch = _first_channel(json.loads(r.stdout or "{}"))
        sn, st = ch.get("snippet", {}) or {}, ch.get("statistics", {}) or {}
        thumbs = sn.get("thumbnails", {}) or {}
        out = {"ok": True, "id": ch.get("id"), "title": sn.get("title"),
               "custom_url": sn.get("customUrl"), "desc": (sn.get("description") or "")[:220],
               "published": sn.get("publishedAt"),
               "subs": st.get("subscriberCount"), "views": st.get("viewCount"), "videos": st.get("videoCount"),
               "hidden_subs": st.get("hiddenSubscriberCount"),
               "empty_stats": not st,  # channels.list returns {} until there's public activity
               "thumb": ((thumbs.get("high") or thumbs.get("medium") or thumbs.get("default") or {}) or {}).get("url"),
               "fetched": now}
        _CHAN.update(data=out, ts=now)
        return out
    except Exception as e:
        return {"ok": False, "error": str(e)[:400]}


# ── actions registry ──────────────────────────────────────────────────────────
# each: label, group, params (for the UI form), and build(params)->argv (validated).
def _proj_names():
    return [os.path.basename(d.rstrip("/")) for d in sorted(glob.glob(os.path.join(PROJECTS, "*/")))]


def vid(*a):
    return [sys.executable, os.path.join(TOOLS, "video.py"), *a]


def soc(*a):
    return [sys.executable, os.path.join(TOOLS, "social.py"), *a]


def eng(*a):
    return [sys.executable, os.path.join(TOOLS, "engage.py"), *a]


def slop_tool(*a):
    return [sys.executable, os.path.join(TOOLS, "slop.py"), *a]


ACTIONS = {
    "doctor":  {"label": "Machine check", "group": "machine", "params": [],
                "build": lambda p: vid("doctor")},
    "wake":    {"label": "Wake lame (GPU box)", "group": "machine", "params": [], "confirm": True,
                "build": lambda p: vid("wake")},
    "social_status": {"label": "Social status", "group": "machine", "params": [],
                      "build": lambda p: soc("status")},

    "status":  {"label": "Pipeline status", "group": "video", "params": ["project?"],
                "build": lambda p: vid("status", p["project"]) if p.get("project") else vid("status")},
    "show":    {"label": "Timeline overview", "group": "video", "params": ["project"],
                "build": lambda p: vid("show", p["project"])},
    "build":   {"label": "Build cut", "group": "video", "params": ["project"],
                "build": lambda p: vid("build", p["project"])},
    "voice":   {"label": "Generate VO", "group": "video", "params": ["project"], "note": "needs lame",
                "build": lambda p: vid("voice", p["project"])},
    "lint":    {"label": "Lint", "group": "video", "params": ["project"],
                "build": lambda p: vid("lint", p["project"])},
    "look":    {"label": "Look → montage → feed", "group": "video", "params": ["project", "n:int=6"],
                "build": lambda p: vid("look", p["project"], "--n", str(int(p.get("n") or 6)))},
    "export":  {"label": "Export mp4", "group": "video", "params": ["project", "final:bool"],
                "build": lambda p: vid("export", p["project"]) + (["--final"] if p.get("final") else [])},
    "transcript": {"label": "Transcript → clipboard", "group": "video", "params": ["project", "srt:bool"],
                   "build": lambda p: vid("transcript", p["project"]) + (["--srt"] if p.get("srt") else [])},
    "regen_captions": {"label": "Regen captions (on-screen transcript)", "group": "video", "params": ["cut"],
                       "note": "portrait cuts — save in the editor first; it live-reloads the result",
                       "build": lambda p: slop_tool("transcript", _need_portrait_cut(p["cut"]))},

    "open_editor": {"label": "Open editor", "group": "launch", "params": ["cut"], "detach": True,
                    "build": lambda p: [EDITOR_EXE, _need_file(p["cut"]), "--cache", "cache"]},
    "open_thumbtool": {"label": "Open thumbnail editor (Gemma brand)", "group": "launch",
                       "params": ["thumb"], "detach": True,
                       "build": lambda p: [THUMB_EXE, _need_file(p["thumb"]), "--brand", BRAND_PKG]},
}


def _need_file(path):
    ap = os.path.realpath(path)
    if not (os.path.isfile(ap) and under_safe_root(ap)):
        raise ValueError(f"file not allowed / missing: {path}")
    return ap


def _need_portrait_cut(path):
    """the animated on-screen transcript is a Shorts convention — regenerating it on a
    landscape cut would INJECT an r_transcript track it never had (same gate as the editor's
    Project-panel button, which only shows for portrait)."""
    ap = _need_file(path)
    try:
        fmt = json.load(open(ap)).get("meta", {}).get("format")
    except Exception as e:
        raise ValueError(f"unreadable cut: {e}")
    if fmt != "portrait":
        raise ValueError(f"{os.path.basename(ap)} is not a portrait cut — the animated "
                         "on-screen transcript is a Shorts-only convention")
    return ap


def build_action(name, params):
    a = ACTIONS.get(name)
    if not a:
        raise ValueError(f"unknown action '{name}'")
    if "project" in a["params"] and params.get("project") not in _proj_names():
        raise ValueError(f"unknown project '{params.get('project')}'")
    return a, a["build"](params)


# ── job runner ────────────────────────────────────────────────────────────────
JOBS = {}
JOB_SEQ = [0]
LOCK = threading.Lock()


def start_job(name, argv, cwd, detach=False):
    with LOCK:
        JOB_SEQ[0] += 1
        jid = JOB_SEQ[0]
        JOBS[jid] = {"id": jid, "action": name, "cmd": argv, "status": "running",
                     "output": [], "rc": None, "started": time.time()}
    threading.Thread(target=_run, args=(jid, argv, cwd, detach), daemon=True).start()
    return jid


def _append(jid, line):
    with LOCK:
        JOBS[jid]["output"].append(line.rstrip("\n"))
        JOBS[jid]["output"] = JOBS[jid]["output"][-600:]  # cap


def _run(jid, argv, cwd, detach):
    try:
        if detach:
            subprocess.Popen(argv, cwd=cwd, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
            _append(jid, "launched (detached): " + " ".join(os.path.basename(x) for x in argv[:1]) + " …")
            with LOCK:
                JOBS[jid]["status"] = "done"; JOBS[jid]["rc"] = 0
            return
        proc = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
        for line in proc.stdout:
            _append(jid, line)
        proc.wait()
        with LOCK:
            JOBS[jid]["status"] = "done" if proc.returncode == 0 else "failed"
            JOBS[jid]["rc"] = proc.returncode
    except Exception as e:
        _append(jid, f"error: {e}")
        with LOCK:
            JOBS[jid]["status"] = "failed"; JOBS[jid]["rc"] = -1


def jobs_json():
    with LOCK:
        return [{"id": j["id"], "action": j["action"], "status": j["status"], "rc": j["rc"],
                 "started": j["started"]} for j in sorted(JOBS.values(), key=lambda x: -x["id"])[:12]]


def job_detail(jid):
    with LOCK:
        j = JOBS.get(jid)
        if not j:
            return None
        return {"id": j["id"], "action": j["action"], "cmd": " ".join(j["cmd"]),
                "status": j["status"], "rc": j["rc"], "output": list(j["output"])}


# ── engagement mutations (drive tools/engage.py — one source of truth) ────────
def engage_mut(body):
    op = body.get("op")
    if op == "ingest":
        argv = eng("ingest", body["source"])
        if body.get("session"):
            argv += ["--session", body["session"]]
    elif op == "skip":
        argv = eng("skip", body["id"]) + (["--reason", body["reason"]] if body.get("reason") else [])
    elif op == "respond":
        argv = eng("respond", body["id"])
        if body.get("url"):
            argv += ["--url", body["url"]]
        if body.get("text"):
            argv += ["--text", body["text"]]
    elif op == "check":
        argv = eng("check")
    else:
        raise ValueError(f"unknown engage op '{op}'")
    return start_job("engage-" + op, argv, ROOT)


def engage_upload(name, data):
    """Stash a browser-uploaded exporter JSON and ingest it (raw/ keeps its own copy)."""
    if not data:
        raise ValueError("empty upload")
    json.loads(data)  # must be JSON — catches a wrong-file drop before it hits ingest
    name = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(name)) or "export.json"
    updir = os.path.join(ROOT, "cache", "engage-uploads")
    os.makedirs(updir, exist_ok=True)
    path = os.path.join(updir, f"{int(time.time())}-{name}")
    open(path, "wb").write(data)
    return start_job("engage-ingest", eng("ingest", path), ROOT)


# ── social mutations ──────────────────────────────────────────────────────────
def mark_posted(pid, platform, url, date):
    argv = soc("post", pid)
    if platform:
        argv += ["--platform", platform]
    if url:
        argv += ["--url", url]
    if date:
        argv += ["--date", date]
    return start_job("post " + pid, argv, ROOT)


def edit_post(pid, body=None, fields=None):
    """Rewrite a queued post's body and/or frontmatter fields (window/priority/platforms/image)."""
    path = os.path.join(SOCIAL_DIR, "queue", pid + ".md")
    if not os.path.isfile(path):
        raise ValueError(f"no queued post '{pid}'")
    m = re.match(r"^---\n(.*?)\n---\n?(.*)$", open(path, encoding="utf-8").read(), re.S)
    if not m:
        raise ValueError("bad post format")
    fm = m.group(1)
    for k, v in (fields or {}).items():
        if k not in ("window", "priority", "platforms", "image", "occasion"):
            continue
        v = str(v).strip()
        if re.search(rf"(?m)^{re.escape(k)}:.*$", fm):
            fm = re.sub(rf"(?m)^{re.escape(k)}:.*$", f"{k}: {v}", fm, count=1)
        elif v:
            fm = fm.rstrip("\n") + f"\n{k}: {v}"
    new_body = m.group(2).strip("\n") if body is None else body.strip("\n")
    open(path, "w", encoding="utf-8").write("---\n" + fm.strip("\n") + "\n---\n\n" + new_body + "\n")


def pair_image(pid, image_path):
    """Rewrite a queue post's image: ref to `image_path` (surgical, minimal churn)."""
    path = os.path.join(SOCIAL_DIR, "queue", pid + ".md")
    if not os.path.isfile(path):
        raise ValueError(f"no queued post '{pid}'")
    if not (os.path.isfile(image_path) and under_safe_root(image_path)):
        raise ValueError("image not allowed / missing")
    text = open(path, encoding="utf-8").read()
    if re.search(r"^image:.*$", text, re.M):
        text = re.sub(r"^image:.*$", "image: " + image_path, text, count=1, flags=re.M)
    else:  # insert before the closing frontmatter fence
        text = re.sub(r"\n---\n", "\nimage: " + image_path + "\n---\n", text, count=1)
    open(path, "w", encoding="utf-8").write(text)


# ── thumbnails ────────────────────────────────────────────────────────────────
def make_thumb(path, w):
    """Return (bytes, 'image/jpeg') for an image or a video poster frame; cached."""
    ap = os.path.realpath(path)
    if not (os.path.isfile(ap) and under_safe_root(ap)):
        return None
    os.makedirs(THUMB_CACHE, exist_ok=True)
    key = hashlib.sha1(f"{ap}:{os.path.getmtime(ap)}:{w}".encode()).hexdigest()[:16]
    cache = os.path.join(THUMB_CACHE, key + ".jpg")
    if os.path.exists(cache):
        return open(cache, "rb").read(), "image/jpeg"
    k = kind_of(ap)
    data = None
    try:
        from PIL import Image
        if k == "image":
            im = Image.open(ap).convert("RGB")
            im.thumbnail((w, w * 4))
            buf = io.BytesIO(); im.save(buf, "JPEG", quality=82); data = buf.getvalue()
        elif k == "video":
            tmp = cache + ".src.jpg"
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-ss", "1", "-i", ap,
                            "-frames:v", "1", "-vf", f"scale={w}:-2", tmp],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
            if os.path.exists(tmp):
                im = Image.open(tmp).convert("RGB")
                buf = io.BytesIO(); im.save(buf, "JPEG", quality=82); data = buf.getvalue()
                os.remove(tmp)
    except Exception:
        return None
    if data:
        open(cache, "wb").write(data)
    return (data, "image/jpeg") if data else None


# ── image-requests (the "art queue": GPT-image briefs the owner fulfils by drop) ──
IMGREQ_DIR = os.path.join(SOCIAL_DIR, "image-requests")
IMGREQ_KEYS = ["id", "category", "status", "priority", "for-post", "image", "requested"]


def imgreq_write(rid, meta, brief):
    """Write/overwrite an image-request .md (same frontmatter shape as a post)."""
    os.makedirs(IMGREQ_DIR, exist_ok=True)
    meta = {**meta, "id": rid}
    lines = [f"{k}: {meta[k]}" for k in IMGREQ_KEYS if str(meta.get(k, "")).strip()]
    open(os.path.join(IMGREQ_DIR, rid + ".md"), "w", encoding="utf-8").write(
        "---\n" + "\n".join(lines) + "\n---\n\n" + (brief or "").strip() + "\n")


def imgreq_get(rid):
    path = os.path.join(IMGREQ_DIR, rid + ".md")
    return social.parse_post(path) if rid and os.path.isfile(path) else None


def imgreq_state():
    """The art queue: briefs to gen (status wanted) + the fulfilled ones (done),
    plus queue posts still asking for art that no request covers yet."""
    try:
        files = [f for f in sorted(os.listdir(IMGREQ_DIR)) if f.endswith(".md")] \
            if os.path.isdir(IMGREQ_DIR) else []
        reqs = [social.parse_post(os.path.join(IMGREQ_DIR, f)) for f in files]
    except Exception as e:
        return {"error": str(e), "wanted": [], "done": [], "orphan_posts": []}
    linked = set()

    def row(r):
        m = r["meta"]
        fp = m.get("for-post", "")
        if fp:
            linked.add(fp)
        ref = m.get("image", "")
        resolved = resolve_ref(ref) if ref and ref not in ("none", "wanted") else None
        return {"id": r["id"], "category": m.get("category", ""), "status": m.get("status", "wanted"),
                "priority": m.get("priority", "normal"), "for_post": fp, "requested": m.get("requested", ""),
                "brief": r["body"], "image_abs": resolved or "",
                "image_ok": bool(resolved and os.path.exists(resolved))}

    rows = [row(r) for r in reqs]
    orphans = []
    try:
        for p in social.load_all(SOCIAL_DIR)["queue"]:
            if p["meta"].get("image", "none") == "wanted" and p["id"] not in linked:
                orphans.append({"id": p["id"], "prompt": p["meta"].get("image-prompt", ""),
                                "platforms": p["platforms"]})
    except Exception:
        pass
    return {"wanted": [r for r in rows if r["status"] != "done"],
            "done": [r for r in rows if r["status"] == "done"], "orphan_posts": orphans}


def imgreq_fulfil(rid, name, data):
    """Save a dropped image into the gen library, mark the request done, auto-pair its post."""
    r = imgreq_get(rid)
    if not r:
        raise ValueError(f"no image-request '{rid}'")
    if not data:
        raise ValueError("empty file")
    ext = os.path.splitext(name)[1].lower()
    if ext not in IMG_EXT:
        raise ValueError(f"not an image ({ext or 'no extension'})")
    os.makedirs(SOCIAL_IMG, exist_ok=True)
    fname, n = rid + ext, 2
    while os.path.exists(os.path.join(SOCIAL_IMG, fname)):
        fname = f"{rid}-{n}{ext}"; n += 1
    dest = os.path.join(SOCIAL_IMG, fname)
    open(dest, "wb").write(data)
    imgreq_write(rid, {**r["meta"], "status": "done", "image": dest}, r["body"])
    paired = None
    fp = r["meta"].get("for-post", "")
    if fp and os.path.isfile(os.path.join(SOCIAL_DIR, "queue", fp + ".md")):
        try:
            pair_image(fp, dest); paired = fp
        except Exception:
            pass
    return {"ok": True, "image": dest, "paired": paired}


def imgreq_mut(body):
    op = body.get("op")
    if op == "add":
        base = re.sub(r"[^a-z0-9-]+", "-", (body.get("id") or body.get("brief", "")[:40]).lower()).strip("-")[:48]
        rid = base or f"req-{int(time.time())}"
        if imgreq_get(rid):
            rid = f"{rid}-{int(time.time())}"
        imgreq_write(rid, {"category": (body.get("category") or "stockpile").strip(), "status": "wanted",
                           "priority": body.get("priority", "normal"), "for-post": (body.get("for_post") or "").strip(),
                           "requested": str(datetime.date.today())}, body.get("brief", ""))
        return {"ok": True, "id": rid}
    r = imgreq_get(body.get("id", ""))
    if not r:
        raise ValueError("no such request")
    if op == "reopen":
        imgreq_write(r["id"], {**r["meta"], "status": "wanted", "image": ""}, r["body"])
    elif op == "delete":
        os.remove(r["path"])
    elif op == "done":  # point at an existing gen without a fresh upload
        img = body.get("image", "")
        if not (img and under_safe_root(img) and os.path.isfile(img)):
            raise ValueError("image not allowed / missing")
        imgreq_write(r["id"], {**r["meta"], "status": "done", "image": img}, r["body"])
    else:
        raise ValueError(f"unknown op '{op}'")
    return {"ok": True}


# ── HTTP ──────────────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # quiet
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/" or u.path == "/index.html":
            return self._send(200, PAGE, "text/html; charset=utf-8")
        if u.path == "/healthz":
            return self._send(200, "ok", "text/plain")
        if u.path == "/favicon.ico":
            self.send_response(204); self.end_headers(); return
        if u.path == "/api/state":
            today = social.pdate(q["date"][0]) if "date" in q else datetime.date.today()
            state = {"today": str(today), "social": social_state(today), "engage": engage_state(),
                     "projects": projects_state(), "imgreq": imgreq_state(),
                     "actions": {k: {"label": v["label"], "group": v["group"], "params": v["params"],
                                     "note": v.get("note", ""), "confirm": v.get("confirm", False)}
                                 for k, v in ACTIONS.items()},
                     "env": {"feed": http_ok(FEED_HEALTH), "lame": tcp_ok(LAME_HOST, 22),
                             "editor": os.path.exists(EDITOR_EXE), "thumbtool": os.path.exists(THUMB_EXE)},
                     "jobs": jobs_json()}
            return self._send(200, state)
        if u.path == "/api/channel":
            return self._send(200, channel_state(force="force" in q))
        if u.path == "/api/job":
            d = job_detail(int(q.get("id", ["0"])[0]))
            return self._send(200 if d else 404, d or {"error": "no job"})
        if u.path == "/api/thumb":
            p = q.get("path", [""])[0]
            w = max(48, min(640, int(q.get("w", ["320"])[0])))
            r = make_thumb(p, w)
            if not r:
                return self._send(404, b"", "image/jpeg")
            data, ct = r
            self.send_response(200)
            self.send_header("Content-Type", ct)
            self.send_header("Cache-Control", "max-age=86400")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if u.path == "/api/file":  # the ORIGINAL bytes, full-res (lightbox / copy), not a thumbnail
            ap = os.path.realpath(q.get("path", [""])[0])
            if not (os.path.isfile(ap) and under_safe_root(ap)):
                return self._send(404, {"error": "not found"})
            data = open(ap, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", mimetypes.guess_type(ap)[0] or "application/octet-stream")
            self.send_header("Cache-Control", "max-age=3600")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        n = int(self.headers.get("Content-Length", 0))
        if u.path == "/api/engage-upload":  # raw file body, not JSON-wrapped
            q = urllib.parse.parse_qs(u.query)
            try:
                jid = engage_upload(q.get("name", ["export.json"])[0], self.rfile.read(n))
                return self._send(200, {"job": jid})
            except Exception as e:
                return self._send(400, {"error": str(e)})
        if u.path == "/api/imgreq-upload":  # raw image body (drag-drop), not JSON-wrapped
            q = urllib.parse.parse_qs(u.query)
            try:
                return self._send(200, imgreq_fulfil(q.get("id", [""])[0],
                                                     q.get("name", ["image.png"])[0], self.rfile.read(n)))
            except Exception as e:
                return self._send(400, {"error": str(e)})
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            return self._send(400, {"error": "bad json"})
        try:
            if u.path == "/api/action":
                a, argv = build_action(body["action"], body.get("params", {}))
                jid = start_job(body["action"], argv, ROOT, detach=a.get("detach", False))
                return self._send(200, {"job": jid})
            if u.path == "/api/post":
                jid = mark_posted(body["id"], body.get("platform"), body.get("url"), body.get("date"))
                return self._send(200, {"job": jid})
            if u.path == "/api/pair":
                pair_image(body["id"], body["image"])
                return self._send(200, {"ok": True})
            if u.path == "/api/edit":
                edit_post(body["id"], body.get("body"), body.get("fields"))
                return self._send(200, {"ok": True})
            if u.path == "/api/engage":
                return self._send(200, {"job": engage_mut(body)})
            if u.path == "/api/imgreq":
                return self._send(200, imgreq_mut(body))
        except Exception as e:
            return self._send(400, {"error": str(e)})
        return self._send(404, {"error": "not found"})


def open_browser(url):
    for launcher in (["explorer.exe"], ["wslview"], ["xdg-open"]):
        if shutil.which(launcher[0]):
            try:
                subprocess.Popen(launcher + [url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--open", action="store_true", help="open the dashboard in a browser")
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    url = f"http://localhost:{a.port}"
    print(f"slopstudio dashboard → {url}   (Ctrl-C to stop)")
    print(f"  social: {SOCIAL_DIR}\n  gens:   {SOCIAL_IMG}")
    if not os.path.isdir(GEMMA):
        print(f"  ⚠ {GEMMA} not found — social section will be empty")
    if a.open:
        open_browser(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


# ── the page (self-contained: inlined CSS + vanilla JS) ───────────────────────
PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>slopstudio · control panel</title>
<style>
:root{--bg:#14141a;--panel:#1c1c24;--panel2:#23232e;--line:#2e2e3a;--ink:#e8e8f0;--dim:#9a9ab0;
--acc:#a86ccf;--good:#5fca7a;--warn:#e6b455;--bad:#e0657a;--chip:#2a2a36;}
*{box-sizing:border-box}
body{margin:0;font:14px/1.45 system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--ink)}
a{color:#c8a6ff}
header{position:sticky;top:0;z-index:5;background:linear-gradient(180deg,#1c1c26,#171720);
border-bottom:1px solid var(--line);padding:10px 16px;display:flex;align-items:center;gap:14px}
header h1{font-size:15px;margin:0;font-weight:650;letter-spacing:.2px}
header .env{margin-left:auto;display:flex;gap:10px;font-size:12px;color:var(--dim)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:4px;vertical-align:1px}
.tabs{display:flex;gap:4px;padding:8px 16px 0}
.tab{padding:7px 14px;border-radius:8px 8px 0 0;cursor:pointer;color:var(--dim);border:1px solid transparent}
.tab.on{background:var(--panel);color:var(--ink);border-color:var(--line);border-bottom-color:var(--panel)}
main{padding:16px 16px 52px;max-width:1180px;margin:0 auto}
.wrap{display:none}.wrap.on{display:block}
h2{font-size:12px;text-transform:uppercase;letter-spacing:.8px;color:var(--dim);margin:22px 0 10px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px}
.row{display:flex;gap:10px;flex-wrap:wrap}
.chip{background:var(--chip);border:1px solid var(--line);border-radius:999px;padding:2px 9px;font-size:12px;color:var(--dim)}
.badge{background:#332a44;border:1px solid #443a5a;color:#d6bcff;border-radius:6px;padding:1px 7px;font-size:11px}
button{font:inherit;background:var(--panel2);color:var(--ink);border:1px solid var(--line);
border-radius:8px;padding:6px 12px;cursor:pointer}
button:hover{border-color:#4a4a5e;background:#2a2a36}
button.pri{background:#4a2f6b;border-color:#5f3d88}button.pri:hover{background:#573781}
button.sm{padding:3px 9px;font-size:12px}
button:disabled{opacity:.45;cursor:default}
select,input[type=text],input[type=number]{font:inherit;background:#15151c;color:var(--ink);
border:1px solid var(--line);border-radius:7px;padding:5px 8px}
label.opt{display:inline-flex;align-items:center;gap:5px;color:var(--dim);font-size:13px}
/* platform cadence */
.plats{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:10px}
.plat{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px 12px}
.plat .st{float:right;font-size:11px;font-weight:700;padding:1px 8px;border-radius:6px}
.st.DUE{background:#4a2530;color:var(--bad)}.st.ok{background:#233a2a;color:var(--good)}
.plat h3{margin:0 0 2px;font-size:14px}.plat .meta{color:var(--dim);font-size:12px}
.low{color:var(--warn);font-size:12px;margin-top:4px}
/* posts */
.posts{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:12px}
.post{display:flex;gap:10px}
.post .thumb{width:92px;height:92px;flex:none;border-radius:8px;background:#0e0e14 center/cover no-repeat;
border:1px solid var(--line);display:flex;align-items:center;justify-content:center;color:#55556a;font-size:11px;cursor:pointer}
.post .body{flex:1;min-width:0}
.post .id{font-weight:650;font-size:13px}
.post .txt{color:#c9c9d6;font-size:12.5px;margin:5px 0;white-space:pre-wrap;max-height:4.4em;overflow:hidden}
.post .txt.full{max-height:none}
.post .acts{display:flex;gap:6px;flex-wrap:wrap;margin-top:6px}
.star{color:var(--warn)}
.miss{color:var(--bad);font-size:11px}
.grp{color:var(--dim);font-size:11px;margin:14px 0 6px;text-transform:uppercase;letter-spacing:.6px}
/* gallery */
.gal{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:10px}
.gi{border:1px solid var(--line);border-radius:9px;overflow:hidden;background:var(--panel);cursor:pointer;position:relative}
.gi img{width:100%;height:112px;object-fit:cover;display:block;background:#0e0e14}
.gi .cap{font-size:10.5px;color:var(--dim);padding:4px 6px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.gi .tag{position:absolute;top:5px;left:5px;font-size:10px;padding:1px 6px;border-radius:5px}
/* engage: suggested replies */
.sug{background:#15151c;border:1px solid var(--line);border-radius:7px;padding:6px 9px;margin-top:5px;
font-size:12.5px;cursor:pointer;white-space:pre-wrap}
.sug:hover{border-color:var(--acc)}
.tag.used{background:#233a2a;color:var(--good)}.tag.free{background:#3a3320;color:var(--warn)}
/* image queue: per-card drop targets */
.post .drop{width:92px;height:92px;flex:none;border-radius:8px;border:1.5px dashed #3a3a48;
background:#0e0e14;display:flex;align-items:center;justify-content:center;text-align:center;
color:#6a6a80;font-size:11px;line-height:1.25;cursor:pointer}
.post .drop:hover{border-color:#4a4a5e}
.post .drop.over{border-color:var(--acc);color:var(--acc);background:#1a1422}
/* launcher */
.acts{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:10px}
.act{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px}
.act h4{margin:0 0 8px;font-size:14px}
.act .opts{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:10px}
/* projects */
.proj{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px;margin-bottom:10px}
.proj h3{margin:0 0 8px}
.check{display:flex;gap:16px;flex-wrap:wrap;font-size:13px}
.check .y{color:var(--good)}.check .n{color:var(--dim)}
/* channel */
.chanhead{display:flex;align-items:center;gap:14px}
.avatar{width:56px;height:56px;border-radius:50%;border:1px solid var(--line)}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-top:12px}
.stat{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:18px;text-align:center}
.stat .num{font-size:26px;font-weight:700}.stat .lbl{color:var(--dim);font-size:12px;margin-top:3px}
code{background:#0e0e14;border:1px solid var(--line);border-radius:4px;padding:1px 5px;font-size:12px}
/* log drawer */
#log{position:fixed;left:0;right:0;bottom:0;background:#0d0d12;border-top:1px solid var(--line);
max-height:42vh;display:flex;flex-direction:column;transform:translateY(calc(100% - 34px));transition:.2s}
#log.up{transform:translateY(0)}
#log .bar{padding:7px 14px;display:flex;gap:10px;align-items:center;cursor:pointer;border-bottom:1px solid var(--line)}
#log .bar .s{font-size:12px;color:var(--dim)}
#log pre{margin:0;padding:10px 14px;overflow:auto;font:12px/1.4 ui-monospace,Menlo,monospace;color:#cfe6d0;flex:1}
.spin{width:9px;height:9px;border:2px solid #55556a;border-top-color:#c8a6ff;border-radius:50%;
display:inline-block;animation:sp .7s linear infinite}@keyframes sp{to{transform:rotate(360deg)}}
/* modal */
#modal{position:fixed;inset:0;background:#000a;display:none;align-items:center;justify-content:center;z-index:20}
#modal.on{display:flex}
#modal .box{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:18px;max-width:560px;width:92%}
#modal img{max-width:100%;max-height:70vh;border-radius:8px;display:block;margin:auto}
.muted{color:var(--dim)}
.hidden{display:none}
</style></head><body>
<header>
  <h1>slopstudio · control panel</h1>
  <div class="env" id="env"></div>
  <button class="sm" onclick="load()">⟳ refresh</button>
</header>
<div class="tabs" id="tabs"></div>
<main>
  <div class="wrap" id="w-social"></div>
  <div class="wrap" id="w-imgreq"></div>
  <div class="wrap" id="w-engage"></div>
  <div class="wrap" id="w-launcher"></div>
  <div class="wrap" id="w-projects"></div>
  <div class="wrap" id="w-channel"></div>
</main>

<div id="log"><div class="bar" onclick="toggleLog()"><b>log</b><span class="s" id="logstat">idle</span>
  <span style="margin-left:auto" class="s">click to toggle</span></div><pre id="logout"></pre></div>

<div id="modal"><div class="box" id="modalbox"></div></div>

<script>
let S=null, TAB='social', curJob=null, poll=null;
const $=id=>document.getElementById(id);
const esc=s=>(s||'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const thumb=(p,w=200)=>'/api/thumb?w='+w+'&path='+encodeURIComponent(p);

async function load(){
  const r=await fetch('/api/state'); S=await r.json();
  renderEnv(); renderTabs(); renderSocial(); renderImages(); renderEngage(); renderLauncher(); renderProjects(); renderJobs();
}
function renderEnv(){
  const e=S.env, d=(on,l)=>`<span><span class="dot" style="background:${on?'var(--good)':'var(--bad)'}"></span>${l}</span>`;
  $('env').innerHTML = `today ${S.today} · `+d(e.feed,'feed')+d(e.lame,'lame')+d(e.editor,'editor')+d(e.thumbtool,'thumbtool');
}
function renderTabs(){
  const tabs=[['social','Social'],['imgreq','Image Queue'],['engage','Engage'],['launcher','Launcher'],['projects','Projects'],['channel','Channel']];
  $('tabs').innerHTML=tabs.map(([k,l])=>`<div class="tab ${k==TAB?'on':''}" onclick="setTab('${k}')">${l}</div>`).join('');
  for(const [k] of tabs) $('w-'+k).classList.toggle('on',k==TAB);
}
function setTab(k){TAB=k;renderTabs();if(k=='channel')renderChannel()}

/* ---------- SOCIAL ---------- */
function renderSocial(){
  const s=S.social; if(s.error){$('w-social').innerHTML='<div class="card">social error: '+esc(s.error)+'</div>';return}
  let h='<h2>platform cadence</h2><div class="plats">';
  for(const p of s.platforms){
    h+=`<div class="plat"><span class="st ${p.due?'DUE':'ok'}">${p.due?'DUE':'ok'}</span>
      <h3>${esc(p.name)}</h3>
      <div class="meta">${esc(p.handle||'')}</div>
      <div class="meta">every ${p.cadence_days}d · ${p.last_posted?('last '+p.last_posted+' ('+p.days_since+'d)'):'never posted'} · ${p.queue_depth} queued</div>
      ${p.queue_depth<3?'<div class="low">⚠ queue low — refill</div>':''}</div>`;
  }
  h+='</div>';
  if(s.suggestions.length){
    h+='<h2>post today</h2><div class="row">'+s.suggestions.map(id=>`<button class="sm pri" onclick="jump('${id}')">${esc(id)}</button>`).join('')+'</div>';
  }
  if(s.stale.length){h+='<div class="card" style="margin-top:12px;border-color:#5a3540">⚠ posts with a missing image/video: '+s.stale.map(esc).join(', ')+'</div>';}

  // queue grouped by window state
  const order=[['open','ready'],['upcoming','upcoming'],['manual','waiting on a trigger'],['expired','expired']];
  h+='<h2>queue ('+s.queue.length+')</h2>';
  for(const [st,lbl] of order){
    const g=s.queue.filter(p=>p.window_state==st); if(!g.length)continue;
    h+=`<div class="grp">${lbl} · ${g.length}</div><div class="posts">`+g.map(postCard).join('')+'</div>';
  }
  // gallery
  h+='<h2>image gens ('+s.images.length+') · '+s.images.filter(i=>!i.paired.length).length+' unpaired</h2>';
  h+='<div class="gal">'+s.images.map(galItem).join('')+'</div>';
  $('w-social').innerHTML=h;
}
function postCard(p){
  const img=(p.image_kind=='image'||p.image_kind=='video')&&p.image_ok;
  const thumbEl = img
    ? `<div class="thumb" style="background-image:url('${thumb(p.image_abs,200)}')" onclick="lightbox('${encodeURIComponent(p.image_abs)}')"></div>`
    : `<div class="thumb" onclick="pairPick('${p.id}')">${p.image_kind=='wanted'?'🖼 wanted':p.image_kind=='none'?'+ pair':'<span class=miss>missing</span>'}</div>`;
  return `<div class="post card"><div id="post-${p.id}"></div>${thumbEl}
    <div class="body">
      <div class="id">${p.priority=='high'?'<span class=star>★</span> ':''}${esc(p.id)}</div>
      <div class="row" style="gap:5px;margin:3px 0">${p.platforms.map(x=>`<span class="badge">${esc(x)}</span>`).join('')}
        <span class="chip">${esc(p.window)}</span></div>
      <div class="txt" id="txt-${p.id}">${esc(p.body)}</div>
      <div class="acts">
        <button class="sm" onclick="toggleFull('${p.id}')">show</button>
        <button class="sm" onclick="copyBody('${p.id}')">copy</button>
        <button class="sm" onclick="editPost('${p.id}')">edit</button>
        <button class="sm pri" onclick="postForm('${p.id}')">mark posted</button>
        <button class="sm" onclick="pairPick('${p.id}')">${img?'change img':'pair img'}</button>
      </div>
      <div id="form-${p.id}"></div>
    </div></div>`;
}
function galItem(i){
  const used=i.paired.length;
  return `<div class="gi" onclick="galClick('${esc(i.name)}','${encodeURIComponent(i.path)}')">
    <span class="tag ${used?'used':'free'}">${used?esc(i.paired[0])+(i.paired.length>1?' +'+(i.paired.length-1):''):'unpaired'}</span>
    <img loading="lazy" src="${thumb(i.path,240)}"><div class="cap">${esc(i.name.replace(/ ChatGPT Image.*/,''))||esc(i.name)}</div></div>`;
}
const bodyOf=id=>{const p=S.social.queue.find(x=>x.id==id)||S.social.posted.find(x=>x.id==id);return p?p.body:''};
function toggleFull(id){$('txt-'+id).classList.toggle('full')}
function copyBody(id){navigator.clipboard.writeText(bodyOf(id));flash('copied '+id)}
function jump(id){setTab('social');const el=$('post-'+id);if(el)el.scrollIntoView({behavior:'smooth',block:'center'});postForm(id)}
function editPost(id){
  const p=S.social.queue.find(x=>x.id==id); const f=$('form-'+id); if(!p)return;
  if($('ed-body-'+id)){f.innerHTML='';return}   // toggle off
  f.innerHTML=`<div class="card" style="margin-top:8px">
    <div class="muted" style="font-size:11px;margin-bottom:4px">edit body + fields (writes ${esc(id)}.md)</div>
    <textarea id="ed-body-${id}" style="width:100%;min-height:130px;font:13px/1.4 ui-monospace,monospace;
      background:#15151c;color:var(--ink);border:1px solid var(--line);border-radius:7px;padding:8px">${esc(p.body)}</textarea>
    <div class="opts" style="margin-top:8px">
      <label class="opt">platforms <input type="text" id="ed-plat-${id}" value="${esc(p.platforms.join(', '))}" size="16"></label>
      <label class="opt">window <input type="text" id="ed-win-${id}" value="${esc(p.window)}" size="16"></label>
      <label class="opt">priority <select id="ed-pri-${id}">
        <option ${p.priority=='high'?'selected':''}>high</option>
        <option ${p.priority!='high'?'selected':''}>normal</option></select></label>
    </div>
    <button class="sm pri" onclick="saveEdit('${id}')">save</button>
    <button class="sm" onclick="editPost('${id}')">cancel</button></div>`;
}
async function saveEdit(id){
  const fields={platforms:$('ed-plat-'+id).value.split(',').map(s=>s.trim()).filter(Boolean).join(', '),
                window:$('ed-win-'+id).value.trim(), priority:$('ed-pri-'+id).value};
  const r=await fetch('/api/edit',{method:'POST',body:JSON.stringify({id,body:$('ed-body-'+id).value,fields})});
  const j=await r.json(); if(j.error){flash('✗ '+j.error);return}
  flash('saved '+id); load();
}
function postForm(id){
  const p=S.social.queue.find(x=>x.id==id); const f=$('form-'+id); if(!p)return;
  if(f.innerHTML){f.innerHTML='';return}
  f.innerHTML=`<div class="card" style="margin-top:8px">
    <div class="opts"><label class="opt">platform
      <select id="pf-plat-${id}"><option value="">all (${p.platforms.join(',')})</option>
      ${p.platforms.map(x=>`<option>${x}</option>`).join('')}</select></label>
      <label class="opt">url <input type="text" id="pf-url-${id}" placeholder="posted link" size="18"></label>
      <label class="opt">date <input type="text" id="pf-date-${id}" placeholder="${S.today}" size="10"></label></div>
    <button class="sm pri" onclick="doPost('${id}')">confirm posted → moves to posted/</button>
    <button class="sm" onclick="$('form-${id}').innerHTML=''">cancel</button></div>`;
}
async function doPost(id){
  const plat=$('pf-plat-'+id).value, url=$('pf-url-'+id).value, date=$('pf-date-'+id).value;
  const r=await fetch('/api/post',{method:'POST',body:JSON.stringify({id,platform:plat||null,url:url||null,date:date||null})});
  const j=await r.json(); if(j.job){watch(j.job)} flash('posting '+id); setTimeout(load,800);
}
function pairPick(id){
  const imgs=S.social.images;
  const opts=imgs.map((i,x)=>`<option value="${x}">${i.paired.length?'●':'○'} ${esc(i.name)}</option>`).join('');
  modal(`<h3>pair an image → ${esc(id)}</h3>
    <select id="pair-sel" size="12" style="width:100%">${opts}</select>
    <div class="row" style="margin-top:12px"><button class="pri" onclick="doPair('${id}')">pair</button>
    <button onclick="closeModal()">cancel</button></div>`);
}
async function doPair(id){
  const x=$('pair-sel').value; const img=S.social.images[x]; if(!img)return;
  await fetch('/api/pair',{method:'POST',body:JSON.stringify({id,image:img.path})});
  closeModal(); flash('paired '+id); load();
}
function galClick(name,path){ lightbox(path); }

/* ---------- IMAGE QUEUE (art briefs → drop the finished gen to fulfil) ---------- */
function renderImages(){
  const q=S.imgreq||{};
  if(q.error){$('w-imgreq').innerHTML='<div class="card">image-queue error: '+esc(q.error)+'</div>';return}
  const wanted=q.wanted||[], done=q.done||[], orphans=q.orphan_posts||[];
  let h=`<div class="card"><div class="row" style="align-items:center">
    <span class="chip">wanted ${wanted.length}</span><span class="chip">done ${done.length}</span>
    <input type="text" id="ir-brief" placeholder="new image brief — the GPT prompt / art direction" style="flex:1;min-width:280px">
    <input type="text" id="ir-cat" placeholder="category" size="9">
    <input type="text" id="ir-for" placeholder="for-post id (optional)" size="15">
    <button class="sm pri" onclick="imgreqAdd()">+ queue</button></div>
    <div class="muted" style="font-size:12px;margin-top:6px">Queue what you want GPT to draw, then gen it and
    <b>drag the finished image onto its card</b> (or click the tile to browse). It saves into the gen library,
    flips to ✓ done, and auto-pairs to its post when one is linked.</div></div>`;
  if(orphans.length){
    h+='<h2>posts waiting on art — no request yet</h2><div class="row">'
      +orphans.map(o=>`<button class="sm" onclick="imgreqFromPost('${esc(o.id)}')">+ ${esc(o.id)}</button>`).join('')+'</div>';
  }
  h+='<h2>to generate ('+wanted.length+')</h2>';
  h+=wanted.length?('<div class="posts">'+wanted.map(reqCard).join('')+'</div>')
    :'<div class="card muted">nothing queued — add a brief above</div>';
  if(done.length){
    h+='<h2>done ('+done.length+')</h2><div class="gal">'+done.map(doneItem).join('')+'</div>';
  }
  $('w-imgreq').innerHTML=h;
}
function reqCard(r){
  return `<div class="post card">
    <div class="drop" ondragover="event.preventDefault();this.classList.add('over')"
      ondragleave="this.classList.remove('over')" ondrop="imgreqDrop(event,'${esc(r.id)}')"
      onclick="$('irf-${esc(r.id)}').click()" title="drop the finished image, or click to browse">⬇ drop<br>image</div>
    <input type="file" id="irf-${esc(r.id)}" accept="image/*" class="hidden"
      onchange="imgreqUpload('${esc(r.id)}',this.files[0]);this.value=''">
    <div class="body">
      <div class="id">${r.priority=='high'?'<span class=star>★</span> ':''}${esc(r.id)}</div>
      <div class="row" style="gap:5px;margin:3px 0"><span class="chip">${esc(r.category||'—')}</span>
        ${r.for_post?`<span class="badge" title="auto-pairs to this post when fulfilled">↪ ${esc(r.for_post)}</span>`:''}</div>
      <div class="txt full" id="irb-${esc(r.id)}">${esc(r.brief)}</div>
      <div class="acts">
        <button class="sm" onclick="copyEl('irb-${esc(r.id)}')">copy prompt</button>
        <button class="sm" onclick="imgreqOp({op:'delete',id:'${esc(r.id)}'})">delete</button>
      </div>
    </div></div>`;
}
function doneItem(r){
  return `<div class="gi">
    <span class="tag ${r.for_post?'used':'free'}">${r.for_post?esc(r.for_post):'✓ done'}</span>
    ${r.image_ok?`<img loading="lazy" src="${thumb(r.image_abs,240)}" onclick="lightbox('${encodeURIComponent(r.image_abs)}')">`
      :'<div class="miss" style="height:112px;display:flex;align-items:center;justify-content:center">missing</div>'}
    <div class="cap">${esc(r.id)}</div>
    <div style="padding:0 6px 6px"><button class="sm" onclick="imgreqOp({op:'reopen',id:'${esc(r.id)}'})">reopen</button></div></div>`;
}
async function imgreqUpload(id,f){
  if(!f)return;
  const r=await fetch('/api/imgreq-upload?id='+encodeURIComponent(id)+'&name='+encodeURIComponent(f.name),{method:'POST',body:f});
  const j=await r.json(); if(j.error){flash('✗ '+j.error);return}
  flash('✓ '+id+(j.paired?(' → paired to '+j.paired):' → saved to gens')); load();
}
function imgreqDrop(ev,id){ev.preventDefault();ev.currentTarget.classList.remove('over');
  const f=ev.dataTransfer.files[0]; if(f)imgreqUpload(id,f);}
async function imgreqOp(body){
  if(body.op=='delete'&&!confirm('delete request '+body.id+'?'))return;
  const r=await fetch('/api/imgreq',{method:'POST',body:JSON.stringify(body)});
  const j=await r.json(); if(j.error){flash('✗ '+j.error);return} load();
}
function imgreqAdd(){
  const brief=$('ir-brief').value.trim(); if(!brief){flash('write a brief first');return}
  imgreqOp({op:'add',brief,category:$('ir-cat').value.trim(),for_post:$('ir-for').value.trim()});
}
function imgreqFromPost(id){
  const o=(S.imgreq.orphan_posts||[]).find(x=>x.id==id);
  $('ir-brief').value=(o&&o.prompt)||('art for '+id); $('ir-for').value=id; $('ir-cat').value='post';
  $('ir-brief').scrollIntoView({behavior:'smooth',block:'center'}); $('ir-brief').focus();
}
function copyEl(id){const el=$(id); if(!el)return; navigator.clipboard.writeText(el.innerText); flash('copied prompt')}

/* ---------- ENGAGE (tools/engage.py — comment-section presence) ---------- */
function renderEngage(){
  const e=S.engage;
  if(e.error){$('w-engage').innerHTML='<div class="card">engage error: '+esc(e.error)+'</div>';return}
  const flagged=e.pending.filter(c=>c.flagged), ok=e.pending.filter(c=>!c.flagged);
  let h=`<div class="card" id="eng-drop" ondragover="event.preventDefault();this.style.borderColor='var(--acc)'"
    ondragleave="this.style.borderColor=''" ondrop="engDrop(event)"><div class="row" style="align-items:center">
    <span class="chip">pending ${ok.length}</span>
    <span class="chip">⚠ flagged ${flagged.length}</span>
    <span class="chip">engaged ${e.engaged.length}</span>
    <span class="chip">skipped ${e.skipped}</span>
    <input type="text" id="eng-src" placeholder="YouTube/reddit/HN URL or exporter .json path" style="flex:1;min-width:260px">
    <button class="sm pri" onclick="engIngest()">capture</button>
    <button class="sm pri" onclick="$('eng-file').click()">upload .json</button>
    <input type="file" id="eng-file" accept=".json,application/json" class="hidden" onchange="engUpload(this.files[0]);this.value=''">
    <button class="sm" onclick="engOp({op:'check'})">⟳ check reply metrics</button></div>
    <div class="muted" style="font-size:12px;margin-top:6px">tweets: browse with the twitter-web-exporter
    userscript → export JSON → <b>upload / drop the file anywhere on this box</b>. Triage + reply drafting:
    ask Claude (engaging-gemma skill). Hard rule: ⚠ flagged = drama/politics — skip, never argue.</div></div>`;
  if(ok.length){h+='<h2>pending ('+ok.length+')</h2><div class="posts">'+ok.map(engCard).join('')+'</div>'}
  if(flagged.length){h+='<h2>⚠ flagged — skip these ('+flagged.length+')</h2><div class="posts">'+flagged.map(engCard).join('')+'</div>'}
  if(!e.pending.length){h+='<div class="card muted" style="margin-top:12px">nothing pending — capture something above</div>'}
  if(e.engaged.length){
    h+='<h2>engaged (latest '+Math.min(e.engaged.length,12)+' of '+e.engaged.length+')</h2><div class="posts">'
      +e.engaged.slice(0,12).map(engCard).join('')+'</div>';
  }
  $('w-engage').innerHTML=h;
}
function engCard(c){
  const done=!!c.responded;
  const perf=done&&c.likes?` · our reply <b>+${esc(c.likes)}</b>${c.replies?' / '+esc(c.replies)+' replies':''}`:'';
  return `<div class="post card"><div class="body">
    <div class="id">${c.flagged?'<span class="miss">⚠ '+esc(c.safety)+'</span> ':''}${esc(c.id)}</div>
    <div class="row" style="gap:5px;margin:3px 0"><span class="badge">${esc(c.platform)}</span>
      <span class="chip">${esc(c.author)}</span><span class="chip">${esc(done?('engaged '+c.responded):c.captured)}</span>
      ${!done&&(c.suggestions||[]).length?'<span class="chip" style="color:var(--acc)">✎ '+c.suggestions.length+' draft'+(c.suggestions.length>1?'s':'')+'</span>':''}</div>
    <div class="muted" style="font-size:12px">${esc(c.stats)}${perf}</div>
    <div class="txt" id="etxt-${c.id}">${esc(c.body)}</div>
    ${!done&&(c.suggestions||[]).length?'<div class="muted" style="font-size:11px;margin-top:6px">suggested replies — click to copy</div>'
      +c.suggestions.map((s,i)=>`<div class="sug" onclick="copySug('${c.id}',${i})">${
        s.reply_to?'<span class="badge" title="post this as a reply to this comment, not on the video/post itself">↪ '+esc(s.reply_to)+'</span> ':''
      }${esc(s.text)}</div>`).join(''):''}
    <div class="acts">
      <button class="sm" onclick="$('etxt-${c.id}').classList.toggle('full')">show</button>
      <a href="${esc(c.url)}" target="_blank"><button class="sm">open ↗</button></a>
      ${done?(c.response_url?`<a href="${esc(c.response_url)}" target="_blank"><button class="sm">our reply ↗</button></a>`:'')
        :`<button class="sm pri" onclick="engRespondForm('${c.id}')">mark engaged</button>
          <button class="sm" onclick="engOp({op:'skip',id:'${c.id}'${c.flagged?",reason:'safety'":''}})">skip${c.flagged?' (safety)':''}</button>`}
    </div><div id="eform-${c.id}"></div></div></div>`;
}
let lastSug={};
function copySug(id,i){
  const c=S.engage.pending.concat(S.engage.engaged).find(x=>x.id==id); if(!c)return;
  const s=c.suggestions[i], txt=s.text||s;   // copies only the reply text, never the ↪ marker
  navigator.clipboard.writeText(txt); lastSug[id]=txt;
  flash(s.reply_to?('copied — post as a reply to '+s.reply_to+', then mark engaged'):'copied — post it, then mark engaged (text prefilled)');
  const t=$('er-text-'+id); if(t)t.value=txt;
}
function engRespondForm(id){
  const f=$('eform-'+id); if(f.innerHTML){f.innerHTML='';return}
  f.innerHTML=`<div class="card" style="margin-top:8px">
    <div class="muted" style="font-size:11px;margin-bottom:4px">what Gemma said + where (YouTube: use the
    comment permalink with &amp;lc= so <i>check</i> can track likes)</div>
    <textarea id="er-text-${id}" placeholder="the reply as posted" style="width:100%;min-height:70px;font:13px/1.4 ui-monospace,monospace;
      background:#15151c;color:var(--ink);border:1px solid var(--line);border-radius:7px;padding:8px">${esc(lastSug[id]||'')}</textarea>
    <div class="opts" style="margin-top:8px"><label class="opt">url <input type="text" id="er-url-${id}" size="34" placeholder="link to our reply"></label></div>
    <button class="sm pri" onclick="engOp({op:'respond',id:'${id}',url:$('er-url-${id}').value,text:$('er-text-${id}').value})">confirm → engaged/</button>
    <button class="sm" onclick="$('eform-${id}').innerHTML=''">cancel</button></div>`;
}
function engIngest(){
  const src=$('eng-src').value.trim(); if(!src){flash('paste a URL or .json path first');return}
  engOp({op:'ingest',source:src}); openLog();
}
async function engUpload(f){
  if(!f)return;
  const r=await fetch('/api/engage-upload?name='+encodeURIComponent(f.name),{method:'POST',body:f});
  const j=await r.json(); if(j.error){flash('✗ '+j.error);return}
  watch(j.job); openLog();
}
function engDrop(ev){
  ev.preventDefault(); $('eng-drop').style.borderColor='';
  engUpload(ev.dataTransfer.files[0]);
}
async function engOp(body){
  const r=await fetch('/api/engage',{method:'POST',body:JSON.stringify(body)});
  const j=await r.json(); if(j.error){flash('✗ '+j.error);return}
  watch(j.job);
}

/* ---------- LAUNCHER ---------- */
function renderLauncher(){
  const A=S.actions, projs=S.projects.map(p=>p.name);
  const groups={machine:'machine',video:'video authoring',launch:'open apps'};
  let h='';
  for(const g of Object.keys(groups)){
    const items=Object.entries(A).filter(([k,v])=>v.group==g);
    if(!items.length)continue;
    h+=`<h2>${groups[g]}</h2><div class="acts">`;
    for(const [k,v] of items) h+=actCard(k,v,projs);
    h+='</div>';
  }
  $('w-launcher').innerHTML=h;
}
function actCard(k,v,projs){
  let opts='';
  for(const pm of v.params){
    const [nm,spec]=pm.split(':'); const opt=nm.endsWith('?'); const name=nm.replace('?','');
    if(name=='project'){
      opts+=`<label class="opt">project <select id="op-${k}-project">${opt?'<option value="">—</option>':''}${projs.map(p=>`<option>${p}</option>`).join('')}</select></label>`;
    }else if(name=='cut'){
      const cuts=S.projects.flatMap(p=>p.cuts.map(c=>[c.name,c.path]));
      opts+=`<label class="opt">cut <select id="op-${k}-cut">${cuts.map(([n,pa])=>`<option value="${esc(pa)}">${esc(n)}</option>`).join('')}</select></label>`;
    }else if(name=='thumb'){
      const th=S.projects.flatMap(p=>p.thumbs.map(c=>[p.name+'/'+c.name,c.path]));
      opts+=`<label class="opt">thumb <select id="op-${k}-thumb">${th.map(([n,pa])=>`<option value="${esc(pa)}">${esc(n)}</option>`).join('')}</select></label>`;
    }else if(spec&&spec.startsWith('int')){
      const def=spec.split('=')[1]||'';
      opts+=`<label class="opt">${name} <input type="number" id="op-${k}-${name}" value="${def}" style="width:64px"></label>`;
    }else if(spec=='bool'){
      opts+=`<label class="opt"><input type="checkbox" id="op-${k}-${name}"> ${name}</label>`;
    }
  }
  return `<div class="act"><h4>${esc(v.label)}${v.note?` <span class="muted" style="font-size:11px">(${esc(v.note)})</span>`:''}</h4>
    <div class="opts">${opts||'<span class="muted">no options</span>'}</div>
    <button class="pri" onclick="runAct('${k}')">${v.group=='launch'?'open':'run'}</button></div>`;
}
async function runAct(k){
  const v=S.actions[k]; const params={};
  for(const pm of v.params){
    const [nm,spec]=pm.split(':'); const name=nm.replace('?','');
    const el=$('op-'+k+'-'+name); if(!el)continue;
    params[name]= el.type=='checkbox'?el.checked : el.value;
  }
  if(v.confirm && !confirm(v.label+'?'))return;
  const r=await fetch('/api/action',{method:'POST',body:JSON.stringify({action:k,params})});
  const j=await r.json();
  if(j.error){flash('✗ '+j.error);return}
  watch(j.job); openLog();
}

/* ---------- PROJECTS ---------- */
function renderProjects(){
  let h='';
  for(const p of S.projects){
    const y=x=>x?'<span class="y">✓</span>':'<span class="n">○</span>';
    h+=`<div class="proj"><h3>${esc(p.name)}</h3>
      <div class="check">
        <span>${y(p.skeletons.length)} skeleton</span>
        <span>${y(p.cuts.length)} cut ${p.cuts.length?('· '+p.cuts.length):''}</span>
        <span>${y(p.thumbs.length)} thumbs ${p.thumbs.length?('· '+p.thumbs.length):''}</span>
        <span>${y(p.packaging.length)} packaging</span>
        <span>${y(p.exports.length)} export ${p.exports.length?('· '+p.exports.map(e=>e.mb+'MB').join(', ')):''}</span>
      </div>
      <div class="acts" style="margin-top:10px">
        <button class="sm" onclick="quick('status','project','${p.name}')">status</button>
        <button class="sm" onclick="quick('show','project','${p.name}')">timeline</button>
        <button class="sm" onclick="quick('lint','project','${p.name}')">lint</button>
        ${p.cuts.length?`<button class="sm" title="${esc(p.cuts[0].path)}" onclick="quickCut('${esc(p.cuts[0].path)}')">open editor · ${esc(p.cuts[0].name)}</button>`:''}
        ${p.cuts.length>1?`<select class="sm" title="open a different cut in the editor" onchange="if(this.value)quickCut(this.value);this.selectedIndex=0;"><option value="">other cuts…</option>${p.cuts.map(c=>`<option value="${esc(c.path)}">${esc(c.name)}</option>`).join('')}</select>`:''}
        ${p.cuts.length?`<button class="sm" title="regen the animated on-screen transcript from the VO lines (portrait cuts)\n${esc(p.cuts[0].path)}" onclick="quick('regen_captions','cut','${esc(p.cuts[0].path)}')">regen captions · ${esc(p.cuts[0].name)}</button>`:''}
      </div></div>`;
  }
  $('w-projects').innerHTML=h||'<div class="card">no projects</div>';
}
async function quick(action,pkey,pval){
  const r=await fetch('/api/action',{method:'POST',body:JSON.stringify({action,params:{[pkey]:pval}})});
  const j=await r.json(); if(j.error){flash('✗ '+j.error);return} watch(j.job); openLog();
}
async function quickCut(path){
  const r=await fetch('/api/action',{method:'POST',body:JSON.stringify({action:'open_editor',params:{cut:path}})});
  const j=await r.json(); if(j.error){flash('✗ '+j.error);return} watch(j.job); flash('opening editor');
}

/* ---------- CHANNEL (yutu, read-only) ---------- */
let channelLoaded=false;
async function renderChannel(force){
  const el=$('w-channel');
  if(!force && channelLoaded) return;
  if(force||!channelLoaded) el.innerHTML='<div class="card muted">loading channel…</div>';
  const r=await fetch('/api/channel'+(force?'?force=1':'')); const c=await r.json();
  channelLoaded=true;
  if(!c.ok){
    const msg=c.need_auth?'YouTube isn\'t connected yet — a one-time owner OAuth is needed.'
      :c.need_setup?'yutu (the YouTube API tool) isn\'t installed.':('channel error: '+esc(c.error||'unknown'));
    el.innerHTML=`<div class="card"><h3 style="margin-top:0">Channel not connected</h3>
      <p>${esc(msg)}</p><p class="muted">${esc(c.hint||'')}</p>
      <p class="muted">Setup: <code>~/.config/yutu/README.md</code> · <code>docs/INFRA.md § yutu</code>. Then hit refresh.</p>
      <button class="sm" onclick="renderChannel(true)">⟳ retry</button></div>`;
    return;
  }
  const num=n=>n==null?'—':Number(n).toLocaleString();
  const subs=c.hidden_subs?'hidden':num(c.subs);
  const stat=(n,l)=>`<div class="stat"><div class="num">${n}</div><div class="lbl">${l}</div></div>`;
  const since=c.published?(' · since '+esc(String(c.published).slice(0,4))):'';
  const link=c.custom_url?`https://youtube.com/${esc(c.custom_url)}`:(c.id?`https://youtube.com/channel/${esc(c.id)}`:'#');
  el.innerHTML=`<div class="card chanhead">
    ${c.thumb?`<img class="avatar" src="${esc(c.thumb)}">`:''}
    <div><h2 style="margin:0">${esc(c.title||'(channel)')}</h2>
      <div class="muted"><a href="${link}" target="_blank">${esc(c.custom_url||c.id||'')}</a>${since}</div></div>
    <button class="sm" style="margin-left:auto" onclick="renderChannel(true)">⟳ refresh</button></div>
    ${c.empty_stats?`<div class="card muted" style="margin-top:12px;border-color:#3a3320">No public stats yet — the channel has no public uploads/activity. Subscriber/view/video counts populate once your first video is live.</div>`
      :`<div class="stats">${stat(subs,'subscribers')}${stat(num(c.views),'views')}${stat(num(c.videos),'videos')}</div>`}
    ${c.desc?`<div class="card muted" style="margin-top:12px;white-space:pre-wrap">${esc(c.desc)}</div>`:''}`;
}

/* ---------- jobs / log ---------- */
function renderJobs(){}
function watch(id){curJob=id; if(poll)clearInterval(poll); pollJob(); poll=setInterval(pollJob,800)}
async function pollJob(){
  if(!curJob)return;
  const r=await fetch('/api/job?id='+curJob); if(!r.ok)return; const j=await r.json();
  $('logout').textContent=(j.cmd?('$ '+j.cmd+'\n\n'):'')+j.output.join('\n');
  $('logout').scrollTop=$('logout').scrollHeight;
  const st=j.status=='running'?'<span class="spin"></span> running':j.status=='done'?'✓ done':'✗ failed (rc '+j.rc+')';
  $('logstat').innerHTML=esc(j.action)+' — '+st;
  if(j.status!='running'){clearInterval(poll);poll=null; if(['post','pair','engage'].some(x=>j.action.startsWith(x)))load();}
}
function openLog(){$('log').classList.add('up')}
function toggleLog(){$('log').classList.toggle('up')}
function flash(m){$('logstat').textContent=m}

/* ---------- modal / lightbox ---------- */
function modal(html){$('modalbox').innerHTML=html;$('modal').classList.add('on')}
const fileUrl=p=>'/api/file?path='+encodeURIComponent(p);   // ORIGINAL bytes (full-res), not a thumb
function lightbox(encPath){const p=decodeURIComponent(encPath);
  modal(`<img src="${fileUrl(p)}">
    <div class="row" style="margin-top:8px;gap:8px;align-items:center">
      <button class="sm pri" onclick="copyImageFull('${encPath}')">copy full-res</button>
      <a href="${fileUrl(p)}" target="_blank"><button class="sm">open original ↗</button></a>
      <span class="muted" style="word-break:break-all;flex:1">${esc(p)}</span></div>`);}
async function copyImageFull(encPath){
  const p=decodeURIComponent(encPath);
  try{
    const blob=await (await fetch(fileUrl(p))).blob();          // the real file, at full resolution
    let out=blob;
    if(blob.type!=='image/png'){                                // clipboard only reliably accepts png → re-encode at NATURAL size
      out=await new Promise((res,rej)=>{const im=new Image();
        im.onload=()=>{const c=document.createElement('canvas');c.width=im.naturalWidth;c.height=im.naturalHeight;
          c.getContext('2d').drawImage(im,0,0);c.toBlob(b=>b?res(b):rej('encode failed'),'image/png');};
        im.onerror=()=>rej('decode failed');im.src=URL.createObjectURL(blob);});
    }
    await navigator.clipboard.write([new ClipboardItem({'image/png':out})]);
    flash('copied full-res image · '+Math.round(out.size/1024)+' KB');
  }catch(e){flash('✗ copy failed ('+e+') — use "open original ↗" then copy from there');}
}
function closeModal(){$('modal').classList.remove('on')}
document.addEventListener('keydown',e=>{if(e.key=='Escape')closeModal()});
$('modal').addEventListener('click',e=>{if(e.target.id=='modal')closeModal()});

load();
</script></body></html>"""

if __name__ == "__main__":
    main()
