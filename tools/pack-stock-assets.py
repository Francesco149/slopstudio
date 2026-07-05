#!/usr/bin/env python3
"""Package the STOCK asset pack for a release: the newest host rig + the shared backgrounds.

The public code master ships NO art — a fresh checkout fetches `stock-assets.zip` from the
release and unpacks it into place (the editor does this on first run; see stock_assets_ready
in editor/src/main.cpp). This is the tool that BUILDS that zip.

    nix develop --command python tools/pack-stock-assets.py            # -> dist/stock-assets.zip
    nix develop --command python tools/pack-stock-assets.py --verify   # list what a checkout is missing

Keep STOCK in sync with tools/localize-project.py (its STOCK_PREFIXES/STOCK_FILES decide
what a project bundle leaves out because the pack provides it).
"""
import os, sys, zipfile

# Paths (relative to repo root) that make up the stock pack. Preserved verbatim in the zip so
# unzipping at the repo root drops them exactly where projects reference them.
STOCK = [
    "presets/avatars/gemma-big",      # the newest host rig (the golden one)
    "presets/avatars/gemma-pngtuber", # the SD single-pose rig (signature-opener/opening self-demos)
    "presets/backgrounds",            # the shared room day/dark backdrops
    "library/images",                 # seeded reaction sprites + the desk outro backdrop
    "library/audio",                  # seeded stingers/sfx
    "library/sfx",                    # built-in transition SFX (regenerable: tools/gen-sfx.py)
]
OUT = "dist/stock-assets.zip"

def iter_files(path):
    if os.path.isfile(path):
        yield path
    elif os.path.isdir(path):
        for root, _, files in os.walk(path):
            for f in sorted(files):
                yield os.path.join(root, f)

def verify():
    missing = [p for p in STOCK if not os.path.exists(p)]
    if missing:
        print("MISSING stock assets (run pack, then unzip / first-run fetch):")
        for m in missing: print("  ", m)
        sys.exit(1)
    print("stock assets present.")

def main():
    if "--verify" in sys.argv:
        verify(); return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    n = 0; total = 0
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for base in STOCK:
            if not os.path.exists(base):
                print(f"  WARN missing: {base}"); continue
            for f in iter_files(base):
                z.write(f, f); n += 1; total += os.path.getsize(f)
    print(f"packed {n} files ({total/1e6:.1f} MB raw) -> {OUT} ({os.path.getsize(OUT)/1e6:.1f} MB)")

if __name__ == "__main__":
    main()
