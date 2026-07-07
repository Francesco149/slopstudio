#!/usr/bin/env python3
"""engage.py — the @GemmaExplains engagement tracker (comment-section presence).

Captured tweets/videos/pages become LLM-readable "context cards": Claude triages them,
drafts in-character replies, the owner posts (YouTube can post via yutu after confirm),
and everything is tracked so high-performing engagements become data points.
Content lives in ../gemma-branding/social/engagement/ — raw/ keeps the untouched
capture for reprocessing, cards/ = pending triage, engaged/ = responded, skipped/ =
passed on. Doctrine + owner setup: ../gemma-branding/SOCIAL.md § Engagement +
the engaging-gemma skill.

  python tools/engage.py ingest URL [--session S]          # YouTube / reddit / HN / any page
  python tools/engage.py ingest export.json [--session S] [--min-faves N]
                                                           # twitter-web-exporter JSON drop
  python tools/engage.py list [--engaged|--skipped] [--platform x] [--session S]
  python tools/engage.py show ID
  python tools/engage.py draft ID --text "..." [--reply-to "@who · thread ID"]  # attach a suggested reply
                                                           # (dashboard: click-to-copy; --reply-to = target a specific
                                                           #  comment instead of the post — shown as a ↪ badge)
  python tools/engage.py respond ID --url U [--text T]     # record what Gemma said → engaged/
  python tools/engage.py skip ID [--reason R]              # pass (safety / stale / meh)
  python tools/engage.py check                             # refresh metrics on our YT replies
  python tools/engage.py status                            # counts + what worked

HARD RULE (keyword flag here + the engaging-gemma skill): ragebait, drama, politics and
negativity are NEVER engaged with. Flagged cards show ⚠ — skip them, don't argue with them.
"""
import argparse, datetime, hashlib, html, json, os, re, subprocess, sys, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DIR = os.path.join(ROOT, "..", "gemma-branding", "social", "engagement")
YUTU_BIN = os.path.expanduser("~/.local/bin/yutu")
YUTU_DIR = os.path.expanduser("~/.config/yutu")  # yutu reads creds from its CWD
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Gecko/20100101 Firefox/128.0"
KINDS = ("cards", "engaged", "skipped")

DEFAULT_TOML = """\
# engagement.toml — engage.py config (see ../../SOCIAL.md § Engagement)
[safety]
# ingest flags a card when any of these hits (word-boundary, case-insensitive).
# Flagged = don't engage unless it's an obvious false positive; when in doubt,
# `skip ID --reason safety`. Crude by design — the engaging-gemma skill is the
# real gate; this just makes the obvious ones loud. (Deliberately no common tech
# words: "vote"/"cancelled"/"fraud" flag half of tech twitter.)
park_keywords = [
  "politics", "political", "election", "president", "congress", "senate",
  "democrat", "republican", "liberal", "conservative", "war", "gaza", "israel",
  "palestine", "ukraine", "russia", "immigration", "woke", "cancel culture",
  "drama", "exposed", "controversy", "controversial", "allegations", "alleged",
  "lawsuit", "scandal", "racist", "racism", "sexist", "sexism", "transphobic",
  "homophobic", "nazi", "fascist", "genocide", "shooting", "boycott", "ratioed",
  "callout", "grifter",
]
"""


def today():
    return str(datetime.date.today())


def edir(base, kind=""):
    d = os.path.join(base, kind) if kind else base
    os.makedirs(d, exist_ok=True)
    return d


def load_config(base):
    import tomllib
    path = os.path.join(edir(base), "engagement.toml")
    if not os.path.exists(path):
        open(path, "w", encoding="utf-8").write(DEFAULT_TOML)
    return tomllib.load(open(path, "rb"))


# ── cards (same frontmatter format as social.py posts) ───────────────────────
def parse_card(path):
    text = open(path, encoding="utf-8").read()
    m = re.match(r"^---\n(.*?)\n---\n?(.*)$", text, re.S)
    if not m:
        raise SystemExit(f"{path}: missing frontmatter")
    meta = {}
    for line in m.group(1).splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            meta[k.strip()] = v.strip()
    return {"path": path, "id": meta.get("id", os.path.splitext(os.path.basename(path))[0]),
            "meta": meta, "body": m.group(2).strip()}


def dump_card(c):
    keys = ["id", "platform", "url", "author", "date", "captured", "session", "stats", "safety",
            "raw", "responded", "response-url", "response-likes", "response-replies", "checked"]
    meta = dict(c["meta"]); meta["id"] = c["id"]
    lines = [f"{k}: {meta[k]}" for k in keys if meta.get(k)]
    lines += [f"{k}: {v}" for k, v in meta.items() if k not in keys and v]
    return "---\n" + "\n".join(lines) + "\n---\n\n" + c["body"] + "\n"


def load_all(base):
    out = {}
    for kind in KINDS:
        d = os.path.join(base, kind)
        out[kind] = [parse_card(os.path.join(d, f))
                     for f in (sorted(os.listdir(d)) if os.path.isdir(d) else [])
                     if f.endswith(".md")]
    return out


def find_card(cards, cid, kinds=KINDS):
    for kind in kinds:
        for c in cards[kind]:
            if c["id"] == cid:
                return kind, c
    raise SystemExit(f"no card '{cid}'")


def write_card(base, cid, meta, body, quiet=False, refresh=False):
    path = os.path.join(edir(base, "cards"), cid + ".md")
    for kind in KINDS:  # never silently resurrect an already-triaged capture
        old = os.path.join(base, kind, cid + ".md")
        if not os.path.exists(old):
            continue
        if not refresh or kind == "engaged":  # engaged history is immutable
            if not quiet:
                print(f"  = {cid} already captured ({kind}/) — skipped")
            return None
        os.remove(old)  # --refresh: recapture fresh (skipped/ cards return to triage)
    meta = {k: v for k, v in meta.items() if v}
    open(path, "w", encoding="utf-8").write(dump_card({"id": cid, "meta": meta, "body": body}))
    flag = " ⚠ " + meta["safety"] if meta.get("safety", "").startswith("flagged") else ""
    if not quiet:
        print(f"  + {cid}{flag}")
    return path


def save_raw(base, name, data):
    path = os.path.join(edir(base, "raw"), name)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode, encoding=None if isinstance(data, bytes) else "utf-8") as f:
        f.write(data)
    return "raw/" + name


def safety_scan(cfg, *texts):
    blob = " ".join(t or "" for t in texts)
    for kw in cfg.get("safety", {}).get("park_keywords", []):
        if re.search(r"\b" + re.escape(kw) + r"\b", blob, re.I):
            return f"flagged ({kw})"
    return "ok"


# ── fetch helpers ─────────────────────────────────────────────────────────────
def fetch(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept-Language": "en"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def strip_html(s):
    s = re.sub(r"<br\s*/?>", "\n", s or "", flags=re.I)
    s = re.sub(r"<[^>]+>", "", s)
    return html.unescape(s).strip()


def yutu(*args):
    if not os.path.exists(YUTU_BIN):
        raise SystemExit("yutu not installed — see docs/INFRA.md § yutu")
    r = subprocess.run([YUTU_BIN, *args, "-o", "json"], cwd=YUTU_DIR,
                       capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        raise SystemExit(f"yutu {' '.join(args[:2])} failed: {(r.stderr or '').strip()[:300]}")
    data = json.loads(r.stdout or "{}")
    return data.get("items", data) if isinstance(data, dict) else data


def n(x):
    try:
        return f"{int(x):,}"
    except (TypeError, ValueError):
        return "?"


# ── channel dossiers (recurring memes/gags per creator — fan-signal fuel) ─────
def chan_slug(s):
    return re.sub(r"[^\w]+", "-", (s or "channel").lower()).strip("-") or "channel"


def update_dossier(base, channel, cid, title, date):
    """Track every capture per channel; the notes section is maintained at triage."""
    path = os.path.join(edir(base, "channels"), chan_slug(channel) + ".md")
    if not os.path.exists(path):
        open(path, "w", encoding="utf-8").write(
            f"# {channel} — channel dossier\n\n"
            "## notes (recurring memes, gags, community bits — update at every triage)\n\n"
            "(none recorded yet)\n\n## captures\n")
    text = open(path, encoding="utf-8").read()
    prior = text.count("\n- ")
    if cid not in text:
        open(path, "a", encoding="utf-8").write(f"- {date} · {cid} — {title}\n")
    return path, prior


# ── ingest: YouTube (official API via yutu — no scraping) ─────────────────────
def yt_transcript(vid):
    """Plain-text transcript from YT subs via yt-dlp (manual or auto); '' if none."""
    import shutil as _sh, tempfile
    if not _sh.which("yt-dlp"):
        print("  (yt-dlp not found — transcript skipped; run inside nix develop)")
        return ""
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["yt-dlp", "--no-warnings", "-q", "--skip-download",
                        "--write-subs", "--write-auto-subs", "--sub-langs", "en,en-orig",
                        "--sub-format", "json3", "-o", os.path.join(td, "v.%(ext)s"),
                        f"https://youtu.be/{vid}"], capture_output=True, timeout=180)
        subs = [f for f in os.listdir(td) if f.endswith(".json3")]
        if not subs:
            return ""
        data = json.loads(open(os.path.join(td, subs[0]), encoding="utf-8").read())
    words = []
    for ev in data.get("events", []):
        for seg in ev.get("segs") or []:
            w = (seg.get("utf8") or "").strip()
            if w:
                words.append(w)
    return re.sub(r"\s+", " ", " ".join(words)).strip()
def yt_video_id(url):
    u = urllib.parse.urlparse(url)
    if u.netloc.endswith("youtu.be"):
        return u.path.strip("/").split("/")[0]
    m = re.search(r"/(?:shorts|live|embed)/([A-Za-z0-9_-]{6,})", u.path)
    if m:
        return m.group(1)
    return urllib.parse.parse_qs(u.query).get("v", [None])[0]


def ingest_youtube(base, cfg, url, session, refresh=False):
    vid = yt_video_id(url)
    if not vid:
        raise SystemExit(f"can't find a video id in {url}")
    items = yutu("video", "list", "--ids", vid, "-p", "id,snippet,statistics,contentDetails")
    if not items:
        raise SystemExit(f"no such video: {vid}")
    v = items[0]
    sn, st = v.get("snippet", {}), v.get("statistics", {})
    try:
        threads = yutu("commentThread", "list", "--videoId", vid, "--order", "relevance",
                       "-n", "8", "-p", "id,snippet")
    except SystemExit as e:  # comments disabled shouldn't kill the capture
        print(f"  (comments unavailable: {e})"); threads = []
    raw = save_raw(base, f"yt-{vid}.json", json.dumps({"video": v, "threads": threads}, indent=1))
    desc = (sn.get("description") or "").strip()
    lines = [f"# {sn.get('title', '(untitled)')}", "",
             f"channel: {sn.get('channelTitle', '?')} · published {str(sn.get('publishedAt', ''))[:10]}"
             f" · {v.get('contentDetails', {}).get('duration', '')}"]
    if sn.get("tags"):
        lines.append("tags: " + ", ".join(sn["tags"][:12]))
    lines += ["", "## description", desc[:700] + ("…" if len(desc) > 700 else "")]
    com_text = []
    if threads:
        lines += ["", "## top comments (by relevance)"]
        for t in threads:
            c = t.get("snippet", {}).get("topLevelComment", {}).get("snippet", {})
            txt = strip_html(c.get("textDisplay", ""))[:280]
            com_text.append(txt)
            lines.append(f"- {c.get('authorDisplayName', '?')} (+{c.get('likeCount', 0)}, "
                         f"{t['snippet'].get('totalReplyCount', 0)} replies · thread {t.get('id', '')}): {txt}")
    trans = yt_transcript(vid)
    if trans:
        save_raw(base, f"yt-{vid}.transcript.txt", trans)
        lines += ["", "## transcript (from subs)",
                  trans[:18000] + ("… (truncated — full text in raw/)" if len(trans) > 18000 else "")]
    dossier, prior = update_dossier(base, sn.get("channelTitle", "?"), f"yt-{vid}",
                                    sn.get("title", ""), str(sn.get("publishedAt", ""))[:10])
    print(f"  channel dossier: {os.path.relpath(dossier, base)} ({prior} prior capture(s))"
          + (" — read the notes before drafting" if prior else ""))
    meta = {"platform": "youtube", "url": f"https://youtu.be/{vid}",
            "author": sn.get("channelTitle", "?"), "date": str(sn.get("publishedAt", ""))[:10],
            "captured": today(), "session": session,
            "stats": f"{n(st.get('viewCount'))} views · {n(st.get('likeCount'))} likes · "
                     f"{n(st.get('commentCount'))} comments",
            "safety": safety_scan(cfg, sn.get("title"), desc[:1500], *com_text), "raw": raw}
    return [write_card(base, f"yt-{vid}", meta, "\n".join(lines), refresh=refresh)]


# ── ingest: twitter-web-exporter JSON (passive GraphQL capture; both the plain
#    and the "include all metadata" shapes) ────────────────────────────────────
def g(d, path, default=None):
    for k in path.split("."):
        if not isinstance(d, dict):
            return default
        d = d.get(k)
    return d if d is not None else default


def tweet_date(created_at, tid):
    """'Mon Jul 06 12:00:00 +0000 2026' or the snowflake id → YYYY-MM-DD (freshness triage)."""
    try:
        return str(datetime.datetime.strptime(created_at, "%a %b %d %H:%M:%S %z %Y").date())
    except (TypeError, ValueError):
        pass
    try:  # snowflake epoch fallback
        ms = (int(tid) >> 22) + 1288834974657
        return str(datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc).date())
    except (TypeError, ValueError):
        return ""


def tweet_fields(t):
    leg = t.get("legacy") or t
    user = (g(t, "core.user_results.result.legacy") or g(t, "core.user_results.result.core")
            or t.get("user") or {})
    text = g(t, "note_tweet.note_tweet_results.result.text") or leg.get("full_text") or leg.get("text")
    return {"id": str(t.get("rest_id") or t.get("id") or leg.get("id_str") or ""),
            "text": (text or "").strip(),
            "screen_name": user.get("screen_name") or t.get("screen_name") or "?",
            "name": user.get("name") or t.get("name") or "",
            "faves": leg.get("favorite_count", t.get("favorite_count", 0)) or 0,
            "rts": leg.get("retweet_count", t.get("retweet_count", 0)) or 0,
            "replies": leg.get("reply_count", t.get("reply_count", 0)) or 0,
            "views": g(t, "views.count") or t.get("views_count") or "",
            "created": tweet_date(leg.get("created_at", t.get("created_at", "")),
                                  t.get("rest_id") or t.get("id") or leg.get("id_str")),
            "media": len(g(leg, "extended_entities.media", t.get("media") or []) or []),
            "quoted": _unwrap_quoted(g(t, "quoted_status_result.result") or t.get("quoted_status"))}


def _unwrap_quoted(q):
    """quoted tweet value → dict, bare id string (twitter-web-exporter's flattened
    format references the quote by id), or None. Unwraps TweetWithVisibilityResults."""
    if isinstance(q, dict) and isinstance(q.get("tweet"), dict):
        return q["tweet"]
    return q if isinstance(q, (dict, str)) else None


def ingest_xjson(base, cfg, path, session, min_faves):
    data = json.loads(open(path, encoding="utf-8").read())
    tweets = data if isinstance(data, list) else data.get("tweets") or data.get("data") or []
    if not isinstance(tweets, list) or not tweets:
        raise SystemExit(f"{path}: doesn't look like a twitter-web-exporter JSON export")
    raw = save_raw(base, f"x-session-{session}-{os.path.basename(path)}",
                   open(path, encoding="utf-8").read())
    made, low = [], 0
    byid = {}   # id → tweet, to resolve flattened bare-id quote references within the export
    for t in tweets:
        if isinstance(t, dict):
            tid = str(t.get("rest_id") or t.get("id") or (t.get("legacy") or {}).get("id_str") or "")
            if tid:
                byid[tid] = t
    for t in tweets:
        f = tweet_fields(t)
        if not f["id"] or not f["text"]:
            continue
        if int(f["faves"]) < min_faves:
            low += 1; continue
        lines = [f["text"]]
        if f["media"]:
            lines.append(f"\n({f['media']} media attachment{'s' if f['media'] > 1 else ''} — "
                         f"open the tweet to see them)")
        if f["quoted"]:
            qt = f["quoted"]
            if isinstance(qt, str):          # bare id ref → resolve within this export
                qt = byid.get(qt)
            if isinstance(qt, dict):
                q = tweet_fields(qt)
                if q["text"]:
                    lines.append(f"\n## quoted tweet — @{q['screen_name']}\n{q['text']}")
            elif isinstance(f["quoted"], str):   # unresolvable (not captured) → leave a pointer
                lines.append(f"\n## quoted tweet\nhttps://x.com/i/status/{f['quoted']} "
                             "(not in this capture — open to read)")
        meta = {"platform": "x", "url": f"https://x.com/{f['screen_name']}/status/{f['id']}",
                "author": f"@{f['screen_name']}" + (f" ({f['name']})" if f["name"] else ""),
                "date": f["created"], "captured": today(), "session": session,
                "stats": f"{n(f['faves'])} faves · {n(f['rts'])} RTs · {n(f['replies'])} replies"
                         + (f" · {n(f['views'])} views" if f["views"] else ""),
                "safety": safety_scan(cfg, f["text"]), "raw": raw}
        made.append(write_card(base, f"x-{f['id']}", meta, "\n".join(lines)))
    if low:
        print(f"  ({low} below --min-faves {min_faves} — not carded; still in {raw})")
    return made


# ── ingest: reddit / HN / generic page ────────────────────────────────────────
def ingest_reddit(base, cfg, url, session):
    ju = url.split("?")[0].rstrip("/") + ".json?limit=12"
    data = json.loads(fetch(ju))
    post = data[0]["data"]["children"][0]["data"]
    raw = save_raw(base, f"rd-{post['id']}.json", json.dumps(data, indent=1))
    body = (post.get("selftext") or "").strip()
    lines = [f"# {post['title']}", "", f"r/{post['subreddit']} · u/{post['author']}"]
    if body:
        lines += ["", body[:1500] + ("…" if len(body) > 1500 else "")]
    if post.get("url") and not post["url"].startswith("https://www.reddit"):
        lines += ["", f"links to: {post['url']}"]
    coms = [c["data"] for c in data[1]["data"]["children"] if c.get("kind") == "t1"][:8]
    if coms:
        lines += ["", "## top comments"]
        lines += [f"- u/{c.get('author', '?')} (+{c.get('score', 0)}): "
                  f"{(c.get('body') or '')[:280]}" for c in coms]
    meta = {"platform": "reddit", "url": "https://www.reddit.com" + post["permalink"],
            "author": "u/" + post["author"],
            "date": str(datetime.datetime.fromtimestamp(post.get("created_utc", 0),
                                                        datetime.timezone.utc).date()) if post.get("created_utc") else "",
            "captured": today(), "session": session,
            "stats": f"{n(post.get('score'))} points · {n(post.get('num_comments'))} comments",
            "safety": safety_scan(cfg, post["title"], body[:1500],
                                  *[c.get("body", "")[:300] for c in coms]), "raw": raw}
    return [write_card(base, f"rd-{post['id']}", meta, "\n".join(lines))]


def ingest_hn(base, cfg, url, session):
    hid = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)["id"][0]
    it = json.loads(fetch(f"https://hn.algolia.com/api/v1/items/{hid}"))
    raw = save_raw(base, f"hn-{hid}.json", json.dumps(it, indent=1))
    kids = sorted(it.get("children") or [], key=lambda c: -(len(c.get("children") or [])))[:8]
    lines = [f"# {it.get('title') or '(comment)'}", ""]
    if it.get("url"):
        lines.append(f"links to: {it['url']}")
    if it.get("text"):
        lines += ["", strip_html(it["text"])[:1500]]
    if kids:
        lines += ["", "## top comments"]
        lines += [f"- {c.get('author', '?')} ({len(c.get('children') or [])} replies): "
                  f"{strip_html(c.get('text') or '')[:280]}" for c in kids]
    meta = {"platform": "hn", "url": f"https://news.ycombinator.com/item?id={hid}",
            "author": it.get("author", "?"), "date": str(it.get("created_at", ""))[:10],
            "captured": today(), "session": session,
            "stats": f"{n(it.get('points'))} points · {len(it.get('children') or [])} top-level comments",
            "safety": safety_scan(cfg, it.get("title"), strip_html(it.get("text") or "")[:1500],
                                  *[strip_html(c.get("text") or "")[:300] for c in kids]), "raw": raw}
    return [write_card(base, f"hn-{hid}", meta, "\n".join(lines))]


def ingest_page(base, cfg, url, session):
    data = fetch(url)
    text = data.decode("utf-8", "replace")
    key = hashlib.sha1(url.encode()).hexdigest()[:10]
    raw = save_raw(base, f"web-{key}.html", data)
    title = strip_html((re.search(r"<title[^>]*>(.*?)</title>", text, re.S | re.I) or [None, ""])[1])
    desc = (re.search(r'<meta[^>]+(?:name|property)=["\'](?:og:)?description["\'][^>]+content=["\']([^"\']*)',
                      text, re.I) or [None, ""])[1]
    body = re.sub(r"<(script|style|nav|header|footer)[^>]*>.*?</\1>", " ", text, flags=re.S | re.I)
    body = re.sub(r"\s+", " ", strip_html(body)).strip()
    lines = [f"# {title or url}", ""]
    if desc:
        lines += [html.unescape(desc), ""]
    lines += ["## page text (auto-extracted — check raw/ if mangled)", body[:2500]]
    meta = {"platform": "web", "url": url, "captured": today(), "session": session,
            "safety": safety_scan(cfg, title, desc, body[:2500]), "raw": raw}
    return [write_card(base, f"web-{key}", meta, "\n".join(lines))]


def cmd_ingest(base, cfg, args):
    session = args.session or today()
    src = args.source
    if os.path.isfile(src):
        made = ingest_xjson(base, cfg, src, session, args.min_faves)
    else:
        host = urllib.parse.urlparse(src).netloc.lower()
        if "youtube.com" in host or "youtu.be" in host:
            made = ingest_youtube(base, cfg, src, session, refresh=args.refresh)
        elif "reddit.com" in host:
            made = ingest_reddit(base, cfg, src, session)
        elif "news.ycombinator.com" in host:
            made = ingest_hn(base, cfg, src, session)
        elif host.startswith(("x.com", "twitter.com")) or ".x.com" in host:
            raise SystemExit("single tweets can't be fetched logged-out — open it in the browser "
                             "with the twitter-web-exporter userscript, export JSON, then "
                             "`ingest export.json` (setup: ../gemma-branding/SOCIAL.md § Engagement)")
        else:
            made = ingest_page(base, cfg, src, session)
    made = [m for m in made if m]
    print(f"ingested {len(made)} card(s) → cards/  (session {session})")
    if made:
        print("next: triage with Claude — list → show → draft replies (engaging-gemma skill)")


# ── triage / tracking ─────────────────────────────────────────────────────────
def cmd_list(cards, args):
    kind = "engaged" if args.engaged else "skipped" if args.skipped else "cards"
    rows = [c for c in cards[kind]
            if (not args.platform or c["meta"].get("platform") == args.platform)
            and (not args.session or c["meta"].get("session") == args.session)]
    # drafted cards float to the top (they're the ones waiting on the owner)
    for c in sorted(rows, key=lambda c: ("## suggested replies" not in c["body"],
                                         c["meta"].get("captured", ""))):
        m = c["meta"]
        flag = "⚠ " if m.get("safety", "").startswith("flagged") else "  "
        nsug = c["body"].count("\n- ", c["body"].find("## suggested replies")) \
            if "## suggested replies" in c["body"] else 0
        pen = f"✎{nsug} " if nsug else "   "
        extra = m.get("responded") or m.get("date") or m.get("captured", "")
        likes = f" · our reply +{m['response-likes']}" if m.get("response-likes") else ""
        head = c["body"].splitlines()[0].lstrip("# ")[:56]
        print(f"  {flag}{pen}{c['id']:<16} [{m.get('platform', '?'):<7}] {extra} · "
              f"{m.get('stats', '')[:36]}{likes} · {head}")
    if not rows:
        print(f"  (no {kind} cards match)")


def cmd_show(cards, cid):
    _, c = find_card(cards, cid)
    for k, v in c["meta"].items():
        print(f"{k}: {v}")
    print("\n" + c["body"])


def move_card(base, c, kind):
    dst = os.path.join(edir(base, kind), c["id"] + ".md")
    open(dst, "w", encoding="utf-8").write(dump_card(c))
    os.remove(c["path"])
    return dst


def cmd_draft(base, cards, args):
    kind, c = find_card(cards, args.id, kinds=("cards",))
    if "## suggested replies" not in c["body"]:
        c["body"] += "\n\n## suggested replies"
    # optional target comment: stored as a machine-parseable "[reply to …]" prefix on the
    # bullet (the dashboard strips it into a badge; click-to-copy copies only the text)
    target = f"[reply to {' '.join(args.reply_to.split())}] " if getattr(args, "reply_to", None) else ""
    c["body"] += "\n- " + target + " ".join(args.text.split())  # one line per option
    open(c["path"], "w", encoding="utf-8").write(dump_card(c))
    print(f"draft attached to {args.id}" + (f" (↪ {args.reply_to})" if getattr(args, "reply_to", None) else ""))


def cmd_respond(base, cards, args):
    kind, c = find_card(cards, args.id)
    if kind == "skipped":
        print(f"(note: {args.id} was skipped — un-skipping into engaged/)")
    c["meta"]["responded"] = args.date or today()
    if args.url:
        c["meta"]["response-url"] = args.url
    if args.text:
        c["body"] += f"\n\n## our response ({c['meta']['responded']})\n{args.text.strip()}"
    move_card(base, c, "engaged")
    print(f"engaged {args.id} → engaged/" + ("" if args.url else "  (no --url — add it when known)"))


def cmd_skip(base, cards, args):
    kind, c = find_card(cards, args.id, kinds=("cards",))
    c["meta"]["skipped"] = today()
    if args.reason:
        c["meta"]["skip-reason"] = args.reason
    move_card(base, c, "skipped")
    print(f"skipped {args.id}" + (f" ({args.reason})" if args.reason else ""))


def cmd_check(base, cards):
    hits = 0
    for c in cards["engaged"]:
        u = c["meta"].get("response-url", "")
        cid = urllib.parse.parse_qs(urllib.parse.urlparse(u).query).get("lc", [None])[0]
        if c["meta"].get("platform") != "youtube" or not cid:
            continue
        cid = cid.split(".")[0]  # lc=parent.reply → stats live on the thread
        try:
            t = yutu("commentThread", "list", "--ids", cid, "-p", "id,snippet")[0]
            sn = t["snippet"]["topLevelComment"]["snippet"]
            c["meta"]["response-likes"] = str(sn.get("likeCount", 0))
            c["meta"]["response-replies"] = str(t["snippet"].get("totalReplyCount", 0))
            c["meta"]["checked"] = today()
            open(c["path"], "w", encoding="utf-8").write(dump_card(c))
            print(f"  {c['id']:<16} our reply: +{c['meta']['response-likes']} "
                  f"· {c['meta']['response-replies']} replies")
            hits += 1
        except (SystemExit, LookupError, json.JSONDecodeError) as e:
            print(f"  {c['id']:<16} check failed: {e}")
    print(f"checked {hits} YouTube replies" if hits else
          "nothing to check — YouTube engagements need a response-url with &lc=<comment id>")


def cmd_status(cards):
    pend = cards["cards"]
    flagged = [c for c in pend if c["meta"].get("safety", "").startswith("flagged")]
    week = str(datetime.date.today() - datetime.timedelta(days=7))
    recent = [c for c in cards["engaged"] if c["meta"].get("responded", "") >= week]
    print(f"── engagement status · {today()} ──")
    by_plat = {}
    for c in pend:
        by_plat.setdefault(c["meta"].get("platform", "?"), []).append(c)
    for p, cs in sorted(by_plat.items()):
        fl = sum(1 for c in cs if c["meta"].get("safety", "").startswith("flagged"))
        print(f"  pending {p:<8} {len(cs)}" + (f"  (⚠ {fl} safety-flagged)" if fl else ""))
    if not pend:
        print("  pending: none — capture something (SOCIAL.md § Engagement)")
    print(f"  engaged {len(cards['engaged'])} total · {len(recent)} this week"
          f" · skipped {len(cards['skipped'])}")
    top = sorted((c for c in cards["engaged"] if c["meta"].get("response-likes")),
                 key=lambda c: -int(c["meta"]["response-likes"]))[:5]
    if top:
        print("\n  what worked (our reply likes — refresh with `check`):")
        for c in top:
            print(f"    +{c['meta']['response-likes']:<4} {c['id']:<16} "
                  f"{c['body'].splitlines()[0].lstrip('# ')[:52]}")
    if flagged:
        print(f"\n  ⚠ flagged, never engage (skip --reason safety): "
              + ", ".join(c["id"] for c in flagged))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR, help="engagement content dir")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ip = sub.add_parser("ingest"); ip.add_argument("source", help="URL or exporter .json")
    ip.add_argument("--session", help="capture-session tag (default: today)")
    ip.add_argument("--min-faves", type=int, default=0, help="tweet floor for timeline dumps")
    ip.add_argument("--refresh", action="store_true",
                    help="recapture an existing card (skipped/ returns to triage; engaged/ is immutable)")
    lp = sub.add_parser("list"); lp.add_argument("--platform"); lp.add_argument("--session")
    lp.add_argument("--engaged", action="store_true"); lp.add_argument("--skipped", action="store_true")
    sp = sub.add_parser("show"); sp.add_argument("id")
    dp = sub.add_parser("draft"); dp.add_argument("id"); dp.add_argument("--text", required=True)
    dp.add_argument("--reply-to", dest="reply_to", default=None,
                    help="which comment this responds to (author + thread id / permalink), if not the post itself")
    rp = sub.add_parser("respond"); rp.add_argument("id"); rp.add_argument("--url")
    rp.add_argument("--text"); rp.add_argument("--date")
    kp = sub.add_parser("skip"); kp.add_argument("id"); kp.add_argument("--reason")
    sub.add_parser("check"); sub.add_parser("status")
    args = ap.parse_args()
    base = os.path.abspath(args.dir)
    cfg = load_config(base)
    if args.cmd == "ingest":
        return cmd_ingest(base, cfg, args)
    cards = load_all(base)
    {"list": lambda: cmd_list(cards, args), "show": lambda: cmd_show(cards, args.id),
     "draft": lambda: cmd_draft(base, cards, args),
     "respond": lambda: cmd_respond(base, cards, args), "skip": lambda: cmd_skip(base, cards, args),
     "check": lambda: cmd_check(base, cards), "status": lambda: cmd_status(cards)}[args.cmd]()


if __name__ == "__main__":
    main()
