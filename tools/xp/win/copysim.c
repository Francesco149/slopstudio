/* copysim.c — trigger Windows XP's stock file-copy progress dialog (the one with the
 * flying-papers "SysAnimate32" animation + progress bar) so we can capture it BOTH
 * ways for video-001's Act 2: untouched ("normal"), and with copyhook.exe drawing our
 * mascot over it ("hijacked", the MinkIt copy-animation trick reproduced).
 *
 * It populates C:\probe\copysrc with `count` files of `kb` KB each (once), wipes
 * C:\probe\copydst, then SHFileOperation(FO_COPY)s the lot.  Many files keep the
 * animated shell dialog on screen long enough to capture (tune via the args).  The
 * progress UI is left ON (no FOF_SILENT/FOF_NOPROGRESS) — that dialog is the subject.
 *
 * The copy is RAM-fast under qemu writeback caching, so XP only raises the dialog when
 * the op runs long enough — the lever is FILE COUNT (per-file shell overhead), not bytes.
 * Split into modes so capture is deterministic: pre-create once, then capture the copy.
 *
 * Usage:  copysim.exe [count] [kb] [mode]      (defaults 12000 2 both; mode=create|copy|both)
 * Build:  i686-w64-mingw32-gcc copysim.c -o copysim.exe -lshell32 -mconsole … (Makefile)
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRC  "C:\\probe\\copysrc"
#define DST  "C:\\probe\\copydst"

static void ensure_files(int count, int kb) {
    CreateDirectoryA(SRC, NULL);
    char *chunk = (char *)malloc((size_t)kb * 1024);
    if (!chunk) return;
    for (int i = 0; i < kb * 1024; i++) chunk[i] = (char)(i * 31 + 7);   /* non-zero filler */
    for (int i = 0; i < count; i++) {
        char path[MAX_PATH];
        wsprintfA(path, "%s\\file_%05d.bin", SRC, i);
        DWORD attr = GetFileAttributesA(path);
        if (attr != INVALID_FILE_ATTRIBUTES) continue;   /* already there (re-run) */
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote;
            WriteFile(h, chunk, kb * 1024, &wrote, NULL);
            CloseHandle(h);
        }
    }
    free(chunk);
}

/* SHFileOperation wants double-NUL-terminated path lists. */
static void copy_now(void) {
    /* No pre-delete: FOF_NOCONFIRMATION copies over any existing files silently, and a
     * pre-wipe of 12000 files is a slow SILENT phase that pushes the copy dialog out
     * unpredictably.  Going straight to the copy makes the animated dialog appear promptly. */
    char from[MAX_PATH + 4] = {0};
    wsprintfA(from, "%s\\*.*", SRC);          /* all files in SRC */
    from[strlen(from) + 1] = 0;               /* extra NUL */
    char to[MAX_PATH + 2] = {0};
    strcpy(to, DST);
    to[strlen(DST) + 1] = 0;

    SHFILEOPSTRUCTA op = {0};
    op.wFunc  = FO_COPY;
    op.pFrom  = from;
    op.pTo    = to;
    /* NO FOF_SILENT / FOF_NOPROGRESS: we WANT the animated progress dialog. */
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR;
    int rc = SHFileOperationA(&op);
    printf("copysim: SHFileOperation rc=%d aborted=%d\n", rc, op.fAnyOperationsAborted);
}

int main(int argc, char **argv) {
    int count = (argc > 1) ? atoi(argv[1]) : 12000;
    int kb    = (argc > 2) ? atoi(argv[2]) : 2;
    const char *mode = (argc > 3) ? argv[3] : "both";
    if (count < 1) count = 1;
    if (kb < 1) kb = 1;
    int do_create = (lstrcmpiA(mode, "copy") != 0);
    int do_copy   = (lstrcmpiA(mode, "create") != 0);
    if (do_create) {
        printf("copysim: preparing %d files x %d KB in %s ...\n", count, kb, SRC);
        ensure_files(count, kb);
    }
    if (do_copy) {
        printf("copysim: copying %s\\*.* -> %s\n", SRC, DST);
        copy_now();
    }
    printf("copysim: done (%s)\n", mode);
    return 0;
}
