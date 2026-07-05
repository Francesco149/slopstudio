#!/usr/bin/env python3
"""provision.py — build a golden Windows-XP qcow2 by an UNATTENDED SP3 install.

No pre-existing VM image exists, so we install fresh from the local SP3 retail ISO
(the memory's stated fallback). Reproducible: a winnt.sif answer file + a first-logon
setup.cmd (firewall off, blank-pw network logon, staged probe tools) ride in on a
1.44MB floppy; XP text-mode setup auto-reads A:\\winnt.sif.

Flow (run inside `nix develop .#xp`):
  python tools/xp/provision.py install        # build media + run the install + monitor
  python tools/xp/provision.py build-media    # just create the qcow2 + floppy
The install is long (~10-20 min under KVM) → run it in the background and watch the
periodic cache/xp/install-*.png shots. Completion signal = the guest answers SMB auth
(which only happens after install + first logon + setup.cmd has turned the firewall off).

The result, cache/xp/xp.qcow2, becomes the golden image xpvm.py boots via an overlay.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xpvm  # noqa: E402
import xpsmb  # noqa: E402

ROOT = xpvm.ROOT
VMDIR = xpvm.DEFAULT_VMDIR
WIN_DIR = os.path.join(ROOT, "tools", "xp", "win")
ANSWER_DIR = os.path.join(ROOT, "tools", "xp", "answer")
KEY_RE = re.compile(r"^[A-Z0-9]{5}(-[A-Z0-9]{5}){4}$")
PROBE_EXES = ["iexec.exe", "winrect.exe", "bitblt.exe", "mascot.exe"]


def find_iso(explicit=None):
    if explicit:
        return os.path.abspath(explicit)
    for pat in ("/opt/iso/sp3-cd-retail/*.iso",
                os.path.expanduser("~/iso/**/*xp*sp3*.iso")):
        hits = glob.glob(pat, recursive=True)
        if hits:
            return os.path.abspath(hits[0])
    raise SystemExit("provision: no SP3 ISO found — pass --iso PATH")


def find_product_key(iso, explicit=None):
    if explicit:
        return explicit
    # the retail tree stashes the key as a sibling directory named after it
    d = os.path.dirname(iso)
    for name in os.listdir(d):
        if KEY_RE.match(name):
            return name
    raise SystemExit("provision: product key not found next to the ISO — pass --product-key")


def _crlf(path, text):
    with open(path, "wb") as f:
        f.write(text.replace("\n", "\r\n").encode("ascii", "replace"))


def build_floppy(product_key, computer_name="SLOPXP"):
    """Create cache/xp/unattend.img (FAT12, 1.44MB) with the filled winnt.sif +
    setup.cmd (CRLF) + the staged probe exes."""
    os.makedirs(VMDIR, exist_ok=True)
    for e in PROBE_EXES:
        if not os.path.exists(os.path.join(WIN_DIR, e)):
            raise SystemExit(f"provision: {e} not built — run `make -C tools/xp/win` first")

    img = os.path.join(VMDIR, "unattend.img")
    staged = os.path.join(VMDIR, "_floppy")
    os.makedirs(staged, exist_ok=True)

    tmpl = open(os.path.join(ANSWER_DIR, "winnt.sif.tmpl")).read()
    sif = tmpl.replace("@PRODUCT_KEY@", product_key).replace("@COMPUTER_NAME@", computer_name)
    _crlf(os.path.join(staged, "winnt.sif"), sif)
    _crlf(os.path.join(staged, "setup.cmd"), open(os.path.join(ANSWER_DIR, "setup.cmd")).read())
    for e in PROBE_EXES:
        subprocess.run(["cp", os.path.join(WIN_DIR, e), os.path.join(staged, e)], check=True)

    with open(img, "wb") as f:           # 1.44MB blank image
        f.truncate(1474560)
    subprocess.run(["mformat", "-f", "1440", "-v", "SLOPXP", "-i", img, "::"], check=True)
    files = [os.path.join(staged, "winnt.sif"), os.path.join(staged, "setup.cmd")]
    files += [os.path.join(staged, e) for e in PROBE_EXES]
    subprocess.run(["mcopy", "-i", img, "-o"] + files + ["::"], check=True)
    print(f"provision: built {img}")
    # show the floppy contents
    subprocess.run(["mdir", "-i", img, "::"])
    return img


def build_disk(size="12G"):
    os.makedirs(VMDIR, exist_ok=True)
    disk = os.path.join(VMDIR, "xp.qcow2")
    if os.path.exists(disk):
        print(f"provision: {disk} exists — reusing (delete it to start clean)")
        return disk
    subprocess.run(["qemu-img", "create", "-f", "qcow2", disk, size],
                   check=True, capture_output=True)
    print(f"provision: created {disk} ({size})")
    return disk


def install(args):
    iso = find_iso(args.iso)
    key = find_product_key(iso, args.product_key)
    print(f"provision: ISO={iso}\nprovision: product-key={key[:5]}-…")
    disk = build_disk(args.size)
    floppy = build_floppy(key, args.computer_name)

    vm = xpvm.XpVm(disk=disk, vmdir=VMDIR, ram_mb=args.ram, smb_port=args.smb_port,
                   vga="cirrus", nic="rtl8139", overlay=False, cdrom=iso, fda=floppy,
                   boot="once=d", vnc=args.vnc)
    vm.start()
    print(f"provision: installer booting — qmp={vm.sock_path}"
          f"{' vnc='+args.vnc if args.vnc else ''}")

    # 1) get past "Press any key to boot from CD..."
    for _ in range(10):
        try:
            vm.qmp.send_keys(["ret"])
        except Exception:
            pass
        time.sleep(1.5)

    # 2) monitor: periodic shots + poll SMB auth (the completion signal)
    vnc_port = (5900 + int(args.vnc.lstrip(":"))) if args.vnc else None
    smb = xpsmb.XpSmb(port=args.smb_port)
    deadline = time.time() + args.timeout * 60
    i = 0
    while time.time() < deadline:
        shot = os.path.join(VMDIR, f"install-{i:03d}.png")
        try:
            vm.qmp.screendump(shot)
            print(f"provision: [{i:03d}] {time.strftime('%H:%M:%S')} shot {os.path.getsize(shot)}B")
        except Exception as e:
            print(f"provision: screendump failed: {e}")
        # nudge past first-logon modals — the "adjust resolution" Display Settings dialog
        # blocks first-logon (and thus the [GuiRunOnce] A:\setup.cmd that bakes in the
        # prereqs) until its default OK is pressed. Enter dismisses it; harmless otherwise.
        try:
            vm.qmp.send_keys(["ret"])
        except Exception:
            pass
        # …and click its OK via VNC: the dialog has no keyboard focus pre-shell so Enter
        # can't clear it; an "active" RFB client click does. (The [Display] answer-file
        # section should prevent it appearing at all — this is belt-and-suspenders.)
        if vnc_port:
            try:
                from vnckey import Rfb
                r = Rfb("127.0.0.1", vnc_port).connect(); r.click(320, 291); r.close()
            except Exception:
                pass
        # once the OS is up, setup.cmd drops the firewall → SMB answers
        if i >= 4:
            try:
                if smb.auth_ok():
                    final = os.path.join(VMDIR, "install-done.png")
                    vm.qmp.screendump(final)
                    print(f"provision: SMB UP — install complete. golden={disk}\nprovision: {final}")
                    if not args.keep_running:
                        vm.stop()
                    return 0
            except Exception:
                pass
        i += 1
        time.sleep(args.interval)

    print("provision: TIMED OUT waiting for the guest to come up — inspect the shots / VNC")
    if not args.keep_running:
        vm.stop()
    return 2


def main(argv=None):
    ap = argparse.ArgumentParser(description="unattended XP SP3 install → golden qcow2")
    sub = ap.add_subparsers(dest="cmd", required=True)

    bm = sub.add_parser("build-media", help="create the qcow2 + unattended floppy only")
    bm.add_argument("--iso"); bm.add_argument("--product-key"); bm.add_argument("--size", default="12G")
    bm.add_argument("--computer-name", default="SLOPXP")

    ins = sub.add_parser("install", help="build media + run the unattended install + monitor")
    ins.add_argument("--iso"); ins.add_argument("--product-key")
    ins.add_argument("--size", default="12G"); ins.add_argument("--computer-name", default="SLOPXP")
    ins.add_argument("--ram", type=int, default=1024)
    ins.add_argument("--smb-port", type=int, default=4445)
    ins.add_argument("--interval", type=int, default=45, help="seconds between progress shots")
    ins.add_argument("--timeout", type=int, default=45, help="overall minutes before giving up")
    ins.add_argument("--vnc", default=None, help="expose a VNC display, e.g. :1")
    ins.add_argument("--keep-running", action="store_true", help="leave the VM up after install")
    args = ap.parse_args(argv)

    if args.cmd == "build-media":
        iso = find_iso(args.iso)
        key = find_product_key(iso, args.product_key)
        build_disk(args.size)
        build_floppy(key, args.computer_name)
        return 0
    if args.cmd == "install":
        return install(args)


if __name__ == "__main__":
    sys.exit(main())
