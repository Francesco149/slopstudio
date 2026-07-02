# HANDOFF — luckymas2 review pass (2026-07-02)

Read after `CLAUDE.md` → `docs/STATUS.md`. This is the live front for the in-progress
**luckymas2 review pass**. Everything below is **committed** (3 commits on `master`,
tip `cd254d7`); working tree clean. Nothing pushed (no remote).

## What this pass did
Rebuilt `examples/luckymas2.slop.json` from `examples/luckymas2.skeleton.json`
(827.6s, 313 clips, **lint-clean**). All 87 VO takes adopted from the prior cut; the 7
changed/new lines were regenerated on lame and the timeline retimed. Feed has a montage.

**Editor (`editor/src/main.cpp`)** — new defaults, all verified headless:
- Host **pose-swap slide + eased motion blur** between contiguous host clips that change
  sprite (emotion/framing/pose/face). Velocity-driven ghosts (`TransFx.vx/vy`).
- Captions with `place:"auto"` → least-busy top **corner** (span-stable; two at once take
  opposite corners).
- Code clips **auto-fit font to frame width** (when no `font_px`) + `dock:"top"`.
- Media inset/fit **pops in** by default; **cutout PNGs get no auto frame** (`image_has_alpha`).
- Diagrams: default **backdrop card**, **width auto-fit**, `traffic:true` (packets along
  edges), `dead:[...]` (muted + red ✕, packets die at the door = "talks to a ghost").

**Compiler (`tools/slop.py skeleton`)** — new authoring surface:
- Host draws **over** content images now (row order).
- `visual:"host-dark"` / `scene:"dark"|"day"` (new `bg_dark` = room-dark.png);
  `{stack:[...],side}`; `plate:{caption,sub}`; `solo:true`; `gag:"clueless"`.
- Sections emit an **"ACT n · TITLE"** corner card. Plate-only beats keep the room bg.

**Assets** (committed under `examples/assets/luckymas/`, + `presets/backgrounds/room-dark.png`):
gcal menu/tasks zoom crops, gcal-schedule-bubble, calc-convert (hero), konata (borderless),
tahoma wizard (real XP, 671×511 vs faithful 586×364), screensaver stills, ruputer site-top
crop, 4x banner.
**Regenerable (gitignored, `assets-src/`)** — recipes:
- `luckymas-crt-saver.mp4` = `ffmpeg -ss 115 -t 5 -i assets-src/luckymas-crt.mp4 -an -vf scale=1280:720 -r 30 -c:v libx264 -pix_fmt yuv420p`.
- `xp-mascot-halo{,-clean,-demo}.png` = a PIL script (a real capture over bliss shows NO
  fringe, so this is an **honest synthesis**: white matte residue in the AA edge band,
  composited straight-alpha vs premultiplied over a dark bg). Re-derive from
  `presets/avatars/gemma-big/smug_front.png`.

## Addressed from the user's review list
Ruputer banner (stack), 18+ & chest gags (dark room + clueless), host-over-image z-order,
corner plates, pose slide+motion-blur, pop default, "2007" VO, calc-convert, transparency
image, konata borderless, code auto-fit + host lower-right, halo A/B, copy stock-vs-hijack
A/B, no bare host-fill shots, gcal 2-zoom-crop stack, mail = schedule screenshot + POP3
plate, dead-server animation, diagram polish, screensaver video, SYGNAS→Cygnus, apology
solo/wide, wrong-font installer A/B (driven on real XP), directory tree, install-section
diagrams, CRT footage limited to screensavers, dark-room outro, act-card branding.

## Not done / verify next
1. **Watch the cut end-to-end.** Only spot-frames were rendered — motion-blur slide,
   act-card timing, and plate-corner conflicts should be checked in motion (editor or a
   full export). No full mp4 was rendered this pass.
2. **BitBlt "start 1 line earlier"** — user wanted the empty-desktop transition to begin
   at *"Screenshot her the normal way"*. Current cut shows her PRESENT on that line
   (PrtScn plate) and EMPTY on the next ("point a lazy capture tool…"), which is the
   logical A/B. Confirm this matches intent; if not, move the empty shot up a beat.
3. **"possibly the coolest hack" VO** renders high-pitched — user said they'll regen it
   (TTS instability). Just `--generate` that clip until it lands.
4. **gcal tasks crop** text is slightly clipped in the source (IE window overlapped it);
   recrop from `../LuckyMasterEN/docs/screenshots/gcal-calendar.png` if it bothers.
5. **Dead-server / ghidra diagrams read small** (auto-fit shrinks 2-node flows). Bump
   `font_px` or add nodes if you want them bolder.

## Rebuild command
```
nix develop --command python tools/slop.py skeleton examples/luckymas2.skeleton.json --out examples/luckymas2.slop.json
python tools/slop.py adopt examples/luckymas2.slop.json --src <prev>.slop.json   # keep VO
python tools/slop.py retime examples/luckymas2.slop.json
python tools/slop.py lint  examples/luckymas2.slop.json
```
Kill a stale Windows editor before headless renders: `tasklist.exe | grep -i slop` →
`taskkill.exe /IM slopstudio.exe /F`.

---

# (older) Still PENDING — BACKUP DEPLOY (nix-lab infra; the user's; no rush, timer is OFF)
Goal: lame keeps NO snapshots (deletes free immediately); cold keeps history via syncoid bookmarks.
- DONE: nix-lab `4ba9221` (lame.nix autoSnapshot=false + `hosts/cold/backup/cold-backup.py` syncoid
  `--use-bookmarks` for `@lame`); `zfs allow` granted; `cold-backup.timer` STOPPED on code (a deploy
  re-arms it → re-stop!); code deployed.
- TODO (deploy-rs needs `deploy --skip-checks` — the schema check is broken w/ `nixpkgs.hostPlatform`;
  clear `~/.cache/nix/eval-cache-v6` if a deploy seems stale):
  1. `cd /opt/src/nix-lab && nix develop -c deploy --skip-checks .#lame` (autoSnapshot off; safe once
     nothing is training on the 3080).
  2. wake cold (`ssh root@code cold-unlock --host cold`) + `deploy --skip-checks .#cold` (the syncoid
     script runs on COLD, not code).
  3. one-time re-baseline on cold (no common snap left): `syncoid --recursive --use-bookmarks
     --force-delete backup@lame:lamedata gigavault/lame-backup` (~700G — do after the runs).
  4. re-arm: `ssh root@code systemctl start cold-backup.timer`.
- NOTE: `hosts/cold/...py` runs on cold; the identical-looking `hosts/code/backup/cold-backup.py` is the
  ORCHESTRATOR (don't touch). The nix-lab backup infra is the user's — they review.
