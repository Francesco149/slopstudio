/* setres.c — set the guest's display mode (ChangeDisplaySettings).  Run once per session
 * to lift the 640x480 golden to a crisper, larger mode for capture — and to silence XP's
 * "your screen resolution is set to a very low level" tray balloon (640x480 triggers it).
 *
 * Enumerates the driver's supported modes, picks the first of a preference list that the
 * driver actually offers (CDS_TEST), applies it (CDS_UPDATEREGISTRY), and logs everything
 * to C:\probe\out\setres.log so the host can see what the cirrus VGA accepted.
 *
 * Usage:  setres.exe [w] [h] [bpp]      (prefers w/h/bpp, else falls back down a list)
 * Build:  i686-w64-mingw32-gcc setres.c -o setres.exe -mconsole -luser32 … (Makefile)
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *lg;
static int try_mode(int w, int h, int bpp) {
    DEVMODEA dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    LONG t = ChangeDisplaySettingsA(&dm, CDS_TEST);
    if (lg) fprintf(lg, "test %dx%dx%d -> %ld\n", w, h, bpp, t);
    if (t != DISP_CHANGE_SUCCESSFUL) return 0;
    LONG r = ChangeDisplaySettingsA(&dm, CDS_UPDATEREGISTRY);
    if (lg) fprintf(lg, "APPLY %dx%dx%d -> %ld\n", w, h, bpp, r);
    return r == DISP_CHANGE_SUCCESSFUL;
}

int main(int argc, char **argv) {
    lg = fopen("C:\\probe\\out\\setres.log", "w");
    /* log what the driver offers */
    if (lg) {
        DEVMODEA dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
        for (int i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++)
            fprintf(lg, "avail %lux%lux%lu @%luHz\n", (unsigned long)dm.dmPelsWidth,
                    (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmBitsPerPel,
                    (unsigned long)dm.dmDisplayFrequency);
    }
    int W = (argc > 1) ? atoi(argv[1]) : 0, H = (argc > 2) ? atoi(argv[2]) : 0, B = (argc > 3) ? atoi(argv[3]) : 0;
    int ok = 0;
    if (W && H && B) ok = try_mode(W, H, B);
    /* preference list (high → low); first the driver accepts wins.  NOTE: on the cirrus
     * VGA, switching to 24bpp throws the session into a ~60s black "Windows XP" transition
     * screen — so we stick to 16bpp (High Colour), which switches cleanly to the Bliss
     * desktop.  16bpp at 1024x768 does NOT trip the "very low" nag (that's the 640x480 boot). */
    if (!ok) ok = try_mode(1024, 768, 16);
    if (!ok) ok = try_mode(800, 600, 16);
    printf("setres: applied=%d\n", ok);
    if (lg) { fprintf(lg, "applied=%d\n", ok); fclose(lg); }
    return ok ? 0 : 1;
}
