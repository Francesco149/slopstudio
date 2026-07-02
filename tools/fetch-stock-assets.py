#!/usr/bin/env python3
"""Fetch + unpack the stock asset pack (host rig + backgrounds) into place.

The public code master ships no art. On a fresh checkout this pulls `stock-assets.zip` from
the GitHub release and unzips it at the repo root, so `presets/avatars/gemma-big` and the
backgrounds land exactly where projects reference them. Idempotent — a no-op once present.

    nix develop --command python tools/fetch-stock-assets.py           # fetch if missing
    nix develop --command python tools/fetch-stock-assets.py --force   # re-fetch

Source: the release asset URL (override with $SLOP_STOCK_URL). Default points at the repo's
own `nightly`/`stock` release; a local dist/stock-assets.zip is used first if present.
"""
import os, sys, zipfile, tempfile, urllib.request

SENTINEL = "presets/avatars/gemma-big/manifest.json"   # present ⇒ stock is unpacked
LOCAL = "dist/stock-assets.zip"
URL = os.environ.get(
    "SLOP_STOCK_URL",
    "https://github.com/Francesco149/slopstudio/releases/download/stock/stock-assets.zip",
)

def unpack(zip_path):
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(".")
    print(f"unpacked {zip_path} -> repo root")

def main():
    force = "--force" in sys.argv
    if os.path.exists(SENTINEL) and not force:
        print("stock assets already present."); return
    if os.path.exists(LOCAL):
        unpack(LOCAL); return
    print(f"fetching stock-assets.zip <- {URL}")
    try:
        with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as tf:
            urllib.request.urlretrieve(URL, tf.name)
            unpack(tf.name)
        os.unlink(tf.name)
    except Exception as e:
        print(f"FAILED: {e}\nBuild it locally instead: python tools/pack-stock-assets.py")
        sys.exit(1)

if __name__ == "__main__":
    main()
