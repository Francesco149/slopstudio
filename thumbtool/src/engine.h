// thumbtool engine — CPU still-image compositor for .thumb.json documents.
//
// Brand-agnostic by construction: every color can be a "$name" reference resolved
// through a *brand package* (a directory with brand.json + fonts/ + assets/), and
// text layers pull their defaults from the package's named text styles. The tool
// never hardcodes a channel's branding — point it at a different package and the
// same document rethemes.
//
// Rendering is split in two (the slopstudio two-layer idea, in miniature):
//   1. build_layers(): each layer rasterizes into a padded RGBA *block* (CPU,
//      stb) — expensive (resize/distance-transform/text), so blocks are CACHED
//      by a param-hash that EXCLUDES position/rotation/opacity. Dragging a layer
//      is pure cache hits.
//   2. compose: blocks are blended onto the canvas. The CPU path below is the
//      deterministic reference (headless --export, no GPU needed); the GUI
//      composites the same blocks on the GPU (D3D11) for a lag-free preview.
//
// Document format (docs/THUMBNAIL_TOOL.md is the authoritative spec):
// {
//   "format": "thumb-1", "canvas": [1280,720], "brand": "path/to/package",
//   "title": "paired video title (lint context)",
//   "layers": [
//     {"id":"bg",  "type":"bg", "fill":"$bg", "grad_to":"#1a0f35", "grad_angle":90,
//      "image":"...", "blur":0, "darken":0, "vignette":0.35},
//     {"id":"host","type":"image", "src":"...", "x":940,"y":420, "scale":1.05,
//      "rot":-3, "flip":false, "outline_px":12, "outline":"$white",
//      "shadow":{"dx":10,"dy":14,"blur":18,"alpha":0.5},
//      "glow":{"px":40,"color":"$magenta","alpha":0.6}},
//     {"id":"h1","type":"text", "text":"IT'S\nALIVE", "style":"headline",
//      "x":340,"y":300, "rot":-4, "px":180, "fill":"$white", "grad_to":"$pink",
//      "stroke_px":14, "stroke":"$black", "align":"center", "max_w":600,
//      "line_height":0.9,   // ×natural leading; <1 packs multi-line text tighter
//      "plate":{"pad_x":28,"pad_y":18,"radius":18,"fill":"$black","alpha":0.75}},
//     {"id":"a1","type":"shape", "shape":"arrow|circle|rect",
//      "x1":..,"y1":..,"x2":..,"y2":..,"width":26,        // arrow
//      "x":..,"y":..,"r":..,"thick":..,                   // circle (thick 0 = disc)
//      "w":..,"h":..,"radius":..,                         // rect  (thick 0 = filled)
//      "fill":"$gold","grad_to":"","grad_angle":90,"outline_px":8,"outline":"$black","shadow":{...}},
//     {"id":"cs","type":"mosaic","x":..,"y":..,"w":..,"h":..,"cell":22},  // censor-pixelate
//     {"id":"wm","type":"watermark"}                      // brand package watermark
//   ]
// }
// Coordinates are in *logical canvas pixels* (e.g. 1280x720); x/y are the layer
// CENTER (arrow uses its endpoints). image scale 1.0 == scaled to canvas height.
#pragma once

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize.h"
#include "stb_truetype.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ───────────────────────────── small utils ─────────────────────────────────
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

// WSL → Windows path translation. The PE runs under WSLInterop: "/mnt/x/…" is
// really drive x:, any other absolute unix path lives inside the distro (reach it
// via the \\wsl.localhost UNC). Relative paths pass through (cwd is inherited).
static std::string host_path(const std::string& p) {
    if (p.size() >= 7 && p.compare(0, 5, "/mnt/") == 0 && isalpha((unsigned char)p[5]) && p[6] == '/') {
        std::string r; r += (char)toupper(p[5]); r += ":";
        for (size_t i = 6; i < p.size(); i++) r += (p[i] == '/') ? '\\' : p[i];
        return r;
    }
    if (!p.empty() && p[0] == '/') {
        const char* distro = getenv("WSL_DISTRO_NAME");
        std::string r = std::string("\\\\wsl.localhost\\") + (distro && *distro ? distro : "NixOS");
        for (char c : p) r += (c == '/') ? '\\' : c;
        return r;
    }
    return p;
}

static bool path_is_abs(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (p.size() >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':') return true;
    return false;
}
static std::string path_dir(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}
static std::string path_join(const std::string& a, const std::string& b) {
    if (b.empty()) return a;
    if (path_is_abs(b)) return b;
    if (a.empty()) return b;
    char sep = (a.find('\\') != std::string::npos && a.find('/') == std::string::npos) ? '\\' : '/';
    return (a.back() == '/' || a.back() == '\\') ? a + b : a + sep + b;
}

// json getters with defaults (missing OR null → default)
static double jf(const json& j, const char* k, double d) { auto it = j.find(k); return (it != j.end() && it->is_number()) ? it->get<double>() : d; }
static int    ji(const json& j, const char* k, int d)    { auto it = j.find(k); return (it != j.end() && it->is_number()) ? (int)it->get<double>() : d; }
static bool   jb(const json& j, const char* k, bool d)   { auto it = j.find(k); return (it != j.end() && it->is_boolean()) ? it->get<bool>() : d; }
static std::string js(const json& j, const char* k, const std::string& d) {
    auto it = j.find(k); return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
}

// ───────────────────────────── color ────────────────────────────────────────
struct RGBA { float r = 1, g = 1, b = 1, a = 1; };

static RGBA parse_hex_color(const std::string& s, bool* ok = nullptr) {
    RGBA c; if (ok) *ok = false;
    if (s.empty() || s[0] != '#') return c;
    auto hex = [&](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return 0;
    };
    if (s.size() == 4) { // #rgb
        c.r = hex(s[1]) / 15.f; c.g = hex(s[2]) / 15.f; c.b = hex(s[3]) / 15.f; c.a = 1;
        if (ok) *ok = true;
    } else if (s.size() >= 7) {
        c.r = (hex(s[1]) * 16 + hex(s[2])) / 255.f;
        c.g = (hex(s[3]) * 16 + hex(s[4])) / 255.f;
        c.b = (hex(s[5]) * 16 + hex(s[6])) / 255.f;
        c.a = (s.size() >= 9) ? (hex(s[7]) * 16 + hex(s[8])) / 255.f : 1.f;
        if (ok) *ok = true;
    }
    return c;
}

// ───────────────────────────── images ───────────────────────────────────────
struct Img {                       // RGBA8, straight (non-premultiplied) alpha
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    void alloc(int W, int H) { w = W; h = H; px.assign((size_t)W * H * 4, 0); }
    uint8_t* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
    bool empty() const { return w <= 0 || h <= 0; }
};
struct Mask {                      // single channel coverage 0..255
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    void alloc(int W, int H) { w = W; h = H; px.assign((size_t)W * H, 0); }
    uint8_t at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : px[(size_t)y * w + x]; }
};

static bool load_image(const std::string& path, Img& out) {
    int w, h, n;
    uint8_t* d = stbi_load(host_path(path).c_str(), &w, &h, &n, 4);
    if (!d) return false;
    out.w = w; out.h = h;
    out.px.assign(d, d + (size_t)w * h * 4);
    stbi_image_free(d);
    return true;
}

// premultiplied high-quality resize (premult avoids dark halos on alpha edges)
static void resize_img(const Img& src, Img& dst, int dw, int dh) {
    if (src.empty() || dw <= 0 || dh <= 0) { dst = Img(); return; }
    std::vector<uint8_t> pre((size_t)src.w * src.h * 4);
    for (size_t i = 0; i < pre.size(); i += 4) {
        int a = src.px[i + 3];
        pre[i] = (uint8_t)(src.px[i] * a / 255); pre[i + 1] = (uint8_t)(src.px[i + 1] * a / 255);
        pre[i + 2] = (uint8_t)(src.px[i + 2] * a / 255); pre[i + 3] = (uint8_t)a;
    }
    std::vector<uint8_t> out((size_t)dw * dh * 4);
    stbir_resize_uint8(pre.data(), src.w, src.h, 0, out.data(), dw, dh, 0, 4);
    dst.alloc(dw, dh);
    for (size_t i = 0; i < out.size(); i += 4) {
        int a = out[i + 3];
        if (a > 0) {
            dst.px[i] = (uint8_t)std::min(255, out[i] * 255 / a);
            dst.px[i + 1] = (uint8_t)std::min(255, out[i + 1] * 255 / a);
            dst.px[i + 2] = (uint8_t)std::min(255, out[i + 2] * 255 / a);
        }
        dst.px[i + 3] = (uint8_t)a;
    }
}

// straight-alpha "src over dst" at integer offset
static void blit_over(Img& dst, const Img& src, int ox, int oy, float opacity = 1.f) {
    int x0 = std::max(0, -ox), y0 = std::max(0, -oy);
    int x1 = std::min(src.w, dst.w - ox), y1 = std::min(src.h, dst.h - oy);
    for (int y = y0; y < y1; y++) {
        const uint8_t* sp = src.at(x0, y);
        uint8_t* dp = dst.at(x0 + ox, y + oy);
        for (int x = x0; x < x1; x++, sp += 4, dp += 4) {
            float sa = sp[3] / 255.f * opacity;
            if (sa <= 0.f) continue;
            float da = dp[3] / 255.f, oa = sa + da * (1 - sa);
            if (oa <= 0.f) continue;
            for (int c = 0; c < 3; c++)
                dp[c] = (uint8_t)clampf((sp[c] * sa + dp[c] * da * (1 - sa)) / oa + 0.5f, 0, 255);
            dp[3] = (uint8_t)clampf(oa * 255.f + 0.5f, 0, 255);
        }
    }
}

// bilinear sample (straight alpha, alpha-weighted color to avoid edge fringes)
static void sample_bilinear(const Img& s, float fx, float fy, float out[4]) {
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - x0, ty = fy - y0;
    float acc[4] = {0, 0, 0, 0}, wsum = 0;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
            int x = x0 + dx, y = y0 + dy;
            float w = (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty);
            if (w <= 0 || x < 0 || y < 0 || x >= s.w || y >= s.h) continue;
            const uint8_t* p = s.at(x, y);
            float a = p[3] / 255.f;
            acc[0] += w * a * p[0]; acc[1] += w * a * p[1]; acc[2] += w * a * p[2];
            acc[3] += w * a; wsum += w;
        }
    (void)wsum;
    if (acc[3] > 1e-6f) { out[0] = acc[0] / acc[3]; out[1] = acc[1] / acc[3]; out[2] = acc[2] / acc[3]; }
    else out[0] = out[1] = out[2] = 0;
    out[3] = acc[3] * 255.f;
}

// composite src over dst rotated by `deg` about src center placed at (cx, cy) in dst
static void blit_rotated(Img& dst, const Img& src, float cx, float cy, float deg, float opacity = 1.f) {
    if (src.empty()) return;
    if (fabsf(deg) < 0.01f) { blit_over(dst, src, (int)lroundf(cx - src.w / 2.f), (int)lroundf(cy - src.h / 2.f), opacity); return; }
    float rad = deg * 3.14159265f / 180.f, cs = cosf(rad), sn = sinf(rad);
    float hw = src.w / 2.f, hh = src.h / 2.f;
    // dst-space bbox of the rotated block
    float ex = fabsf(cs) * hw + fabsf(sn) * hh, ey = fabsf(sn) * hw + fabsf(cs) * hh;
    int x0 = std::max(0, (int)floorf(cx - ex) - 1), x1 = std::min(dst.w, (int)ceilf(cx + ex) + 1);
    int y0 = std::max(0, (int)floorf(cy - ey) - 1), y1 = std::min(dst.h, (int)ceilf(cy + ey) + 1);
    for (int y = y0; y < y1; y++) {
        uint8_t* dp = dst.at(x0 > 0 ? x0 : 0, y);
        for (int x = x0; x < x1; x++, dp += 4) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float sx = cs * dx + sn * dy + hw - 0.5f;   // inverse rotation
            float sy = -sn * dx + cs * dy + hh - 0.5f;
            if (sx < -1 || sy < -1 || sx > src.w || sy > src.h) continue;
            float sc[4]; sample_bilinear(src, sx, sy, sc);
            float sa = sc[3] / 255.f * opacity;
            if (sa <= 0.003f) continue;
            float da = dp[3] / 255.f, oa = sa + da * (1 - sa);
            for (int c = 0; c < 3; c++)
                dp[c] = (uint8_t)clampf((sc[c] * sa + dp[c] * da * (1 - sa)) / oa + 0.5f, 0, 255);
            dp[3] = (uint8_t)clampf(oa * 255.f + 0.5f, 0, 255);
        }
    }
}

// ───────────────────── mask ops: blur / distance / colorize ─────────────────
static void box_blur_mask(Mask& m, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> tmp(m.px.size());
    for (int pass = 0; pass < 3; pass++) {
        int rr = std::max(1, r / 2);
        // horizontal
        for (int y = 0; y < m.h; y++) {
            int sum = 0, n = 2 * rr + 1;
            const uint8_t* row = &m.px[(size_t)y * m.w];
            for (int x = -rr; x <= rr; x++) sum += row[std::max(0, std::min(m.w - 1, x))];
            for (int x = 0; x < m.w; x++) {
                tmp[(size_t)y * m.w + x] = (uint8_t)(sum / n);
                int xi = std::min(m.w - 1, x + rr + 1), xo = std::max(0, x - rr);
                sum += row[xi] - row[xo];
            }
        }
        // vertical
        for (int x = 0; x < m.w; x++) {
            int sum = 0, n = 2 * rr + 1;
            for (int y = -rr; y <= rr; y++) sum += tmp[(size_t)std::max(0, std::min(m.h - 1, y)) * m.w + x];
            for (int y = 0; y < m.h; y++) {
                m.px[(size_t)y * m.w + x] = (uint8_t)(sum / n);
                int yi = std::min(m.h - 1, y + rr + 1), yo = std::max(0, y - rr);
                sum += tmp[(size_t)yi * m.w + x] - tmp[(size_t)yo * m.w + x];
            }
        }
    }
}

// chamfer distance (px) to the nearest covered pixel — for outlines/glows.
// dist 0 inside coverage, grows outward; good to a fraction of a px, plenty for stickers.
static void chamfer_dist(const Mask& m, std::vector<float>& d) {
    const float BIG = 1e9f, W1 = 1.f, W2 = 1.4142f;
    d.assign((size_t)m.w * m.h, BIG);
    for (size_t i = 0; i < d.size(); i++)
        if (m.px[i] >= 128) d[i] = 0;
        else if (m.px[i] > 0) d[i] = 1.f - m.px[i] / 255.f;  // soft edge: sub-pixel head start
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) {
            float& v = d[(size_t)y * m.w + x];
            if (x > 0) v = std::min(v, d[(size_t)y * m.w + x - 1] + W1);
            if (y > 0) {
                v = std::min(v, d[(size_t)(y - 1) * m.w + x] + W1);
                if (x > 0) v = std::min(v, d[(size_t)(y - 1) * m.w + x - 1] + W2);
                if (x < m.w - 1) v = std::min(v, d[(size_t)(y - 1) * m.w + x + 1] + W2);
            }
        }
    for (int y = m.h - 1; y >= 0; y--)
        for (int x = m.w - 1; x >= 0; x--) {
            float& v = d[(size_t)y * m.w + x];
            if (x < m.w - 1) v = std::min(v, d[(size_t)y * m.w + x + 1] + W1);
            if (y < m.h - 1) {
                v = std::min(v, d[(size_t)(y + 1) * m.w + x] + W1);
                if (x < m.w - 1) v = std::min(v, d[(size_t)(y + 1) * m.w + x + 1] + W2);
                if (x > 0) v = std::min(v, d[(size_t)(y + 1) * m.w + x - 1] + W2);
            }
        }
}

// paint `color` into dst wherever alphaMask says, under|over existing content
static void paint_mask(Img& dst, const Mask& m, RGBA color, bool under = false) {
    for (int y = 0; y < dst.h; y++) {
        uint8_t* dp = dst.at(0, y);
        const uint8_t* mp = &m.px[(size_t)y * m.w];
        for (int x = 0; x < dst.w; x++, dp += 4, mp++) {
            float sa = (*mp / 255.f) * color.a;
            if (sa <= 0.f) continue;
            float da = dp[3] / 255.f;
            if (!under) {
                float oa = sa + da * (1 - sa);
                for (int c = 0; c < 3; c++) {
                    float sc = (c == 0 ? color.r : c == 1 ? color.g : color.b) * 255.f;
                    dp[c] = (uint8_t)clampf((sc * sa + dp[c] * da * (1 - sa)) / oa + 0.5f, 0, 255);
                }
                dp[3] = (uint8_t)clampf(oa * 255.f + 0.5f, 0, 255);
            } else {                       // dst over (color*mask): fills only where dst is transparent
                float oa = da + sa * (1 - da);
                if (oa <= 0) continue;
                for (int c = 0; c < 3; c++) {
                    float sc = (c == 0 ? color.r : c == 1 ? color.g : color.b) * 255.f;
                    dp[c] = (uint8_t)clampf((dp[c] * da + sc * sa * (1 - da)) / oa + 0.5f, 0, 255);
                }
                dp[3] = (uint8_t)clampf(oa * 255.f + 0.5f, 0, 255);
            }
        }
    }
}

// ───────────────────────────── fonts / text ────────────────────────────────
struct FontEntry { std::vector<uint8_t> data; stbtt_fontinfo info; bool ok = false; };
static std::map<std::string, FontEntry>& font_cache() { static std::map<std::string, FontEntry> c; return c; }

static FontEntry* get_font(const std::string& path) {
    auto& c = font_cache();
    auto it = c.find(path);
    if (it != c.end()) return it->second.ok ? &it->second : nullptr;
    FontEntry& e = c[path];
    std::string bytes = read_text_file(host_path(path));
    if (bytes.empty()) return nullptr;
    e.data.assign(bytes.begin(), bytes.end());
    e.ok = stbtt_InitFont(&e.info, e.data.data(), stbtt_GetFontOffsetForIndex(e.data.data(), 0)) != 0;
    return e.ok ? &e : nullptr;
}

static const char* utf8_next(const char* s, unsigned* cp) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) { *cp = c; return s + 1; }
    if ((c >> 5) == 6 && s[1]) { *cp = ((c & 31) << 6) | (s[1] & 63); return s + 2; }
    if ((c >> 4) == 14 && s[1] && s[2]) { *cp = ((c & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); return s + 3; }
    if ((c >> 3) == 30 && s[1] && s[2] && s[3]) { *cp = ((c & 7) << 18) | ((s[1] & 63) << 12) | ((s[2] & 63) << 6) | (s[3] & 63); return s + 4; }
    *cp = '?'; return s + 1;
}

// rasterize multi-line text into a tight mask; returns baseline metrics via outs
static bool raster_text(const std::string& text, FontEntry* fe, float px, float tracking,
                        float lineHeightMul, const std::string& align, Mask& out) {
    if (!fe || text.empty()) return false;
    float s = stbtt_ScaleForPixelHeight(&fe->info, px);
    int ia, id, ig; stbtt_GetFontVMetrics(&fe->info, &ia, &id, &ig);
    float ascent = ia * s, descent = id * s, lineAdv = (ia - id + ig) * s * lineHeightMul;

    std::vector<std::string> lines; { std::stringstream ss(text); std::string l; while (std::getline(ss, l)) lines.push_back(l); }
    if (lines.empty()) return false;

    std::vector<float> widths;
    float maxW = 0;
    for (auto& l : lines) {
        float w = 0; unsigned prev = 0;
        for (const char* p = l.c_str(); *p;) {
            unsigned cp; p = utf8_next(p, &cp);
            int adv, lsb; stbtt_GetCodepointHMetrics(&fe->info, (int)cp, &adv, &lsb);
            if (prev) w += stbtt_GetCodepointKernAdvance(&fe->info, (int)prev, (int)cp) * s;
            w += adv * s + tracking;
            prev = cp;
        }
        widths.push_back(w); maxW = std::max(maxW, w);
    }
    int W = (int)ceilf(maxW) + 8;
    int H = (int)ceilf(ascent - descent + lineAdv * (lines.size() - 1)) + 8;
    out.alloc(W, H);
    for (size_t li = 0; li < lines.size(); li++) {
        float xo = 4;
        if (align == "center") xo = (W - widths[li]) / 2.f;
        else if (align == "right") xo = W - 4 - widths[li];
        float base = 4 + ascent + lineAdv * li;
        unsigned prev = 0;
        for (const char* p = lines[li].c_str(); *p;) {
            unsigned cp; p = utf8_next(p, &cp);
            if (prev) xo += stbtt_GetCodepointKernAdvance(&fe->info, (int)prev, (int)cp) * s;
            int adv, lsb; stbtt_GetCodepointHMetrics(&fe->info, (int)cp, &adv, &lsb);
            int x0, y0, x1, y1;
            float shift = xo - floorf(xo);
            stbtt_GetCodepointBitmapBoxSubpixel(&fe->info, (int)cp, s, s, shift, 0, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                std::vector<uint8_t> g((size_t)gw * gh);
                stbtt_MakeCodepointBitmapSubpixel(&fe->info, g.data(), gw, gh, gw, s, s, shift, 0, (int)cp);
                int dx0 = (int)floorf(xo) + x0, dy0 = (int)lroundf(base) + y0;
                for (int y = 0; y < gh; y++)
                    for (int x = 0; x < gw; x++) {
                        int X = dx0 + x, Y = dy0 + y;
                        if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                        uint8_t& dp = out.px[(size_t)Y * W + X];
                        dp = std::max(dp, g[(size_t)y * gw + x]);
                    }
            }
            xo += adv * s + tracking;
            prev = cp;
        }
    }
    return true;
}

// ───────────────────────────── brand package ───────────────────────────────
struct Brand {
    std::string dir, name = "(none)";
    std::map<std::string, std::string> palette;   // name → "#RRGGBB"
    std::map<std::string, std::string> fonts;     // name → absolute-ish path
    json styles = json::object();                 // name → text style defaults
    json sticker = json::object();                // default image-layer outline/shadow
    std::vector<std::string> sprite_roots;
    json watermark = json::object();
    json templates = json::array();
    json lint = json::object();
    bool ok = false;
    std::string err;

    RGBA color(const std::string& spec, RGBA def = RGBA{1, 1, 1, 1}) const {
        if (spec.empty()) return def;
        if (spec[0] == '$') {
            auto it = palette.find(spec.substr(1));
            if (it == palette.end()) return def;
            bool k; RGBA c = parse_hex_color(it->second, &k); return k ? c : def;
        }
        bool k; RGBA c = parse_hex_color(spec, &k); return k ? c : def;
    }
    std::string font_path(const std::string& fname) const {
        auto it = fonts.find(fname);
        if (it != fonts.end()) return it->second;
        return fonts.count("display") ? fonts.at("display") : "";
    }
};

static Brand load_brand(const std::string& dirIn) {
    Brand b; b.dir = dirIn;
    std::string txt = read_text_file(host_path(path_join(dirIn, "brand.json")));
    if (txt.empty()) { b.err = "brand.json not found in " + dirIn; return b; }
    json j = json::parse(txt, nullptr, false);
    if (j.is_discarded()) { b.err = "brand.json parse error"; return b; }
    b.name = js(j, "name", "unnamed");
    if (j.contains("palette")) for (auto& [k, v] : j["palette"].items()) if (v.is_string()) b.palette[k] = v;
    if (j.contains("fonts"))   for (auto& [k, v] : j["fonts"].items())   if (v.is_string()) b.fonts[k] = path_join(dirIn, v.get<std::string>());
    if (j.contains("styles"))    b.styles = j["styles"];
    if (j.contains("sticker"))   b.sticker = j["sticker"];
    if (j.contains("watermark")) { b.watermark = j["watermark"];
        if (b.watermark.contains("src")) b.watermark["src"] = path_join(dirIn, b.watermark["src"].get<std::string>()); }
    if (j.contains("templates")) b.templates = j["templates"];
    if (j.contains("lint"))      b.lint = j["lint"];
    if (j.contains("sprite_roots")) for (auto& v : j["sprite_roots"]) if (v.is_string()) b.sprite_roots.push_back(path_join(dirIn, v.get<std::string>()));
    b.ok = true;
    return b;
}

// layer param lookup: layer override → brand text style → fallback
static json style_of(const Brand& b, const json& layer) {
    std::string st = js(layer, "style", "");
    if (!st.empty() && b.styles.contains(st)) return b.styles[st];
    if (b.styles.contains("headline")) return b.styles["headline"];
    return json::object();
}
static double sf(const json& layer, const json& style, const char* k, double d) {
    if (layer.contains(k) && layer[k].is_number()) return layer[k].get<double>();
    return jf(style, k, d);
}
static std::string ss_(const json& layer, const json& style, const char* k, const std::string& d) {
    if (layer.contains(k) && layer[k].is_string()) return layer[k].get<std::string>();
    return js(style, k, d);
}
static json sj(const json& layer, const json& style, const char* k) {
    if (layer.contains(k) && layer[k].is_object()) return layer[k];
    if (style.contains(k) && style[k].is_object()) return style[k];
    return json();
}

// ─────────────────────── block effects (glow/outline/shadow) ────────────────
// Blocks are tight, padded RGBA scraps; effects render *under* the content.
struct FxSpec {
    float outline_px = 0; RGBA outline{0, 0, 0, 1};
    float glow_px = 0; RGBA glow{1, 1, 1, 1}; float glow_alpha = 0.6f;
    bool has_shadow = false; float sh_dx = 0, sh_dy = 0, sh_blur = 0, sh_alpha = 0.5f; RGBA sh_col{0, 0, 0, 1};
};

static float fx_margin(const FxSpec& fx) {
    float m = fx.outline_px + (fx.glow_px > 0 ? fx.glow_px * 1.5f : 0);
    if (fx.has_shadow) m = std::max(m, std::max(fabsf(fx.sh_dx), fabsf(fx.sh_dy)) + fx.sh_blur + fx.outline_px);
    return m + 4;
}

static FxSpec parse_fx(const json& layer, const json& defaults, const Brand& brand, float SS,
                       const char* outlineKey = "outline_px", const char* outlineColKey = "outline") {
    FxSpec fx;
    auto num = [&](const json& a, const json& b, const char* k, double d) {
        if (a.contains(k) && a[k].is_number()) return a[k].get<double>();
        return jf(b, k, d);
    };
    auto str = [&](const json& a, const json& b, const char* k, const std::string& d) {
        if (a.contains(k) && a[k].is_string()) return a[k].get<std::string>();
        return js(b, k, d);
    };
    fx.outline_px = (float)num(layer, defaults, outlineKey, 0) * SS;
    fx.outline = brand.color(str(layer, defaults, outlineColKey, "#000000"), RGBA{0, 0, 0, 1});
    json glow = layer.contains("glow") && layer["glow"].is_object() ? layer["glow"]
              : (defaults.contains("glow") && defaults["glow"].is_object() ? defaults["glow"] : json());
    if (!glow.is_null() && jf(glow, "px", 0) > 0) {
        fx.glow_px = (float)jf(glow, "px", 0) * SS;
        fx.glow = brand.color(js(glow, "color", "#ffffff"));
        fx.glow_alpha = (float)jf(glow, "alpha", 0.6);
    }
    json sh = layer.contains("shadow") && layer["shadow"].is_object() ? layer["shadow"]
            : (defaults.contains("shadow") && defaults["shadow"].is_object() ? defaults["shadow"] : json());
    if (!sh.is_null() && !jb(sh, "off", false)) {
        fx.has_shadow = true;
        fx.sh_dx = (float)jf(sh, "dx", 8) * SS; fx.sh_dy = (float)jf(sh, "dy", 10) * SS;
        fx.sh_blur = (float)jf(sh, "blur", 12) * SS; fx.sh_alpha = (float)jf(sh, "alpha", 0.5);
        fx.sh_col = brand.color(js(sh, "color", "#000000"), RGBA{0, 0, 0, 1});
    }
    return fx;
}

// content already in `block`; carve its alpha into a mask and stack fx underneath
static void apply_fx(Img& block, const FxSpec& fx) {
    Mask m; m.alloc(block.w, block.h);
    for (int i = 0; i < block.w * block.h; i++) m.px[i] = block.px[(size_t)i * 4 + 3];

    std::vector<float> dist;
    bool needDist = fx.outline_px > 0.01f || fx.glow_px > 0.01f;
    if (needDist) chamfer_dist(m, dist);

    Img under; under.alloc(block.w, block.h);
    // glow (deepest)
    if (fx.glow_px > 0.01f) {
        Mask g; g.alloc(block.w, block.h);
        for (size_t i = 0; i < dist.size(); i++) {
            float d = dist[i] - fx.outline_px;                     // glow starts at the outline edge
            g.px[i] = (uint8_t)(255.f * expf(-std::max(0.f, d) / (fx.glow_px * 0.5f)));
        }
        RGBA gc = fx.glow; gc.a *= fx.glow_alpha;
        paint_mask(under, g, gc);
    }
    // sticker outline
    Mask omask;
    if (fx.outline_px > 0.01f) {
        omask.alloc(block.w, block.h);
        for (size_t i = 0; i < dist.size(); i++)
            omask.px[i] = (uint8_t)(255.f * clampf(fx.outline_px + 0.5f - dist[i], 0.f, 1.f));
        paint_mask(under, omask, fx.outline);
    }
    // drop shadow of (content ∪ outline)
    if (fx.has_shadow) {
        Mask sh; sh.alloc(block.w, block.h);
        const Mask& srcm = fx.outline_px > 0.01f ? omask : m;
        int dx = (int)lroundf(fx.sh_dx), dy = (int)lroundf(fx.sh_dy);
        for (int y = 0; y < block.h; y++)
            for (int x = 0; x < block.w; x++) {
                int sx = x - dx, sy = y - dy;
                uint8_t v = srcm.at(sx, sy);
                if (fx.outline_px <= 0.01f) v = std::max(v, m.at(sx, sy));
                sh.px[(size_t)y * block.w + x] = v;
            }
        box_blur_mask(sh, (int)fx.sh_blur);
        RGBA sc = fx.sh_col; sc.a *= fx.sh_alpha;
        Img shadowImg; shadowImg.alloc(block.w, block.h);
        paint_mask(shadowImg, sh, sc);
        blit_over(shadowImg, under, 0, 0);           // under-stack over the shadow
        under = std::move(shadowImg);
    }
    blit_over(under, block, 0, 0);                    // content on top
    block = std::move(under);
}

// ─────────────────────────── shape SDFs (block masks) ───────────────────────
static float sd_segment(float px, float py, float ax, float ay, float bx, float by) {
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = clampf((pax * bax + pay * bay) / (bax * bax + bay * bay + 1e-9f), 0.f, 1.f);
    float dx = pax - bax * h, dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy);
}
static float sd_triangle(float px, float py, const float v[6]) {
    // iq's exact triangle SDF
    float e0x = v[2] - v[0], e0y = v[3] - v[1], e1x = v[4] - v[2], e1y = v[5] - v[3], e2x = v[0] - v[4], e2y = v[1] - v[5];
    float p0x = px - v[0], p0y = py - v[1], p1x = px - v[2], p1y = py - v[3], p2x = px - v[4], p2y = py - v[5];
    auto dot2 = [](float x, float y) { return x * x + y * y; };
    float q0 = clampf((p0x * e0x + p0y * e0y) / dot2(e0x, e0y), 0, 1);
    float q1 = clampf((p1x * e1x + p1y * e1y) / dot2(e1x, e1y), 0, 1);
    float q2 = clampf((p2x * e2x + p2y * e2y) / dot2(e2x, e2y), 0, 1);
    float d = std::min({dot2(p0x - e0x * q0, p0y - e0y * q0), dot2(p1x - e1x * q1, p1y - e1y * q1), dot2(p2x - e2x * q2, p2y - e2y * q2)});
    float s = e0x * p0y - e0y * p0x, s1 = e1x * p1y - e1y * p1x, s2 = e2x * p2y - e2y * p2x;
    bool inside = (s <= 0 && s1 <= 0 && s2 <= 0) || (s >= 0 && s1 >= 0 && s2 >= 0);
    return inside ? -sqrtf(d) : sqrtf(d);
}

// ─────────────────────────── document rendering ────────────────────────────
struct LayerInfo { std::string id, type; float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

static std::map<std::string, Img>& image_cache() { static std::map<std::string, Img> c; return c; }
static Img* get_image(const std::string& path) {
    auto& c = image_cache();
    auto it = c.find(path);
    if (it != c.end()) return it->second.empty() ? nullptr : &it->second;
    Img& im = c[path];
    load_image(path, im);
    return im.empty() ? nullptr : &im;
}

// fill the SS-space canvas from a bg layer
static void render_bg(Img& canvas, const json& L, const Brand& brand, const std::string& docDir, float SS) {
    RGBA c0 = brand.color(js(L, "fill", "#101018"), RGBA{0.06f, 0.06f, 0.1f, 1});
    std::string gradTo = js(L, "grad_to", "");
    bool grad = !gradTo.empty();
    RGBA c1 = grad ? brand.color(gradTo) : c0;
    float ang = (float)jf(L, "grad_angle", 90) * 3.14159265f / 180.f;
    float gx = cosf(ang), gy = sinf(ang);
    for (int y = 0; y < canvas.h; y++) {
        uint8_t* dp = canvas.at(0, y);
        for (int x = 0; x < canvas.w; x++, dp += 4) {
            RGBA c = c0;
            if (grad) {
                float t = clampf(((x / (float)canvas.w - 0.5f) * gx + (y / (float)canvas.h - 0.5f) * gy) + 0.5f, 0, 1);
                c.r = c0.r + (c1.r - c0.r) * t; c.g = c0.g + (c1.g - c0.g) * t; c.b = c0.b + (c1.b - c0.b) * t;
            }
            dp[0] = (uint8_t)(c.r * 255); dp[1] = (uint8_t)(c.g * 255); dp[2] = (uint8_t)(c.b * 255); dp[3] = 255;
        }
    }
    std::string imgPath = js(L, "image", "");
    if (!imgPath.empty()) {
        Img* src = get_image(path_join(docDir, imgPath));
        if (src) {
            float sc = std::max(canvas.w / (float)src->w, canvas.h / (float)src->h);  // cover fit
            int dw = (int)ceilf(src->w * sc), dh = (int)ceilf(src->h * sc);
            Img scaled; resize_img(*src, scaled, dw, dh);
            Img crop; crop.alloc(canvas.w, canvas.h);
            int ox = (dw - canvas.w) / 2, oy = (dh - canvas.h) / 2;
            for (int y = 0; y < canvas.h; y++)
                memcpy(crop.at(0, y), scaled.at(ox, y + oy), (size_t)canvas.w * 4);
            int blur = (int)(jf(L, "blur", 0) * SS);
            if (blur > 0) {  // blur RGB via three mask blurs (bg is opaque)
                Mask ch; ch.alloc(crop.w, crop.h);
                for (int c = 0; c < 3; c++) {
                    for (int i = 0; i < crop.w * crop.h; i++) ch.px[i] = crop.px[(size_t)i * 4 + c];
                    box_blur_mask(ch, blur);
                    for (int i = 0; i < crop.w * crop.h; i++) crop.px[(size_t)i * 4 + c] = ch.px[i];
                }
            }
            float op = (float)jf(L, "opacity", 1.0);
            blit_over(canvas, crop, 0, 0, op);
        }
    }
    float darken = (float)jf(L, "darken", 0);
    if (darken > 0)
        for (size_t i = 0; i < canvas.px.size(); i += 4)
            for (int c = 0; c < 3; c++) canvas.px[i + c] = (uint8_t)(canvas.px[i + c] * (1 - darken));
    float vig = (float)jf(L, "vignette", 0);
    if (vig > 0) {
        float cx = canvas.w / 2.f, cy = canvas.h / 2.f, maxd = sqrtf(cx * cx + cy * cy);
        for (int y = 0; y < canvas.h; y++) {
            uint8_t* dp = canvas.at(0, y);
            for (int x = 0; x < canvas.w; x++, dp += 4) {
                float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / maxd;
                float f = 1.f - vig * clampf((d - 0.45f) / 0.55f, 0, 1) * clampf((d - 0.45f) / 0.55f, 0, 1);
                dp[0] = (uint8_t)(dp[0] * f); dp[1] = (uint8_t)(dp[1] * f); dp[2] = (uint8_t)(dp[2] * f);
            }
        }
    }
}

// ─────────────────── built layers (the block cache) ─────────────────────────
// A layer builds into a padded RGBA block whose CONTENT does not depend on
// x/y/rot/opacity — so the block is cached by the remaining params and moving a
// layer never re-rasterizes anything. Compose (CPU below, GPU in the app) just
// places blocks.
struct BuiltLayer {
    std::shared_ptr<Img> block;          // null for mosaic / hidden / broken layers
    bool isBg = false, isMosaic = false;
    float cx = 0, cy = 0, rot = 0, opacity = 1;   // SS-canvas coords
    int mos[5] = {0, 0, 0, 0, 0};                 // mosaic: x0,y0,x1,y1,cell (SS px)
    // live-gesture path: block was rasterized at a LOWER ss than the compose
    // space (upscale by sscale) and is NOT in the block cache (fresh every frame)
    float sscale = 1;
    bool transient = false;
};

static int g_blockCacheGen = 1;   // bumped when the cache is purged (GPU tex cache keys off block pointers)
static std::map<std::string, std::shared_ptr<Img>>& block_cache() {
    static std::map<std::string, std::shared_ptr<Img>> c; return c;
}
static std::shared_ptr<Img>* block_cache_find(const std::string& key) {
    auto& c = block_cache();
    auto it = c.find(key);
    return it == c.end() ? nullptr : &it->second;
}
static void block_cache_store(const std::string& key, std::shared_ptr<Img> img) {
    auto& c = block_cache();
    if (c.size() > 64) { c.clear(); g_blockCacheGen++; }   // crude but effective cap (bg blocks are canvas-sized)
    c[key] = std::move(img);
}

// cache key: the layer params that AFFECT BLOCK CONTENT (position/rot/opacity out;
// arrows keep only their delta), plus context that changes rasterization.
static std::string block_key(const json& L, const Brand& brand, const std::string& docDir, int cw, int ch, int ss) {
    json k = L;
    for (const char* drop : {"x", "y", "rot", "opacity", "hidden", "id", "_comment"}) k.erase(drop);
    if (js(L, "type", "") == "shape" && js(L, "shape", "") == "arrow") {
        for (const char* drop : {"x1", "y1", "x2", "y2"}) k.erase(drop);
        k["adx"] = jf(L, "x2", 300) - jf(L, "x1", 100);
        k["ady"] = jf(L, "y2", 300) - jf(L, "y1", 100);
    }
    char ctx[160];
    snprintf(ctx, sizeof ctx, "|%dx%d|ss%d|%s|%s", cw, ch, ss, brand.dir.c_str(), docDir.c_str());
    return k.dump() + ctx;
}

// Build one layer. Returns false if the layer produced nothing drawable.
// `info` gets the logical-space bbox either way.
// `transient` (live-gesture rebuild): bypass the block cache entirely — a drag
// that changes block content would otherwise flood the 64-entry cache with
// one-frame keys and trigger wholesale purges (= every layer re-rasters).
static bool build_layer(const json& L, const Brand& brand, const std::string& docDir,
                        int cw, int ch, int ssFactor, BuiltLayer& out, LayerInfo& li, std::string* err,
                        bool transient = false) {
    float SS = (float)ssFactor;
    std::string type = js(L, "type", "");
    li.id = js(L, "id", ""); li.type = type;
    out = BuiltLayer();
    if (jb(L, "hidden", false)) return false;

    if (type == "mosaic") {
        float mw = (float)jf(L, "w", 240), mh = (float)jf(L, "h", 140);
        out.isMosaic = true;
        out.mos[0] = std::max(0, (int)(((float)jf(L, "x", cw / 2.0) - mw / 2) * SS));
        out.mos[1] = std::max(0, (int)(((float)jf(L, "y", ch / 2.0) - mh / 2) * SS));
        out.mos[2] = std::min(cw * ssFactor, (int)(out.mos[0] + mw * SS));
        out.mos[3] = std::min(ch * ssFactor, (int)(out.mos[1] + mh * SS));
        out.mos[4] = std::max(2, (int)(jf(L, "cell", 22) * SS));
        li.x0 = out.mos[0] / SS; li.y0 = out.mos[1] / SS; li.x1 = out.mos[2] / SS; li.y1 = out.mos[3] / SS;
        return true;
    }

    std::string key = transient ? std::string() : block_key(L, brand, docDir, cw, ch, ssFactor);
    std::shared_ptr<Img>* cached = transient ? nullptr : block_cache_find(key);
    std::shared_ptr<Img> block = cached ? *cached : nullptr;

    if (type == "bg") {
        if (!block) {
            block = std::make_shared<Img>();
            block->alloc(cw * ssFactor, ch * ssFactor);
            render_bg(*block, L, brand, docDir, SS);
            if (!transient) block_cache_store(key, block);
        }
        out.block = block; out.isBg = true;
        out.cx = cw * SS / 2; out.cy = ch * SS / 2;
        li.x1 = (float)cw; li.y1 = (float)ch;
        return true;
    }

    if (type == "image") {
        std::string src = js(L, "src", "");
        if (!block) {
            Img* im = src.empty() ? nullptr : get_image(path_join(docDir, src));
            if (!im) {
                if (err && !src.empty()) *err += "image not found: " + src + "\n";
                return false;
            }
            float scale = (float)jf(L, "scale", 1.0);
            float targetH = scale * ch * SS;
            float targetW = targetH * im->w / im->h;
            Img scaled; resize_img(*im, scaled, std::max(1, (int)lroundf(targetW)), std::max(1, (int)lroundf(targetH)));
            if (jb(L, "flip", false)) {
                for (int y = 0; y < scaled.h; y++)
                    for (int x = 0; x < scaled.w / 2; x++) {
                        uint8_t* a = scaled.at(x, y), * b = scaled.at(scaled.w - 1 - x, y);
                        for (int c = 0; c < 4; c++) std::swap(a[c], b[c]);
                    }
            }
            FxSpec fx = parse_fx(L, brand.sticker, brand, SS);
            int mg = (int)ceilf(fx_margin(fx));
            block = std::make_shared<Img>();
            block->alloc(scaled.w + mg * 2, scaled.h + mg * 2);
            blit_over(*block, scaled, mg, mg);
            apply_fx(*block, fx);
            if (!transient) block_cache_store(key, block);
        }
        out.block = block;
        out.cx = (float)jf(L, "x", cw / 2.0) * SS; out.cy = (float)jf(L, "y", ch / 2.0) * SS;
        out.rot = (float)jf(L, "rot", 0); out.opacity = (float)jf(L, "opacity", 1.0);
        float ex = block->w / 2.f / SS, ey = block->h / 2.f / SS;
        li.x0 = out.cx / SS - ex; li.y0 = out.cy / SS - ey; li.x1 = out.cx / SS + ex; li.y1 = out.cy / SS + ey;
        return true;
    }

    if (type == "text") {
        std::string text = js(L, "text", "");
        json style = style_of(brand, L);
        if (!block) {
            std::string fontName = ss_(L, style, "font", "display");
            FontEntry* fe = get_font(brand.font_path(fontName));
            if (!fe || text.empty()) {
                if (err && !fe) *err += "font not found: " + fontName + " (brand: " + brand.dir + ")\n";
                return false;
            }
            float px = (float)sf(L, style, "px", 160) * SS;
            float tracking = (float)sf(L, style, "tracking", 0) * SS;
            float lineH = (float)sf(L, style, "line_height", 1.02);
            std::string align = ss_(L, style, "align", "center");
            float maxW = (float)jf(L, "max_w", 0) * SS;
            Mask tm;
            if (!raster_text(text, fe, px, tracking, lineH, align, tm)) return false;
            if (maxW > 0 && tm.w > maxW) {   // auto-fit: shrink px to fit max_w
                px *= maxW / tm.w;
                raster_text(text, fe, px, tracking * maxW / tm.w, lineH, align, tm);
            }
            FxSpec fx = parse_fx(L, style, brand, SS, "stroke_px", "stroke");
            json plate = sj(L, style, "plate");
            float platePadX = plate.is_null() ? 0 : (float)jf(plate, "pad_x", 28) * SS;
            float platePadY = plate.is_null() ? 0 : (float)jf(plate, "pad_y", 18) * SS;
            int mg = (int)ceilf(fx_margin(fx) + std::max(platePadX, platePadY));
            block = std::make_shared<Img>();
            block->alloc(tm.w + mg * 2, tm.h + mg * 2);
            // fill: solid or vertical gradient across the glyph bbox
            RGBA f0 = brand.color(ss_(L, style, "fill", "#ffffff"));
            std::string gt = ss_(L, style, "grad_to", "");
            RGBA f1 = gt.empty() ? f0 : brand.color(gt);
            for (int y = 0; y < tm.h; y++) {
                float t = gt.empty() ? 0.f : clampf((y) / (float)std::max(1, tm.h - 1), 0, 1);
                RGBA c{f0.r + (f1.r - f0.r) * t, f0.g + (f1.g - f0.g) * t, f0.b + (f1.b - f0.b) * t, f0.a};
                uint8_t* dp = block->at(mg, y + mg);
                const uint8_t* mp = &tm.px[(size_t)y * tm.w];
                for (int x = 0; x < tm.w; x++, dp += 4, mp++) {
                    if (!*mp) continue;
                    dp[0] = (uint8_t)(c.r * 255); dp[1] = (uint8_t)(c.g * 255); dp[2] = (uint8_t)(c.b * 255);
                    dp[3] = (uint8_t)(*mp * c.a);
                }
            }
            apply_fx(*block, fx);
            // plate: rounded rect behind everything
            if (!plate.is_null()) {
                float rad = (float)jf(plate, "radius", 18) * SS;
                RGBA pc = brand.color(js(plate, "fill", "#000000"), RGBA{0, 0, 0, 1});
                pc.a *= (float)jf(plate, "alpha", 0.8);
                Img plateImg; plateImg.alloc(block->w, block->h);
                float rx0 = mg - platePadX, ry0 = mg - platePadY, rx1 = mg + tm.w + platePadX, ry1 = mg + tm.h + platePadY;
                float rcx = (rx0 + rx1) / 2, rcy = (ry0 + ry1) / 2, rhw = (rx1 - rx0) / 2 - rad, rhh = (ry1 - ry0) / 2 - rad;
                for (int y = 0; y < block->h; y++) {
                    uint8_t* dp = plateImg.at(0, y);
                    for (int x = 0; x < block->w; x++, dp += 4) {
                        float qx = fabsf(x - rcx) - rhw, qy = fabsf(y - rcy) - rhh;
                        float d = sqrtf(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - rad;
                        float aa = clampf(0.5f - d, 0, 1) * pc.a;
                        if (aa <= 0) continue;
                        dp[0] = (uint8_t)(pc.r * 255); dp[1] = (uint8_t)(pc.g * 255); dp[2] = (uint8_t)(pc.b * 255);
                        dp[3] = (uint8_t)(aa * 255);
                    }
                }
                blit_over(plateImg, *block, 0, 0);
                *block = std::move(plateImg);
            }
            if (!transient) block_cache_store(key, block);
        }
        out.block = block;
        out.cx = (float)jf(L, "x", cw / 2.0) * SS; out.cy = (float)jf(L, "y", ch / 2.0) * SS;
        out.rot = (float)jf(L, "rot", 0); out.opacity = (float)jf(L, "opacity", 1.0);
        float ex = block->w / 2.f / SS, ey = block->h / 2.f / SS;
        li.x0 = out.cx / SS - ex; li.y0 = out.cy / SS - ey; li.x1 = out.cx / SS + ex; li.y1 = out.cy / SS + ey;
        return true;
    }

    if (type == "shape") {
        std::string shape = js(L, "shape", "arrow");
        float bx0, by0, bx1, by1;      // logical block bounds
        float width = (float)jf(L, "width", 24);
        if (shape == "arrow") {
            bx0 = std::min(jf(L, "x1", 100), jf(L, "x2", 300)) - width * 3;
            bx1 = std::max(jf(L, "x1", 100), jf(L, "x2", 300)) + width * 3;
            by0 = std::min(jf(L, "y1", 100), jf(L, "y2", 300)) - width * 3;
            by1 = std::max(jf(L, "y1", 100), jf(L, "y2", 300)) + width * 3;
        } else if (shape == "circle") {
            float r = (float)jf(L, "r", 120), th = (float)jf(L, "thick", 16);
            bx0 = jf(L, "x", 300) - r - th; bx1 = jf(L, "x", 300) + r + th;
            by0 = jf(L, "y", 300) - r - th; by1 = jf(L, "y", 300) + r + th;
        } else { // rect
            float w2 = (float)jf(L, "w", 300) / 2, h2 = (float)jf(L, "h", 180) / 2;
            bx0 = jf(L, "x", 300) - w2 - 4; bx1 = jf(L, "x", 300) + w2 + 4;
            by0 = jf(L, "y", 300) - h2 - 4; by1 = jf(L, "y", 300) + h2 + 4;
        }
        FxSpec fx = parse_fx(L, json::object(), brand, SS);
        int mg = (int)ceilf(fx_margin(fx));
        int bw = (int)ceilf((bx1 - bx0) * SS) + mg * 2, bh = (int)ceilf((by1 - by0) * SS) + mg * 2;
        if (bw <= 0 || bh <= 0 || bw > 8192 || bh > 8192) return false;
        if (!block) {
            block = std::make_shared<Img>();
            block->alloc(bw, bh);
            RGBA fill = brand.color(js(L, "fill", "$gold"), RGBA{0.95f, 0.7f, 0.2f, 1});
            std::string sgt = js(L, "grad_to", "");             // linear gradient across the shape bbox
            RGBA fill1 = sgt.empty() ? fill : brand.color(sgt);
            float sga = (float)jf(L, "grad_angle", 90) * 3.14159265f / 180.f;
            float sgx = cosf(sga), sgy = sinf(sga);
            auto toBlockX = [&](float lx) { return (lx - bx0) * SS + mg; };
            auto toBlockY = [&](float ly) { return (ly - by0) * SS + mg; };
            for (int y = 0; y < bh; y++) {
                uint8_t* dp = block->at(0, y);
                for (int x = 0; x < bw; x++, dp += 4) {
                    float d = 1e9f;
                    if (shape == "arrow") {
                        float ax = toBlockX((float)jf(L, "x1", 100)), ay = toBlockY((float)jf(L, "y1", 100));
                        float bx = toBlockX((float)jf(L, "x2", 300)), by = toBlockY((float)jf(L, "y2", 300));
                        float w = width * SS;
                        float dirx = bx - ax, diry = by - ay, len = sqrtf(dirx * dirx + diry * diry) + 1e-6f;
                        dirx /= len; diry /= len;
                        float headLen = w * 2.4f, headW = w * 2.2f;
                        float hx = bx - dirx * headLen, hy = by - diry * headLen; // head base
                        d = sd_segment((float)x, (float)y, ax, ay, hx, hy) - w / 2;
                        float tri[6] = {bx, by, hx - diry * headW, hy + dirx * headW, hx + diry * headW, hy - dirx * headW};
                        d = std::min(d, sd_triangle((float)x, (float)y, tri));
                    } else if (shape == "circle") {
                        float cx = toBlockX((float)jf(L, "x", 300)), cy = toBlockY((float)jf(L, "y", 300));
                        float r = (float)jf(L, "r", 120) * SS, th = (float)jf(L, "thick", 16) * SS;
                        float dl = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
                        d = th > 0 ? fabsf(dl - r) - th / 2 : dl - r;
                    } else {
                        float cx = toBlockX((float)jf(L, "x", 300)), cy = toBlockY((float)jf(L, "y", 300));
                        float w2 = (float)jf(L, "w", 300) / 2 * SS, h2 = (float)jf(L, "h", 180) / 2 * SS;
                        float rad = (float)jf(L, "radius", 12) * SS, th = (float)jf(L, "thick", 0) * SS;
                        float qx = fabsf(x - cx) - (w2 - rad), qy = fabsf(y - cy) - (h2 - rad);
                        float dd = sqrtf(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - rad;
                        d = th > 0 ? fabsf(dd) - th / 2 : dd;
                    }
                    float aa = clampf(0.5f - d, 0, 1) * fill.a;
                    if (aa <= 0) continue;
                    RGBA c = fill;
                    if (!sgt.empty()) {
                        float t = clampf(((x / (float)bw - 0.5f) * sgx + (y / (float)bh - 0.5f) * sgy) + 0.5f, 0, 1);
                        c.r += (fill1.r - c.r) * t; c.g += (fill1.g - c.g) * t; c.b += (fill1.b - c.b) * t;
                    }
                    dp[0] = (uint8_t)(c.r * 255); dp[1] = (uint8_t)(c.g * 255); dp[2] = (uint8_t)(c.b * 255);
                    dp[3] = (uint8_t)(aa * 255);
                }
            }
            apply_fx(*block, fx);
            if (!transient) block_cache_store(key, block);
        }
        out.block = block;
        out.cx = (bx0 + bx1) / 2 * SS; out.cy = (by0 + by1) / 2 * SS;
        out.rot = (float)jf(L, "rot", 0); out.opacity = (float)jf(L, "opacity", 1.0);
        li.x0 = bx0; li.y0 = by0; li.x1 = bx1; li.y1 = by1;
        return true;
    }

    if (type == "watermark") {
        const json& wm = brand.watermark;
        std::string src = js(wm, "src", "");
        if (!block) {
            Img* im = src.empty() ? nullptr : get_image(src);
            if (!im) return false;
            float hpx = (float)jf(wm, "px", 72) * SS;
            float wpx = hpx * im->w / im->h;
            block = std::make_shared<Img>();
            resize_img(*im, *block, std::max(1, (int)wpx), std::max(1, (int)hpx));
            if (!transient) block_cache_store(key, block);
        }
        float margin = (float)jf(wm, "margin", 24) * SS;
        std::string anchor = js(wm, "anchor", "tl");
        int ox = (anchor == "tr" || anchor == "br") ? (int)(cw * SS - margin - block->w) : (int)margin;
        int oy = (anchor == "bl" || anchor == "br") ? (int)(ch * SS - margin - block->h) : (int)margin;
        out.block = block;
        out.cx = ox + block->w / 2.f; out.cy = oy + block->h / 2.f;
        out.opacity = (float)jf(wm, "opacity", 0.9);
        li.x0 = ox / (float)ssFactor; li.y0 = oy / (float)ssFactor;
        li.x1 = (ox + block->w) / (float)ssFactor; li.y1 = (oy + block->h) / (float)ssFactor;
        return true;
    }

    return false;
}

// Build every layer of the doc (cache-hot for unchanged layers).
// `liveLayer`/`liveSS` (GUI live-gesture path): that one layer builds at the
// reduced supersample `liveSS`, uncached, and its BuiltLayer carries the
// compose-space upscale in `sscale` — a resize/outline drag re-rasters ONLY the
// touched layer, at 1/4 the pixels, with zero cache churn. Compose space (and
// every other layer) stays at ssFactor; pass -1 (the default) for the exact path.
static void build_layers(const json& doc, const Brand& brand, const std::string& docDir, int ssFactor,
                         int& cw, int& ch, std::vector<BuiltLayer>& out,
                         std::vector<LayerInfo>* info, std::string* err,
                         int liveLayer = -1, int liveSS = 0) {
    cw = 1280; ch = 720;
    if (doc.contains("canvas") && doc["canvas"].is_array() && doc["canvas"].size() == 2) {
        cw = doc["canvas"][0].get<int>(); ch = doc["canvas"][1].get<int>();
    }
    out.clear();
    if (info) info->clear();
    if (!doc.contains("layers") || !doc["layers"].is_array()) return;
    const json& ls = doc["layers"];
    for (size_t i = 0; i < ls.size(); i++) {
        const json& L = ls[i];
        if (!L.is_object()) continue;
        bool live = (int)i == liveLayer && liveSS > 0 && liveSS != ssFactor && js(L, "type", "") != "mosaic";
        BuiltLayer bl; LayerInfo li;
        build_layer(L, brand, docDir, cw, ch, live ? liveSS : ssFactor, bl, li, err, live);
        if (live && bl.block) {
            float k = ssFactor / (float)liveSS;
            bl.cx *= k; bl.cy *= k; bl.sscale = k; bl.transient = true;
        }
        out.push_back(std::move(bl));
        if (info) info->push_back(li);
    }
}

// CPU mosaic: average cell blocks of the current canvas inside the rect
static void mosaic_cpu(Img& canvas, const int mos[5]) {
    int rx0 = mos[0], ry0 = mos[1], rx1 = mos[2], ry1 = mos[3], cell = mos[4];
    for (int by = ry0; by < ry1; by += cell)
        for (int bx = rx0; bx < rx1; bx += cell) {
            int ex = std::min(rx1, bx + cell), ey = std::min(ry1, by + cell);
            long r = 0, g = 0, b = 0, n = 0;
            for (int y = by; y < ey; y++) {
                const uint8_t* p = canvas.at(bx, y);
                for (int x = bx; x < ex; x++, p += 4) { r += p[0]; g += p[1]; b += p[2]; n++; }
            }
            if (!n) continue;
            uint8_t cr = (uint8_t)(r / n), cg = (uint8_t)(g / n), cb = (uint8_t)(b / n);
            for (int y = by; y < ey; y++) {
                uint8_t* p = canvas.at(bx, y);
                for (int x = bx; x < ex; x++, p += 4) { p[0] = cr; p[1] = cg; p[2] = cb; }
            }
        }
}

// Renders `doc` at supersample factor `ss` and downsamples to logical size.
// This CPU path is the deterministic reference (headless export needs no GPU);
// the GUI composites the same cached blocks on the GPU instead.
// `info` (optional) receives logical-space bounding boxes per layer.
static bool render_doc(const json& doc, const Brand& brand, const std::string& docDir,
                       int ssFactor, Img& out, std::vector<LayerInfo>* info, std::string* err) {
    int cw, ch;
    std::vector<BuiltLayer> built;
    build_layers(doc, brand, docDir, ssFactor, cw, ch, built, info, err);

    Img canvas; canvas.alloc(cw * ssFactor, ch * ssFactor);
    // default deep-space bg if the doc has no bg layer
    for (size_t i = 0; i < canvas.px.size(); i += 4) { canvas.px[i] = 12; canvas.px[i + 1] = 8; canvas.px[i + 2] = 24; canvas.px[i + 3] = 255; }

    for (const BuiltLayer& bl : built) {
        if (bl.isMosaic) mosaic_cpu(canvas, bl.mos);
        else if (bl.block) blit_rotated(canvas, *bl.block, bl.cx, bl.cy, bl.rot, bl.opacity);
    }

    if (ssFactor > 1) resize_img(canvas, out, cw, ch);
    else out = std::move(canvas);
    return true;
}

// ─────────────────────────── export helpers ────────────────────────────────
static bool write_png(const Img& img, const std::string& path) {
    return stbi_write_png(host_path(path).c_str(), img.w, img.h, 4, img.px.data(), img.w * 4) != 0;
}
static void make_proof(const Img& full, Img& proof, int pw = 168, int ph = 94) {
    // portrait canvases get a portrait proof (Shorts grid size)
    if (full.h > full.w) { pw = 94; ph = 168; }
    resize_img(full, proof, pw, ph);
}
