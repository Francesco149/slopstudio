/* copyhook.c — reproduce the MinkIt "copy-animation" trick (video-001, Act 2): watch
 * for Windows XP's shell file-copy progress dialog and draw our own mascot over it,
 * replacing the stock flying-papers animation with an anime girl walking across the bar.
 *
 * Mechanism (a faithful minimal repro — MinkIt's exact internals aren't fully RE'd;
 * the notes attribute detection to a process/window watch in MinkIt.dll, drawing via
 * masked GDI blits):
 *   1. WATCH  — poll the desktop (EnumWindows) for a top-level dialog (#32770) that
 *               owns a "SysAnimate32" child = XP's copy/move "flying papers" control.
 *   2. HIJACK — hide that animation control, then on a ~30 fps timer AlphaBlend our
 *               premultiplied sprite (png2raw.py) across that band of the dialog's DC,
 *               redrawing the band each frame so there's no trail.  Pure GDI, no inject.
 *
 * Pair with copysim.exe (which raises the dialog).  Launch copyhook FIRST so it's
 * already watching; it self-exits after `seconds`.  Capture the dialog with copyhook
 * running ("hijacked") vs not ("normal") for the side-by-side.
 *
 * Usage:  copyhook.exe [sprite.raw] [w] [h] [seconds]      (defaults: diamond 110 150 45)
 * Build:  i686-w64-mingw32-gcc copyhook.c -o copyhook.exe -lgdi32 -luser32 -lmsimg32 -mconsole
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sprite.h"

typedef struct { HWND dlg; HWND anim; } Found;

static FILE *glog;          /* debug log (C:\probe\out\copyhook.log) */
static int   g_scans;       /* find_copy_dialog calls */
static int   g_top;         /* total top-level windows seen this scan */
static int   g_seen32770;   /* #32770 windows seen this scan */
static int   g_seenAnim;    /* SysAnimate32 controls seen this scan */
#define LOG(...) do { if (glog) { fprintf(glog, "[%lu] ", GetTickCount()); fprintf(glog, __VA_ARGS__); fflush(glog); } } while (0)

static BOOL CALLBACK find_anim_cb(HWND child, LPARAM lp) {
    char cls[64];
    GetClassNameA(child, cls, sizeof(cls));
    if (lstrcmpiA(cls, "SysAnimate32") == 0) {
        g_seenAnim++;
        *((HWND *)lp) = child;
        return FALSE;   /* stop */
    }
    return TRUE;
}

static int g_dump;          /* when set, log every visible top-level window's class */

static BOOL CALLBACK find_dlg_cb(HWND top, LPARAM lp) {
    g_top++;
    if (!IsWindowVisible(top)) return TRUE;
    char cls[64];
    GetClassNameA(top, cls, sizeof(cls));
    if (g_dump) { char ti[96]; GetWindowTextA(top, ti, sizeof(ti)); LOG("   top cls=[%s] title=[%s]\n", cls, ti); }
    if (lstrcmpiA(cls, "#32770") != 0) return TRUE;     /* not a dialog */
    g_seen32770++;
    HWND anim = NULL;
    EnumChildWindows(top, find_anim_cb, (LPARAM)&anim); /* recurses into descendants */
    if (anim) {
        Found *f = (Found *)lp;
        f->dlg = top; f->anim = anim;
        return FALSE;   /* got it — stop enumerating */
    }
    return TRUE;
}

static HWND find_copy_dialog(HWND *outAnim) {
    Found f = {0};
    g_scans++; g_top = 0; g_seen32770 = 0; g_seenAnim = 0;
    g_dump = (g_scans % 48 == 1);   /* full class dump ~every 3s */
    if (g_dump) LOG("--- dump scan #%d ---\n", g_scans);
    EnumWindows(find_dlg_cb, (LPARAM)&f);
    /* log a heartbeat ~1/sec (scans run ~16/sec) so we can see what it enumerates */
    if (g_scans % 16 == 1)
        LOG("scan #%d: top=%d dlgs(#32770)=%d anim32=%d -> %s\n",
            g_scans, g_top, g_seen32770, g_seenAnim, f.dlg ? "MATCH" : "none");
    *outAnim = f.anim;
    return f.dlg;
}

int main(int argc, char **argv) {
    const char *sprite = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    int W       = (argc > 2) ? atoi(argv[2]) : 110;
    int H       = (argc > 3) ? atoi(argv[3]) : 150;
    int seconds = (argc > 4) ? atoi(argv[4]) : 45;
    if (W < 8) W = 8; if (H < 8) H = 8;

    /* build the sprite source DC (32bpp top-down DIB, premultiplied BGRA) */
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HDC src = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ oldsrc = SelectObject(src, dib);

    unsigned char *spr = slurp_sprite(sprite, W, H);
    if (spr) { memcpy(bits, spr, (size_t)W * H * 4); free(spr); }
    else {
        unsigned char *p = (unsigned char *)bits;     /* fallback diamond */
        double cx = W / 2.0, cy = H / 2.0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            double dx = fabs(i - cx) / (W / 2.0), dy = fabs(j - cy) / (H / 2.0);
            double d = dx + dy, a = (d < 1.0) ? (1.0 - d) : 0.0; a *= a;
            unsigned char *px = p + (j * W + i) * 4;
            px[0] = (unsigned char)(210 * a); px[1] = (unsigned char)(60 * a);
            px[2] = (unsigned char)(180 * a); px[3] = (unsigned char)(a * 255.0);
        }
    }

    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;

    glog = fopen("C:\\probe\\out\\copyhook.log", "w");
    LOG("copyhook start: sprite=%s W=%d H=%d seconds=%d spr_loaded=%d\n",
        sprite ? sprite : "(none)", W, H, seconds, spr != NULL);

    /* iexec launches us on the console session, but our thread can land on a non-input
     * desktop → EnumWindows would see none of the visible windows.  Bind to the desktop
     * that actually receives input (where the copy dialog lives) BEFORE any window calls. */
    HDESK hd = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (hd) {
        BOOL sd = SetThreadDesktop(hd);
        LOG("OpenInputDesktop=%p SetThreadDesktop=%d (err=%lu)\n", (void *)hd, sd, GetLastError());
    } else {
        LOG("OpenInputDesktop FAILED err=%lu — trying OpenDesktop(default)\n", GetLastError());
        hd = OpenDesktopA("default", 0, FALSE, GENERIC_ALL);
        if (hd) LOG("OpenDesktop(default)=%p SetThreadDesktop=%d\n", (void *)hd, SetThreadDesktop(hd));
    }

    printf("copyhook: watching for the XP copy dialog (%ds)...\n", seconds);
    DWORD deadline = GetTickCount() + (DWORD)seconds * 1000;
    HWND dlg = NULL, anim = NULL;
    RECT band = {0};        /* slide track, dialog-client coords */
    double phase = 0.0;
    int hijacked = 0;
    int draws = 0;

    while ((int)(deadline - GetTickCount()) > 0) {
        if (!dlg || !IsWindow(dlg)) {
            dlg = find_copy_dialog(&anim);
            if (!dlg) { Sleep(60); continue; }
            char dcls[64]; GetClassNameA(dlg, dcls, sizeof(dcls));
            /* compute the slide band from the animation control, then hide it */
            RECT ar; GetWindowRect(anim, &ar);
            POINT tl = {ar.left, ar.top}, br = {ar.right, ar.bottom};
            ScreenToClient(dlg, &tl); ScreenToClient(dlg, &br);
            RECT cr; GetClientRect(dlg, &cr);
            band.left = cr.left; band.right = cr.right;           /* full width track */
            band.top = tl.y; band.bottom = br.y;
            ShowWindow(anim, SW_HIDE);
            LOG("FOUND dialog=%p cls=[%s] anim=%p anim_screen=(%ld,%ld,%ld,%ld) "
                "client=(%ld,%ld,%ld,%ld) band=(%ld,%ld,%ld,%ld)\n",
                (void *)dlg, dcls, (void *)anim, ar.left, ar.top, ar.right, ar.bottom,
                cr.left, cr.top, cr.right, cr.bottom, band.left, band.top, band.right, band.bottom);
            if (!hijacked) { printf("copyhook: HIJACKING copy dialog %p\n", (void *)dlg); hijacked = 1; }
            phase = 0.0;
        }

        int bw = band.right - band.left;
        if (bw < 1) bw = 1;
        /* march left -> right across the band, then wrap (re-enter from the left) */
        int span = bw + W;
        int px = band.left - W + (int)(fmod(phase * 3.0, (double)span));
        int bandh = band.bottom - band.top;
        int py = band.top + (bandh - H) / 2 + (int)lround(sin(phase * 0.5) * 4.0);  /* slight bob */

        /* clean the band (the hidden anim won't repaint; gives a trail-free bg), then blit */
        RedrawWindow(dlg, &band, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        HDC hdc = GetDC(dlg);
        if (hdc) {
            BOOL ok = AlphaBlend(hdc, px, py, W, H, src, 0, 0, W, H, bf);
            ReleaseDC(dlg, hdc);
            if (draws < 3) LOG("draw#%d at (%d,%d) AlphaBlend=%d\n", draws, px, py, ok);
        } else {
            if (draws < 3) LOG("draw#%d GetDC FAILED\n", draws);
        }
        draws++;
        phase += 1.0;
        Sleep(33);
    }

    LOG("exit hijacked=%d draws=%d\n", hijacked, draws);
    if (glog) fclose(glog);
    printf("copyhook: done (hijacked=%d)\n", hijacked);
    SelectObject(src, oldsrc); DeleteObject(dib); DeleteDC(src); ReleaseDC(NULL, screen);
    return 0;
}
