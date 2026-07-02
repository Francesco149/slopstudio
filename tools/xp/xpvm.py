#!/usr/bin/env python3
"""xpvm.py — host-side QEMU driver for the slopstudio XP capture harness.

Boots a Windows-XP guest under KVM with a QMP control socket and exposes the
primitives the capture choreography needs:

  • screendump — capture the LIVE framebuffer host-side, straight off QEMU's VGA
                 surface.  This sees per-pixel-alpha LAYERED windows that an
                 in-guest GDI BitBlt cannot — it is the GROUND-TRUTH side of the
                 BitBlt-vs-screenshot demo (and it never goes black: no physical
                 monitor to fall asleep, unlike the real XP boxes).
  • input      — absolute mouse move/click + key/text via QMP (a usb-tablet gives
                 absolute coordinates, so choreography can target a screen pixel).
  • snapshots  — savevm/loadvm so a choreography can reset to a known desktop.

Networking is user-mode slirp with a host→guest forward to SMB:445, so the
agent-less control layer (tools/xp/xpsmb.py — netexec + smbclient + iexec) can
push files and launch GUI apps on the guest's interactive desktop.

Two classes:
  Qmp   — a minimal QMP client; works against ANY running QEMU's QMP unix socket.
  XpVm  — manages the qemu-system-x86_64 process + an embedded Qmp.

Run inside `nix develop` (provides qemu + pillow). Library + CLI — see `--help`.

  # boot (stays foreground; background it with the harness or a shell '&'):
  nix develop --command python tools/xp/xpvm.py boot --disk cache/xp/xp.qcow2 --fresh
  # then, from another shell, drive the already-running VM via its QMP socket:
  nix develop --command python tools/xp/xpvm.py --connect cache/xp/qmp.sock shot a.png
  nix develop --command python tools/xp/xpvm.py --connect cache/xp/qmp.sock move 400 300
  nix develop --command python tools/xp/xpvm.py --connect cache/xp/qmp.sock click
  nix develop --command python tools/xp/xpvm.py --connect cache/xp/qmp.sock type "hello"
  nix develop --command python tools/xp/xpvm.py --connect cache/xp/qmp.sock snapshot clean
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.environ.get("SLOPSTUDIO_ROOT") or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_VMDIR = os.path.join(ROOT, "cache", "xp")


# ── ASCII → QMP qcode mapping (for type_text) ─────────────────────────────────
# QEMU key qcodes (a subset, enough for paths/commands). Uppercase + shifted
# symbols are sent as [shift, key].
_QCODE = {
    "a": "a", "b": "b", "c": "c", "d": "d", "e": "e", "f": "f", "g": "g",
    "h": "h", "i": "i", "j": "j", "k": "k", "l": "l", "m": "m", "n": "n",
    "o": "o", "p": "p", "q": "q", "r": "r", "s": "s", "t": "t", "u": "u",
    "v": "v", "w": "w", "x": "x", "y": "y", "z": "z",
    "0": "0", "1": "1", "2": "2", "3": "3", "4": "4",
    "5": "5", "6": "6", "7": "7", "8": "8", "9": "9",
    " ": "spc", "\t": "tab", "\n": "ret",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    "\\": "backslash", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    ",": "comma", ".": "dot", "/": "slash",
}
# characters that require Shift, mapped to the unshifted qcode
_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
    "{": "bracket_left", "}": "bracket_right", "|": "backslash",
    ":": "semicolon", '"': "apostrophe", "~": "grave_accent",
    "<": "comma", ">": "dot", "?": "slash",
}


def char_to_keys(ch):
    """Return the list of qcodes to press for one character, or None if unmapped."""
    if ch in _QCODE:
        return [_QCODE[ch]]
    if ch.isupper() and ch.lower() in _QCODE:
        return ["shift", _QCODE[ch.lower()]]
    if ch in _SHIFTED:
        return ["shift", _SHIFTED[ch]]
    return None


class QmpError(RuntimeError):
    pass


class Qmp:
    """Minimal QMP client over a unix socket (no external deps)."""

    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None
        self.f = None
        self.events = []
        self._fbsize = None  # cached (w, h) from the last screendump

    def connect(self, timeout=60.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.sock_path)
                self.sock = s
                self.f = s.makefile("rwb", buffering=0)
                break
            except (FileNotFoundError, ConnectionRefusedError, OSError) as e:
                last = e
                time.sleep(0.25)
        else:
            raise QmpError(f"QMP socket {self.sock_path} not ready: {last}")
        # greeting, then negotiate capabilities
        self._readline_json()  # {"QMP": {...}}
        self.execute("qmp_capabilities")
        return self

    def _readline_json(self):
        line = self.f.readline()
        if not line:
            raise QmpError("QMP connection closed")
        return json.loads(line.decode("utf-8"))

    def execute(self, command, **arguments):
        msg = {"execute": command}
        if arguments:
            msg["arguments"] = arguments
        self.f.write((json.dumps(msg) + "\r\n").encode("utf-8"))
        while True:
            obj = self._readline_json()
            if "event" in obj:
                self.events.append(obj)
                continue
            if "error" in obj:
                raise QmpError(f"{command}: {obj['error'].get('class')}: {obj['error'].get('desc')}")
            if "return" in obj:
                return obj["return"]
            # unknown line — keep reading

    def hmc(self, command_line):
        """Run a Human Monitor Command (for savevm/loadvm etc.)."""
        return self.execute("human-monitor-command", **{"command-line": command_line})

    # ── vCPU run-state (deterministic capture) ──────────────────────────────
    # QMP `stop`/`cont` freeze/thaw the guest vCPUs. screendump on a STOPPED guest
    # reads the framebuffer without the guest running — so a dump can't steal guest
    # time the way a dump on a RUNNING guest does (the documented starvation). Freeze
    # → dump → thaw → run a fixed wall-slice ⇒ frames are evenly spaced in guest-time
    # regardless of how long each dump takes. Under KVM the guest clock is paused while
    # stopped, so the guest never "sees" the dump latency.
    def pause(self):
        """Freeze the guest vCPUs (QMP `stop`)."""
        return self.execute("stop")

    def resume(self):
        """Thaw the guest vCPUs (QMP `cont`)."""
        return self.execute("cont")

    # ── capture ───────────────────────────────────────────────────────────
    def screendump(self, path):
        """Capture the framebuffer to PNG. Tries QMP png format, falls back to
        PPM + Pillow conversion for older QEMU."""
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        try:
            self.execute("screendump", filename=path, format="png")
        except QmpError:
            ppm = path + ".ppm"
            self.execute("screendump", filename=ppm)
            from PIL import Image
            Image.open(ppm).save(path)
            os.remove(ppm)
        # cache framebuffer size for absolute mouse mapping
        try:
            from PIL import Image
            with Image.open(path) as im:
                self._fbsize = im.size
        except Exception:
            pass
        return path

    def fb_size(self):
        if self._fbsize is None:
            tmp = os.path.join(DEFAULT_VMDIR, "_fbprobe.png")
            self.screendump(tmp)
        return self._fbsize

    # ── keyboard ──────────────────────────────────────────────────────────
    def send_keys(self, qcodes, hold_ms=0):
        """Press a chord of qcodes simultaneously (e.g. ['ctrl','alt','delete'])."""
        keys = [{"type": "qcode", "data": k} for k in qcodes]
        args = {"keys": keys}
        if hold_ms:
            args["hold-time"] = hold_ms
        self.execute("send-key", **args)

    def type_text(self, text, per_key_ms=12):
        """Type a string into the guest, char by char."""
        for ch in text:
            ks = char_to_keys(ch)
            if ks is None:
                continue
            self.send_keys(ks)
            if per_key_ms:
                time.sleep(per_key_ms / 1000.0)

    # ── mouse (absolute, via usb-tablet) ──────────────────────────────────
    def move(self, x, y):
        """Move the absolute pointer to screen pixel (x, y)."""
        w, h = self.fb_size()
        ax = max(0, min(32767, round(x / max(1, w - 1) * 32767)))
        ay = max(0, min(32767, round(y / max(1, h - 1) * 32767)))
        self.execute("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ])

    def button(self, down, btn="left"):
        self.execute("input-send-event", events=[
            {"type": "btn", "data": {"down": bool(down), "button": btn}},
        ])

    def click(self, btn="left", double=False, settle_ms=40):
        self.button(True, btn); time.sleep(settle_ms / 1000.0); self.button(False, btn)
        if double:
            time.sleep(settle_ms / 1000.0)
            self.button(True, btn); time.sleep(settle_ms / 1000.0); self.button(False, btn)

    # ── savestate ─────────────────────────────────────────────────────────
    def snapshot(self, name):
        return self.hmc(f"savevm {name}")

    def restore(self, name):
        return self.hmc(f"loadvm {name}")

    def quit(self):
        try:
            self.execute("quit")
        except QmpError:
            pass

    def close(self):
        try:
            if self.f:
                self.f.close()
            if self.sock:
                self.sock.close()
        except Exception:
            pass


class XpVm:
    """Manage a qemu-system-x86_64 XP guest + its QMP client."""

    def __init__(self, disk, vmdir=DEFAULT_VMDIR, ram_mb=1024, smb_port=4445,
                 cpu="host", vga="std", nic="rtl8139", vnc=None, fresh=False,
                 overlay=True, cdrom=None, fda=None, boot=None,
                 qemu="qemu-system-x86_64"):
        self.disk = os.path.abspath(disk) if disk else None
        self.vmdir = os.path.abspath(vmdir)
        self.ram_mb = ram_mb
        self.smb_port = smb_port
        self.cpu = cpu
        self.vga = vga
        self.nic = nic
        self.vnc = vnc          # e.g. ":1" to expose a VNC display for eyeballing
        self.fresh = fresh
        self.overlay = overlay  # True: boot a non-destructive overlay; False: boot disk directly
        self.cdrom = os.path.abspath(cdrom) if cdrom else None
        self.fda = os.path.abspath(fda) if fda else None
        self.boot = boot        # qemu -boot arg, e.g. "once=d" (install) / "order=c" (disk)
        self.qemu = qemu
        self.proc = None
        self.qmp = None
        self.sock_path = os.path.join(self.vmdir, "qmp.sock")
        self.boot_disk = None

    def _prepare_disk(self):
        """For capture runs, boot a qcow2 OVERLAY backed by the golden image so the
        golden is never mutated (--fresh recreates it → a clean desktop every run).
        For install/direct (overlay=False), boot the disk itself."""
        if not self.disk:
            return None
        os.makedirs(self.vmdir, exist_ok=True)
        if not self.overlay:
            return self.disk
        overlay = os.path.join(self.vmdir, "overlay.qcow2")
        if os.path.abspath(self.disk) == overlay:
            return self.disk    # already pointed at the overlay
        if self.fresh or not os.path.exists(overlay):
            if os.path.exists(overlay):
                os.remove(overlay)
            subprocess.run(
                ["qemu-img", "create", "-f", "qcow2", "-b", self.disk,
                 "-F", "qcow2", overlay],
                check=True, capture_output=True)
        return overlay

    def argv(self):
        a = [self.qemu, "-name", "slopxp",
             "-machine", "pc,accel=kvm:tcg", "-cpu", self.cpu,
             "-m", str(self.ram_mb),
             "-rtc", "base=localtime",
             "-usb", "-device", "usb-tablet",
             "-vga", self.vga,
             "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{self.smb_port}-:445",
             "-device", f"{self.nic},netdev=n0",
             "-qmp", f"unix:{self.sock_path},server=on,wait=off",
             "-monitor", "none", "-serial", "none"]
        if self.boot_disk:
            a += ["-drive", f"file={self.boot_disk},if=ide,format=qcow2,cache=writeback"]
        if self.cdrom:
            a += ["-cdrom", self.cdrom]
        if self.fda:
            a += ["-fda", self.fda]
        if self.boot:
            a += ["-boot", self.boot]
        elif self.boot_disk and not self.cdrom:
            a += ["-boot", "order=c"]   # boot straight from disk, skip the iPXE attempt
        if self.vnc is not None:
            a += ["-display", f"vnc={self.vnc}"]
        else:
            a += ["-display", "none"]
        return a

    def start(self, qmp_timeout=60.0):
        os.makedirs(self.vmdir, exist_ok=True)
        self.boot_disk = self._prepare_disk()
        if os.path.exists(self.sock_path):
            os.remove(self.sock_path)
        argv = self.argv()
        sys.stderr.write("xpvm: " + " ".join(argv) + "\n")
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # surface an early qemu crash instead of hanging on the QMP connect
        time.sleep(0.3)
        if self.proc.poll() is not None:
            out = self.proc.stdout.read().decode("utf-8", "replace") if self.proc.stdout else ""
            raise RuntimeError(f"qemu exited rc={self.proc.returncode}\n{out}")
        self.qmp = Qmp(self.sock_path).connect(timeout=qmp_timeout)
        return self

    def stop(self, timeout=10.0):
        if self.qmp:
            self.qmp.quit(); self.qmp.close()
        if self.proc:
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


# ── CLI ───────────────────────────────────────────────────────────────────────
def _check_kvm():
    ok = os.access("/dev/kvm", os.R_OK | os.W_OK)
    if not ok:
        sys.stderr.write("xpvm: WARNING /dev/kvm not accessible — falling back to TCG (slow)\n")
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(description="QEMU XP host driver for the capture harness")
    ap.add_argument("--connect", metavar="SOCK", help="drive an already-running VM via its QMP socket")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("boot", help="launch the VM (stays foreground until quit/Ctrl-C)")
    b.add_argument("--disk", required=True, help="golden qcow2 (booted via a fresh overlay) or a working image")
    b.add_argument("--vmdir", default=DEFAULT_VMDIR)
    b.add_argument("--ram", type=int, default=1024)
    b.add_argument("--smb-port", type=int, default=4445)
    b.add_argument("--cpu", default="host")
    b.add_argument("--vga", default="cirrus")   # XP ships a Cirrus driver → 1024x768
    b.add_argument("--nic", default="rtl8139")
    b.add_argument("--vnc", default=None, help="expose a VNC display, e.g. :1 (for eyeballing)")
    b.add_argument("--fresh", action="store_true", help="recreate the overlay (clean desktop)")
    b.add_argument("--no-overlay", action="store_true",
                   help="boot the disk directly (mutates it) instead of a non-destructive overlay")

    sub.add_parser("shot", help="screendump → PNG").add_argument("out")
    sub.add_parser("type").add_argument("text")
    kp = sub.add_parser("key"); kp.add_argument("qcodes", nargs="+", help="chord, e.g. ctrl alt delete")
    mp = sub.add_parser("move"); mp.add_argument("x", type=int); mp.add_argument("y", type=int)
    cp = sub.add_parser("click"); cp.add_argument("button", nargs="?", default="left")
    cp.add_argument("--double", action="store_true")
    snp = sub.add_parser("snapshot"); snp.add_argument("name")
    rsp = sub.add_parser("restore"); rsp.add_argument("name")
    sub.add_parser("quit")
    args = ap.parse_args(argv)

    if args.cmd == "boot":
        _check_kvm()
        vm = XpVm(disk=args.disk, vmdir=args.vmdir, ram_mb=args.ram, smb_port=args.smb_port,
                  cpu=args.cpu, vga=args.vga, nic=args.nic, vnc=args.vnc, fresh=args.fresh,
                  overlay=not args.no_overlay)
        vm.start()
        vm.qmp.close(); vm.qmp = None     # free the QMP socket so `xpvm --connect` can drive it
        sys.stderr.write(f"xpvm: booted — qmp={vm.sock_path} (free for --connect) "
                         f"smb=127.0.0.1:{vm.smb_port}{' vnc='+args.vnc if args.vnc else ''}\n")
        try:
            while vm.proc.poll() is None:
                time.sleep(0.5)
        except KeyboardInterrupt:
            sys.stderr.write("\nxpvm: stopping\n")
            vm.proc.terminate()
        return 0

    # all other subcommands drive an existing VM via --connect
    if not args.connect:
        ap.error("subcommand requires --connect SOCK (the running VM's QMP socket)")
    q = Qmp(args.connect).connect()
    try:
        if args.cmd == "shot":
            print(q.screendump(args.out))
        elif args.cmd == "type":
            q.type_text(args.text)
        elif args.cmd == "key":
            q.send_keys(args.qcodes)
        elif args.cmd == "move":
            q.move(args.x, args.y)
        elif args.cmd == "click":
            q.click(args.button, double=args.double)
        elif args.cmd == "snapshot":
            print(q.snapshot(args.name))
        elif args.cmd == "restore":
            print(q.restore(args.name))
        elif args.cmd == "quit":
            q.quit()
    finally:
        q.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
