/* sprite.h — load a premultiplied-BGRA raw sprite (from tools/xp/png2raw.py).
 *
 * The guest has no PNG decoder, so the host converts a PNG → a flat top-down
 * BGRA blob (alpha premultiplied into B/G/R, exactly the layout UpdateLayeredWindow
 * and AlphaBlend want) sized to W*H*4.  This loader slurps that blob; callers blit
 * it straight into a 32-bpp top-down DIB section.
 *
 * Shared by mascot.c (layered window) and copyhook.c (masked AlphaBlend over the
 * copy dialog).  Returns a malloc'd W*H*4 buffer, or NULL if the file is missing or
 * the wrong size (caller falls back to a procedural sprite).
 */
#ifndef SLOP_SPRITE_H
#define SLOP_SPRITE_H
#include <stdio.h>
#include <stdlib.h>

static unsigned char *slurp_sprite(const char *path, int w, int h) {
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long want = (long)w * h * 4;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != want) { fclose(f); return NULL; }     /* dims must match the args */
    unsigned char *buf = (unsigned char *)malloc((size_t)want);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)want, f);
    fclose(f);
    if (got != (size_t)want) { free(buf); return NULL; }
    return buf;   /* BGRA, premultiplied, top-down */
}

#endif
