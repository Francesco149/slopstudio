/* copyanim.c — reproduce the MinkIt "copy-animation" trick (video-001, Act 2) in ONE
 * self-contained program: copy a pile of files (raising XP's shell copy dialog) AND draw
 * our own mascot over that dialog, replacing the stock flying-papers animation.
 *
 * Why one process (not a separate watcher): MinkIt's engine both triggers and draws the
 * animation.  More practically, a separate iexec-launched watcher couldn't reliably see
 * the copy dialog via EnumWindows (cross-process / console-session desktop association),
 * but a program watching its OWN dialog is guaranteed on the same desktop.  So:
 *   • a worker thread runs SHFileOperation(FO_COPY)  → the dialog appears;
 *   • the main thread polls for OUR dialog (#32770 owning a SysAnimate32, same PID),
 *     hides the animation control, and on a ~30 fps timer AlphaBlends the sprite
 *     (png2raw.py, premultiplied) across that band of the dialog's DC.  Pure GDI.
 *
 * --plain copies WITHOUT hijacking → the stock dialog, for the "normal" capture.
 *
 * Usage:  copyanim.exe [count] [kb] [sprite.raw] [w] [h] [--plain]   (def 12000 2 — 110 150)
 * Build:  i686-w64-mingw32-gcc copyanim.c -o copyanim.exe -lgdi32 -luser32 -lmsimg32 -lshell32 -mconsole
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sprite.h"

/* SRC is unique PER RUN (set in main from the tick count): a fresh source dir each time means the
 * copy does the full slow work and the dialog lingers ~seconds. With a FIXED source, the 2nd copy
 * of the same files (e.g. the hijack run right after the plain run in a demo) reads them straight
 * from the qemu page cache and flashes by — so the dialog is gone before capture. Renaming the
 * DEST aside (copy_thread) only freshened the dest; the cached SOURCE was what made it fast. */
static char SRC[MAX_PATH];
#define DST  "C:\\probe\\copydst"

static volatile int g_copydone;

static void ensure_files(int count, int kb) {
    CreateDirectoryA(SRC, NULL);
    char *chunk = (char *)malloc((size_t)kb * 1024);
    if (!chunk) return;
    for (int i = 0; i < kb * 1024; i++) chunk[i] = (char)(i * 31 + 7);
    for (int i = 0; i < count; i++) {
        char path[MAX_PATH];
        wsprintfA(path, "%s\\file_%05d.bin", SRC, i);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) continue;
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, chunk, kb * 1024, &w, NULL); CloseHandle(h); }
    }
    free(chunk);
}

static DWORD WINAPI copy_thread(LPVOID p) {
    (void)p;
    /* Rename any existing dest ASIDE (an instant directory rename) so the copy is always
     * FRESH — overwriting 12000 cached files is RAM-fast under qemu writeback and the
     * dialog flashes by; a from-scratch copy of that many files lingers ~10s.  The
     * renamed-aside dirs are harmless (gitignored on C:, gone on the next --fresh boot). */
    char old[MAX_PATH];
    wsprintfA(old, "%s_old_%lu", DST, GetTickCount());
    MoveFileA(DST, old);                       /* no-op if DST doesn't exist yet */

    char from[MAX_PATH + 4] = {0};
    wsprintfA(from, "%s\\*.*", SRC);
    from[strlen(from) + 1] = 0;                /* double-NUL list */
    char to[MAX_PATH + 2] = {0};
    strcpy(to, DST);
    to[strlen(DST) + 1] = 0;
    CreateDirectoryA(DST, NULL);
    SHFILEOPSTRUCTA op = {0};
    op.wFunc = FO_COPY; op.pFrom = from; op.pTo = to;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR;   /* keep the animated progress dialog */
    SHFileOperationA(&op);
    g_copydone = 1;
    return 0;
}

/* find THIS process's copy dialog (#32770 owning a SysAnimate32) */
static DWORD g_pid;
static BOOL CALLBACK anim_cb(HWND c, LPARAM lp) {
    char cls[64]; GetClassNameA(c, cls, sizeof(cls));
    if (lstrcmpiA(cls, "SysAnimate32") == 0) { *(HWND *)lp = c; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK dlg_cb(HWND top, LPARAM lp) {
    DWORD pid = 0; GetWindowThreadProcessId(top, &pid);
    if (pid != g_pid || !IsWindowVisible(top)) return TRUE;
    char cls[64]; GetClassNameA(top, cls, sizeof(cls));
    if (lstrcmpiA(cls, "#32770") != 0) return TRUE;
    HWND anim = NULL; EnumChildWindows(top, anim_cb, (LPARAM)&anim);
    if (anim) { HWND *out = (HWND *)lp; out[0] = top; out[1] = anim; return FALSE; }
    return TRUE;
}

int main(int argc, char **argv) {
    int count = 12000, kb = 2, W = 110, H = 150, plain = 0;
    const char *sprite = NULL;
    int ni = 0;
    for (int i = 1; i < argc; i++) {
        if (lstrcmpiA(argv[i], "--plain") == 0) { plain = 1; continue; }
        if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            int v = atoi(argv[i]);
            if (ni == 0) count = v; else if (ni == 1) kb = v; else if (ni == 2) W = v; else if (ni == 3) H = v;
            ni++;
        } else if (!sprite) sprite = argv[i];
    }
    if (W < 8) W = 8; if (H < 8) H = 8;
    g_pid = GetCurrentProcessId();
    wsprintfA(SRC, "C:\\probe\\copysrc_%lu", GetTickCount());   /* fresh source dir per run (cold copy → dialog lingers) */

    printf("copyanim: preparing %d files x %d KB in %s ...\n", count, kb, SRC);
    ensure_files(count, kb);

    /* sprite source DC (premultiplied BGRA, top-down) */
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC screen = GetDC(NULL), src = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(src, dib);
    unsigned char *spr = slurp_sprite(sprite, W, H);
    if (spr) { memcpy(bits, spr, (size_t)W * H * 4); free(spr); }
    else {
        unsigned char *p = (unsigned char *)bits; double cx = W / 2.0, cy = H / 2.0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            double dx = fabs(i - cx) / (W / 2.0), dy = fabs(j - cy) / (H / 2.0);
            double d = dx + dy, a = (d < 1.0) ? (1.0 - d) : 0.0; a *= a;
            unsigned char *px = p + (j * W + i) * 4;
            px[0] = (unsigned char)(210 * a); px[1] = (unsigned char)(60 * a);
            px[2] = (unsigned char)(180 * a); px[3] = (unsigned char)(a * 255.0);
        }
    }
    BLENDFUNCTION bf; bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;

    printf("copyanim: copying %s\\*.* -> %s (%s)\n", SRC, DST, plain ? "plain" : "HIJACK");
    HANDLE th = CreateThread(NULL, 0, copy_thread, NULL, 0, NULL);

    DWORD t_start = GetTickCount(), t_found = 0; int draws = 0;
    if (!plain) {
        HWND dlg = NULL, anim = NULL; RECT band = {0}; double phase = 0.0;
        int bw = 0, bh = 0;
        HDC memDC = NULL, bgDC = NULL; HBITMAP memBmp = NULL, bgBmp = NULL;   /* double buffer */
        while (!g_copydone) {
            if (!dlg || !IsWindow(dlg)) {
                HWND fr[2] = {0}; EnumWindows(dlg_cb, (LPARAM)fr);
                dlg = fr[0]; anim = fr[1];
                if (!dlg) { Sleep(40); continue; }
                RECT ar; GetWindowRect(anim, &ar);
                POINT tl = {ar.left, ar.top}, br = {ar.right, ar.bottom};
                ScreenToClient(dlg, &tl); ScreenToClient(dlg, &br);
                RECT cr; GetClientRect(dlg, &cr);
                band.left = cr.left; band.right = cr.right; band.top = tl.y; band.bottom = br.y;
                ShowWindow(anim, SW_HIDE);
                if (!t_found) t_found = GetTickCount();
                printf("copyanim: HIJACKING dialog %p\n", (void *)dlg);
                /* DOUBLE BUFFER (kills the sprite flicker): the old path RedrawWindow-cleared the
                 * band then AlphaBlended on top, so a frame captured between the two showed the
                 * EMPTY band -> flicker. Instead: stash the clean band background ONCE (the
                 * SysAnimate32 control is now hidden, so the band shows the dialog's static panel
                 * bg), then each frame composite bg+mascot in an off-screen memDC and BitBlt it to
                 * the screen in ONE op. The memDC bounds also clip the mascot to the band (so it
                 * subsumes the old IntersectClipRect trail fix). */
                if (memDC) { DeleteDC(memDC); DeleteObject(memBmp); DeleteDC(bgDC); DeleteObject(bgBmp); }
                bw = band.right - band.left; bh = band.bottom - band.top;
                if (bw < 1) bw = 1; if (bh < 1) bh = 1;
                HDC ddc = GetDC(dlg);
                memDC = CreateCompatibleDC(ddc); memBmp = CreateCompatibleBitmap(ddc, bw, bh); SelectObject(memDC, memBmp);
                bgDC  = CreateCompatibleDC(ddc); bgBmp  = CreateCompatibleBitmap(ddc, bw, bh); SelectObject(bgDC, bgBmp);
                RedrawWindow(dlg, &band, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);   /* clean the band once */
                BitBlt(bgDC, 0, 0, bw, bh, ddc, band.left, band.top, SRCCOPY);                /* stash the clean bg */
                ReleaseDC(dlg, ddc);
            }
            draws++;
            int span = bw + W;
            /* walk RIGHT -> LEFT (px decreases from off-the-right to off-the-left): the sprite pose
             * reads as striding left, so the old left->right travel looked like moonwalking. */
            int px = band.right - (int)(fmod(phase * 3.0, (double)span));
            int py = band.top + (bh - H) / 2 + (int)lround(sin(phase * 0.5) * 4.0);
            HDC ddc = GetDC(dlg);
            if (ddc && memDC) {
                BitBlt(memDC, 0, 0, bw, bh, bgDC, 0, 0, SRCCOPY);                              /* clean slate */
                AlphaBlend(memDC, px - band.left, py - band.top, W, H, src, 0, 0, W, H, bf);   /* mascot, band-relative (memDC bounds clip it) */
                BitBlt(ddc, band.left, band.top, bw, bh, memDC, 0, 0, SRCCOPY);                /* ONE screen update -> no flicker */
                ReleaseDC(dlg, ddc);
            } else if (ddc) ReleaseDC(dlg, ddc);
            phase += 1.0;
            Sleep(33);
        }
        if (memDC) { DeleteDC(memDC); DeleteObject(memBmp); DeleteDC(bgDC); DeleteObject(bgBmp); }
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    DeleteObject(dib); DeleteDC(src); ReleaseDC(NULL, screen);
    printf("copyanim: done — total=%lums dialog_first_seen=+%lums draws=%d (dialog up ~%.1fs)\n",
           GetTickCount() - t_start, t_found ? (t_found - t_start) : 0, draws, draws * 0.033);
    return 0;
}
