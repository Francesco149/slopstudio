/* bitblt.c — capture the screen via a plain GDI BitBlt to a 24-bit BMP.
 *
 * This is the NAIVE capture path: BitBlt with SRCCOPY (no CAPTUREBLT) copies only
 * the layered desktop composition WITHOUT per-pixel-alpha LAYERED windows — so a
 * floating mascot drawn via UpdateLayeredWindow (see mascot.c) is INVISIBLE here.
 * The host-side QMP `screendump` (tools/xp/xpvm.py) captures the true framebuffer
 * and DOES see it.  That contrast is the BitBlt-vs-screenshot demo for video-001.
 *
 * Writes a BMP (no libpng in the guest); the host converts BMP→PNG with Pillow.
 * Usage:  bitblt.exe [C:\probe\out\bitblt.bmp]
 * Build:  i686-w64-mingw32-gcc bitblt.c -o bitblt.exe -lgdi32 -luser32 -mconsole … (see Makefile)
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "C:\\probe\\out\\bitblt.bmp";
    int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);

    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, W, H);
    HGDIOBJ old = SelectObject(hDC, hBmp);
    /* SRCCOPY only — deliberately NOT |CAPTUREBLT, so layered windows are excluded. */
    BitBlt(hDC, 0, 0, W, H, hScreen, 0, 0, SRCCOPY);
    SelectObject(hDC, old);

    BITMAPINFOHEADER bi; ZeroMemory(&bi, sizeof(bi));
    bi.biSize = sizeof(bi); bi.biWidth = W; bi.biHeight = H; bi.biPlanes = 1;
    bi.biBitCount = 24; bi.biCompression = BI_RGB;
    int stride = (W * 3 + 3) & ~3;
    DWORD imgsize = (DWORD)stride * H;
    unsigned char *bits = (unsigned char *)malloc(imgsize);
    if (!bits) { printf("bitblt: oom\n"); return 1; }
    GetDIBits(hScreen, hBmp, 0, H, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);

    BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4D42;                                   /* 'BM' */
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + imgsize;

    FILE *f = fopen(out, "wb");
    if (!f) { printf("bitblt: cannot open %s\n", out); free(bits); return 1; }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(bits, imgsize, 1, f);
    fclose(f);
    printf("bitblt: wrote %dx%d -> %s\n", W, H, out);

    free(bits);
    DeleteObject(hBmp); DeleteDC(hDC); ReleaseDC(NULL, hScreen);
    return 0;
}
