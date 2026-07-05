#!/usr/bin/env python3
"""Make a slop project SELF-CONTAINED: copy every referenced asset that lives outside
the project's own dir (cache:// gen output, assets-src/ raw B-roll, code-repo files)
INTO <projdir>/assets/<stem>/ and rewrite the URIs **project-relative** ("assets/…").
A project dir (project.slop.json + assets/) is then portable — it can live in its own
repo (the slopstudio-projects monorepo) while the editor/tools run from the code repo
(they resolve plain uris CWD-first, then project-relative).

STOCK assets (the shared host rig + the room/desk backgrounds) are left as-is so they
can be provided by the downloaded stock pack instead of duplicated per project.

    nix develop --command python tools/localize-project.py <path>/<name>.slop.json

Idempotent (uris already resolving inside the project dir are skipped). Run
`slop.py lint` after to confirm 0 missing.
"""
import json, os, sys, shutil

# stock assets that ship in the release pack (never duplicated into a project bundle):
# the host rig + the shared backgrounds (the canon room day/dark + the desk).
STOCK_PREFIXES = ("presets/avatars/gemma-big", "presets/avatars/gemma-pngtuber",
                  "presets/backgrounds/", "library/images/", "library/audio/",
                  "library/sfx/")
STOCK_FILES = ()

def resolve(uri, cache="cache"):
    if uri.startswith("cache://"): return cache + "/" + uri[len("cache://"):]
    if uri.startswith("file://"):  return uri[len("file://"):]
    return uri

def is_stock(uri):
    u = uri.replace("\\", "/")
    return any(u.startswith(p) for p in STOCK_PREFIXES) or u in STOCK_FILES

def main():
    proj = sys.argv[1]
    projdir = os.path.dirname(os.path.abspath(proj)) or "."
    stem = os.path.basename(proj).split(".slop.json")[0]
    root_rel = f"assets/{stem}"
    d = json.load(open(proj, encoding="utf-8"))
    n_copied = 0

    def localize(uri, sub_hint=None):
        """returns the (possibly rewritten) uri; copies the file in if needed."""
        nonlocal n_copied
        u = uri.replace("\\", "/")
        if not u or is_stock(u): return uri
        # already resolves inside the project dir → portable, leave it
        if not os.path.isabs(u) and "://" not in u and os.path.exists(os.path.join(projdir, u)):
            return uri
        src = resolve(u)
        if not os.path.exists(src):
            print(f"  MISSING (skip): {uri}")
            return uri
        ext = os.path.splitext(src)[1].lower()
        sub = sub_hint or ("audio" if ext in (".wav", ".mp3", ".flac", ".ogg") else
                           "video" if ext in (".mp4", ".mov", ".mkv", ".webm") else
                           "viseme" if ext == ".json" else "misc")
        # stable name: keep the basename (cache names are already content-addressed)
        dstrel = f"{root_rel}/{sub}/{os.path.basename(src)}"
        dst = os.path.join(projdir, dstrel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(dst):
            shutil.copy2(src, dst); n_copied += 1
        return dstrel

    for ak, a in d.get("assets", {}).items():
        if a.get("uri"): a["uri"] = localize(a["uri"])
        # video assets also carry a `src`/`proxy` that must follow
        for k in ("src", "proxy"):
            if isinstance(a.get(k), str) and a[k]:
                a[k] = localize(a[k], sub_hint="video")
    json.dump(d, open(proj, "w", encoding="utf-8", newline=""), ensure_ascii=False, indent=2)
    print(f"localized {proj}: copied {n_copied} files into {projdir}/{root_rel}/ (stock refs left in place)")

if __name__ == "__main__":
    main()
