/* wlist.c — dump every visible top-level window (handle/class/title) and its full
 * descendant class tree to a file.  A throwaway diagnostic for copyhook.c: run it while
 * the shell copy dialog is up to learn the REAL window/animation-control classes.
 *
 * Usage:  wlist.exe [C:\probe\out\wlist.txt]
 * Build:  i686-w64-mingw32-gcc wlist.c -o wlist.exe -mconsole -luser32 … (Makefile)
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>

static FILE *g;
static int   depth;

static BOOL CALLBACK child_cb(HWND w, LPARAM lp) {
    (void)lp;
    char cls[96], ti[160];
    GetClassNameA(w, cls, sizeof(cls));
    GetWindowTextA(w, ti, sizeof(ti));
    fprintf(g, "      child cls=[%s] title=[%s] vis=%d\n", cls, ti, IsWindowVisible(w));
    return TRUE;
}

static BOOL CALLBACK top_cb(HWND w, LPARAM lp) {
    (void)lp;
    if (!IsWindowVisible(w)) return TRUE;
    char cls[96], ti[160];
    GetClassNameA(w, cls, sizeof(cls));
    GetWindowTextA(w, ti, sizeof(ti));
    RECT r; GetWindowRect(w, &r);
    fprintf(g, "TOP hwnd=%p cls=[%s] title=[%s] rect=(%ld,%ld,%ld,%ld)\n",
            (void *)w, cls, ti, r.left, r.top, r.right, r.bottom);
    EnumChildWindows(w, child_cb, 0);   /* recurses into all descendants */
    return TRUE;
}

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "C:\\probe\\out\\wlist.txt";
    g = fopen(out, "w");
    if (!g) return 1;
    (void)depth;
    EnumWindows(top_cb, 0);
    fclose(g);
    return 0;
}
