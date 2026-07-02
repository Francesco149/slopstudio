#!/usr/bin/env python3
"""xpsmb.py — agent-LESS SMB control layer for the XP guest.

Adapted from ../retro-hardware/projects/minkit-en-patch/deploy-xp.sh for a QEMU VM
whose guest SMB:445 is forwarded to a host port (127.0.0.1:<port>).  Three channels,
matching the proven xp-ops cheatsheet:

  exec   nxc -x '<cmd>'                          run as SYSTEM/Administrator (no GUI)
  gui    nxc --exec-method smbexec -x 'iexec …'  launch on the interactive console desktop
  files  smbclient -m NT1 -p <port>              push/get files (force NT1 → XP SMBv1)

netexec + samba are pulled on demand via `nix run`/`nix shell` (not baked into the
harness shell), exactly like deploy-xp.sh.

Library + CLI:
  python tools/xp/xpsmb.py --port 4445 auth
  python tools/xp/xpsmb.py --port 4445 stage tools/xp/win tools/xp/payload/nircmd.exe
  python tools/xp/xpsmb.py --port 4445 exec 'ver'
  python tools/xp/xpsmb.py --port 4445 iexec -w6 'C:\\probe\\nircmd.exe savescreenshotfull C:\\probe\\out\\b.png'
  python tools/xp/xpsmb.py --port 4445 get 'C:\\probe\\out\\b.png' bitblt.png
"""
import argparse
import os
import subprocess
import sys
import time


class XpSmb:
    def __init__(self, host="127.0.0.1", port=445, user="Administrator", pw="",
                 nxc_supports_port=None):
        self.host = host
        self.port = int(port)
        self.user = user
        self.pw = pw
        # netexec's smb protocol historically has no --port; default to assuming
        # not, unless told otherwise (the harness probes once and passes this in).
        self.nxc_supports_port = bool(nxc_supports_port)

    # ── channel: exec / gui (netexec) ─────────────────────────────────────
    def _nxc_base(self, smbexec=False):
        cmd = ["nix", "run", "nixpkgs#netexec", "--", "smb", self.host,
               "-u", self.user, "-p", self.pw]
        if self.port != 445:
            cmd += ["--port", str(self.port)]   # netexec smb supports --port (verified)
        if smbexec:
            cmd += ["--exec-method", "smbexec"]
        return cmd

    def _run(self, argv, timeout=120, check=False):
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        if check and p.returncode != 0:
            raise RuntimeError(f"cmd failed ({p.returncode}): {' '.join(argv)}\n{p.stdout}\n{p.stderr}")
        return p

    def exec(self, cmd, timeout=120):
        """Run <cmd> as SYSTEM/Administrator (no interactive desktop)."""
        return self._run(self._nxc_base() + ["-x", cmd], timeout=timeout)

    def gui(self, cmd, timeout=120):
        """Run <cmd> via smbexec (LocalSystem) — needed for iexec → console desktop."""
        return self._run(self._nxc_base(smbexec=True) + ["-x", cmd], timeout=timeout)

    def iexec(self, cmdline, wait=0, timeout=120):
        """Launch a GUI/console program on the interactive desktop via iexec."""
        w = f"-w{wait} " if wait else ""
        return self.gui(f"C:\\probe\\iexec.exe {w}{cmdline}", timeout=timeout)

    # ── channel: files (smbclient, forced NT1) ────────────────────────────
    def _smb(self, script, timeout=180):
        argv = ["nix", "shell", "nixpkgs#samba", "-c", "smbclient",
                f"//{self.host}/C$", "-U", f"{self.user}%{self.pw}",
                "-p", str(self.port), "-m", "NT1",
                "--option=client min protocol=NT1", "-c", script]
        return self._run(argv, timeout=timeout)

    def put(self, localpath, remote_dir, remote_name=None):
        localpath = os.path.abspath(localpath)
        ld, lf = os.path.dirname(localpath), os.path.basename(localpath)
        rn = remote_name or lf
        return self._smb(f"prompt OFF; lcd {ld}; cd \"{remote_dir}\"; put {lf} {rn}")

    def get(self, remote_path, localpath):
        localpath = os.path.abspath(localpath)
        ld, lf = os.path.dirname(localpath) or ".", os.path.basename(localpath)
        return self._smb(f"lcd {ld}; get \"{remote_path}\" {lf}")

    def mkdir(self, remote_dir):
        # ignore "already exists"
        return self._smb(f"mkdir \"{remote_dir}\"")

    # ── higher-level ──────────────────────────────────────────────────────
    def auth_ok(self):
        p = self.exec("cmd /c echo SLOP_OK", timeout=60)
        return "Pwn3d" in (p.stdout + p.stderr) or "SLOP_OK" in p.stdout

    def wait_smb(self, timeout=900, interval=10):
        """Poll until the guest answers SMB + authenticates (after install/boot)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                if self.auth_ok():
                    return True
            except Exception:
                pass
            time.sleep(interval)
        return False

    def stage_probe(self, win_dir="tools/xp/win", extra=()):
        """Create C:\\probe\\out and push iexec.exe + winrect.exe (+ any extras,
        e.g. nircmd.exe) so the guest is drivable + BitBlt-capturable."""
        self.exec("cmd /c mkdir C:\\probe 2>nul & mkdir C:\\probe\\out 2>nul & echo done")
        for f in ("iexec.exe", "winrect.exe"):
            p = os.path.join(win_dir, f)
            if os.path.exists(p):
                self.put(p, "\\probe")
        for f in extra:
            if os.path.exists(f):
                self.put(f, "\\probe")
        return True


def main(argv=None):
    ap = argparse.ArgumentParser(description="agent-less SMB control for the XP guest")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4445)
    ap.add_argument("--user", default="Administrator")
    ap.add_argument("--pw", default="")
    ap.add_argument("--nxc-port", action="store_true", help="this netexec supports smb --port")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("auth")
    sub.add_parser("wait").add_argument("--timeout", type=int, default=900)
    ep = sub.add_parser("exec"); ep.add_argument("command")
    gp = sub.add_parser("iexec"); gp.add_argument("rest", nargs=argparse.REMAINDER)
    pp = sub.add_parser("put"); pp.add_argument("local"); pp.add_argument("remote_dir"); pp.add_argument("name", nargs="?")
    gtp = sub.add_parser("get"); gtp.add_argument("remote"); gtp.add_argument("local")
    stp = sub.add_parser("stage"); stp.add_argument("win_dir", nargs="?", default="tools/xp/win"); stp.add_argument("extra", nargs="*")
    args = ap.parse_args(argv)

    c = XpSmb(args.host, args.port, args.user, args.pw, nxc_supports_port=args.nxc_port)
    if args.cmd == "auth":
        print("AUTH OK" if args.cmd == "auth" and c.auth_ok() else "AUTH FAIL")
    elif args.cmd == "wait":
        print("UP" if c.wait_smb(timeout=args.timeout) else "TIMEOUT")
    elif args.cmd == "exec":
        p = c.exec(args.command); sys.stdout.write(p.stdout); sys.stderr.write(p.stderr)
    elif args.cmd == "iexec":
        rest = args.rest
        wait = 0
        if rest and rest[0].startswith("-w"):
            wait = int(rest[0][2:] or "0"); rest = rest[1:]
        p = c.iexec(" ".join(rest), wait=wait); sys.stdout.write(p.stdout); sys.stderr.write(p.stderr)
    elif args.cmd == "put":
        p = c.put(args.local, args.remote_dir, args.name); sys.stdout.write(p.stdout)
    elif args.cmd == "get":
        p = c.get(args.remote, args.local); sys.stdout.write(p.stdout)
    elif args.cmd == "stage":
        c.stage_probe(args.win_dir, extra=args.extra); print("staged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
