#!/usr/bin/env python3
"""vnckey.py — minimal RFB (VNC) input client: send a keypress or a click.

The harness drives the guest via QMP, but while a long-running owner (e.g.
provision.py) holds the single QMP socket, QMP is unavailable. QEMU's VNC display
is an independent input path, so this tiny pure-stdlib RFB client can still poke the
guest — handy for dismissing a first-boot modal (the XP "adjust resolution" dialog)
or letting input through while another process monitors.

Usage (the VM must be booted with a VNC display, e.g. xpvm --vnc :1 → port 5901):
  python tools/xp/vnckey.py --port 5901 key enter
  python tools/xp/vnckey.py --port 5901 key esc
  python tools/xp/vnckey.py --port 5901 click 318 291
"""
import argparse
import socket
import struct
import sys
import time

# X keysyms for the keys we need
KEYSYMS = {
    "enter": 0xFF0D, "return": 0xFF0D, "esc": 0xFF1B, "escape": 0xFF1B,
    "tab": 0xFF09, "space": 0x0020, "backspace": 0xFF08,
    "up": 0xFF52, "down": 0xFF54, "left": 0xFF51, "right": 0xFF53,
    "f8": 0xFFC5, "y": 0x0079, "n": 0x006E,
}


class Rfb:
    def __init__(self, host="127.0.0.1", port=5901):
        self.host, self.port = host, port
        self.s = None
        self.w = self.h = 0

    def connect(self):
        self.s = socket.create_connection((self.host, self.port), timeout=10)
        ver = self._recv(12)                       # "RFB 003.00x\n"
        self.s.sendall(b"RFB 003.008\n")
        ntypes = self._recv(1)[0]
        if ntypes == 0:
            reason = self._recv(struct.unpack(">I", self._recv(4))[0])
            raise RuntimeError(f"VNC connection failed: {reason!r}")
        types = self._recv(ntypes)
        if 1 not in types:                          # 1 = None (no auth)
            raise RuntimeError(f"VNC needs auth (types={list(types)}); expected None")
        self.s.sendall(bytes([1]))                  # select None
        res = struct.unpack(">I", self._recv(4))[0]  # SecurityResult
        if res != 0:
            raise RuntimeError("VNC SecurityResult != OK")
        self.s.sendall(bytes([1]))                  # ClientInit: shared=1
        init = self._recv(24)                       # ServerInit fixed part
        self.w, self.h = struct.unpack(">HH", init[0:4])
        namelen = struct.unpack(">I", init[20:24])[0]
        if namelen:
            self._recv(namelen)
        # Become an "active" client: some servers (QEMU) only process injected input
        # after the client has declared encodings + requested a framebuffer update.
        self.s.sendall(struct.pack(">BBH", 2, 0, 1) + struct.pack(">i", 0))   # SetEncodings: Raw
        self.s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, 1, 1))              # tiny FramebufferUpdateRequest
        return self

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("VNC connection closed")
            buf += chunk
        return buf

    def key(self, keysym, down):
        # KeyEvent: type=4, down-flag, padding(2), keysym(4)
        self.s.sendall(struct.pack(">BBHI", 4, 1 if down else 0, 0, keysym))

    def tap(self, keysym, hold=0.05):
        self.key(keysym, True); time.sleep(hold); self.key(keysym, False)

    def pointer(self, x, y, buttons=0):
        # PointerEvent: type=5, button-mask, x(2), y(2)
        self.s.sendall(struct.pack(">BBHH", 5, buttons, x, y))

    def click(self, x, y, hold=0.05):
        self.pointer(x, y, 0)
        self.pointer(x, y, 1); time.sleep(hold); self.pointer(x, y, 0)

    def close(self):
        if self.s:
            self.s.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description="send a VNC keypress or click")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5901)
    sub = ap.add_subparsers(dest="cmd", required=True)
    kp = sub.add_parser("key"); kp.add_argument("name")
    cp = sub.add_parser("click"); cp.add_argument("x", type=int); cp.add_argument("y", type=int)
    args = ap.parse_args(argv)

    r = Rfb(args.host, args.port).connect()
    try:
        if args.cmd == "key":
            ks = KEYSYMS.get(args.name.lower())
            if ks is None:
                ks = ord(args.name) if len(args.name) == 1 else None
            if ks is None:
                sys.exit(f"unknown key: {args.name}")
            r.tap(ks)
            print(f"vnckey: sent {args.name} ({r.w}x{r.h})")
        elif args.cmd == "click":
            r.click(args.x, args.y)
            print(f"vnckey: clicked {args.x},{args.y} ({r.w}x{r.h})")
    finally:
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
