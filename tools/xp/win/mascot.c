/* mascot.c — a per-pixel-alpha LAYERED window (WS_EX_LAYERED + UpdateLayeredWindow):
 * the SAME technique the video's Launch.exe uses for its floating desktop mascots.
 *
 * Loads a real sprite (a premultiplied-BGRA blob baked by tools/xp/png2raw.py) and
 * floats it over the desktop with true per-pixel alpha, no window chrome, always on
 * top, and click-through (WS_EX_TRANSPARENT).  A WM_TIMER gives it a gentle idle bob
 * + slow horizontal drift so a frame-by-frame capture (choreograph.py) shows it MOVING.
 * No sprite arg (or a bad/size-mismatched file) → a procedural purple diamond fallback.
 *
 * The point of the demo: a plain GDI BitBlt (bitblt.exe) CANNOT capture this window;
 * the host-side QMP screendump (tools/xp/xpvm.py) CAN.  Run it, capture both ways —
 * the mascot is present in the screendump, absent in the BitBlt.
 *
 * Persistent (fire-and-forget via iexec): close with `taskkill /f /im mascot.exe`.
 *
 * Usage:  mascot.exe x y size [sprite.raw]          (square; back-compat)
 *         mascot.exe x y w h [sprite.raw]            (explicit w/h)
 *         mascot.exe x y w h sprite.raw --static     (no motion, for the still demo)
 * Build:  i686-w64-mingw32-gcc mascot.c -o mascot.exe -lgdi32 -luser32 -mwindows … (Makefile)
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sprite.h"

/* animation + draw state shared with the timer proc */
static HWND  g_win;
static HDC   g_screen, g_mem;
static SIZE  g_sz;
static int   g_baseX, g_baseY;
static double g_phase = 0.0;
static int   g_static = 0;
static int   g_driftRange = 180;   /* px of horizontal sway */

static void redraw_at(int x, int y) {
    POINT src = {0, 0}, dst = {x, y};
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_win, g_screen, &dst, &g_sz, g_mem, &src, 0, &bf, ULW_ALPHA);
}

static void tick(void) {
    g_phase += 0.13;
    /* gentle vertical bob + slow horizontal drift (sinusoidal, never leaves screen) */
    int bob   = (int)lround(sin(g_phase) * 10.0);
    int drift = (int)lround(sin(g_phase * 0.37) * (g_driftRange / 2.0));
    redraw_at(g_baseX + drift, g_baseY + bob);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER)     { tick(); return 0; }
    if (m == WM_DESTROY)   { PostQuitMessage(0); return 0; }
    if (m == WM_RBUTTONUP) { DestroyWindow(h); return 0; }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)show;
    int x = 420, y = 180, W = 200, H = 200;
    char sprite[MAX_PATH] = {0};

    /* parse: leading ints fill [x y w h]; first non-numeric token = sprite path;
     * "--static" anywhere disables motion. 3 ints => square (back-compat). */
    if (cmd && *cmd) {
        char buf[1024]; strncpy(buf, cmd, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        int ints[4], ni = 0;
        char *tok = strtok(buf, " \t");
        while (tok) {
            if (strcmp(tok, "--static") == 0) {
                g_static = 1;
            } else if (ni < 4 && (tok[0] == '-' || (tok[0] >= '0' && tok[0] <= '9'))
                       && strspn(tok, "-0123456789") == strlen(tok)) {
                ints[ni++] = atoi(tok);
            } else if (!sprite[0]) {
                strncpy(sprite, tok, MAX_PATH - 1);
            }
            tok = strtok(NULL, " \t");
        }
        if (ni >= 1) x = ints[0];
        if (ni >= 2) y = ints[1];
        if (ni == 3) { W = H = ints[2]; }
        if (ni >= 4) { W = ints[2]; H = ints[3]; }
    }
    if (W < 16) W = 16; if (W > 1024) W = 1024;
    if (H < 16) H = 16; if (H > 1024) H = 1024;
    g_baseX = x; g_baseY = y; g_sz.cx = W; g_sz.cy = H;

    WNDCLASSEX wc; ZeroMemory(&wc, sizeof(wc)); wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = "SlopMascot";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassEx(&wc);

    /* WS_EX_TRANSPARENT = click-through, like a real desktop mascot */
    g_win = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
                           "SlopMascot", "mascot", WS_POPUP, x, y, W, H,
                           NULL, NULL, hi, NULL);
    if (!g_win) return 1;

    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    g_screen = GetDC(NULL);
    g_mem = CreateCompatibleDC(g_screen);
    HBITMAP dib = CreateDIBSection(g_screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(g_mem, dib);

    unsigned char *spr = slurp_sprite(sprite[0] ? sprite : NULL, W, H);
    if (spr) {
        memcpy(bits, spr, (size_t)W * H * 4);    /* premultiplied BGRA, as-is */
        free(spr);
    } else {
        /* procedural fallback: a soft translucent purple diamond */
        unsigned char *p = (unsigned char *)bits;
        double cx = W / 2.0, cy = H / 2.0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            double dx = fabs(i - cx) / (W / 2.0), dy = fabs(j - cy) / (H / 2.0);
            double d = dx + dy, a = (d < 1.0) ? (1.0 - d) : 0.0; a *= a;
            double core = (d < 0.35) ? 1.0 : 0.0;
            double r = 150 + 90 * core, g = 40 + 60 * core, b = 210 + 30 * core;
            unsigned char *px = p + (j * W + i) * 4;
            px[0] = (unsigned char)(b * a); px[1] = (unsigned char)(g * a);
            px[2] = (unsigned char)(r * a); px[3] = (unsigned char)(a * 255.0);
        }
    }

    redraw_at(g_baseX, g_baseY);
    ShowWindow(g_win, SW_SHOWNA);
    if (!g_static) SetTimer(g_win, 1, 40, NULL);   /* ~25 fps idle motion */

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    SelectObject(g_mem, old); DeleteObject(dib); DeleteDC(g_mem); ReleaseDC(NULL, g_screen);
    return 0;
}
