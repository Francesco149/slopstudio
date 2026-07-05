#!/usr/bin/env python3
"""social.py — the @GemmaExplains social-presence scheduler (LLM- and owner-facing).

Content lives in ../gemma-branding/social/ (platforms.toml + queue/*.md + posted/*.md);
this CLI is the daily check-in: what's due, what to post, what's coming up.

  python tools/social.py status                 # THE daily ritual: due platforms + suggested posts
  python tools/social.py list [--platform x] [--posted]
  python tools/social.py show ID
  python tools/social.py draft --platforms x,bluesky [--id ID] [--occasion O] [--window W]
  python tools/social.py post ID [--platform x] [--url U] [--date YYYY-MM-DD]

Post files are markdown with a simple `key: value` frontmatter (see ../gemma-branding/SOCIAL.md).
Windows: `anytime` | `YYYY-MM-DD` | `YYYY-MM-DD..YYYY-MM-DD` | `manual: <trigger>`.
`post` stamps the date + moves the file to posted/; with --platform it splits a
cross-post so the remaining platforms stay queued."""
import argparse, datetime, os, re, sys, tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DIR = os.path.join(ROOT, "..", "gemma-branding", "social")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

def pdate(s): return datetime.date.fromisoformat(s)

def parse_post(path):
    text = open(path, encoding="utf-8").read()
    m = re.match(r"^---\n(.*?)\n---\n?(.*)$", text, re.S)
    if not m: raise SystemExit(f"{path}: missing frontmatter")
    meta = {}
    for line in m.group(1).splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            meta[k.strip()] = v.strip()
    plats = [p.strip() for p in meta.get("platforms", "").split(",") if p.strip()]
    return {"path": path, "id": meta.get("id", os.path.splitext(os.path.basename(path))[0]),
            "platforms": plats, "meta": meta, "body": m.group(2).strip()}

def dump_post(p):
    keys = ["id", "platforms", "occasion", "window", "priority", "image", "image-prompt",
            "author", "posted", "posted-url"]
    meta = dict(p["meta"]); meta["id"] = p["id"]; meta["platforms"] = ", ".join(p["platforms"])
    lines = [f"{k}: {meta[k]}" for k in keys if meta.get(k)]
    lines += [f"{k}: {v}" for k, v in meta.items() if k not in keys and v]
    return "---\n" + "\n".join(lines) + "\n---\n\n" + p["body"] + "\n"

def load_all(sdir):
    posts = {"queue": [], "posted": []}
    for kind in posts:
        d = os.path.join(sdir, kind)
        for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if f.endswith(".md"): posts[kind].append(parse_post(os.path.join(d, f)))
    return posts

def window_state(win, today):
    """-> ('open'|'upcoming'|'expired'|'manual', sort_key, human)"""
    win = (win or "anytime").strip()
    if win.startswith("manual"): return ("manual", None, win.split(":", 1)[-1].strip() or "manual")
    if win == "anytime": return ("open", datetime.date.max, "anytime")
    a, _, b = win.partition("..")
    start, end = pdate(a), pdate(b) if b else pdate(a)
    if today < start: return ("upcoming", start, win)
    if today > end: return ("expired", end, win)
    return ("open", end, win)  # dated-and-open sorts before anytime (date.max)

def last_posted(posts, plat):
    ds = [pdate(p["meta"]["posted"]) for p in posts["posted"]
          if plat in p["platforms"] and DATE_RE.match(p["meta"].get("posted", ""))]
    return max(ds) if ds else None

def cmd_status(sdir, posts, today):
    cfg = tomllib.load(open(os.path.join(sdir, "platforms.toml"), "rb"))
    print(f"── social status · {today} ──")
    due_plats = []
    for plat, pc in cfg.items():
        lp = last_posted(posts, plat)
        cad = pc.get("cadence_days", 3)
        since = (today - lp).days if lp else None
        due = since is None or since >= cad
        if due: due_plats.append(plat)
        depth = sum(1 for p in posts["queue"] if plat in p["platforms"]
                    and window_state(p["meta"].get("window"), today)[0] in ("open", "upcoming"))
        tag = "DUE" if due else "ok "
        lasts = f"last {lp} ({since}d ago)" if lp else "never posted"
        low = "  ⚠ queue low — refill with Claude" if depth < 3 else ""
        print(f"  [{tag}] {plat:<10} every {cad}d · {lasts} · {depth} queued{low}")
    # suggestions: window-open posts for due platforms; dated windows first, then priority
    sug = []
    for p in posts["queue"]:
        st, key, _ = window_state(p["meta"].get("window"), today)
        if st == "open" and any(pl in due_plats for pl in p["platforms"]):
            sug.append((key, 0 if p["meta"].get("priority") == "high" else 1, p))
    sug.sort(key=lambda t: (t[0], t[1]))
    if sug:
        print("\n  post today:")
        for _, _, p in sug[:4]:
            img = "" if p["meta"].get("image", "none") in ("none", "") else f" · image: {p['meta']['image']}"
            print(f"    {p['id']:<24} [{','.join(p['platforms'])}] {p['meta'].get('window','anytime')}{img}")
        print(f"    → tools/social.py show ID · then post ID when it's up")
    elif due_plats:
        print("\n  due but nothing eligible queued — draft or ask Claude for a refill")
    ups = [(window_state(p["meta"].get("window"), today), p) for p in posts["queue"]]
    upcoming = sorted([(k, p) for (s, k, _), p in ups if s == "upcoming" and (k - today).days <= 10])
    if upcoming:
        print("\n  upcoming windows:")
        for k, p in upcoming: print(f"    {k} ({(k-today).days}d) {p['id']} [{','.join(p['platforms'])}]")
    trig = [p for p in posts["queue"] if window_state(p["meta"].get("window"), today)[0] == "manual"]
    if trig:
        print("\n  waiting on a trigger:")
        for p in trig: print(f"    {p['id']:<24} when: {window_state(p['meta'].get('window'), today)[2]}")
    exp = [p for p in posts["queue"] if window_state(p["meta"].get("window"), today)[0] == "expired"]
    if exp: print(f"\n  ⚠ expired windows (rework or rm): {', '.join(p['id'] for p in exp)}")

def cmd_list(posts, plat, kind):
    for p in posts[kind]:
        if plat and plat not in p["platforms"]: continue
        extra = p["meta"].get("posted") or p["meta"].get("window", "anytime")
        print(f"  {p['id']:<26} [{','.join(p['platforms'])}] {extra} · {p['body'].splitlines()[0][:60]}")

def cmd_show(posts, pid):
    for p in posts["queue"] + posts["posted"]:
        if p["id"] == pid:
            for k, v in p["meta"].items(): print(f"{k}: {v}")
            print("\n" + p["body"]); return
    raise SystemExit(f"no post '{pid}'")

def cmd_draft(sdir, args):
    pid = args.id or f"draft-{args.platforms.replace(',', '-')}-{len(os.listdir(os.path.join(sdir, 'queue')))}"
    path = os.path.join(sdir, "queue", pid + ".md")
    if os.path.exists(path): raise SystemExit(f"{path} exists")
    p = {"id": pid, "platforms": [s.strip() for s in args.platforms.split(",")],
         "meta": {"occasion": args.occasion or "evergreen", "window": args.window or "anytime",
                  "priority": "normal", "image": "none", "author": "owner"},
         "body": "(write the post here — voice rules: ../gemma-branding/SOCIAL.md + the writing-gemma skill)"}
    open(path, "w", encoding="utf-8").write(dump_post(p))
    print(f"drafted → {path}")

def cmd_post(sdir, posts, args, today):
    for p in posts["queue"]:
        if p["id"] != args.id: continue
        done = [s.strip() for s in args.platform.split(",")] if args.platform else p["platforms"]
        rest = [pl for pl in p["platforms"] if pl not in done]
        stamp = dict(p); stamp = {**p, "platforms": done,
                                  "meta": {**p["meta"], "posted": args.date or str(today)}}
        if args.url: stamp["meta"]["posted-url"] = args.url
        suffix = "" if not rest else "-" + "-".join(done)
        open(os.path.join(sdir, "posted", p["id"] + suffix + ".md"), "w", encoding="utf-8").write(dump_post(stamp))
        if rest:
            keep = {**p, "platforms": rest}
            open(p["path"], "w", encoding="utf-8").write(dump_post(keep))
            print(f"posted {p['id']} on {','.join(done)}; still queued for {','.join(rest)}")
        else:
            os.remove(p["path"]); print(f"posted {p['id']} on {','.join(done)} → posted/")
        return
    raise SystemExit(f"no queued post '{args.id}'")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR, help="social content dir")
    ap.add_argument("--date", help="pretend today is YYYY-MM-DD (testing)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    lp = sub.add_parser("list"); lp.add_argument("--platform"); lp.add_argument("--posted", action="store_true")
    sp = sub.add_parser("show"); sp.add_argument("id")
    dp = sub.add_parser("draft"); dp.add_argument("--platforms", required=True)
    dp.add_argument("--id"); dp.add_argument("--occasion"); dp.add_argument("--window")
    pp = sub.add_parser("post"); pp.add_argument("id"); pp.add_argument("--platform")
    pp.add_argument("--url"); pp.add_argument("--date", dest="pdate")
    args = ap.parse_args()
    sdir = os.path.abspath(args.dir)
    today = pdate(args.date) if args.date else datetime.date.today()
    posts = load_all(sdir)
    if args.cmd == "status": cmd_status(sdir, posts, today)
    elif args.cmd == "list": cmd_list(posts, args.platform, "posted" if args.posted else "queue")
    elif args.cmd == "show": cmd_show(posts, args.id)
    elif args.cmd == "draft": cmd_draft(sdir, args)
    elif args.cmd == "post":
        args.date = args.pdate; cmd_post(sdir, posts, args, today)

if __name__ == "__main__":
    main()
