/* winrect.c — print a window's screen rectangle so the host can crop a full-frame
 * QMP `screendump` (or a PrtScn) down to just that window.  Runs on the XP guest's
 * console desktop (launch it via iexec, or directly via netexec).
 *
 *   iexec.exe -w4 C:\probe\winrect.exe "About"      # match a title substring
 *   iexec.exe -w4 C:\probe\winrect.exe              # the foreground window
 *
 * Writes the rect to BOTH stdout and C:\probe\out\winrect.txt — fetch the file with
 * smbclient (SMB-exec stdout is flaky for some cmdlines, the file is always reliable).
 * Emits the full WINDOW rect and the CLIENT rect (content area, title bar excluded),
 * both in screen coordinates, as `left top right bottom width height`.
 *
 * Build (32-bit, XP subsystem) — `make` in this dir inside `nix develop`, or:
 *   i686-w64-mingw32-gcc winrect.c -o winrect.exe -luser32 -O2 -s -mconsole \
 *     -D_WIN32_WINNT=0x0501 -Wl,--major-subsystem-version=5,--minor-subsystem-version=1
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define OUTFILE "C:\\probe\\out\\winrect.txt"

static char g_needle[256];
static HWND g_match = NULL;

/* lowercase-substring match on the visible top-level window title */
static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    char title[512];
    int n = GetWindowTextA(h, title, sizeof(title));
    if (n <= 0) return TRUE;
    char lo[512]; int i;
    for (i = 0; i < n && i < (int)sizeof(lo) - 1; i++) {
        char c = title[i];
        lo[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lo[i] = 0;
    if (strstr(lo, g_needle)) { g_match = h; return FALSE; }  /* first match wins */
    return TRUE;
}

static void emit(FILE *f, HWND h) {
    char title[512] = "";
    GetWindowTextA(h, title, sizeof(title));
    RECT w; GetWindowRect(h, &w);
    RECT c; GetClientRect(h, &c);
    POINT tl = { c.left, c.top }, br = { c.right, c.bottom };
    ClientToScreen(h, &tl); ClientToScreen(h, &br);
    fprintf(f, "HWND=0x%p\n", (void*)h);
    fprintf(f, "TITLE=%s\n", title);
    fprintf(f, "WINDOW=%ld %ld %ld %ld %ld %ld\n",
            (long)w.left, (long)w.top, (long)w.right, (long)w.bottom,
            (long)(w.right - w.left), (long)(w.bottom - w.top));
    fprintf(f, "CLIENT=%ld %ld %ld %ld %ld %ld\n",
            (long)tl.x, (long)tl.y, (long)br.x, (long)br.y,
            (long)(br.x - tl.x), (long)(br.y - tl.y));
}

int main(int argc, char **argv) {
    HWND h = NULL;
    if (argc > 1 && argv[1][0]) {
        int i; size_t n = strlen(argv[1]);
        if (n >= sizeof(g_needle)) n = sizeof(g_needle) - 1;
        for (i = 0; i < (int)n; i++) {
            char ch = argv[1][i];
            g_needle[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
        }
        g_needle[n] = 0;
        EnumWindows(enumProc, 0);
        h = g_match;
        if (!h) { printf("winrect: no visible window matching \"%s\"\n", argv[1]); return 1; }
    } else {
        h = GetForegroundWindow();
        if (!h) { printf("winrect: no foreground window\n"); return 1; }
    }

    emit(stdout, h);
    FILE *f = fopen(OUTFILE, "w");
    if (f) { emit(f, h); fclose(f); }
    return 0;
}
