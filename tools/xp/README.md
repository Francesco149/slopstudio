# XP capture harness (`tools/xp/`)

An autonomously-drivable **Windows-XP VM** that produces capture clips for the videos —
notably the **BitBlt-vs-screenshot demo** for video-001 (a per-pixel-alpha *layered
window* is visible to a real screen capture but invisible to a GDI `BitBlt`).

It reuses the agent-less SMB control model proven on the real XP boxes
(`../retro-hardware/projects/xp-remote-probe`) but targets a QEMU guest, where the
host can capture the framebuffer and inject input directly via **QMP** — no physical
monitor to fall asleep, and the layered window is captured with full fidelity.

## Everything runs in the `.#xp` dev shell
Isolated from the editor shell so qemu's closure doesn't tax every `nix develop`:
```sh
nix develop .#xp --command <cmd>      # qemu + i686 cross + python/pillow + mtools
```

## Pieces
| file | role |
|---|---|
| `win/iexec.c` | launch a GUI/console child on the guest's **interactive console desktop** from a session-0 SMB-exec (the key agent-less primitive). i686 PE, subsystem 5.1. |
| `win/winrect.c` | print a window's screen rect (host-side cropping). |
| `win/bitblt.c` | naive **GDI `BitBlt`** screen grab → BMP (the capture that MISSES layered windows). |
| `win/mascot.c` | a `WS_EX_LAYERED` + `UpdateLayeredWindow` floating **sprite** (loads a `png2raw` blob; falls back to a procedural diamond), click-through (`WS_EX_TRANSPARENT`), self-animating bob+drift (`--static` to freeze for an A/B). The video's `Launch.exe` technique. |
| `win/copyanim.c` | **the copy-hook repro** (one process): a worker thread runs `SHFileOperation(FO_COPY)` (raises XP's copy dialog) while the main thread finds *its own* dialog (`#32770` owning a `SysAnimate32`), hides the flying-papers control, and AlphaBlends the mascot across the bar. `--plain` = stock dialog (the "normal" capture). |
| `win/copysim.c` | older split copy trigger (`create`/`copy`/`both` modes) — superseded by `copyanim` for the demo; kept for raising a plain dialog. |
| `win/setres.c` | set the display mode (enumerates supported modes, picks the best of a preference list). Run once/session → 1024×768 (crisper captures + silences XP's "very low resolution" tray balloon that 640×480 triggers). cirrus maxes at 24bpp → it lands 1024×768×16. |
| `win/wlist.c` | diagnostic: dump every visible top-level window + its descendant class tree (used to confirm the copy dialog is `#32770`/`SysAnimate32`). |
| `png2raw.py` | host: PNG → premultiplied-BGRA top-down `.raw` sprite blob (the guest has no PNG decoder) for `mascot`/`copyanim`. |
| `xpvm.py` | host QEMU driver: boot under KVM + QMP; **`screendump`** capture, abs mouse / key / text input, snapshots. `Qmp` works against any running VM's socket. |
| `xpsmb.py` | agent-less SMB control: `exec` (nxc), `gui`/`iexec` (smbexec→console desktop), `files` (smbclient NT1) — against the guest's forwarded SMB port. |
| `provision.py` | build a golden qcow2 by an **unattended SP3 install** (winnt.sif floppy + first-logon `setup.cmd`), monitored over QMP. |
| `choreograph.py` | scripted choreography (mouse paths/clicks/typing/launch/holds) → frame capture → mp4 → slopstudio `video` asset; plus the **BitBlt demo**. |
| `vnckey.py` | tiny RFB client to inject a key/click via VNC (when a long-running owner holds the single QMP socket). |
| `answer/` | `winnt.sif.tmpl` + `setup.cmd` (firewall off, blank-pw network logon, autologon, staged probe tools). |

## Build the guest tools
```sh
nix develop .#xp --command make -C tools/xp/win      # iexec/winrect/bitblt/mascot .exe (gitignored)
```

## Provision the golden image (unattended, from the local SP3 ISO)
```sh
nix develop .#xp --command python tools/xp/provision.py install --vnc :1
# ~15-20 min under KVM; watch cache/xp/install-*.png. Completes when the guest answers
# SMB auth (setup.cmd dropped the firewall). Result: cache/xp/xp.qcow2 (the golden).
```
No pre-built VM image exists; the SP3 retail ISO + key are local (`/opt/iso/sp3-cd-retail/`).
`provision.py` auto-detects both.

## Boot + drive
```sh
# boot the golden via a non-destructive overlay; QMP is left free for --connect:
nix develop .#xp --command python tools/xp/xpvm.py boot --disk cache/xp/xp.qcow2 --fresh &
# capture / input from another shell:
nix develop .#xp --command python tools/xp/xpvm.py --connect cache/xp/qmp.sock shot a.png
nix develop .#xp --command python tools/xp/xpvm.py --connect cache/xp/qmp.sock move 400 300
# agent-less SMB control:
nix develop .#xp --command python tools/xp/xpsmb.py --port 4445 stage tools/xp/win
nix develop .#xp --command python tools/xp/xpsmb.py --port 4445 exec 'ver'
```

## The video-001 demos (`choreograph.py`)
First bake the sprite blobs (host, once), then drive the demos. Parent flags (`--out`/`--fps`/
`--port`) go **before** the subcommand. Capture is **deterministic stepped** by default: each
frame runs the guest exactly `1/fps`, then **freezes it (QMP `stop`) for the dump** and thaws
(`cont`) — so the dump can't steal vCPU time (no starvation/stretch) and frames are evenly spaced
in guest-time. That makes **`--fps 60` smooth** (the source motion is preserved, just sampled
finer); wall-time is longer (dump cost is out of band). `--realtime` is the old free-running grab
(fast but jittery). For the hero shots: `--fps 60`.
```sh
# bake sprites from a Gemma cutout (premultiplied BGRA the guest can read)
nix develop .#xp --command python tools/xp/png2raw.py library/images/gemma-smug.png cache/xp/sprites/mascot.raw --w 220 --h 300 --anchor bottom
nix develop .#xp --command python tools/xp/png2raw.py library/images/gemma-smug.png cache/xp/sprites/hook.raw   --w 96  --h 132 --anchor bottom --pad 2

# stages all guest tools + sprites + sets 1024x768, then runs every demo (add --fps 60 for the hero cut):
nix develop .#xp --command python tools/xp/choreograph.py --fps 60 --out cache/xp/demo all
#   mascot-move.mp4         — the layered mascot floating/bobbing (host screendump sees it)
#   demo-comparison.png     — BitBlt-vs-screenshot A/B (mascot PRESENT in screendump, ABSENT in GDI BitBlt)
#   copy-normal.mp4 / .png  — XP's stock flying-papers copy dialog
#   copy-hijack.mp4 / .png  — our mascot walking across the copy bar (the MinkIt trick, reproduced)
# individual: `… stage`, `… mascot`, `… demo`, `… copy`.  Register a clip into a project:
nix develop .#xp --command python tools/xp/choreograph.py --out cache/xp/demo run script.json --into examples/luckymas.slop.json --key xp_copyhijack
```

## Networking
User-mode slirp forwards the guest's SMB :445 → host `127.0.0.1:4445` (qemu can't bind
the privileged 445 as a user). `netexec smb --port 4445` and `smbclient -p 4445` both
target it; the guest's firewall is disabled by `setup.cmd` so :445 is reachable.

## Status / gotchas
- **Proven on this box:** KVM works in WSL2 (`/dev/kvm`); QMP `screendump` + key + abs
  mouse all verified; the guest tools build as i386/subsystem-5.1 PEs; the unattended
  install drives text-mode + GUI setup from the floppy.
- The **"adjust resolution" Display Settings dialog** appears at first-logon and blocks
  it (and thus `A:\setup.cmd`) until its default OK is pressed — `provision.py`'s monitor
  sends a periodic QMP Enter to clear it.
- QMP is **single-client** per socket; `xpvm boot` frees its QMP connection after startup
  so `--connect` can drive it. `vnckey.py` is the fallback when a process must hold QMP.
- **Command exec: use `iexec`/`gui` (smbexec), NOT `exec`.** netexec's default *atexec* method
  reconnects to a hard-coded **:445** (the forward only exposes **:4445**) → "Connection refused".
  smbexec + smbclient both work on :4445. `XpSmb.auth_ok` returns True on the `Pwn3d!` *banner*
  even when the command itself fails over atexec — don't trust it as proof a command ran.
- **A separate watcher can't reliably see another process's windows.** An iexec-launched
  watcher's `EnumWindows` saw the desktop shell (Shell_TrayWnd/Progman) but **never** another
  process's copy dialog — a console-session desktop-association quirk; `OpenInputDesktop`+
  `SetThreadDesktop` didn't fix it. So `copyanim.c` runs the copy **and** the hijack in ONE
  process (watching its *own* dialog) — guaranteed same desktop. (`copyhook.c` kept as the
  cross-process attempt + a window-class diagnostic.)
- **Clean captures: run `setres` first.** The golden boots **640×480** (the `[Display]` answer-file
  knob didn't stick), which trips XP's "very low resolution" tray balloon. `setres` → 1024×768
  removes the nag + enlarges the frame. The overlay resets per `--fresh` boot, so the demos
  re-apply it. Sprites are baked host-side (`png2raw.py`) into premultiplied BGRA blobs.
- XP needs an **IDE** disk + `rtl8139` NIC (built-in drivers) and the **cirrus** VGA
  (native XP driver → 1024×768). SMB is **SMBv1/NT1 only** (smbclient `-m NT1`).
