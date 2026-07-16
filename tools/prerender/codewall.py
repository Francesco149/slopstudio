#!/usr/bin/env python3
"""codewall — bake a big syntax-highlighted "poster" of a source file: many tiny
columns of code, One-Dark palette, the way a coverage-maxxing video shows an entire
program at once.  Emits ONE PNG + a JSON sidecar that maps line numbers (and named
functions) to normalized (x,y,w,h) rects inside the poster.

The sidecar rects are what the reusable `widgets.codewall` scene widget pans/zooms
between: start tight on one function, ACCELERATE out to the whole wall (the b05 reveal),
or the reverse to zoom back IN to a specific function (b51).

    codewall.py FILE.c --out wall.png --sidecar wall.json \
        --start-line 74000 --end-line 82000 --cols 9 \
        --mark grant=74941 --mark wage=80746

Reusable for any decompiled/source file — nothing game-specific here.
"""
from __future__ import annotations
import argparse, json, re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

# One Dark (matches the editor code card / theme.code in std.lua)
BG      = (26, 30, 38)       # #1a1e26 wall backdrop
CARD    = (18, 21, 28)
DEFAULT = (171, 178, 191)    # #abb2bf
KEYWORD = (198, 120, 221)    # #c678dd purple
TYPE    = (229, 192, 123)    # #e5c07b yellow
STRING  = (152, 195, 121)    # #98c379 green
NUMBER  = (209, 154, 102)    # #d19a66 orange
COMMENT = (92, 99, 112)      # #5c6370 grey
SYMBOL  = (97, 175, 239)     # #61afef blue (FUN_/DAT_ handles)
FUNCDEF = (86, 182, 194)     # #56b6c2 cyan (function definitions)

MONO = "/nix/store/azl1xb5c1jcmq99jjl1fwhqrxn5q4i0z-source/assets/fonts/JetBrainsMono-Regular.ttf"

C_KW = set("if else for while do return switch case break continue goto default sizeof "
           "struct union enum typedef static const volatile extern register auto void".split())
C_TY = set("int char short long float double bool unsigned signed uint undefined undefined1 "
           "undefined2 undefined4 undefined8 byte word dword int8_t int16_t int32_t int64_t "
           "uint8_t uint16_t uint32_t uint64_t code".split())

TOK = re.compile(r"//.*|/\*|\*/|\"(?:\\.|[^\"])*\"|'(?:\\.|[^'])*'|[A-Za-z_]\w*|0x[0-9A-Fa-f]+|\d+|\s+|.")


def _color(tok, in_block):
    if in_block:
        return COMMENT
    if tok.startswith("//") or tok.startswith("/*"):
        return COMMENT
    if tok.startswith('"') or tok.startswith("'"):
        return STRING
    if tok[0].isdigit() or tok.startswith("0x"):
        return NUMBER
    if tok.startswith("FUN_") or tok.startswith("DAT_") or tok.startswith("_DAT_"):
        return SYMBOL
    if tok in C_KW:
        return KEYWORD
    if tok in C_TY:
        return TYPE
    return DEFAULT


def render(lines, cols, char_px, out_png, sidecar, marks, title):
    # metrics
    fh = char_px + 2                    # line height
    fw = char_px * 0.60                 # mono advance
    font = ImageFont.truetype(MONO, char_px)
    per_col = (len(lines) + cols - 1) // cols
    max_chars = 96                      # clamp very long decomp lines
    col_w = int(fw * max_chars) + int(char_px * 1.6)
    col_h = per_col * fh
    pad = int(char_px * 2.2)
    W = cols * col_w + pad * 2
    H = col_h + pad * 2 + int(char_px * 3)
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([pad * 0.4, pad * 0.4, W - pad * 0.4, H - pad * 0.4], outline=(44, 50, 62), width=2)

    # line -> (col, row within col)
    def place(gidx):
        c = gidx // per_col
        r = gidx - c * per_col
        x0 = pad + c * col_w
        y0 = pad + int(char_px * 2.4) + r * fh
        return x0, y0

    in_block = False
    for gidx, raw in enumerate(lines):
        x0, y0 = place(gidx)
        x = x0
        s = raw.rstrip("\n")[:max_chars]
        for m in TOK.finditer(s):
            tok = m.group(0)
            if tok == "/*":
                in_block = True; col = COMMENT
            elif tok == "*/":
                col = COMMENT
            elif tok.isspace():
                x += fw * len(tok);
                if "\n" in tok: pass
                continue
            else:
                col = _color(tok, in_block)
            d.text((x, y0), tok, font=font, fill=col)
            x += fw * len(tok)
            if tok == "*/":
                in_block = False
        # comments end at newline
        if in_block and "//" in s:
            pass

    # title strip (top-left, faint — reads as a chrome caption; the camera usually
    # starts BELOW it, so it doesn't spoil the reveal)
    if title:
        tf = ImageFont.truetype(MONO, int(char_px * 1.6))
        d.text((pad, int(pad * 0.5)), title, font=tf, fill=(70, 78, 92))

    img.save(out_png)

    # sidecar: normalized rects. A "mark" is a function whose tight rect frames its
    # header + first ~14 lines; also emit the whole-wall rect.
    rects = {"_full": [0, 0, 1, 1]}
    for name, gidx in marks.items():   # marks are already slice-relative 0-based indices
        x0, y0 = place(gidx)
        rw = col_w
        rh = fh * 16
        rects[name] = [round(x0 / W, 5), round((y0 - fh) / H, 5),
                       round(rw / W, 5), round(rh / H, 5)]
    Path(sidecar).write_text(json.dumps({"image_w": W, "image_h": H, "rects": rects}, indent=1))
    print(f"codewall: {out_png} {W}x{H} ({cols} cols x {per_col} lines)  marks={list(marks)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sidecar", required=True)
    ap.add_argument("--start-line", type=int, default=1)
    ap.add_argument("--end-line", type=int, default=None)
    ap.add_argument("--cols", type=int, default=9)
    ap.add_argument("--char-px", type=int, default=13)
    ap.add_argument("--title", default="")
    ap.add_argument("--mark", action="append", default=[], help="name=absLine (repeatable)")
    a = ap.parse_args()
    all_lines = Path(a.file).read_text(errors="replace").splitlines()
    s = max(0, a.start_line - 1)
    e = a.end_line or len(all_lines)
    lines = all_lines[s:e]
    marks = {}
    for mk in a.mark:
        name, _, ln = mk.partition("=")
        marks[name] = int(ln) - 1 - s          # slice-relative 0-based
    render(lines, a.cols, a.char_px, a.out, a.sidecar, marks, a.title)


if __name__ == "__main__":
    main()
