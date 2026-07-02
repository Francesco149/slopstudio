#!/usr/bin/env python3
"""Fetch the stock royalty-free music beds into library/music/ from a curated manifest.

The catalogue lives in `library/music/catalogue.json` — a list of tracks, each with a
direct download `url`, a `license` (must be commercial-safe: CC0 / CC-BY / public-domain),
and, for CC-BY, an `attribution` string that the editor surfaces as an on-screen credit
(same path as the Jamendo integration). Non-commercial licenses are REJECTED — this repo's
output is monetized (see docs/RESEARCH.md).

    nix develop --command python tools/fetch-stock-music.py            # fetch all
    nix develop --command python tools/fetch-stock-music.py --list     # show the catalogue

Curation is intentionally left to the channel owner (taste + license vetting). Good
commercial-safe sources as of 2026:
  • CC0 (no attribution): Pixabay Music, OpenGameArt CC0 packs, archive.org PD collections.
  • CC-BY (auto-credited): Kevin MacLeod / Incompetech, the Free Music Archive CC-BY tier.
Style targets for @GemmaExplains: SummoningSalt-flavoured synthwave/retro + chiptune beds.
"""
import json, os, sys, urllib.request

MUSIC_DIR = "library/music"
CATALOGUE = os.path.join(MUSIC_DIR, "catalogue.json")
OK_LICENSES = ("cc0", "cc-by", "cc-by-4.0", "public-domain", "pd")

def load_catalogue():
    if not os.path.exists(CATALOGUE):
        print(f"no catalogue at {CATALOGUE} — add tracks (see the module docstring).")
        return []
    return json.load(open(CATALOGUE, encoding="utf-8")).get("tracks", [])

def main():
    tracks = load_catalogue()
    if "--list" in sys.argv:
        for t in tracks:
            print(f"  {t.get('name','?'):32} {t.get('license','?'):12} {t.get('url','')}")
        print(f"{len(tracks)} tracks.")
        return
    os.makedirs(MUSIC_DIR, exist_ok=True)
    for t in tracks:
        lic = t.get("license", "").lower()
        if lic not in OK_LICENSES:
            print(f"  SKIP (license '{lic}' not commercial-safe): {t.get('name')}"); continue
        name = t["name"]; url = t["url"]
        ext = os.path.splitext(url)[1] or ".mp3"
        dst = os.path.join(MUSIC_DIR, name + ext)
        if os.path.exists(dst):
            print(f"  have: {name}"); continue
        try:
            print(f"  fetch: {name} <- {url}")
            urllib.request.urlretrieve(url, dst)
            # write the CC-BY attribution sidecar the editor turns into a credit
            if lic.startswith("cc-by") and t.get("attribution"):
                json.dump({"meta": {"attribution": {"attribution_text": t["attribution"]}}},
                          open(dst + ".meta.json", "w", encoding="utf-8"), indent=2)
        except Exception as e:
            print(f"    FAILED: {e}")
    print("done.")

if __name__ == "__main__":
    main()
