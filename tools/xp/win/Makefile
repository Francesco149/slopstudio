# tools/xp/win/Makefile — XP-subsystem (i686 PE) helper tools for the capture harness.
#
# Built with the slopstudio flake's mingw32 cross (i686-w64-mingw32-gcc) so the PEs
# are stamped subsystem 5.1 and actually run on Windows XP.  Run inside `nix develop`:
#
#     nix develop --command make -C tools/xp/win
#
# Output .exe's are gitignored (like the editor build) — staged onto the VM at C:\probe\
# by the host driver (tools/xp/xpvm.py).  The dev shell pre-sets CC to host gcc, so we
# pin the cross explicitly via MINGW32_CC (exported by the flake; literal fallback if not).

MINGW32_CC ?= i686-w64-mingw32-gcc

CFLAGS  := -O2 -s -D_WIN32_WINNT=0x0501
# Stamp the PE as Windows 5.1 (XP) — stock mingw marks 6.x and XP refuses to load it.
XPLDV   := -Wl,--major-subsystem-version=5,--minor-subsystem-version=1

# iexec/winrect/bitblt/copysim are console tools; mascot is a GUI (-mwindows) layered
# window; copyhook is a console watcher that draws over the shell copy dialog.
BINS := iexec.exe winrect.exe bitblt.exe mascot.exe copysim.exe copyanim.exe copyhook.exe wlist.exe setres.exe

all: $(BINS)

iexec.exe: iexec.c
	$(MINGW32_CC) $< -o $@ -mconsole -lwtsapi32 -luserenv $(CFLAGS) $(XPLDV)

winrect.exe: winrect.c
	$(MINGW32_CC) $< -o $@ -mconsole -luser32 $(CFLAGS) $(XPLDV)

bitblt.exe: bitblt.c
	$(MINGW32_CC) $< -o $@ -mconsole -lgdi32 -luser32 $(CFLAGS) $(XPLDV)

mascot.exe: mascot.c sprite.h
	$(MINGW32_CC) $< -o $@ -mwindows -lgdi32 -luser32 $(CFLAGS) $(XPLDV)

copysim.exe: copysim.c
	$(MINGW32_CC) $< -o $@ -mconsole -lshell32 $(CFLAGS) $(XPLDV)

copyanim.exe: copyanim.c sprite.h
	$(MINGW32_CC) $< -o $@ -mconsole -lgdi32 -luser32 -lmsimg32 -lshell32 $(CFLAGS) $(XPLDV)

copyhook.exe: copyhook.c sprite.h
	$(MINGW32_CC) $< -o $@ -mconsole -lgdi32 -luser32 -lmsimg32 $(CFLAGS) $(XPLDV)

wlist.exe: wlist.c
	$(MINGW32_CC) $< -o $@ -mconsole -luser32 $(CFLAGS) $(XPLDV)

setres.exe: setres.c
	$(MINGW32_CC) $< -o $@ -mconsole -luser32 $(CFLAGS) $(XPLDV)

clean:
	rm -f $(BINS)

.PHONY: all clean
