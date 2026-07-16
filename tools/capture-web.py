#!/usr/bin/env python3
"""Capture a reproducible high-DPI webpage source plus provenance metadata.

Examples:
  nix develop .#webcapture --command python tools/capture-web.py \
    https://example.com/article assets/web/source/article.png

  nix develop .#webcapture --command python tools/capture-web.py \
    URL out.png --dismiss "Reject All" --wait-for "article"

The default 1920x1080 CSS viewport at DPR 2 produces a 3840x2160 bitmap, suitable
for widgets.document zooms.  Capture the source whole; author excerpt crops in the
timeline so a later editorial change never requires a lossy recapture.
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import struct
from datetime import datetime, timezone
from pathlib import Path

from playwright.async_api import TimeoutError as PlaywrightTimeoutError
from playwright.async_api import async_playwright


def png_size(path: Path) -> tuple[int, int]:
    """Read a PNG's IHDR dimensions without adding an image-library dependency."""
    header = path.read_bytes()[:24]
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Playwright did not write a valid PNG: {path}")
    return struct.unpack(">II", header[16:24])


def parse_size(value: str) -> tuple[int, int]:
    try:
        w, h = (int(x) for x in value.lower().split("x", 1))
    except (ValueError, TypeError):
        raise argparse.ArgumentTypeError("expected WIDTHxHEIGHT, for example 1920x1080")
    if w < 320 or h < 240:
        raise argparse.ArgumentTypeError("viewport must be at least 320x240")
    return w, h


async def dismiss_text(page, label: str, timeout_s: float = 5.0) -> bool:
    """Click an exact consent/modal label in the page or any child frame.

    Consent managers commonly inject after network-idle, so poll rather than
    taking one immediate snapshot of the DOM.
    """
    deadline = asyncio.get_running_loop().time() + timeout_s
    while asyncio.get_running_loop().time() < deadline:
        for root in [page, *page.frames]:
            for locator in (root.get_by_role("button", name=label, exact=True),
                            root.get_by_role("link", name=label, exact=True),
                            root.get_by_text(label, exact=True),
                            root.locator(f"button:has-text({json.dumps(label)})")):
                try:
                    if await locator.count() and await locator.first.is_visible():
                        await locator.first.click(timeout=1500, force=True)
                        return True
                except PlaywrightTimeoutError:
                    pass
        await page.wait_for_timeout(250)
    return False


async def capture(args) -> None:
    width, height = args.viewport
    out = Path(args.output).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)

    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True)
        context = await browser.new_context(
            viewport={"width": width, "height": height},
            device_scale_factor=args.dpr,
            color_scheme=args.color_scheme,
            locale=args.locale,
        )
        page = await context.new_page()
        response = await page.goto(args.url, wait_until=args.wait_until,
                                   timeout=args.timeout * 1000)
        clicked = []
        for label in args.dismiss:
            if await dismiss_text(page, label, args.dismiss_timeout):
                clicked.append(label)
                await page.wait_for_timeout(500)
        if args.wait_for:
            await page.locator(args.wait_for).first.wait_for(
                state="visible", timeout=args.timeout * 1000)
        await page.wait_for_timeout(round(args.settle * 1000))

        if args.selector:
            await page.locator(args.selector).first.screenshot(path=str(out))
        else:
            await page.screenshot(path=str(out), full_page=args.full_page)

        output_width, output_height = png_size(out)
        meta = {
            "schema": "slopstudio.web-capture/v1",
            "captured_at": datetime.now(timezone.utc).isoformat(),
            "requested_url": args.url,
            "final_url": page.url,
            "title": await page.title(),
            "http_status": response.status if response else None,
            "viewport_css_px": {"width": width, "height": height},
            "device_scale_factor": args.dpr,
            "output_px": {"width": output_width, "height": output_height},
            "full_page": args.full_page,
            "selector": args.selector,
            "wait_for": args.wait_for,
            "dismiss_requested": args.dismiss,
            "dismiss_clicked": clicked,
            "color_scheme": args.color_scheme,
            "locale": args.locale,
            "sha256": hashlib.sha256(out.read_bytes()).hexdigest(),
        }
        await browser.close()

    sidecar = out.with_suffix(out.suffix + ".capture.json")
    sidecar.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n")
    print(f"captured {meta['final_url']} -> {out}")
    print(f"provenance -> {sidecar}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url")
    ap.add_argument("output")
    ap.add_argument("--viewport", type=parse_size, default=parse_size("1920x1080"))
    ap.add_argument("--dpr", type=float, default=2.0)
    ap.add_argument("--full-page", action="store_true",
                    help="capture the full document height (default: one stable viewport)")
    ap.add_argument("--selector", help="capture one element instead of the page viewport")
    ap.add_argument("--wait-for", help="CSS selector that must become visible before capture")
    ap.add_argument("--dismiss", action="append", default=[], metavar="TEXT",
                    help="exact consent/modal text to click; repeat as needed")
    ap.add_argument("--dismiss-timeout", type=float, default=5.0,
                    help="seconds to wait for each late-injected consent label")
    ap.add_argument("--settle", type=float, default=1.5, help="seconds to settle after load/dismiss")
    ap.add_argument("--timeout", type=int, default=45, help="navigation/selector timeout in seconds")
    ap.add_argument("--wait-until", choices=["commit", "domcontentloaded", "load", "networkidle"],
                    default="networkidle")
    ap.add_argument("--color-scheme", choices=["light", "dark", "no-preference"], default="light")
    ap.add_argument("--locale", default="en-US")
    args = ap.parse_args()
    if args.dpr < 1 or args.dpr > 4:
        ap.error("--dpr must be between 1 and 4")
    asyncio.run(capture(args))


if __name__ == "__main__":
    main()
