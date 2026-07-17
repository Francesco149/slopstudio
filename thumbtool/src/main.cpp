// slopthumb — the slopstudio thumbnail tool (separate app from the editor).
//
// A brand-package-driven still compositor + GUI for authoring, iterating and
// A/B-managing video thumbnails. The branding (palette, fonts, text styles,
// sticker defaults, sprite library, watermark, templates) lives in a *package*
// directory the doc points at — nothing channel-specific is hardcoded here.
//
// Usage:
//   slopthumb.exe doc.thumb.json                      open in the GUI
//   slopthumb.exe doc.thumb.json --export out.png     headless render (no GPU)
//        [--ss N] [--proof out168.png] [--info out.json] [--brand dir]
//   slopthumb.exe doc.thumb.json --shot ui.png [--frames N]   GUI screenshot (verify)
//
// LLM workflow: edit the .thumb.json (directly or via tools/thumb.py) — a GUI
// instance watching that file hot-reloads it on save, so authored changes are
// immediately visible to the human. Undo snapshots persist to <doc>.undo.jsonl
// so the full edit history survives across sessions.

#include "engine.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <d3d11.h>
#include <deque>
#include <filesystem>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_stdlib.h"

namespace fs = std::filesystem;

// ───────────────────────────── app state ────────────────────────────────────
struct TexEntry { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

struct App {
    std::string docPath, docDir;
    json doc = json::object();
    std::string brandOverride;           // --brand
    Brand brand;

    Img preview;
    std::vector<LayerInfo> layerInfo;
    TexEntry previewTex;                          // CPU-fallback preview texture
    ID3D11ShaderResourceView* previewSrv = nullptr;  // what the panels draw (GPU RT or fallback)
    size_t renderedKey = 0;
    int sel = -1;                        // primary selection (what the inspector shows)
    std::vector<int> selMulti;           // full selection set (always contains sel when sel>=0)
    bool focusText = false;              // double-clicked a text layer → focus the inspector text box
    std::string perf;                             // "build 3ms · composite 1ms (GPU)"

    // undo (teidraw pattern: doc mutates live during a gesture; ONE snapshot pushes
    // when the gesture settles — so a drag is one undo step, not a hundred)
    std::deque<std::string> undo, redo;
    std::string undoBase;                // committed doc state
    std::string savedDump;               // last state loaded-from/saved-to disk
    bool undoDirty = false, activePrev = false;
    double nudgeUndoAt = 0;              // arrow-key nudge burst coalescing deadline (teidraw: 0.6s)

    // external-edit watch
    fs::file_time_type docMtime{};
    float watchTimer = 0;
    bool conflict = false;               // changed on disk while we hold local edits

    std::string status, renderErr;
    std::map<std::string, TexEntry> thumbCache;   // sprite/history preview textures
    bool showSquint = true;
    char spriteFilter[64] = "";

    // live-gesture tracking: while a drag is changing the selected layer's
    // BLOCK CONTENT (resize/outline/thickness — anything in its block key), the
    // preview rebuilds that layer transiently at SS1 instead of SS2 (see
    // ensure_preview). Position/rot/opacity drags never trip this (cache hits).
    float wheelGesture = 0;              // seconds left in a ctrl+wheel resize burst
    bool liveGesture = false;
    std::string prevSelKey;              // selected layer's block key last frame
    int prevSelIdx = -1;
};
static App g_app;

// ───────────────────── canvas gizmo state (teidraw port) ────────────────────
// One drag state machine for every canvas gesture. The doc mutates live from a
// SNAPSHOT taken at press + the absolute offset since press (never incremental
// deltas), so modifiers (shift axis-lock, ctrl snap) rewrite the drag without
// drift and the undo system records exactly one step per gesture.
enum DragMode { DM_NONE, DM_PENDING, DM_MOVE, DM_MARQUEE, DM_HANDLE, DM_ROTATE, DM_CROP, DM_EDGE, DM_ARROW_A, DM_ARROW_B };
struct Gizmo {
    DragMode mode = DM_NONE;
    int layer = -1, handle = -1;            // handle: corner 0..3 (tl,tr,br,bl) or edge 0..3 (l,r,t,b)
    ImVec2 pressL, pressScr;                // logical / screen coords at press
    json snap;                              // primary layer snapshot at press
    std::vector<std::pair<int, json>> snaps;  // all selected layers at press (move)
    ImVec2 fixedL, centerL;                 // scale: fixed opposite corner + obb center at press
    float startDist = 1;
    float startAngle = 0; ImVec2 pivotL;    // rotate
    float boundsMn[2] = {0, 0}, boundsMx[2] = {0, 0};   // moving selection AABB at press (snap)
    bool moved = false;
    float iw = 0, ih = 0;                   // crop: source image dims
    std::vector<int> marqueeBase;           // selection kept under a shift-marquee
    double lastClickT = -1e9; ImVec2 lastClickScr; int lastClickLayer = -1;
};
static Gizmo g_giz;

// OS file drop (WM_DROPFILES) → pending list consumed by the canvas panel
static std::vector<std::string> g_dropFiles; static POINT g_dropPt{}; static bool g_hasDrop = false;

// canvas placement of the last frame (drive scripts address logical canvas coords)
static ImVec2 g_canvasP0(0, 0); static float g_canvasSc = 1;

static inline float deg2rad(float d) { return d * 3.14159265f / 180.f; }
static inline ImVec2 rot_vec(ImVec2 v, float a) {
    float s = sinf(a), c = cosf(a);
    return ImVec2(v.x * c - v.y * s, v.x * s + v.y * c);
}
static inline ImVec2 rot_about(ImVec2 p, ImVec2 c, float a) {
    ImVec2 d(p.x - c.x, p.y - c.y), r = rot_vec(d, a);
    return ImVec2(c.x + r.x, c.y + r.y);
}
static inline float vlen(ImVec2 v) { return sqrtf(v.x * v.x + v.y * v.y); }

static bool sel_contains(int i) {
    for (int s : g_app.selMulti) if (s == i) return true;
    return false;
}
static void select_one(int i) { g_app.sel = i; g_app.selMulti.assign(1, i); }
static void sel_clear() { g_app.sel = -1; g_app.selMulti.clear(); }
static void sel_toggle(int i) {
    for (size_t k = 0; k < g_app.selMulti.size(); k++)
        if (g_app.selMulti[k] == i) {
            g_app.selMulti.erase(g_app.selMulti.begin() + k);
            g_app.sel = g_app.selMulti.empty() ? -1 : g_app.selMulti.back();
            return;
        }
    g_app.selMulti.push_back(i); g_app.sel = i;
}

// ───────────────────────────── d3d11 boilerplate ────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
static LRESULT WINAPI wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
    case WM_SIZE:
        if (g_dev && w != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            create_rtv();
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_DROPFILES: {                     // drag image files from the OS onto the canvas
        HDROP hd = (HDROP)w;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t wb[2048] = {0};
            if (!DragQueryFileW(hd, i, wb, 2048)) continue;
            char ub[4096] = {0};
            WideCharToMultiByte(CP_UTF8, 0, wb, -1, ub, sizeof ub, nullptr, nullptr);
            g_dropFiles.push_back(ub);
        }
        DragQueryPoint(hd, &g_dropPt);       // client coords == ImGui screen coords (single viewport)
        g_hasDrop = !g_dropFiles.empty();
        DragFinish(hd);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void upload_tex(TexEntry& t, const Img& img) {
    if (t.srv && (t.w != img.w || t.h != img.h)) { t.srv->Release(); t.srv = nullptr; }
    if (!t.srv) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = img.w; td.Height = img.h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { img.px.data(), (UINT)(img.w * 4), 0 };
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex)) || !tex) return;
        g_dev->CreateShaderResourceView(tex, nullptr, &t.srv);
        tex->Release();
        t.w = img.w; t.h = img.h;
    } else {
        ID3D11Resource* res = nullptr; t.srv->GetResource(&res);
        if (res) { g_ctx->UpdateSubresource(res, 0, nullptr, img.px.data(), img.w * 4, 0); res->Release(); }
    }
}

static TexEntry* thumb_tex(const std::string& path, int maxPx = 128) {
    std::string key = path + "@" + std::to_string(maxPx);   // same file at 96px (sprite) vs 512px (crop ghost) coexists
    auto it = g_app.thumbCache.find(key);
    if (it != g_app.thumbCache.end()) return it->second.srv ? &it->second : nullptr;
    TexEntry& t = g_app.thumbCache[key];
    Img im;
    if (!load_image(path, im)) return nullptr;
    float sc = std::min(1.f, maxPx / (float)std::max(im.w, im.h));
    Img small; resize_img(im, small, std::max(1, (int)(im.w * sc)), std::max(1, (int)(im.h * sc)));
    upload_tex(t, small);
    return t.srv ? &t : nullptr;
}

// ───────────────────────────── GPU compositor ───────────────────────────────
// Composites the engine's cached blocks on the GPU (premultiplied rotated quads
// + a quantize shader for mosaic layers) into an offscreen RT the preview shows
// directly. Dragging a layer = cache-hit blocks + a handful of quads → lag-free.
// PNG export stays on the CPU render_doc path (deterministic reference).
#include <d3dcompiler.h>

static const char* GPU_HLSL = R"(
struct VSIn  { float2 pos : POS; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vsmain(VSIn i) { VSOut o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; return o; }
Texture2D tex0 : register(t0);
SamplerState smp : register(s0);
cbuffer CB : register(b0) { float4 P; };
float4 pstex(VSOut i) : SV_Target { return tex0.Sample(smp, i.uv) * P.x; }        // premult * opacity
float4 psmos(VSOut i) : SV_Target {                                               // censor-mosaic quantize
    float2 uv = (floor((i.uv - P.xy) / P.zw) + 0.5) * P.zw + P.xy;
    float4 c = tex0.Sample(smp, uv);
    return float4(c.rgb, 1);
}
)";

struct GpuComp {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* psTex = nullptr, * psMos = nullptr;
    ID3D11InputLayout* il = nullptr;
    ID3D11Buffer* vb = nullptr, * cb = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11Texture2D* rt = nullptr;      ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* rtSrv = nullptr;
    ID3D11Texture2D* scratch = nullptr; ID3D11ShaderResourceView* scratchSrv = nullptr;
    int rtW = 0, rtH = 0;
    std::map<const Img*, TexEntry> texCache;   // block → premultiplied SRV
    int cacheGen = 0;
    bool ok = false;

    bool init() {
        ID3DBlob* vsb = nullptr, * psb = nullptr, * err = nullptr;
        if (FAILED(D3DCompile(GPU_HLSL, strlen(GPU_HLSL), nullptr, nullptr, nullptr, "vsmain", "vs_4_0", 0, 0, &vsb, &err))) return false;
        g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        D3D11_INPUT_ELEMENT_DESC ied[] = {
            {"POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        g_dev->CreateInputLayout(ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);
        vsb->Release();
        if (FAILED(D3DCompile(GPU_HLSL, strlen(GPU_HLSL), nullptr, nullptr, nullptr, "pstex", "ps_4_0", 0, 0, &psb, &err))) return false;
        g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &psTex); psb->Release();
        if (FAILED(D3DCompile(GPU_HLSL, strlen(GPU_HLSL), nullptr, nullptr, nullptr, "psmos", "ps_4_0", 0, 0, &psb, &err))) return false;
        g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &psMos); psb->Release();

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 6 * 4 * sizeof(float); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&bd, nullptr, &vb);
        bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&bd, nullptr, &cb);

        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;                  // premultiplied over
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bl, &blend);

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_dev->CreateSamplerState(&sd, &samp);
        ok = vs && psTex && psMos && il && vb && cb && blend && samp;
        return ok;
    }

    void ensure_rt(int W, int H) {
        if (rt && rtW == W && rtH == H) return;
        if (rtv) rtv->Release(); if (rtSrv) rtSrv->Release(); if (rt) rt->Release();
        if (scratchSrv) scratchSrv->Release(); if (scratch) scratch->Release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&td, nullptr, &rt);
        g_dev->CreateRenderTargetView(rt, nullptr, &rtv);
        g_dev->CreateShaderResourceView(rt, nullptr, &rtSrv);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&td, nullptr, &scratch);
        g_dev->CreateShaderResourceView(scratch, nullptr, &scratchSrv);
        rtW = W; rtH = H;
    }

    ID3D11ShaderResourceView* block_srv(const Img* img) {
        if (cacheGen != g_blockCacheGen) {          // engine purged its block cache → pointers are stale
            for (auto& [k, t] : texCache) if (t.srv) t.srv->Release();
            texCache.clear();
            cacheGen = g_blockCacheGen;
        }
        auto it = texCache.find(img);
        if (it != texCache.end()) return it->second.srv;
        TexEntry& t = texCache[img];
        std::vector<uint8_t> pre((size_t)img->w * img->h * 4);   // premultiply for correct filtering/blending
        for (size_t i = 0; i < pre.size(); i += 4) {
            int a = img->px[i + 3];
            pre[i] = (uint8_t)(img->px[i] * a / 255); pre[i + 1] = (uint8_t)(img->px[i + 1] * a / 255);
            pre[i + 2] = (uint8_t)(img->px[i + 2] * a / 255); pre[i + 3] = (uint8_t)a;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = img->w; td.Height = img->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { pre.data(), (UINT)(img->w * 4), 0 };
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex)) || !tex) return nullptr;
        g_dev->CreateShaderResourceView(tex, nullptr, &t.srv);
        tex->Release();
        t.w = img->w; t.h = img->h;
        return t.srv;
    }

    // transient (live-gesture) block: it is rebuilt fresh every frame and never
    // enters the engine cache, so pointer-keyed caching would be wrong (and a
    // freed Img* could collide with a later cached block). One dedicated
    // texture, premultiplied + re-uploaded per frame (only ONE layer is ever
    // live at a time — the selected one).
    TexEntry liveTex;
    Img livePre;
    ID3D11ShaderResourceView* live_srv(const Img* img) {
        livePre.w = img->w; livePre.h = img->h;
        livePre.px.resize((size_t)img->w * img->h * 4);
        for (size_t i = 0; i < livePre.px.size(); i += 4) {
            int a = img->px[i + 3];
            livePre.px[i] = (uint8_t)(img->px[i] * a / 255);
            livePre.px[i + 1] = (uint8_t)(img->px[i + 1] * a / 255);
            livePre.px[i + 2] = (uint8_t)(img->px[i + 2] * a / 255);
            livePre.px[i + 3] = (uint8_t)a;
        }
        upload_tex(liveTex, livePre);
        return liveTex.srv;
    }

    void draw_quad(float cx, float cy, float w, float h, float rotDeg, float u0, float v0, float u1, float v1) {
        float rad = rotDeg * 3.14159265f / 180.f, cs = cosf(rad), sn = sinf(rad);
        float hw = w / 2, hh = h / 2;
        float lx[4] = {-hw, hw, -hw, hw}, ly[4] = {-hh, -hh, hh, hh};
        float uu[4] = {u0, u1, u0, u1}, vv[4] = {v0, v0, v1, v1};
        float px[4], py[4];
        for (int i = 0; i < 4; i++) {
            float wx = cx + cs * lx[i] - sn * ly[i], wy = cy + sn * lx[i] + cs * ly[i];
            px[i] = wx * 2.f / rtW - 1.f; py[i] = 1.f - wy * 2.f / rtH;
        }
        int order[6] = {0, 1, 2, 2, 1, 3};
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(g_ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        float* v = (float*)map.pData;
        for (int i = 0; i < 6; i++) {
            int o = order[i];
            *v++ = px[o]; *v++ = py[o]; *v++ = uu[o]; *v++ = vv[o];
        }
        g_ctx->Unmap(vb, 0);
        g_ctx->Draw(6, 0);
    }

    void set_cb(float a, float b, float c, float d) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(g_ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        float* p = (float*)map.pData; p[0] = a; p[1] = b; p[2] = c; p[3] = d;
        g_ctx->Unmap(cb, 0);
    }

    void composite(const std::vector<BuiltLayer>& built, int W, int H) {
        ensure_rt(W, H);
        const float clear[4] = { 12 / 255.f, 8 / 255.f, 24 / 255.f, 1 };   // engine's default deep-space bg
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->ClearRenderTargetView(rtv, clear);
        D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->IASetInputLayout(il);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = 16, off = 0;
        g_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        g_ctx->VSSetShader(vs, nullptr, 0);
        g_ctx->PSSetSamplers(0, 1, &samp);
        g_ctx->PSSetConstantBuffers(0, 1, &cb);
        float bf[4] = {0, 0, 0, 0};
        g_ctx->OMSetBlendState(blend, bf, 0xffffffff);

        for (const BuiltLayer& bl : built) {
            if (bl.isMosaic) {
                // snapshot the RT, then redraw the region quantized
                ID3D11ShaderResourceView* nul = nullptr;
                g_ctx->PSSetShaderResources(0, 1, &nul);
                g_ctx->CopyResource(scratch, rt);
                g_ctx->PSSetShader(psMos, nullptr, 0);
                g_ctx->PSSetShaderResources(0, 1, &scratchSrv);
                float x0 = (float)bl.mos[0], y0 = (float)bl.mos[1], x1 = (float)bl.mos[2], y1 = (float)bl.mos[3];
                float cell = (float)bl.mos[4];
                set_cb(x0 / W, y0 / H, cell / W, cell / H);
                draw_quad((x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0, 0, x0 / W, y0 / H, x1 / W, y1 / H);
                continue;
            }
            if (!bl.block) continue;
            ID3D11ShaderResourceView* srv = bl.transient ? live_srv(bl.block.get()) : block_srv(bl.block.get());
            if (!srv) continue;
            g_ctx->PSSetShader(psTex, nullptr, 0);
            g_ctx->PSSetShaderResources(0, 1, &srv);
            set_cb(bl.opacity, 0, 0, 0);
            draw_quad(bl.cx, bl.cy, bl.block->w * bl.sscale, bl.block->h * bl.sscale, bl.rot, 0, 0, 1, 1);
        }
        // unbind so ImGui can sample the RT and the next pass can copy it
        ID3D11ShaderResourceView* nul = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &nul);
        ID3D11RenderTargetView* nulrtv = nullptr;
        g_ctx->OMSetRenderTargets(1, &nulrtv, nullptr);
    }
};
static GpuComp g_gpu;

// ───────────────────────────── doc I/O + undo ───────────────────────────────
static std::string undo_log_path() { return g_app.docPath + ".undo.jsonl"; }

static void load_undo_log() {
    g_app.undo.clear(); g_app.redo.clear();
    std::ifstream f(host_path(undo_log_path()));
    std::string line;
    while (std::getline(f, line))
        if (!line.empty() && line != g_app.undoBase) g_app.undo.push_back(line);
    while (g_app.undo.size() > 300) g_app.undo.pop_front();
    // drop a trailing entry identical to current state
    if (!g_app.undo.empty() && g_app.undo.back() == g_app.undoBase) g_app.undo.pop_back();
}
static void append_undo_log(const std::string& snap) {
    std::ofstream f(host_path(undo_log_path()), std::ios::app);
    f << snap << "\n";
}

// auto-discover a gemma brand package by walking up from the doc's dir, so the interactive
// UI (and export) load branding even when the doc doesn't name one. Returns a path relative
// to startDir, or "".
static std::string find_brand_package(const std::string& startDir) {
    std::string up = ".";
    for (int i = 0; i < 8; i++) {
        for (const char* sub : {"/gemma-branding/brand-package", "/brand-package"}) {
            std::string rel = up + sub;
            std::error_code ec;
            if (fs::exists(host_path(path_join(startDir, rel + "/brand.json")), ec)) return rel;
        }
        up += "/..";
    }
    return "";
}

static void resolve_brand() {
    std::string b = !g_app.brandOverride.empty() ? g_app.brandOverride : js(g_app.doc, "brand", "");
    if (b.empty()) b = find_brand_package(g_app.docDir);   // auto-load the gemma brand if present
    g_app.brand = b.empty() ? Brand() : load_brand(path_join(g_app.docDir, b));
}

static bool load_doc(const std::string& path) {
    std::string txt = read_text_file(host_path(path));
    if (txt.empty()) return false;
    json j = json::parse(txt, nullptr, false);
    if (j.is_discarded()) return false;
    g_app.doc = std::move(j);
    g_app.docPath = path;
    g_app.docDir = path_dir(path);
    g_app.undoBase = g_app.doc.dump();
    g_app.savedDump = g_app.undoBase;
    sel_clear();
    resolve_brand();
    load_undo_log();
    std::error_code ec;
    g_app.docMtime = fs::last_write_time(host_path(path), ec);
    g_app.conflict = false;
    g_app.status = "loaded " + path;
    return true;
}

static bool save_doc() {
    if (g_app.docPath.empty()) return false;
    std::ofstream f(host_path(g_app.docPath), std::ios::binary);
    if (!f) return false;
    f << g_app.doc.dump(2) << "\n";
    f.close();
    g_app.savedDump = g_app.doc.dump();
    std::error_code ec;
    g_app.docMtime = fs::last_write_time(host_path(g_app.docPath), ec);
    g_app.status = "saved " + g_app.docPath;
    return true;
}

// Gesture-settle undo (the teidraw rule): the doc mutates live while ANY gesture
// is in flight — an active widget, a canvas gizmo drag, a ctrl+wheel burst, an
// arrow-key nudge burst — and exactly ONE snapshot pushes when everything
// settles. The identical-snapshot guard drops no-op gestures for free.
static void undo_settle_check() {
    double now = ImGui::GetTime();
    if (g_app.nudgeUndoAt > 0 && now >= g_app.nudgeUndoAt) g_app.nudgeUndoAt = 0;
    bool gesture = ImGui::IsAnyItemActive() || g_giz.mode != DM_NONE
                || g_app.wheelGesture > 0 || g_app.nudgeUndoAt > 0;
    if (!gesture && (g_app.activePrev || g_app.undoDirty)) {
        std::string cur = g_app.doc.dump();
        if (cur != g_app.undoBase) {
            g_app.undo.push_back(g_app.undoBase);
            if (g_app.undo.size() > 300) g_app.undo.pop_front();
            g_app.redo.clear();
            g_app.undoBase = cur;
            append_undo_log(cur);
        }
        g_app.undoDirty = false;
    }
    g_app.activePrev = gesture;
}
static void do_undo() {
    if (g_app.undo.empty()) return;
    g_app.redo.push_back(g_app.doc.dump());
    g_app.doc = json::parse(g_app.undo.back());
    g_app.undo.pop_back();
    g_app.undoBase = g_app.doc.dump();
    resolve_brand();
}
static void do_redo() {
    if (g_app.redo.empty()) return;
    g_app.undo.push_back(g_app.doc.dump());
    g_app.doc = json::parse(g_app.redo.back());
    g_app.redo.pop_back();
    g_app.undoBase = g_app.doc.dump();
    resolve_brand();
}

// external edit watch (the LLM-authoring loop: agent saves file → we hot-reload)
static void watch_tick(float dt) {
    if (g_app.docPath.empty()) return;
    g_app.watchTimer += dt;
    if (g_app.watchTimer < 0.5f) return;
    g_app.watchTimer = 0;
    std::error_code ec;
    auto mt = fs::last_write_time(host_path(g_app.docPath), ec);
    if (ec || mt == g_app.docMtime) return;
    g_app.docMtime = mt;
    if (g_app.doc.dump() == g_app.savedDump) {  // no local edits → take it, undoably
        std::string prev = g_app.doc.dump();
        std::deque<std::string> keepUndo = g_app.undo;
        if (load_doc(g_app.docPath) && g_app.doc.dump() != prev) {
            g_app.undo = keepUndo;               // keep session history across the reload
            g_app.undo.push_back(prev);          // external edit is a normal undo step
            if (g_app.undo.size() > 300) g_app.undo.pop_front();
            append_undo_log(g_app.undoBase);
        }
        g_app.status = "hot-reloaded (external edit)";
    } else g_app.conflict = true;
}

// ───────────────────────────── render preview ───────────────────────────────
// GPU path: build cached blocks (only touched layers re-rasterize) + composite
// on the GPU. Dragging = pure cache hits + a few quads → no lag. CPU fallback
// keeps the old full render if D3D shader init failed.
//
// Live gesture: params like scale/outline_px/stroke_px/thick/width live INSIDE
// the block key, so dragging them re-rasterizes the layer every frame (stbir
// resize + chamfer distance transform + shadow blur at SS2) — the reported
// chug. While such a drag is active, the touched layer rebuilds at SS1 (1/4 the
// pixels), uncached (no 64-cap purge churn), upscaled 2x by the compositor; the
// release frame re-renders it once at SS2 through the normal cache, so the
// settled preview — and every export, which is CPU render_doc — is identical to
// a cold render.
static json& layers();
static int canvas_w();
static int canvas_h();

static void ensure_preview() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_app.wheelGesture > 0) g_app.wheelGesture -= io.DeltaTime;
    bool gesture = (ImGui::IsAnyItemActive() && ImGui::IsMouseDown(0)) || g_app.wheelGesture > 0;

    // live iff the SELECTED layer's block key (content params only — x/y/rot/
    // opacity excluded by design) changed since last frame, mid-gesture. Move
    // drags therefore stay on the exact SS2 cache-hit path.
    std::string selKey;
    if (g_app.sel >= 0 && g_app.sel < (int)layers().size()) {
        const json& L = layers()[g_app.sel];
        if (L.is_object() && js(L, "type", "") != "mosaic")
            selKey = block_key(L, g_app.brand, g_app.docDir, canvas_w(), canvas_h(), 2);
    }
    if (!gesture) g_app.liveGesture = false;
    else if (g_app.sel == g_app.prevSelIdx && !selKey.empty() && !g_app.prevSelKey.empty() && selKey != g_app.prevSelKey)
        g_app.liveGesture = true;
    g_app.prevSelKey = std::move(selKey);
    g_app.prevSelIdx = g_app.sel;
    bool live = g_app.liveGesture && g_gpu.ok;

    size_t key = std::hash<std::string>{}(g_app.doc.dump() + g_app.brand.dir) * 4 + (live ? 2 : 0) + (g_gpu.ok ? 1 : 0);
    if (key == g_app.renderedKey && g_app.previewSrv) return;
    g_app.renderErr.clear();
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    if (g_gpu.ok) {
        int cw, ch;
        std::vector<BuiltLayer> built;
        build_layers(g_app.doc, g_app.brand, g_app.docDir, 2, cw, ch, built, &g_app.layerInfo, &g_app.renderErr,
                     live ? g_app.sel : -1, 1);
        QueryPerformanceCounter(&t1);
        g_gpu.composite(built, cw * 2, ch * 2);
        g_app.previewSrv = g_gpu.rtSrv;
        QueryPerformanceCounter(&t2);
        char buf[96];
        snprintf(buf, sizeof buf, "build %.0fms + gpu %.1fms%s",
                 (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
                 (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart,
                 live ? " · live" : "");
        g_app.perf = buf;
    } else {
        render_doc(g_app.doc, g_app.brand, g_app.docDir, 2, g_app.preview, &g_app.layerInfo, &g_app.renderErr);
        upload_tex(g_app.previewTex, g_app.preview);
        g_app.previewSrv = g_app.previewTex.srv;
        QueryPerformanceCounter(&t1);
        char buf[96];
        snprintf(buf, sizeof buf, "cpu render %.0fms", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        g_app.perf = buf;
    }
    g_app.renderedKey = key;
}

// ───────────────────────────── layer helpers ────────────────────────────────
static json& layers() {
    if (!g_app.doc.contains("layers") || !g_app.doc["layers"].is_array()) g_app.doc["layers"] = json::array();
    return g_app.doc["layers"];
}
static std::string unique_id(const char* base) {
    auto& ls = layers();
    for (int i = 1; i < 100; i++) {
        std::string id = std::string(base) + std::to_string(i);
        bool used = false;
        for (auto& L : ls) if (js(L, "id", "") == id) used = true;
        if (!used) return id;
    }
    return base;
}
static void add_layer(json l, bool select = true) {
    layers().push_back(std::move(l));
    if (select) select_one((int)layers().size() - 1);
    g_app.undoDirty = true;
}
static int canvas_w() { auto& d = g_app.doc; return d.contains("canvas") && d["canvas"].is_array() ? d["canvas"][0].get<int>() : 1280; }
static int canvas_h() { auto& d = g_app.doc; return d.contains("canvas") && d["canvas"].is_array() ? d["canvas"][1].get<int>() : 720; }

// Image treatment styles — the brand package's reusable looks plus two built-ins:
//   "sticker" = clear overrides so the brand.sticker defaults apply (border + shadow)
//   "plain"   = raw image, no border/shadow/glow/tilt
//   anything else = brand.image_styles[name] stamped as LITERAL values (visible +
//   tweakable in the inspector afterwards). Falls back to the classic card look.
static void apply_image_style(json& L, const std::string& style) {
    for (const char* k : {"rot", "outline_px", "outline", "glow", "shadow"}) L.erase(k);
    if (style == "plain") {
        L["outline_px"] = 0;
        L["shadow"] = json{{"off", true}};
    } else if (style != "sticker") {
        const json& styles = g_app.brand.image_styles;
        json c = (styles.contains(style) && styles[style].is_object()) ? styles[style] : json::object();
        if (c.empty()) c = json{{"rot", -2.0}, {"outline_px", 10.0},
                                {"glow", json{{"px", 30}, {"color", "$gold"}, {"alpha", 0.4}}}};
        for (auto& [k, v] : c.items()) if (k != "_comment") L[k] = v;
    }
    g_app.undoDirty = true;
}
static void brand_image(json& L, const char* style = "card") { apply_image_style(L, style); }

// name the treatment a layer currently wears (for the inspector's quick-style combo)
static std::string detect_image_style(const json& L) {
    for (auto& [name, st] : g_app.brand.image_styles.items()) {
        if (!st.is_object() || name == "_comment") continue;
        bool match = true;
        for (auto& [k, v] : st.items()) { if (k == "_comment") continue; if (!L.contains(k) || L[k] != v) { match = false; break; } }
        if (match) return name;
    }
    bool anyOverride = false;
    for (const char* k : {"rot", "outline_px", "outline", "glow", "shadow"})
        if (L.contains(k)) anyOverride = true;
    if (!anyOverride) return "sticker";
    if (jf(L, "outline_px", -1) == 0 && L.contains("shadow") && jb(L["shadow"], "off", false)) return "plain";
    return "custom";
}

// ─────────────────────── inspector color/number widgets ─────────────────────
// A typed color commits on Enter OR on defocus (IsItemDeactivatedAfterEdit — ImGui keeps the
// buffer live-updated, so the typed text survives to the deactivation frame). Enter-only silently
// DISCARDED a hex typed then clicked away, which read as "only palette swatches work" (owner
// 2026-07-08). Bare hex ("ff0000") gets its missing '#' prefixed.
static std::string normalize_color_input(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty() || s[0] == '#' || s[0] == '$') return s;
    bool allhex = (s.size() == 3 || s.size() == 6 || s.size() == 8);
    for (char ch : s) if (!isxdigit((unsigned char)ch)) allhex = false;
    return allhex ? "#" + s : s;
}
static bool color_field(const char* label, json& obj, const char* key, const std::string& def) {
    std::string cur = js(obj, key, def);
    bool changed = false;
    RGBA c = g_app.brand.color(cur);
    ImGui::PushID(label);
    ImGui::ColorButton("##cur", ImVec4(c.r, c.g, c.b, c.a), ImGuiColorEditFlags_NoTooltip, ImVec2(22, 22));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    std::string edit = cur;
    bool entered = ImGui::InputText(label, &edit, ImGuiInputTextFlags_EnterReturnsTrue);
    if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
        std::string norm = normalize_color_input(edit);
        if (norm != cur) { obj[key] = norm; changed = true; }
    }
    // palette swatch row
    int n = 0;
    for (auto& [name, hex] : g_app.brand.palette) {
        if (n++) ImGui::SameLine(0, 3);
        else { ImGui::Dummy(ImVec2(26, 0)); ImGui::SameLine(0, 3); }
        RGBA pc = parse_hex_color(hex);
        ImGui::PushID(name.c_str());
        if (ImGui::ColorButton("##sw", ImVec4(pc.r, pc.g, pc.b, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16))) {
            obj[key] = "$" + name; changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("$%s", name.c_str());
        ImGui::PopID();
        if (n >= 8) { n = 1; }  // wrap naturally via SameLine budget
    }
    ImGui::PopID();
    if (changed) g_app.undoDirty = true;
    return changed;
}
static bool num_field(const char* label, json& obj, const char* key, double def, float speed = 1.f, double mn = -1e6, double mx = 1e6, const char* fmt = "%.1f") {
    float v = (float)jf(obj, key, def);
    ImGui::SetNextItemWidth(140);
    if (ImGui::DragFloat(label, &v, speed, (float)mn, (float)mx, fmt)) { obj[key] = v; return true; }
    return false;
}
static bool fx_section(json& L) {   // shadow + glow editors (shared by image/text/shape)
    bool ch = false;
    if (ImGui::TreeNode("shadow")) {
        json sh = L.contains("shadow") && L["shadow"].is_object() ? L["shadow"] : json::object();
        ch |= num_field("dx", sh, "dx", 8, 0.5f); ch |= num_field("dy", sh, "dy", 10, 0.5f);
        ch |= num_field("blur", sh, "blur", 12, 0.5f, 0, 200); ch |= num_field("alpha", sh, "alpha", 0.5, 0.01f, 0, 1);
        bool off = jb(sh, "off", false);
        if (ImGui::Checkbox("off", &off)) { sh["off"] = off; ch = true; }
        if (ch) L["shadow"] = sh;
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("glow")) {
        json gl = L.contains("glow") && L["glow"].is_object() ? L["glow"] : json::object();
        bool c2 = false;
        c2 |= num_field("px", gl, "px", 0, 0.5f, 0, 300);
        c2 |= color_field("color", gl, "color", "#ffffff");
        c2 |= num_field("alpha", gl, "alpha", 0.6, 0.01f, 0, 1);
        if (c2) { L["glow"] = gl; ch = true; }
        ImGui::TreePop();
    }
    return ch;
}

// ───────────────────────────── snapshots / history ──────────────────────────
static std::string doc_stem() {
    fs::path p(g_app.docPath);
    std::string s = p.filename().string();
    size_t dot = s.find(".thumb.json");
    return dot == std::string::npos ? s : s.substr(0, dot);
}
static void take_snapshot() {
    if (g_app.docPath.empty()) return;
    std::string hdir = path_join(g_app.docDir, "history");
    std::error_code ec; fs::create_directories(host_path(hdir), ec);
    int n = 1;
    while (n < 1000) {
        char buf[64]; snprintf(buf, sizeof buf, "%s-%03d", doc_stem().c_str(), n);
        std::string base = path_join(hdir, buf);
        if (!fs::exists(host_path(base + ".thumb.json"), ec)) {
            std::ofstream f(host_path(base + ".thumb.json"), std::ios::binary);
            f << g_app.doc.dump(2) << "\n";
            Img full; std::string e;
            render_doc(g_app.doc, g_app.brand, g_app.docDir, 2, full, nullptr, &e);
            write_png(full, base + ".png");
            g_app.status = std::string("snapshot ") + buf;
            return;
        }
        n++;
    }
}

// ───────────────────────────── panels ───────────────────────────────────────
static void panel_layers() {
    auto& ls = layers();
    ImGui::TextDisabled("LAYERS");
    int move = -1, moveDir = 0, del = -1, dup = -1;
    for (int i = (int)ls.size() - 1; i >= 0; i--) {   // topmost first
        json& L = ls[i];
        ImGui::PushID(i);
        bool hidden = jb(L, "hidden", false);
        if (ImGui::SmallButton(hidden ? " " : "o")) { L["hidden"] = !hidden; g_app.undoDirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("visibility");
        ImGui::SameLine();
        std::string label = js(L, "id", "?") + "  [" + js(L, "type", "?") + "]";
        if (js(L, "type", "") == "text") {
            std::string t = js(L, "text", "");
            std::replace(t.begin(), t.end(), '\n', ' ');
            label += "  \"" + t.substr(0, 14) + (t.size() > 14 ? ".." : "") + "\"";
        }
        if (ImGui::Selectable(label.c_str(), sel_contains(i))) {
            if (ImGui::GetIO().KeyShift) sel_toggle(i); else select_one(i);
        }
        if (ImGui::BeginPopupContextItem("lctx")) {
            if (ImGui::MenuItem("move up"))   { move = i; moveDir = 1; }
            if (ImGui::MenuItem("move down")) { move = i; moveDir = -1; }
            if (ImGui::MenuItem("duplicate")) dup = i;
            if (ImGui::MenuItem("delete"))    del = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (move >= 0) {
        int j = move + moveDir;
        if (j >= 0 && j < (int)ls.size()) { std::swap(ls[move], ls[j]); select_one(j); g_app.undoDirty = true; }
    }
    if (dup >= 0) {
        json c = ls[dup]; c["id"] = unique_id((js(c, "id", "layer") + "_").c_str());
        ls.insert(ls.begin() + dup + 1, c); select_one(dup + 1); g_app.undoDirty = true;
    }
    if (del >= 0) { ls.erase(ls.begin() + del); sel_clear(); g_app.undoDirty = true; }

    ImGui::Separator();
    ImGui::TextDisabled("ADD");
    if (ImGui::SmallButton("text")) add_layer({{"id", unique_id("txt")}, {"type", "text"}, {"text", "NEW TEXT"}, {"style", "headline"}, {"x", canvas_w() / 2}, {"y", canvas_h() / 2}});
    ImGui::SameLine();
    if (ImGui::SmallButton("image..")) {
        char file[1024] = "";
        OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = "Images\0*.png;*.jpg;*.jpeg;*.webp\0\0";
        ofn.lpstrFile = file; ofn.nMaxFile = sizeof file; ofn.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&ofn))
            add_layer({{"id", unique_id("img")}, {"type", "image"}, {"src", std::string(file)}, {"x", canvas_w() / 2}, {"y", canvas_h() / 2}, {"scale", 0.8}});
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("card..")) {   // add an image ALREADY branded (tilt + border + glow) — one action
        char file[1024] = "";
        OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = "Images\0*.png;*.jpg;*.jpeg;*.webp\0\0";
        ofn.lpstrFile = file; ofn.nMaxFile = sizeof file; ofn.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
            json L = {{"id", unique_id("img")}, {"type", "image"}, {"src", std::string(file)}, {"x", canvas_w() / 2}, {"y", canvas_h() / 2}, {"scale", 0.7}};
            brand_image(L);
            add_layer(L);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("add an image already set up the branded way: a slight tilt, the white border, and a gold accent glow.");
    ImGui::SameLine();
    if (ImGui::SmallButton("arrow")) add_layer({{"id", unique_id("arrow")}, {"type", "shape"}, {"shape", "arrow"}, {"x1", canvas_w() / 4}, {"y1", canvas_h() * 3 / 4}, {"x2", canvas_w() / 2}, {"y2", canvas_h() / 2}, {"width", 24}, {"fill", "$gold"}, {"outline_px", 6}});
    if (ImGui::SmallButton("circle")) add_layer({{"id", unique_id("circ")}, {"type", "shape"}, {"shape", "circle"}, {"x", canvas_w() / 2}, {"y", canvas_h() / 2}, {"r", 130}, {"thick", 14}, {"fill", "$gold"}, {"outline_px", 5}});
    ImGui::SameLine();
    if (ImGui::SmallButton("rect")) add_layer({{"id", unique_id("rect")}, {"type", "shape"}, {"shape", "rect"}, {"x", canvas_w() / 2}, {"y", canvas_h() / 2}, {"w", 360}, {"h", 200}, {"radius", 14}, {"thick", 10}, {"fill", "$gold"}, {"outline_px", 4}});
    ImGui::SameLine();
    if (ImGui::SmallButton("bg")) { layers().insert(layers().begin(), json{{"id", "bg"}, {"type", "bg"}, {"fill", "$bg"}, {"vignette", 0.3}}); g_app.sel = 0; g_app.undoDirty = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("watermark")) add_layer({{"id", "wm"}, {"type", "watermark"}});
    ImGui::SameLine();
    if (ImGui::SmallButton("mosaic")) add_layer({{"id", unique_id("mosaic")}, {"type", "mosaic"}, {"x", canvas_w() / 2}, {"y", canvas_h() / 2}, {"w", 260}, {"h", 160}, {"cell", 22}});

    // brand templates
    if (g_app.brand.templates.is_array() && !g_app.brand.templates.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("TEMPLATES (%s)", g_app.brand.name.c_str());
        for (auto& t : g_app.brand.templates) {
            std::string name = js(t, "name", "?");
            if (ImGui::SmallButton(name.c_str()) && t.contains("layers")) {
                g_app.doc["layers"] = t["layers"];
                g_app.sel = -1;
                g_app.undoDirty = true;
            }
            if (ImGui::IsItemHovered() && t.contains("hint")) ImGui::SetTooltip("%s", js(t, "hint", "").c_str());
        }
    }
}

static void panel_history() {
    ImGui::TextDisabled("HISTORY / VARIANTS");
    if (ImGui::SmallButton("snapshot now")) take_snapshot();
    std::error_code ec;
    // sibling docs (A/B variants live side by side in the same dir)
    for (auto& e : fs::directory_iterator(host_path(g_app.docDir), ec)) {
        std::string name = e.path().filename().string();
        if (name.size() < 11 || name.substr(name.size() - 11) != ".thumb.json") continue;
        bool cur = (name == fs::path(g_app.docPath).filename().string());
        if (ImGui::Selectable((std::string(cur ? "> " : "  ") + name).c_str(), cur) && !cur)
            load_doc(path_join(g_app.docDir, name));
    }
    // snapshots
    std::string hdir = path_join(g_app.docDir, "history");
    std::vector<std::string> snaps;
    for (auto& e : fs::directory_iterator(host_path(hdir), ec)) {
        std::string name = e.path().filename().string();
        if (name.size() > 11 && name.substr(name.size() - 11) == ".thumb.json") snaps.push_back(name);
    }
    std::sort(snaps.rbegin(), snaps.rend());
    for (auto& name : snaps) {
        std::string base = name.substr(0, name.size() - 11);
        std::string png = path_join(hdir, base + ".png");
        ImGui::PushID(name.c_str());
        TexEntry* t = thumb_tex(png, 160);
        if (t) {
            float w = 150, h = w * t->h / std::max(1, t->w);
            if (ImGui::ImageButton("##snap", (ImTextureID)(intptr_t)t->srv, ImVec2(w, h))) {
                std::string txt = read_text_file(host_path(path_join(hdir, name)));
                json j = json::parse(txt, nullptr, false);
                if (!j.is_discarded()) { g_app.doc = std::move(j); g_app.sel = -1; g_app.undoDirty = true; resolve_brand(); }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("restore %s (undoable)", base.c_str());
        } else if (ImGui::SmallButton(base.c_str())) {
            std::string txt = read_text_file(host_path(path_join(hdir, name)));
            json j = json::parse(txt, nullptr, false);
            if (!j.is_discarded()) { g_app.doc = std::move(j); g_app.sel = -1; g_app.undoDirty = true; resolve_brand(); }
        }
        ImGui::PopID();
    }
}

static void panel_inspector() {
    auto& ls = layers();
    if (g_app.sel < 0 || g_app.sel >= (int)ls.size()) { ImGui::TextDisabled("no layer selected"); return; }
    json& L = ls[g_app.sel];
    std::string type = js(L, "type", "");
    bool ch = false;

    std::string id = js(L, "id", "");
    ImGui::SetNextItemWidth(140);
    if (ImGui::InputText("id", &id, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        if (id != js(L, "id", "")) { L["id"] = id; ch = true; }   // Enter OR defocus commits (same trap as color_field)
    }
    ImGui::SameLine(); ImGui::TextDisabled("[%s]", type.c_str());

    if (type == "bg") {
        ch |= color_field("fill", L, "fill", "#101018");
        ch |= color_field("grad_to", L, "grad_to", "");
        ch |= num_field("grad_angle", L, "grad_angle", 90, 1, 0, 360);
        std::string img = js(L, "image", "");
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("image", &img, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
            if (img != js(L, "image", "")) { L["image"] = img; ch = true; }
        }
        ch |= num_field("blur", L, "blur", 0, 0.5f, 0, 100);
        ch |= num_field("darken", L, "darken", 0, 0.01f, 0, 1);
        ch |= num_field("opacity", L, "opacity", 1, 0.01f, 0, 1);
        ch |= num_field("vignette", L, "vignette", 0, 0.01f, 0, 1);
    } else if (type == "image") {
        std::string src = js(L, "src", "");
        ImGui::SetNextItemWidth(230);
        if (ImGui::InputText("src", &src, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
            if (src != js(L, "src", "")) { L["src"] = src; ch = true; }
        }
        // quick treatment swap — the branded card default is one combo away from
        // plain / sticker / any other brand image style
        std::string curStyle = detect_image_style(L);
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("style", curStyle.c_str())) {
            auto opt = [&](const std::string& name, const char* hint) {
                if (ImGui::Selectable(name.c_str(), curStyle == name)) { apply_image_style(L, name); ch = true; }
                if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
            };
            opt("sticker", "brand default: white border + drop shadow, no tilt/glow");
            opt("plain", "raw image - no border, shadow, tilt or glow");
            for (auto& [k, v] : g_app.brand.image_styles.items())
                if (k != "_comment" && v.is_object()) opt(k, nullptr);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("image treatment (from the '%s' brand package). Fine-tune rot / outline / glow below.", g_app.brand.name.c_str());
        ch |= num_field("x", L, "x", canvas_w() / 2.0); ImGui::SameLine(); ch |= num_field("y", L, "y", canvas_h() / 2.0);
        ch |= num_field("scale", L, "scale", 1.0, 0.005f, 0.02, 6);
        ch |= num_field("rot", L, "rot", 0, 0.2f, -180, 180);
        bool flip = jb(L, "flip", false);
        if (ImGui::Checkbox("flip", &flip)) { L["flip"] = flip; ch = true; }
        if (L.contains("crop")) {
            ImGui::SameLine();
            if (ImGui::SmallButton("reset crop")) { L.erase("crop"); ch = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("cropped - ctrl+drag a corner handle on the canvas to re-crop");
        } else {
            ImGui::SameLine(); ImGui::TextDisabled("(ctrl+corner = crop)");
        }
        ch |= num_field("opacity", L, "opacity", 1, 0.01f, 0, 1);
        ch |= num_field("outline_px", L, "outline_px", jf(g_app.brand.sticker, "outline_px", 0), 0.2f, 0, 60);
        ch |= color_field("outline", L, "outline", js(g_app.brand.sticker, "outline", "#ffffff"));
        ch |= fx_section(L);
    } else if (type == "text") {
        std::string text = js(L, "text", "");
        if (g_app.focusText) { ImGui::SetKeyboardFocusHere(); g_app.focusText = false; }
        if (ImGui::InputTextMultiline("text", &text, ImVec2(-1, 60))) { L["text"] = text; ch = true; }
        // style picker
        std::string style = js(L, "style", "headline");
        if (ImGui::BeginCombo("style", style.c_str())) {
            for (auto& [k, v] : g_app.brand.styles.items())
                if (ImGui::Selectable(k.c_str(), k == style)) { L["style"] = k; ch = true; }
            ImGui::EndCombo();
        }
        ch |= num_field("x", L, "x", canvas_w() / 2.0); ImGui::SameLine(); ch |= num_field("y", L, "y", canvas_h() / 2.0);
        ch |= num_field("px", L, "px", 0, 1, 0, 800);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = style default. Text wider than max_w auto-shrinks to fit it — raise max_w (or cut words) for bigger text.");
        ch |= num_field("rot", L, "rot", 0, 0.2f, -180, 180);
        ch |= num_field("max_w", L, "max_w", 0, 2, 0, 4000);
        // surface the silent cap: when the rendered text is filling max_w, px is being
        // auto-shrunk to fit — raising px does nothing until max_w goes up.
        if (jf(L, "max_w", 0) > 0 && g_app.sel >= 0 && g_app.sel < (int)g_app.layerInfo.size()) {
            const LayerInfo& li = g_app.layerInfo[g_app.sel];
            if (li.x1 - li.x0 >= jf(L, "max_w", 0))
                ImGui::TextColored(ImVec4(0.92f, 0.72f, 0.32f, 1.f),
                                   "  \xe2\x86\xb3 auto-fit: filling max_w \xe2\x80\x94 raise it or cut words for bigger text");
        }
        ch |= color_field("fill", L, "fill", "");
        ch |= color_field("grad_to", L, "grad_to", "");
        ch |= num_field("stroke_px", L, "stroke_px", -1, 0.2f, -1, 60);
        ch |= color_field("stroke", L, "stroke", "");
        ch |= num_field("tracking", L, "tracking", 0, 0.2f);
        // line spacing: multiplier on the natural line height (default from the style,
        // else 1.02). <1 packs multi-line text closer vertically, >1 opens it up.
        double lhDef = 1.02;
        { std::string st = js(L, "style", "headline");
          if (g_app.brand.styles.contains(st) && g_app.brand.styles[st].contains("line_height")
              && g_app.brand.styles[st]["line_height"].is_number())
              lhDef = g_app.brand.styles[st]["line_height"].get<double>(); }
        ch |= num_field("line height", L, "line_height", lhDef, 0.01f, 0.4, 3.0, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("x natural leading — <1 packs lines closer, >1 opens them up");
        ch |= fx_section(L);
    } else if (type == "shape") {
        std::string shape = js(L, "shape", "arrow");
        if (shape == "arrow") {
            ch |= num_field("x1", L, "x1", 100); ImGui::SameLine(); ch |= num_field("y1", L, "y1", 100);
            ch |= num_field("x2", L, "x2", 300); ImGui::SameLine(); ch |= num_field("y2", L, "y2", 300);
            ch |= num_field("width", L, "width", 24, 0.5f, 2, 200);
        } else if (shape == "circle") {
            ch |= num_field("x", L, "x", 300); ImGui::SameLine(); ch |= num_field("y", L, "y", 300);
            ch |= num_field("r", L, "r", 120, 1, 4, 2000);
            ch |= num_field("thick", L, "thick", 16, 0.5f, 0, 300);
        } else {
            ch |= num_field("x", L, "x", 300); ImGui::SameLine(); ch |= num_field("y", L, "y", 300);
            ch |= num_field("w", L, "w", 300, 1, 4, 4000); ImGui::SameLine(); ch |= num_field("h", L, "h", 180, 1, 4, 4000);
            ch |= num_field("radius", L, "radius", 12, 0.5f, 0, 200);
            ch |= num_field("thick", L, "thick", 0, 0.5f, 0, 300);
        }
        ch |= num_field("rot", L, "rot", 0, 0.2f, -180, 180);
        ch |= color_field("fill", L, "fill", "$gold");
        ch |= num_field("outline_px", L, "outline_px", 0, 0.2f, 0, 60);
        ch |= color_field("outline", L, "outline", "#000000");
        ch |= fx_section(L);
    } else if (type == "mosaic") {
        ch |= num_field("x", L, "x", canvas_w() / 2.0); ImGui::SameLine(); ch |= num_field("y", L, "y", canvas_h() / 2.0);
        ch |= num_field("w", L, "w", 260, 1, 8, 4000); ImGui::SameLine(); ch |= num_field("h", L, "h", 160, 1, 8, 4000);
        ch |= num_field("cell", L, "cell", 22, 0.5f, 2, 200);
        ImGui::TextDisabled("pixelates whatever is beneath the rect");
    } else if (type == "watermark") {
        ImGui::TextDisabled("drawn from the brand package's watermark spec");
    }
    if (ch) { /* widget-driven edits settle via IsAnyItemActive edge */ }
}

static void panel_sprites() {
    if (g_app.brand.sprite_roots.empty()) { ImGui::TextDisabled("no sprite roots in brand package"); return; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sfilter", "filter sprites..", g_app.spriteFilter, sizeof g_app.spriteFilter);
    std::string filter = g_app.spriteFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    float availW = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)(availW / 92));
    int n = 0, shown = 0;
    std::error_code ec;
    for (auto& root : g_app.brand.sprite_roots) {
        for (auto& e : fs::directory_iterator(host_path(root), ec)) {
            if (e.is_directory(ec)) continue;
            std::string name = e.path().filename().string();
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".png") continue;
            if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
            if (shown++ > 120) { ImGui::TextDisabled(".. (filter to see more)"); return; }
            std::string full = path_join(root, name);
            TexEntry* t = thumb_tex(full, 96);
            if (!t) continue;
            if (n++ % cols) ImGui::SameLine();
            ImGui::PushID(full.c_str());
            float w = 84, h = w * t->h / std::max(1, t->w);
            if (ImGui::ImageButton("##sp", (ImTextureID)(intptr_t)t->srv, ImVec2(w, std::min(h, 110.f))))
                add_layer({{"id", unique_id("img")}, {"type", "image"}, {"src", full}, {"x", canvas_w() / 2}, {"y", canvas_h() / 2}, {"scale", 0.9}});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
            ImGui::PopID();
        }
    }
}

// ─────────────── canvas: preview + unified manipulation (teidraw port) ──────
// Rotation-aware OBB selection · corner scale about the fixed opposite corner ·
// rotate ring just outside each corner (shift = 15° snap) · ctrl+corner crop on
// images with a full-source ghost · edge handles (text wrap width, rect/mosaic
// w/h) · arrow endpoint gizmos · marquee multi-select · ctrl = snap-with-guides
// · shift = axis lock / multi-toggle. Every drag mutates from a press-time
// snapshot + absolute offset, so modifiers rewrite the gesture without drift.

struct Obb { bool ok = false; ImVec2 c; float hw = 0, hh = 0, rotDeg = 0; };

static Obb layer_obb(int i) {
    Obb o;
    auto& ls = layers();
    if (i < 0 || i >= (int)g_app.layerInfo.size() || i >= (int)ls.size()) return o;
    const LayerInfo& li = g_app.layerInfo[i];
    if (li.type.empty() || li.type == "bg" || li.x1 <= li.x0 || li.y1 <= li.y0) return o;
    o.ok = true;
    bool isArrow = li.type == "shape" && js(ls[i], "shape", "") == "arrow";
    o.rotDeg = (li.type == "mosaic" || li.type == "watermark" || isArrow) ? 0.f : (float)jf(ls[i], "rot", 0);
    // hug the VISIBLE content (tight alpha bbox + border), not the padded block;
    // the content can sit off-center in the block, and rotation pivots on the
    // BLOCK center — so rotate the content center about it to place the box
    bool hasContent = li.px1 > li.px0 && li.py1 > li.py0;
    float px0 = hasContent ? li.px0 : li.x0, py0 = hasContent ? li.py0 : li.y0;
    float px1 = hasContent ? li.px1 : li.x1, py1 = hasContent ? li.py1 : li.y1;
    ImVec2 blockC((li.x0 + li.x1) / 2, (li.y0 + li.y1) / 2);
    ImVec2 d = rot_vec(ImVec2((px0 + px1) / 2 - blockC.x, (py0 + py1) / 2 - blockC.y), deg2rad(o.rotDeg));
    o.c = ImVec2(blockC.x + d.x, blockC.y + d.y);
    o.hw = std::max(4.f, (px1 - px0) / 2);
    o.hh = std::max(4.f, (py1 - py0) / 2);
    return o;
}
static bool obb_hit(const Obb& o, ImVec2 pL) {
    if (!o.ok) return false;
    ImVec2 lp = rot_about(pL, o.c, -deg2rad(o.rotDeg));
    return fabsf(lp.x - o.c.x) <= o.hw && fabsf(lp.y - o.c.y) <= o.hh;
}
static void obb_corners(const Obb& o, ImVec2 out[4]) {   // tl,tr,br,bl
    float a = deg2rad(o.rotDeg);
    ImVec2 ex[4] = {{-o.hw, -o.hh}, {o.hw, -o.hh}, {o.hw, o.hh}, {-o.hw, o.hh}};
    for (int i = 0; i < 4; i++) { ImVec2 r = rot_vec(ex[i], a); out[i] = ImVec2(o.c.x + r.x, o.c.y + r.y); }
}
// marquee: partial overlap selects (SAT on the rect's + obb's axes)
static bool obb_touches_rect(const Obb& o, ImVec2 mn, ImVec2 mx) {
    if (!o.ok) return false;
    ImVec2 cs[4]; obb_corners(o, cs);
    float ax0 = 1e9f, ax1 = -1e9f, ay0 = 1e9f, ay1 = -1e9f;
    for (auto& p : cs) { ax0 = std::min(ax0, p.x); ax1 = std::max(ax1, p.x); ay0 = std::min(ay0, p.y); ay1 = std::max(ay1, p.y); }
    if (ax1 < mn.x || ax0 > mx.x || ay1 < mn.y || ay0 > mx.y) return false;
    float a = deg2rad(o.rotDeg);
    ImVec2 axes[2] = {rot_vec(ImVec2(1, 0), a), rot_vec(ImVec2(0, 1), a)};
    ImVec2 rc[4] = {mn, {mx.x, mn.y}, mx, {mn.x, mx.y}};
    for (int ai = 0; ai < 2; ai++) {
        float oc = axes[ai].x * o.c.x + axes[ai].y * o.c.y, oh = ai == 0 ? o.hw : o.hh;
        float r0 = 1e9f, r1 = -1e9f;
        for (auto& p : rc) { float d = axes[ai].x * p.x + axes[ai].y * p.y; r0 = std::min(r0, d); r1 = std::max(r1, d); }
        if (r1 < oc - oh || r0 > oc + oh) return false;
    }
    return true;
}

static bool layer_selectable(int i) {
    auto& ls = layers();
    if (i < 0 || i >= (int)ls.size() || i >= (int)g_app.layerInfo.size()) return false;
    if (jb(ls[i], "hidden", false)) return false;
    const std::string& t = g_app.layerInfo[i].type;
    return !t.empty() && t != "bg";
}

// ctrl-snap while moving: pull the moving selection's edges/centers onto canvas
// guides (edges · center · thirds) and other layers' edges/centers, drawing mint
// guide lines through the match (teidraw snap_move, 8 screen px threshold).
static ImVec2 snap_move_offset(ImVec2 want, ImDrawList* dl, ImVec2 p0, ImVec2 sz, float sc) {
    float cw = (float)canvas_w(), chh = (float)canvas_h();
    std::vector<float> tx = {0, cw / 3, cw / 2, 2 * cw / 3, cw};
    std::vector<float> ty = {0, chh / 3, chh / 2, 2 * chh / 3, chh};
    for (int i = 0; i < (int)layers().size(); i++) {
        if (!layer_selectable(i) || sel_contains(i)) continue;
        Obb o = layer_obb(i); if (!o.ok) continue;
        ImVec2 cs[4]; obb_corners(o, cs);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (auto& p : cs) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        tx.push_back(x0); tx.push_back((x0 + x1) / 2); tx.push_back(x1);
        ty.push_back(y0); ty.push_back((y0 + y1) / 2); ty.push_back(y1);
    }
    float thr = 8.f / sc;
    float mnx = g_giz.boundsMn[0] + want.x, mxx = g_giz.boundsMx[0] + want.x;
    float mny = g_giz.boundsMn[1] + want.y, mxy = g_giz.boundsMx[1] + want.y;
    float probeX[3] = {mnx, (mnx + mxx) / 2, mxx}, probeY[3] = {mny, (mny + mxy) / 2, mxy};
    float bestDx = 0, bestDy = 0, bestAx = thr, bestAy = thr;
    float gx = 0, gy = 0; bool hitX = false, hitY = false;
    for (float p : probeX) for (float t : tx) { float d = t - p; if (fabsf(d) < bestAx) { bestAx = fabsf(d); bestDx = d; gx = t; hitX = true; } }
    for (float p : probeY) for (float t : ty) { float d = t - p; if (fabsf(d) < bestAy) { bestAy = fabsf(d); bestDy = d; gy = t; hitY = true; } }
    want.x += hitX ? bestDx : 0; want.y += hitY ? bestDy : 0;
    const ImU32 GUIDE = IM_COL32(0x7f, 0xd8, 0xa8, 190);
    if (hitX) dl->AddLine(ImVec2(p0.x + gx * sc, p0.y), ImVec2(p0.x + gx * sc, p0.y + sz.y), GUIDE, 1.f);
    if (hitY) dl->AddLine(ImVec2(p0.x, p0.y + gy * sc), ImVec2(p0.x + sz.x, p0.y + gy * sc), GUIDE, 1.f);
    return want;
}

// [ / ] one z step, shift = all the way (the layers array is bottom→top)
static void reorder_layer(int i, int dir, bool allTheWay) {
    auto& ls = layers();
    int n = (int)ls.size();
    if (i < 0 || i >= n || n < 2) return;
    if (allTheWay) {
        json L = ls[i]; ls.erase(ls.begin() + i);
        int j = dir > 0 ? n - 1 : 0;
        ls.insert(ls.begin() + j, L); select_one(j);
    } else {
        int j = i + dir;
        if (j < 0 || j >= n) return;
        std::swap(ls[i], ls[j]); select_one(j);
    }
    g_app.undoDirty = true;
}

static void panel_canvas() {
    ensure_preview();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!g_app.previewSrv || avail.x < 32 || avail.y < 32) return;
    float cw = (float)canvas_w(), chh = (float)canvas_h();
    float sc = std::min((avail.x - 8) / cw, (avail.y - 8) / chh);
    ImVec2 sz(cw * sc, chh * sc);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 p0(origin.x + (avail.x - sz.x) / 2, origin.y + (avail.y - sz.y) / 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p0.x + sz.x + 1, p0.y + sz.y + 1), IM_COL32(90, 90, 110, 255));
    // the button spans the whole panel so handles/ring just outside the canvas stay grabbable
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    dl->AddImage((ImTextureID)(intptr_t)g_app.previewSrv, p0, ImVec2(p0.x + sz.x, p0.y + sz.y));

    g_canvasP0 = p0; g_canvasSc = sc;
    auto toScr = [&](ImVec2 l) { return ImVec2(p0.x + l.x * sc, p0.y + l.y * sc); };
    ImVec2 m = io.MousePos;
    ImVec2 mL((m.x - p0.x) / sc, (m.y - p0.y) / sc);

    // sanitize selection (undo/redo/hot-reload can reshape the doc under us)
    {
        int n = (int)layers().size();
        for (size_t k = g_app.selMulti.size(); k-- > 0;)
            if (g_app.selMulti[k] < 0 || g_app.selMulti[k] >= n)
                g_app.selMulti.erase(g_app.selMulti.begin() + k);
        if (g_app.sel >= n) g_app.sel = -1;
        if (g_app.sel < 0 && !g_app.selMulti.empty()) g_app.sel = g_app.selMulti.back();
        if (g_app.sel >= 0 && !sel_contains(g_app.sel)) g_app.selMulti.push_back(g_app.sel);
    }

    // OS drop → branded card image layer(s) at the drop point (style swappable after)
    if (g_hasDrop) {
        float lx = (g_dropPt.x - p0.x) / sc, ly = (g_dropPt.y - p0.y) / sc;
        if (lx < 0 || ly < 0 || lx > cw || ly > chh) { lx = cw / 2; ly = chh / 2; }
        sel_clear();
        int made = 0;
        for (auto& f : g_dropFiles) {
            json L = {{"id", unique_id("img")}, {"type", "image"}, {"src", f},
                      {"x", lx + made * 26}, {"y", ly + made * 26}, {"scale", 0.55}};
            apply_image_style(L, "card");
            layers().push_back(L);
            g_app.selMulti.push_back((int)layers().size() - 1);
            g_app.sel = (int)layers().size() - 1;
            made++;
        }
        if (made) {
            g_app.undoDirty = true;
            char b[128]; snprintf(b, sizeof b, "dropped %d image%s as branded card (swap the style in the inspector)", made, made == 1 ? "" : "s");
            g_app.status = b;
        }
        g_dropFiles.clear(); g_hasDrop = false;
        return;   // layerInfo refreshes next frame
    }

    // ── hover: handles of the single-selected layer first, then layer bodies ──
    enum { HK_NONE, HK_CORNER, HK_RING, HK_EDGE, HK_ARROW_A, HK_ARROW_B };
    const float HR = 5.f;
    int hovKind = HK_NONE, hovIdx = -1;
    bool single = g_app.selMulti.size() == 1 && g_app.sel >= 0 && g_app.sel < (int)layers().size();
    Obb selObb; ImVec2 selCorScr[4]{}, selCorL[4]{};
    bool selIsArrow = false; std::string selType;
    if (single) {
        json& L = layers()[g_app.sel];
        selType = js(L, "type", "");
        selIsArrow = selType == "shape" && js(L, "shape", "") == "arrow";
        if (selIsArrow) {
            ImVec2 a = toScr(ImVec2((float)jf(L, "x1", 100), (float)jf(L, "y1", 100)));
            ImVec2 b = toScr(ImVec2((float)jf(L, "x2", 300), (float)jf(L, "y2", 300)));
            if (vlen(ImVec2(m.x - a.x, m.y - a.y)) <= HR + 4) { hovKind = HK_ARROW_A; }
            else if (vlen(ImVec2(m.x - b.x, m.y - b.y)) <= HR + 4) { hovKind = HK_ARROW_B; }
        } else if (selType != "watermark") {
            selObb = layer_obb(g_app.sel);
            if (selObb.ok) {
                obb_corners(selObb, selCorL);
                for (int j = 0; j < 4; j++) selCorScr[j] = toScr(selCorL[j]);
                for (int j = 0; j < 4 && hovKind == HK_NONE; j++)
                    if (vlen(ImVec2(m.x - selCorScr[j].x, m.y - selCorScr[j].y)) <= HR + 3) { hovKind = HK_CORNER; hovIdx = j; }
                // edge handles: text = wrap width (l,r); rect/mosaic = w/h (l,r,t,b)
                bool edgeLR = selType == "text" || (selType == "shape" && js(L, "shape", "") == "rect") || selType == "mosaic";
                bool edgeTB = (selType == "shape" && js(L, "shape", "") == "rect") || selType == "mosaic";
                if (hovKind == HK_NONE && (edgeLR || edgeTB)) {
                    ImVec2 mid[4] = {   // l,r,t,b
                        ImVec2((selCorScr[0].x + selCorScr[3].x) / 2, (selCorScr[0].y + selCorScr[3].y) / 2),
                        ImVec2((selCorScr[1].x + selCorScr[2].x) / 2, (selCorScr[1].y + selCorScr[2].y) / 2),
                        ImVec2((selCorScr[0].x + selCorScr[1].x) / 2, (selCorScr[0].y + selCorScr[1].y) / 2),
                        ImVec2((selCorScr[3].x + selCorScr[2].x) / 2, (selCorScr[3].y + selCorScr[2].y) / 2)};
                    int nEdge = edgeTB ? 4 : 2;
                    for (int j = 0; j < nEdge && hovKind == HK_NONE; j++)
                        if (vlen(ImVec2(m.x - mid[j].x, m.y - mid[j].y)) <= HR + 3) { hovKind = HK_EDGE; hovIdx = j; }
                }
                // rotate ring: a band just outside each corner (teidraw: r+3..r+17)
                bool canRot = selType == "image" || selType == "text" || (selType == "shape" && !selIsArrow);
                if (hovKind == HK_NONE && canRot)
                    for (int j = 0; j < 4 && hovKind == HK_NONE; j++) {
                        float d = vlen(ImVec2(m.x - selCorScr[j].x, m.y - selCorScr[j].y));
                        if (d >= HR + 3 && d < HR + 17) { hovKind = HK_RING; hovIdx = j; }
                    }
            }
        }
    }
    int hovLayer = -1;
    if (hovKind == HK_NONE && ImGui::IsItemHovered()) {
        for (int s : g_app.selMulti)   // sticky: a selected layer under the cursor wins
            if (layer_selectable(s) && obb_hit(layer_obb(s), mL)) { hovLayer = s; break; }
        if (hovLayer < 0)
            for (int i = (int)layers().size() - 1; i >= 0; i--)
                if (layer_selectable(i) && obb_hit(layer_obb(i), mL)) { hovLayer = i; break; }
    }

    // ── press dispatch ──
    if (ImGui::IsItemClicked(0)) {
        g_giz.pressL = mL; g_giz.pressScr = m; g_giz.moved = false; g_giz.snaps.clear();
        if (single && hovKind != HK_NONE) {
            json& L = layers()[g_app.sel];
            g_giz.layer = g_app.sel; g_giz.snap = L; g_giz.handle = hovIdx;
            if (hovKind == HK_ARROW_A) g_giz.mode = DM_ARROW_A;
            else if (hovKind == HK_ARROW_B) g_giz.mode = DM_ARROW_B;
            else if (hovKind == HK_CORNER && io.KeyCtrl && selType == "image") {
                Img* im = get_image(path_join(g_app.docDir, js(L, "src", "")));
                if (im) { g_giz.iw = (float)im->w; g_giz.ih = (float)im->h; g_giz.mode = DM_CROP; }
            } else if (hovKind == HK_CORNER) {
                g_giz.mode = DM_HANDLE;
                g_giz.fixedL = selCorL[(hovIdx + 2) & 3];
                // scale about the fixed corner maps EVERY point by k — including the
                // layer center (which may sit off the visible-content center)
                g_giz.centerL = ImVec2((float)jf(L, "x", cw / 2.0), (float)jf(L, "y", chh / 2.0));
                g_giz.startDist = std::max(4.f / sc, vlen(ImVec2(mL.x - g_giz.fixedL.x, mL.y - g_giz.fixedL.y)));
            } else if (hovKind == HK_RING) {
                g_giz.mode = DM_ROTATE;
                g_giz.pivotL = selObb.c;
                g_giz.startAngle = atan2f(mL.y - selObb.c.y, mL.x - selObb.c.x);
            } else if (hovKind == HK_EDGE) {
                g_giz.mode = DM_EDGE;
                g_giz.centerL = selObb.c;
                g_giz.startDist = (hovIdx < 2 ? selObb.hw : selObb.hh) * 2;   // box size along the dragged axis at press
            }
        } else if (hovLayer >= 0) {
            if (io.KeyShift) sel_toggle(hovLayer);
            else if (!sel_contains(hovLayer)) select_one(hovLayer);
            else g_app.sel = hovLayer;                    // primary follows the click
            if (sel_contains(hovLayer)) {
                g_giz.mode = DM_PENDING; g_giz.layer = hovLayer;
                float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                for (int s : g_app.selMulti) {
                    g_giz.snaps.push_back({s, layers()[s]});
                    Obb o = layer_obb(s);
                    if (o.ok) {
                        ImVec2 cs[4]; obb_corners(o, cs);
                        for (auto& p : cs) { bx0 = std::min(bx0, p.x); bx1 = std::max(bx1, p.x); by0 = std::min(by0, p.y); by1 = std::max(by1, p.y); }
                    }
                }
                g_giz.boundsMn[0] = bx0; g_giz.boundsMn[1] = by0; g_giz.boundsMx[0] = bx1; g_giz.boundsMx[1] = by1;
            }
        } else if (ImGui::IsItemHovered()) {
            g_giz.mode = DM_MARQUEE; g_giz.layer = -1;
            g_giz.marqueeBase = io.KeyShift ? g_app.selMulti : std::vector<int>();
            if (!io.KeyShift) sel_clear();
        }
    }

    // ── drag update / release ──
    bool escCanceledDrag = false;
    if (g_giz.mode != DM_NONE) {
        auto& ls = layers();
        bool layerOk = g_giz.layer >= 0 && g_giz.layer < (int)ls.size();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {          // cancel: revert the gesture
            if (!g_giz.snaps.empty()) { for (auto& [i, s] : g_giz.snaps) if (i < (int)ls.size()) ls[i] = s; }
            else if (layerOk && g_giz.mode != DM_MARQUEE && g_giz.mode != DM_PENDING) ls[g_giz.layer] = g_giz.snap;
            g_giz.mode = DM_NONE; g_giz.snaps.clear();
            escCanceledDrag = true;
        } else if (!io.MouseDown[0]) {                       // release → settle (one undo step)
            double now = ImGui::GetTime();
            bool wasClick = !g_giz.moved && g_giz.mode != DM_MARQUEE;
            if (wasClick && layerOk) {
                if (now - g_giz.lastClickT < 0.32 && vlen(ImVec2(m.x - g_giz.lastClickScr.x, m.y - g_giz.lastClickScr.y)) < 6
                    && g_giz.layer == g_giz.lastClickLayer && js(ls[g_giz.layer], "type", "") == "text")
                    g_app.focusText = true;                  // double-click text → edit it in the inspector
                g_giz.lastClickT = now; g_giz.lastClickScr = m; g_giz.lastClickLayer = g_giz.layer;
            }
            g_giz.mode = DM_NONE; g_giz.snaps.clear();
        } else switch (g_giz.mode) {
        case DM_PENDING:
            if (vlen(ImVec2(m.x - g_giz.pressScr.x, m.y - g_giz.pressScr.y)) > 4.f) g_giz.mode = DM_MOVE;
            break;
        case DM_MOVE: {
            ImVec2 want(mL.x - g_giz.pressL.x, mL.y - g_giz.pressL.y);
            if (io.KeyShift) { if (fabsf(want.x) >= fabsf(want.y)) want.y = 0; else want.x = 0; }   // axis lock
            if (io.KeyCtrl) want = snap_move_offset(want, dl, p0, sz, sc);
            for (auto& [i, s] : g_giz.snaps) {
                if (i >= (int)ls.size()) continue;
                json& L = ls[i];
                if (js(s, "type", "") == "shape" && js(s, "shape", "") == "arrow") {
                    L["x1"] = jf(s, "x1", 0) + want.x; L["y1"] = jf(s, "y1", 0) + want.y;
                    L["x2"] = jf(s, "x2", 0) + want.x; L["y2"] = jf(s, "y2", 0) + want.y;
                } else {
                    L["x"] = jf(s, "x", cw / 2.0) + want.x;
                    L["y"] = jf(s, "y", chh / 2.0) + want.y;
                }
            }
            if (want.x != 0 || want.y != 0) g_giz.moved = true;
            g_app.undoDirty = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            break;
        }
        case DM_HANDLE: {
            if (!layerOk) break;
            float k = vlen(ImVec2(mL.x - g_giz.fixedL.x, mL.y - g_giz.fixedL.y)) / g_giz.startDist;
            k = clampf(k, 0.05f, 50.f);
            json& L = ls[g_giz.layer]; const json& s = g_giz.snap;
            std::string t = js(s, "type", "");
            if (t == "image") L["scale"] = jf(s, "scale", 1.0) * k;
            else if (t == "text") {
                double px0 = jf(s, "px", 0);
                if (px0 <= 0) px0 = jf(style_of(g_app.brand, s), "px", 160);
                L["px"] = px0 * k;
                if (jf(s, "max_w", 0) > 0) L["max_w"] = jf(s, "max_w", 0) * k;
            } else if (t == "shape") {
                std::string sh = js(s, "shape", "");
                if (sh == "circle") { L["r"] = jf(s, "r", 120) * k; if (jf(s, "thick", 16) > 0) L["thick"] = jf(s, "thick", 16) * k; }
                else if (sh == "rect") {
                    L["w"] = jf(s, "w", 300) * k; L["h"] = jf(s, "h", 180) * k;
                    L["radius"] = jf(s, "radius", 12) * k;
                    if (jf(s, "thick", 0) > 0) L["thick"] = jf(s, "thick", 0) * k;
                }
            } else if (t == "mosaic") { L["w"] = jf(s, "w", 260) * k; L["h"] = jf(s, "h", 160) * k; }
            ImVec2 c1(g_giz.fixedL.x + (g_giz.centerL.x - g_giz.fixedL.x) * k,
                      g_giz.fixedL.y + (g_giz.centerL.y - g_giz.fixedL.y) * k);
            L["x"] = c1.x; L["y"] = c1.y;
            g_giz.moved = true; g_app.undoDirty = true;
            ImGui::SetMouseCursor((g_giz.handle & 1) ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE);
            break;
        }
        case DM_ROTATE: {
            if (!layerOk) break;
            const json& s = g_giz.snap;
            float ang = atan2f(mL.y - g_giz.pivotL.y, mL.x - g_giz.pivotL.x);
            float rot = (float)jf(s, "rot", 0) + (ang - g_giz.startAngle) * 180.f / 3.14159265f;
            if (io.KeyShift) rot = roundf(rot / 15.f) * 15.f;   // 15° steps
            while (rot > 180.f) rot -= 360.f;
            while (rot < -180.f) rot += 360.f;
            json& L = ls[g_giz.layer];
            L["rot"] = fabsf(rot) < 0.001f ? 0.f : rot;
            // spin around the VISIBLE content center (the ring's pivot), which may
            // sit off the layer center: re-place x/y so that point stays put
            ImVec2 c0((float)jf(s, "x", cw / 2.0), (float)jf(s, "y", chh / 2.0));
            ImVec2 oc = rot_vec(ImVec2(g_giz.pivotL.x - c0.x, g_giz.pivotL.y - c0.y), -deg2rad((float)jf(s, "rot", 0)));
            ImVec2 od = rot_vec(oc, deg2rad(rot));
            L["x"] = g_giz.pivotL.x - od.x; L["y"] = g_giz.pivotL.y - od.y;
            g_giz.moved = true; g_app.undoDirty = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            break;
        }
        case DM_EDGE: {
            if (!layerOk) break;
            json& L = ls[g_giz.layer]; const json& s = g_giz.snap;
            std::string t = js(s, "type", "");
            float rotR = (t == "mosaic") ? 0.f : deg2rad((float)jf(s, "rot", 0));
            ImVec2 lp = rot_vec(ImVec2(mL.x - g_giz.centerL.x, mL.y - g_giz.centerL.y), -rotR);
            float d0 = g_giz.startDist;                       // box size along the axis at press
            int h = g_giz.handle;                             // 0=l 1=r 2=t 3=b
            float lpv = h < 2 ? lp.x : lp.y;
            float opp = (h == 0 || h == 2) ? d0 / 2 : -d0 / 2;
            float newS = clampf(fabsf(lpv - opp), 12.f, 8000.f);
            float cShift = opp + ((h == 0 || h == 2) ? -newS / 2 : newS / 2);
            ImVec2 dW = rot_vec(h < 2 ? ImVec2(cShift, 0) : ImVec2(0, cShift), rotR);
            // newS is measured on the VISIBLE box; the doc params (w/h/max_w) are inner
            // values — apply the box delta so nothing pops on grab
            if (t == "text") {
                float base = (float)jf(s, "max_w", 0); if (base <= 0) base = d0;
                L["max_w"] = std::max(30.f, base + newS - d0);
            } else {
                const char* key = h < 2 ? "w" : "h";
                float base = (float)jf(s, key, h < 2 ? 300.0 : 180.0);
                L[key] = std::max(8.f, base + newS - d0);
            }
            L["x"] = jf(s, "x", cw / 2.0) + dW.x;
            L["y"] = jf(s, "y", chh / 2.0) + dW.y;
            g_giz.moved = true; g_app.undoDirty = true;
            ImGui::SetMouseCursor(h < 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            break;
        }
        case DM_CROP: {
            if (!layerOk || g_giz.iw <= 0 || g_giz.ih <= 0) break;
            json& L = ls[g_giz.layer]; const json& s = g_giz.snap;
            float rotR = deg2rad((float)jf(s, "rot", 0));
            float sxc = (float)jf(s, "x", cw / 2.0), syc = (float)jf(s, "y", chh / 2.0);
            float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
            if (s.contains("crop") && s["crop"].is_array() && s["crop"].size() == 4) {
                u0 = (float)s["crop"][0].get<double>(); v0 = (float)s["crop"][1].get<double>();
                u1 = (float)s["crop"][2].get<double>(); v1 = (float)s["crop"][3].get<double>();
            }
            float fullH = (float)jf(s, "scale", 1.0) * chh, fullW = fullH * g_giz.iw / g_giz.ih;
            // full-source rect in the crop-center local frame (unrotated)
            ImVec2 fc(fullW * (0.5f - (u0 + u1) / 2), fullH * (0.5f - (v0 + v1) / 2));
            ImVec2 fmn(fc.x - fullW / 2, fc.y - fullH / 2), fmx(fc.x + fullW / 2, fc.y + fullH / 2);
            ImVec2 mn(-fullW * (u1 - u0) / 2, -fullH * (v1 - v0) / 2), mx(-mn.x, -mn.y);
            ImVec2 lp = rot_vec(ImVec2(mL.x - sxc, mL.y - syc), -rotR);
            ImVec2 p(clampf(lp.x, fmn.x, fmx.x), clampf(lp.y, fmn.y, fmx.y));
            float minPx = 8.f;
            switch (g_giz.handle) {   // tl,tr,br,bl
            case 0: mn.x = std::min(p.x, mx.x - minPx); mn.y = std::min(p.y, mx.y - minPx); break;
            case 1: mx.x = std::max(p.x, mn.x + minPx); mn.y = std::min(p.y, mx.y - minPx); break;
            case 2: mx.x = std::max(p.x, mn.x + minPx); mx.y = std::max(p.y, mn.y + minPx); break;
            case 3: mn.x = std::min(p.x, mx.x - minPx); mx.y = std::max(p.y, mn.y + minPx); break;
            }
            L["crop"] = {(mn.x - fmn.x) / fullW, (mn.y - fmn.y) / fullH,
                         (mx.x - fmn.x) / fullW, (mx.y - fmn.y) / fullH};
            ImVec2 dc = rot_vec(ImVec2((mn.x + mx.x) / 2, (mn.y + mx.y) / 2), rotR);
            L["x"] = sxc + dc.x; L["y"] = syc + dc.y;
            g_giz.moved = true; g_app.undoDirty = true;
            ImGui::SetMouseCursor((g_giz.handle & 1) ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE);
            break;
        }
        case DM_ARROW_A: case DM_ARROW_B: {
            if (!layerOk) break;
            json& L = ls[g_giz.layer]; const json& s = g_giz.snap;
            bool isA = g_giz.mode == DM_ARROW_A;
            ImVec2 p = mL;
            if (io.KeyShift) {   // 45° snap about the other end
                ImVec2 other(isA ? (float)jf(s, "x2", 300) : (float)jf(s, "x1", 100),
                             isA ? (float)jf(s, "y2", 300) : (float)jf(s, "y1", 100));
                ImVec2 d(p.x - other.x, p.y - other.y);
                float a = roundf(atan2f(d.y, d.x) / (3.14159265f / 4)) * (3.14159265f / 4);
                float len = vlen(d);
                p = ImVec2(other.x + cosf(a) * len, other.y + sinf(a) * len);
            }
            L[isA ? "x1" : "x2"] = p.x; L[isA ? "y1" : "y2"] = p.y;
            g_giz.moved = true; g_app.undoDirty = true;
            break;
        }
        case DM_MARQUEE: {
            ImVec2 mn(std::min(g_giz.pressL.x, mL.x), std::min(g_giz.pressL.y, mL.y));
            ImVec2 mx(std::max(g_giz.pressL.x, mL.x), std::max(g_giz.pressL.y, mL.y));
            g_app.selMulti = g_giz.marqueeBase;
            for (int i = 0; i < (int)layers().size(); i++)
                if (layer_selectable(i) && !sel_contains(i) && obb_touches_rect(layer_obb(i), mn, mx))
                    g_app.selMulti.push_back(i);
            g_app.sel = g_app.selMulti.empty() ? -1 : g_app.selMulti.back();
            ImVec2 smn = toScr(mn), smx = toScr(mx);
            dl->AddRectFilled(smn, smx, IM_COL32(0x7f, 0xd8, 0xa8, 26));
            dl->AddRect(smn, smx, IM_COL32(0x7f, 0xd8, 0xa8, 170));
            break;
        }
        default: break;
        }
    }

    // hover cursor hints (not mid-drag)
    if (g_giz.mode == DM_NONE) {
        if (hovKind == HK_CORNER) ImGui::SetMouseCursor((io.KeyCtrl && selType == "image") ? ImGuiMouseCursor_ResizeAll
                                                        : ((hovIdx & 1) ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE));
        else if (hovKind == HK_RING) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (hovKind == HK_EDGE) ImGui::SetMouseCursor(hovIdx < 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }

    // ctrl+wheel scale on hover (kept from the old UX; a burst = one undo step)
    if (ImGui::IsItemHovered() && io.KeyCtrl && io.MouseWheel != 0 && g_app.sel >= 0 && g_app.sel < (int)layers().size()) {
        json& L = layers()[g_app.sel];
        float f = io.MouseWheel > 0 ? 1.05f : 1 / 1.05f;
        std::string t = js(L, "type", "");
        if (t == "image") L["scale"] = jf(L, "scale", 1.0) * f;
        else if (t == "text") { double px = jf(L, "px", 0); if (px <= 0) px = 160; L["px"] = px * f; }
        else if (t == "shape") {
            if (js(L, "shape", "") == "circle") L["r"] = jf(L, "r", 120) * f;
            else if (js(L, "shape", "") == "rect") { L["w"] = jf(L, "w", 300) * f; L["h"] = jf(L, "h", 180) * f; }
            else L["width"] = jf(L, "width", 24) * f;
        }
        g_app.undoDirty = true;
        g_app.wheelGesture = 0.30f;   // discrete ticks → a short live-gesture window
    }

    // ── draw: crop ghost → hover outline → selection → handles ──
    const ImU32 ACCENT = IM_COL32(0x7f, 0xd8, 0xa8, 220), HOVER = IM_COL32(0x88, 0x78, 0xd0, 150);
    const ImU32 HFILL = IM_COL32(240, 240, 250, 255);
    bool cropAffordance = single && selType == "image" && (g_giz.mode == DM_CROP || (g_giz.mode == DM_NONE && io.KeyCtrl && hovKind == HK_CORNER));
    if (cropAffordance && g_app.sel < (int)layers().size()) {
        json& L = layers()[g_app.sel];
        Img* im = get_image(path_join(g_app.docDir, js(L, "src", "")));
        TexEntry* gt = im ? thumb_tex(path_join(g_app.docDir, js(L, "src", "")), 512) : nullptr;
        if (im && gt) {
            float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
            if (L.contains("crop") && L["crop"].is_array() && L["crop"].size() == 4) {
                u0 = (float)L["crop"][0].get<double>(); v0 = (float)L["crop"][1].get<double>();
                u1 = (float)L["crop"][2].get<double>(); v1 = (float)L["crop"][3].get<double>();
            }
            float rotR = deg2rad((float)jf(L, "rot", 0));
            float fullH = (float)jf(L, "scale", 1.0) * chh, fullW = fullH * im->w / im->h;
            ImVec2 c((float)jf(L, "x", cw / 2.0), (float)jf(L, "y", chh / 2.0));
            ImVec2 fcL(fullW * (0.5f - (u0 + u1) / 2), fullH * (0.5f - (v0 + v1) / 2));
            ImVec2 fcW(c.x + rot_vec(fcL, rotR).x, c.y + rot_vec(fcL, rotR).y);
            ImVec2 ex[4] = {{-fullW / 2, -fullH / 2}, {fullW / 2, -fullH / 2}, {fullW / 2, fullH / 2}, {-fullW / 2, fullH / 2}};
            ImVec2 q[4];
            for (int j = 0; j < 4; j++) { ImVec2 r = rot_vec(ex[j], rotR); q[j] = toScr(ImVec2(fcW.x + r.x, fcW.y + r.y)); }
            bool flip = jb(L, "flip", false);
            ImVec2 uva(flip ? 1.f : 0.f, 0), uvb(flip ? 0.f : 1.f, 0), uvc(flip ? 0.f : 1.f, 1), uvd(flip ? 1.f : 0.f, 1);
            dl->AddImageQuad((ImTextureID)(intptr_t)gt->srv, q[0], q[1], q[2], q[3], uva, uvb, uvc, uvd, IM_COL32(255, 255, 255, 80));
            dl->AddPolyline(q, 4, IM_COL32(255, 255, 255, 110), ImDrawFlags_Closed, 1.f);
        }
    }
    if (hovLayer >= 0 && !sel_contains(hovLayer) && g_giz.mode == DM_NONE) {
        Obb o = layer_obb(hovLayer);
        if (o.ok) { ImVec2 cs[4], ss[4]; obb_corners(o, cs); for (int j = 0; j < 4; j++) ss[j] = toScr(cs[j]);
                    dl->AddPolyline(ss, 4, HOVER, ImDrawFlags_Closed, 1.25f); }
    }
    for (int s : g_app.selMulti) {
        if (!layer_selectable(s)) continue;
        json& L = layers()[s];
        bool isArrow = js(L, "type", "") == "shape" && js(L, "shape", "") == "arrow";
        if (isArrow) {
            ImVec2 a = toScr(ImVec2((float)jf(L, "x1", 100), (float)jf(L, "y1", 100)));
            ImVec2 b = toScr(ImVec2((float)jf(L, "x2", 300), (float)jf(L, "y2", 300)));
            dl->AddLine(a, b, IM_COL32(0x7f, 0xd8, 0xa8, 90), 1.f);
            dl->AddCircleFilled(a, HR, HFILL); dl->AddCircle(a, HR, ACCENT, 0, 1.5f);
            dl->AddCircleFilled(b, HR, HFILL); dl->AddCircle(b, HR, ACCENT, 0, 1.5f);
            continue;
        }
        Obb o = layer_obb(s);
        if (!o.ok) continue;
        ImVec2 cs[4], ss[4]; obb_corners(o, cs);
        for (int j = 0; j < 4; j++) ss[j] = toScr(cs[j]);
        dl->AddPolyline(ss, 4, ACCENT, ImDrawFlags_Closed, 1.75f);
        if (single && s == g_app.sel && js(L, "type", "") != "watermark") {
            for (int j = 0; j < 4; j++) { dl->AddCircleFilled(ss[j], HR, HFILL); dl->AddCircle(ss[j], HR, ACCENT, 0, 1.5f); }
            std::string t = js(L, "type", "");
            bool edgeLR = t == "text" || (t == "shape" && js(L, "shape", "") == "rect") || t == "mosaic";
            bool edgeTB = (t == "shape" && js(L, "shape", "") == "rect") || t == "mosaic";
            if (edgeLR || edgeTB) {
                ImVec2 mid[4] = {ImVec2((ss[0].x + ss[3].x) / 2, (ss[0].y + ss[3].y) / 2),
                                 ImVec2((ss[1].x + ss[2].x) / 2, (ss[1].y + ss[2].y) / 2),
                                 ImVec2((ss[0].x + ss[1].x) / 2, (ss[0].y + ss[1].y) / 2),
                                 ImVec2((ss[3].x + ss[2].x) / 2, (ss[3].y + ss[2].y) / 2)};
                int nEdge = edgeTB ? 4 : 2;
                for (int j = 0; j < nEdge; j++) {
                    ImVec2 a(mid[j].x - 3.5f, mid[j].y - 3.5f), b(mid[j].x + 3.5f, mid[j].y + 3.5f);
                    dl->AddRectFilled(a, b, HFILL); dl->AddRect(a, b, ACCENT, 0, 0, 1.5f);
                }
            }
        }
    }

    // right-click context menu (teidraw parity, thumb flavored)
    if (ImGui::IsItemClicked(1)) {
        if (hovLayer >= 0 && !sel_contains(hovLayer)) select_one(hovLayer);
        if (g_app.sel >= 0) ImGui::OpenPopup("canvasctx");
    }
    if (ImGui::BeginPopup("canvasctx")) {
        auto& ls = layers();
        if (g_app.sel >= 0 && g_app.sel < (int)ls.size()) {
            json& L = ls[g_app.sel];
            std::string t = js(L, "type", "");
            if (ImGui::MenuItem("duplicate", "Ctrl+D")) {
                json c = L; c["id"] = unique_id((js(c, "id", "layer") + "_").c_str());
                ls.insert(ls.begin() + g_app.sel + 1, c); select_one(g_app.sel + 1); g_app.undoDirty = true;
            }
            if (ImGui::MenuItem("delete", "Del")) { ls.erase(ls.begin() + g_app.sel); sel_clear(); g_app.undoDirty = true; }
            ImGui::Separator();
            if (ImGui::MenuItem("bring forward", "]")) reorder_layer(g_app.sel, 1, false);
            if (ImGui::MenuItem("send backward", "[")) reorder_layer(g_app.sel, -1, false);
            if (ImGui::MenuItem("bring to front", "Shift+]")) reorder_layer(g_app.sel, 1, true);
            if (ImGui::MenuItem("send to back", "Shift+[")) reorder_layer(g_app.sel, -1, true);
            if (g_app.sel >= 0 && g_app.sel < (int)ls.size()) {
                json& L2 = ls[g_app.sel];
                if (t == "image") {
                    ImGui::Separator();
                    if (ImGui::BeginMenu("image style")) {
                        std::string cur = detect_image_style(L2);
                        if (ImGui::MenuItem("sticker (brand default)", nullptr, cur == "sticker")) apply_image_style(L2, "sticker");
                        if (ImGui::MenuItem("plain (no treatment)", nullptr, cur == "plain")) apply_image_style(L2, "plain");
                        for (auto& [k, v] : g_app.brand.image_styles.items())
                            if (k != "_comment" && v.is_object() && ImGui::MenuItem(k.c_str(), nullptr, cur == k)) apply_image_style(L2, k);
                        ImGui::EndMenu();
                    }
                    bool flip = jb(L2, "flip", false);
                    if (ImGui::MenuItem("flip", nullptr, flip)) { L2["flip"] = !flip; g_app.undoDirty = true; }
                    if (L2.contains("crop") && ImGui::MenuItem("reset crop")) { L2.erase("crop"); g_app.undoDirty = true; }
                }
                if (jf(L2, "rot", 0) != 0 && ImGui::MenuItem("reset rotation")) { L2["rot"] = 0; g_app.undoDirty = true; }
            }
        }
        ImGui::EndPopup();
    }

    // ── canvas keys (skip while typing) ──
    if (!io.WantTextInput) {
        bool sh = io.KeyShift;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket) && g_app.sel >= 0) reorder_layer(g_app.sel, -1, sh);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket) && g_app.sel >= 0) reorder_layer(g_app.sel, 1, sh);
        float step = sh ? 10.f : 1.f;
        ImVec2 nd(0, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nd.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nd.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nd.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nd.y += step;
        if ((nd.x != 0 || nd.y != 0) && !g_app.selMulti.empty() && g_giz.mode == DM_NONE) {
            for (int i : g_app.selMulti) {
                if (i < 0 || i >= (int)layers().size()) continue;
                json& L = layers()[i];
                if (js(L, "type", "") == "shape" && js(L, "shape", "") == "arrow") {
                    L["x1"] = jf(L, "x1", 0) + nd.x; L["y1"] = jf(L, "y1", 0) + nd.y;
                    L["x2"] = jf(L, "x2", 0) + nd.x; L["y2"] = jf(L, "y2", 0) + nd.y;
                } else {
                    L["x"] = jf(L, "x", cw / 2.0) + nd.x;
                    L["y"] = jf(L, "y", chh / 2.0) + nd.y;
                }
            }
            g_app.undoDirty = true;
            g_app.nudgeUndoAt = ImGui::GetTime() + 0.6;   // a nudge burst coalesces into one undo step
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_giz.mode == DM_NONE && !escCanceledDrag) sel_clear();
        if (ImGui::IsKeyPressed(ImGuiKey_A) && io.KeyCtrl) {
            g_app.selMulti.clear();
            for (int i = 0; i < (int)layers().size(); i++) if (layer_selectable(i)) g_app.selMulti.push_back(i);
            g_app.sel = g_app.selMulti.empty() ? -1 : g_app.selMulti.back();
        }
    }
    // squint-test inset: the thumb at ~channel-page display size (measured off youtube.com,
    // grid thumb ≈240-340px) so YouTube's fixed ~38x20px pill reads at its true proportion.
    if (g_app.showSquint) {
        float pw = chh > cw ? 135.f : 240.f, ph = chh > cw ? 240.f : 135.f;
        ImVec2 q0(p0.x + sz.x - pw - 10, p0.y + 10);
        dl->AddRectFilled(ImVec2(q0.x - 3, q0.y - 3), ImVec2(q0.x + pw + 3, q0.y + ph + 3), IM_COL32(20, 20, 26, 235));
        dl->AddImage((ImTextureID)(intptr_t)g_app.previewSrv, q0, ImVec2(q0.x + pw, q0.y + ph));
        // YouTube stamps a ~fixed 20px duration pill bottom-right on long-form (not Shorts) —
        // at feed size it eats a big fraction, so draw it here to gauge face<->pill collisions.
        // Preview-only overlay (not in render_doc → exports stay byte-identical).
        if (cw >= chh) {
            std::string dur = g_app.doc.value("preview_dur", std::string("12:00"));
            ImVec2 ts = ImGui::CalcTextSize(dur.c_str());
            float padx = 4.f, pady = 1.f, m = 8.f;              // measured: 8px margin, 4px pad, radius 4, bg rgba(0,0,0,.6)
            ImVec2 b1(q0.x + pw - m, q0.y + ph - m);
            ImVec2 b0(b1.x - (ts.x + 2 * padx), b1.y - (ts.y + 2 * pady));
            dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 153), 4.f);
            dl->AddText(ImVec2(b0.x + padx, b0.y + pady), IM_COL32(255, 255, 255, 235), dur.c_str());
            float pbh = 3.f;                                     // red resume progress bar (partially-watched videos)
            dl->AddRectFilled(ImVec2(q0.x, q0.y + ph - pbh), ImVec2(q0.x + pw, q0.y + ph), IM_COL32(255, 255, 255, 70));
            dl->AddRectFilled(ImVec2(q0.x, q0.y + ph - pbh), ImVec2(q0.x + pw * 0.45f, q0.y + ph), IM_COL32(255, 0, 0, 235));
        }
        dl->AddRect(ImVec2(q0.x - 3, q0.y - 3), ImVec2(q0.x + pw + 3, q0.y + ph + 3), IM_COL32(120, 120, 140, 255));
        dl->AddText(ImVec2(q0.x, q0.y + ph + 6), IM_COL32(150, 150, 160, 255), "channel feed size");
    }
}

static void panel_toolbar() {
    if (ImGui::Button("Open..")) {
        char file[1024] = "";
        OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = "thumb docs (*.thumb.json)\0*.thumb.json\0\0";
        ofn.lpstrFile = file; ofn.nMaxFile = sizeof file; ofn.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) load_doc(file);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save") ) save_doc();
    ImGui::SameLine();
    if (ImGui::Button("Export PNG")) {
        char file[1024] = "";
        std::string suggested = doc_stem() + ".png";
        strncpy(file, suggested.c_str(), sizeof file - 1);
        OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = "PNG\0*.png\0\0"; ofn.lpstrDefExt = "png";
        ofn.lpstrFile = file; ofn.nMaxFile = sizeof file; ofn.Flags = OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameA(&ofn)) {
            Img full; std::string e;
            render_doc(g_app.doc, g_app.brand, g_app.docDir, 2, full, nullptr, &e);
            write_png(full, file);
            g_app.status = std::string("exported ") + file;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Snapshot")) take_snapshot();
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("Undo") || (ImGui::IsKeyPressed(ImGuiKey_Z) && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)) do_undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo") || (ImGui::IsKeyPressed(ImGuiKey_Y) && ImGui::GetIO().KeyCtrl)
        || (ImGui::IsKeyPressed(ImGuiKey_Z) && ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift)) do_redo();
    ImGui::SameLine(); ImGui::TextDisabled("(%d)", (int)g_app.undo.size());
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    int cw = canvas_w(), chh = canvas_h();
    bool portrait = chh > cw;
    if (ImGui::Button(portrait ? "9:16" : "16:9")) {
        g_app.doc["canvas"] = portrait ? json{1280, 720} : json{1080, 1920};
        g_app.undoDirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("toggle landscape (1280x720) / Shorts cover (1080x1920)");
    ImGui::SameLine();
    ImGui::Checkbox("squint", &g_app.showSquint);
    ImGui::SameLine();
    ImGui::TextDisabled("brand: %s%s", g_app.brand.name.c_str(), g_app.brand.ok ? "" : " (missing)");
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", g_app.perf.c_str());
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "%s", g_app.status.c_str());
    if (!g_app.renderErr.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "! %s", g_app.renderErr.c_str());
    }
    if (g_app.conflict) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "file changed on disk (external edit) —");
        ImGui::SameLine();
        if (ImGui::SmallButton("reload theirs")) { load_doc(g_app.docPath); g_app.conflict = false; }
        ImGui::SameLine();
        if (ImGui::SmallButton("keep mine")) g_app.conflict = false;
    }
    // global shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_S) && ImGui::GetIO().KeyCtrl) save_doc();
    if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
        && !g_app.selMulti.empty() && !ImGui::GetIO().WantTextInput) {
        std::vector<int> idx = g_app.selMulti;
        std::sort(idx.rbegin(), idx.rend());
        for (int i : idx) if (i >= 0 && i < (int)layers().size()) layers().erase(layers().begin() + i);
        sel_clear(); g_app.undoDirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_D) && ImGui::GetIO().KeyCtrl && g_app.sel >= 0 && g_app.sel < (int)layers().size()) {
        json c = layers()[g_app.sel]; c["id"] = unique_id((js(c, "id", "layer") + "_").c_str());
        layers().insert(layers().begin() + g_app.sel + 1, c); select_one(g_app.sel + 1); g_app.undoDirty = true;
    }
}

// ───────────────────────────── headless paths ───────────────────────────────
static int headless_export(const std::string& docPath, const std::string& brandOverride,
                           const std::string& outPng, const std::string& proofPng,
                           const std::string& infoJson, int ssFactor) {
    std::string txt = read_text_file(host_path(docPath));
    if (txt.empty()) { fprintf(stderr, "cannot read %s\n", docPath.c_str()); return 1; }
    json doc = json::parse(txt, nullptr, false);
    if (doc.is_discarded()) { fprintf(stderr, "parse error in %s\n", docPath.c_str()); return 1; }
    std::string docDir = path_dir(docPath);
    std::string b = !brandOverride.empty() ? brandOverride : js(doc, "brand", "");
    if (b.empty()) b = find_brand_package(docDir);   // auto-load the gemma brand if present
    Brand brand = b.empty() ? Brand() : load_brand(path_join(docDir, b));
    if (!b.empty() && !brand.ok) fprintf(stderr, "warning: %s\n", brand.err.c_str());

    Img full; std::vector<LayerInfo> info; std::string err;
    render_doc(doc, brand, docDir, ssFactor, full, &info, &err);
    if (!err.empty()) fprintf(stderr, "render warnings:\n%s", err.c_str());
    if (!outPng.empty()) {
        if (!write_png(full, outPng)) { fprintf(stderr, "cannot write %s\n", outPng.c_str()); return 1; }
        printf("wrote %s (%dx%d)\n", outPng.c_str(), full.w, full.h);
    }
    if (!proofPng.empty()) {
        Img proof; make_proof(full, proof);
        write_png(proof, proofPng);
        printf("wrote %s (%dx%d)\n", proofPng.c_str(), proof.w, proof.h);
    }
    if (!infoJson.empty()) {
        json out = {{"canvas", {full.w, full.h}}, {"title", js(doc, "title", "")}, {"brand", brand.name}, {"layers", json::array()}};
        size_t li = 0;
        for (const auto& L : doc["layers"]) {
            json e = {{"id", js(L, "id", "")}, {"type", js(L, "type", "")}, {"hidden", jb(L, "hidden", false)}};
            if (li < info.size()) {
                e["bbox"] = {info[li].x0, info[li].y0, info[li].x1, info[li].y1};
                e["content"] = {info[li].px0, info[li].py0, info[li].px1, info[li].py1};
            }
            if (js(L, "type", "") == "text") e["text"] = js(L, "text", "");
            out["layers"].push_back(e);
            li++;
        }
        std::ofstream f(host_path(infoJson), std::ios::binary);
        f << out.dump(2) << "\n";
        printf("wrote %s\n", infoJson.c_str());
    }
    return 0;
}

// backbuffer → png (for --shot self-verification)
static bool capture_backbuffer(const std::string& path) {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return false;
    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st)) || !st) { back->Release(); return false; }
    g_ctx->CopyResource(st, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &map))) { st->Release(); return false; }
    Img img; img.alloc(d.Width, d.Height);
    for (UINT y = 0; y < d.Height; y++) {
        const uint8_t* src = (const uint8_t*)map.pData + y * map.RowPitch;
        memcpy(img.at(0, y), src, (size_t)d.Width * 4);
    }
    for (size_t i = 0; i < img.px.size(); i += 4) img.px[i + 3] = 255;
    g_ctx->Unmap(st, 0);
    st->Release();
    return write_png(img, path);
}

// ───────────────────────────── chrome theme ─────────────────────────────────
// Same chrome as the video editor (editor/src/main.cpp apply_editor_theme): the
// cosmic2d palette (deep-purple base, mint accent, periwinkle focus) + rounded
// metrics. CHROME ONLY — thumbnail content rendering is untouched.
static void apply_editor_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    auto C = [](unsigned rgba){ return ImVec4(((rgba>>24)&255)/255.f, ((rgba>>16)&255)/255.f, ((rgba>>8)&255)/255.f, (rgba&255)/255.f); };
    const ImVec4 bg=C(0x141220ffu), panel=C(0x1e1b2effu), panel2=C(0x262238ffu), panel3=C(0x322d48ffu),
                 edge=C(0x3a3560ffu), edgeHot=C(0x6a60a0ffu), active=C(0x4a4370ffu), accent=C(0x7fd8a8ffu),
                 accentHot=C(0x9fe8c0ffu), focus=C(0x8878d0ffu), text=C(0xe8e4ffffu), dim=C(0x8a84b0ffu);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]=text;                    c[ImGuiCol_TextDisabled]=dim;
    c[ImGuiCol_WindowBg]=panel;               c[ImGuiCol_ChildBg]=C(0x191527ffu);       c[ImGuiCol_PopupBg]=C(0x1e1b2ef7u);
    c[ImGuiCol_Border]=edge;                  c[ImGuiCol_BorderShadow]=ImVec4(0,0,0,0);
    c[ImGuiCol_FrameBg]=panel2;               c[ImGuiCol_FrameBgHovered]=panel3;        c[ImGuiCol_FrameBgActive]=active;
    c[ImGuiCol_TitleBg]=bg;                   c[ImGuiCol_TitleBgActive]=panel2;         c[ImGuiCol_TitleBgCollapsed]=bg;
    c[ImGuiCol_MenuBarBg]=C(0x181528ffu);
    c[ImGuiCol_ScrollbarBg]=ImVec4(0,0,0,0);  c[ImGuiCol_ScrollbarGrab]=panel3;         c[ImGuiCol_ScrollbarGrabHovered]=edgeHot; c[ImGuiCol_ScrollbarGrabActive]=focus;
    c[ImGuiCol_CheckMark]=accent;             c[ImGuiCol_SliderGrab]=accent;            c[ImGuiCol_SliderGrabActive]=accentHot;
    c[ImGuiCol_Button]=panel2;                c[ImGuiCol_ButtonHovered]=panel3;         c[ImGuiCol_ButtonActive]=active;
    c[ImGuiCol_Header]=panel3;                c[ImGuiCol_HeaderHovered]=edgeHot;        c[ImGuiCol_HeaderActive]=focus;
    c[ImGuiCol_Separator]=edge;               c[ImGuiCol_SeparatorHovered]=edgeHot;     c[ImGuiCol_SeparatorActive]=focus;
    c[ImGuiCol_ResizeGrip]=panel3;            c[ImGuiCol_ResizeGripHovered]=edgeHot;    c[ImGuiCol_ResizeGripActive]=focus;
    c[ImGuiCol_Tab]=panel;                    c[ImGuiCol_TabHovered]=panel3;            c[ImGuiCol_TabSelected]=panel3;
    c[ImGuiCol_TabDimmed]=bg;                 c[ImGuiCol_TabDimmedSelected]=panel2;
    c[ImGuiCol_TabSelectedOverline]=accent;   c[ImGuiCol_TabDimmedSelectedOverline]=edge;
    c[ImGuiCol_PlotLines]=accent;             c[ImGuiCol_PlotLinesHovered]=accentHot;   c[ImGuiCol_PlotHistogram]=accent;
    c[ImGuiCol_TextSelectedBg]=C(0x7fd8a855u); c[ImGuiCol_DragDropTarget]=accent;       c[ImGuiCol_NavCursor]=focus;
    s.WindowRounding=8; s.ChildRounding=6; s.FrameRounding=6; s.PopupRounding=6; s.GrabRounding=5; s.TabRounding=6; s.ScrollbarRounding=8;
    s.WindowBorderSize=1; s.ChildBorderSize=1; s.PopupBorderSize=1; s.FrameBorderSize=0; s.SeparatorTextBorderSize=2;
    s.WindowPadding=ImVec2(10,10); s.FramePadding=ImVec2(8,4); s.CellPadding=ImVec2(6,4);
    s.ItemSpacing=ImVec2(8,6); s.ItemInnerSpacing=ImVec2(6,5); s.IndentSpacing=18;
    s.ScrollbarSize=13; s.GrabMinSize=11;
}

// ───────────────────────────── main ─────────────────────────────────────────
int main(int argc, char** argv) {
    std::string docPath, exportPng, proofPng, infoJson, shotPng, brandDir, driveScript;
    int ssFactor = 2, shotFrames = 5;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--export") exportPng = next();
        else if (a == "--proof") proofPng = next();
        else if (a == "--info") infoJson = next();
        else if (a == "--shot") shotPng = next();
        else if (a == "--frames") shotFrames = atoi(next().c_str());
        else if (a == "--brand") brandDir = next();
        else if (a == "--drive") driveScript = next();
        else if (a == "--ss") ssFactor = std::max(1, atoi(next().c_str()));
        else if (a[0] != '-') docPath = a;
    }
    if (!exportPng.empty() || !infoJson.empty() || (!proofPng.empty() && shotPng.empty())) {
        if (docPath.empty()) { fprintf(stderr, "usage: slopthumb doc.thumb.json --export out.png [--proof p.png] [--info i.json] [--brand dir] [--ss N]\n"); return 1; }
        return headless_export(docPath, brandDir, exportPng, proofPng, infoJson, ssFactor);
    }

    // ── GUI ──
    SetProcessDPIAware();
    WNDCLASSEXW wc = { sizeof wc, CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"slopthumb", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(L"slopthumb", L"slopthumb — thumbnail tool", WS_OVERLAPPEDWINDOW,
                              80, 60, 1660, 980, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx))) {
        fprintf(stderr, "D3D11 init failed\n"); return 1;
    }
    create_rtv();
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    DragAcceptFiles(hwnd, TRUE);   // accept image files dragged from the OS

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    apply_editor_theme();          // same chrome as the video editor
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const char* uiFonts[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\tahoma.ttf" };
    for (const char* f : uiFonts) { FILE* t = fopen(f, "rb"); if (t) { fclose(t); io.Fonts->AddFontFromFileTTF(f, 17.0f); break; } }
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    if (!g_gpu.init()) fprintf(stderr, "GPU compositor init failed -- preview falls back to CPU\n");

    g_app.brandOverride = brandDir;
    if (!docPath.empty()) load_doc(docPath);
    else {  // blank starter doc so the tool is never empty
        g_app.doc = {{"format", "thumb-1"}, {"canvas", {1280, 720}},
                     {"layers", {{{"id", "bg"}, {"type", "bg"}, {"fill", "#141021"}, {"grad_to", "#2a1140"}, {"vignette", 0.35}}}}};
        g_app.undoBase = g_app.doc.dump(); g_app.savedDump = g_app.undoBase;
    }
    if (!shotPng.empty() && g_app.doc.contains("layers")) {   // --shot: select the topmost layer so the gizmo shows
        auto& ls = g_app.doc["layers"];
        for (int i = (int)ls.size() - 1; i >= 0; i--)
            if (ls[i].is_object() && js(ls[i], "type", "") != "bg" && !jb(ls[i], "hidden", false)) { select_one(i); break; }
    }

    int frame = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // --drive: scripted synthetic input through the REAL event queue (pushed after the
        // Win32 backend so scripted state wins the frame). One op per frame; ';'-separated:
        //   wait:N  move:x,y  down  up  rdown  rup  wheel:d  key:[ctrl+][shift+]Name
        //   shot:file.png  quit      (Name = ImGui key name: Z, Escape, LeftBracket, ...)
        if (!driveScript.empty()) {
            static std::vector<std::string> ops; static size_t opi = 0; static int waitN = 30;
            static std::vector<ImGuiKey> keyUps;   // released one frame after press
            static bool parsed = false;
            if (!parsed) {
                parsed = true;
                std::stringstream ss(driveScript); std::string tok;
                while (std::getline(ss, tok, ';')) if (!tok.empty()) ops.push_back(tok);
            }
            for (ImGuiKey k : keyUps) io.AddKeyEvent(k, false);
            keyUps.clear();
            // re-assert the scripted cursor every frame — the Win32 backend pushes the REAL
            // cursor position on frames the mouse isn't tracked, which would yank a drag
            static ImVec2 dmPos(-1, -1);
            if (dmPos.x >= 0) io.AddMousePosEvent(dmPos.x, dmPos.y);
            if (waitN > 0) waitN--;
            else while (opi < ops.size()) {
                std::string op = ops[opi++];
                size_t c = op.find(':');
                std::string cmd = c == std::string::npos ? op : op.substr(0, c);
                std::string arg = c == std::string::npos ? "" : op.substr(c + 1);
                if (cmd == "wait") { waitN = atoi(arg.c_str()); break; }
                if (cmd == "move") { sscanf(arg.c_str(), "%f,%f", &dmPos.x, &dmPos.y); io.AddMousePosEvent(dmPos.x, dmPos.y); break; }
                if (cmd == "lmove") {   // LOGICAL canvas coords → screen via the live canvas transform
                    float x = 0, y = 0; sscanf(arg.c_str(), "%f,%f", &x, &y);
                    dmPos = ImVec2(g_canvasP0.x + x * g_canvasSc, g_canvasP0.y + y * g_canvasSc);
                    io.AddMousePosEvent(dmPos.x, dmPos.y); break;
                }
                if (cmd == "down") { io.AddMouseButtonEvent(0, true); break; }
                if (cmd == "up")   { io.AddMouseButtonEvent(0, false); break; }
                if (cmd == "rdown") { io.AddMouseButtonEvent(1, true); break; }
                if (cmd == "rup")   { io.AddMouseButtonEvent(1, false); break; }
                if (cmd == "wheel") { io.AddMouseWheelEvent(0, (float)atof(arg.c_str())); break; }
                if (cmd == "key") {
                    bool ctrl = arg.rfind("ctrl+", 0) == 0; if (ctrl) arg = arg.substr(5);
                    bool shift = arg.rfind("shift+", 0) == 0; if (shift) arg = arg.substr(6);
                    ImGuiKey key = ImGuiKey_None;
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
                        if (!strcmp(ImGui::GetKeyName((ImGuiKey)k), arg.c_str())) { key = (ImGuiKey)k; break; }
                    if (ctrl) { io.AddKeyEvent(ImGuiMod_Ctrl, true); keyUps.push_back(ImGuiMod_Ctrl); }
                    if (shift) { io.AddKeyEvent(ImGuiMod_Shift, true); keyUps.push_back(ImGuiMod_Shift); }
                    if (key != ImGuiKey_None) { io.AddKeyEvent(key, true); keyUps.push_back(key); }
                    break;
                }
                if (cmd == "drop") {   // drop:lx,ly,path — synth an OS file drop at logical canvas coords
                    float x = 0, y = 0; char pbuf[1024] = {0};
                    sscanf(arg.c_str(), "%f,%f,%1023[^;]", &x, &y, pbuf);
                    g_dropPt.x = (LONG)(g_canvasP0.x + x * g_canvasSc); g_dropPt.y = (LONG)(g_canvasP0.y + y * g_canvasSc);
                    g_dropFiles.push_back(pbuf); g_hasDrop = true; break;
                }
                if (cmd == "ctrl") { io.AddKeyEvent(ImGuiMod_Ctrl, arg == "1"); break; }
                if (cmd == "shift") { io.AddKeyEvent(ImGuiMod_Shift, arg == "1"); break; }
                if (cmd == "shot") { capture_backbuffer(arg); printf("wrote %s\n", arg.c_str()); fflush(stdout); continue; }
                if (cmd == "quit") { done = true; break; }
            }
            if (opi >= ops.size() && waitN <= 0 && keyUps.empty() && !done) done = true;   // script exhausted
            if (done) { PostQuitMessage(0); }
        }
        ImGui::NewFrame();
        watch_tick(io.DeltaTime);

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
        panel_toolbar();
        ImGui::Separator();

        float leftW = 300, rightW = 360;
        ImGui::BeginChild("left", ImVec2(leftW, 0), ImGuiChildFlags_None);
        panel_layers();
        ImGui::Separator();
        panel_history();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("center", ImVec2(ImGui::GetContentRegionAvail().x - rightW - 8, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        panel_canvas();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(rightW, 0), ImGuiChildFlags_None);
        ImGui::TextDisabled("INSPECTOR");
        panel_inspector();
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Sprites (brand library)", ImGuiTreeNodeFlags_DefaultOpen))
            panel_sprites();
        ImGui::EndChild();

        ImGui::End();
        undo_settle_check();

        ImGui::Render();
        const float clear[4] = { 0.05f, 0.05f, 0.07f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);

        frame++;
        if (!shotPng.empty() && frame >= shotFrames) {
            capture_backbuffer(shotPng);
            printf("wrote %s\n", shotPng.c_str());
            break;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(L"slopthumb", wc.hInstance);
    return 0;
}
