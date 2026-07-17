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
    int sel = -1;
    std::string perf;                             // "build 3ms · composite 1ms (GPU)"

    // undo (editor pattern: compact-dump snapshots at gesture settle)
    std::deque<std::string> undo, redo;
    std::string undoBase;                // committed doc state
    std::string savedDump;               // last state loaded-from/saved-to disk
    bool undoDirty = false, activePrev = false;

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
    auto it = g_app.thumbCache.find(path);
    if (it != g_app.thumbCache.end()) return it->second.srv ? &it->second : nullptr;
    TexEntry& t = g_app.thumbCache[path];
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
    g_app.sel = -1;
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

static void undo_settle_check() {
    bool active = ImGui::IsAnyItemActive();
    if ((g_app.activePrev && !active) || g_app.undoDirty) {
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
    g_app.activePrev = active;
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
    if (select) g_app.sel = (int)layers().size() - 1;
    g_app.undoDirty = true;
}
static int canvas_w() { auto& d = g_app.doc; return d.contains("canvas") && d["canvas"].is_array() ? d["canvas"][0].get<int>() : 1280; }
static int canvas_h() { auto& d = g_app.doc; return d.contains("canvas") && d["canvas"].is_array() ? d["canvas"][1].get<int>() : 720; }

// Stamp the brand package's reusable image treatment onto a layer — the one-click "set it up the
// branded way (tilt + border + glow)" the owner wanted. Reads brand.image_styles[style] so the look
// stays in sync with the package (falls back to a sane literal); writes LITERAL rot/outline_px/glow
// onto the layer so they stay visible + tweakable in the inspector afterwards. The white sticker
// border + drop shadow already come free from brand.sticker.
static void brand_image(json& L, const char* style = "card") {
    const json& styles = g_app.brand.image_styles;
    json c = styles.contains(style) ? styles[style] : json::object();
    L["rot"]        = jf(c, "rot", -2.0);
    L["outline_px"] = jf(c, "outline_px", 10.0);
    L["glow"]       = (c.contains("glow") && c["glow"].is_object()) ? c["glow"]
                                                                    : json{{"px", 30}, {"color", "$gold"}, {"alpha", 0.4}};
    g_app.undoDirty = true;
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
        if (ImGui::Selectable(label.c_str(), g_app.sel == i)) g_app.sel = i;
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
        if (j >= 0 && j < (int)ls.size()) { std::swap(ls[move], ls[j]); g_app.sel = j; g_app.undoDirty = true; }
    }
    if (dup >= 0) {
        json c = ls[dup]; c["id"] = unique_id((js(c, "id", "layer") + "_").c_str());
        ls.insert(ls.begin() + dup + 1, c); g_app.sel = dup + 1; g_app.undoDirty = true;
    }
    if (del >= 0) { ls.erase(ls.begin() + del); g_app.sel = -1; g_app.undoDirty = true; }

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
    if (ImGui::SmallButton("image\xE2\x98\x85")) {   // add an image ALREADY branded (tilt + border + glow) — one action
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
        ch |= num_field("x", L, "x", canvas_w() / 2.0); ImGui::SameLine(); ch |= num_field("y", L, "y", canvas_h() / 2.0);
        ch |= num_field("scale", L, "scale", 1.0, 0.005f, 0.02, 6);
        ch |= num_field("rot", L, "rot", 0, 0.2f, -180, 180);
        bool flip = jb(L, "flip", false);
        if (ImGui::Checkbox("flip", &flip)) { L["flip"] = flip; ch = true; }
        ch |= num_field("opacity", L, "opacity", 1, 0.01f, 0, 1);
        ch |= num_field("outline_px", L, "outline_px", jf(g_app.brand.sticker, "outline_px", 0), 0.2f, 0, 60);
        ch |= color_field("outline", L, "outline", js(g_app.brand.sticker, "outline", "#ffffff"));
        ch |= fx_section(L);
        if (ImGui::Button("\xE2\x98\x85 brand it (tilt + border + glow)")) { brand_image(L); ch = true; }   // one-click branded setup
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("stamp the brand '%s' card treatment onto this image: a slight tilt, the white sticker border,\nand a gold accent glow. Tweak rot / outline / glow above afterwards; change glow color for a different accent.", g_app.brand.name.c_str());
    } else if (type == "text") {
        std::string text = js(L, "text", "");
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

// canvas: preview + selection + drag-to-move + squint inset
static void panel_canvas() {
    ensure_preview();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!g_app.previewSrv || avail.x < 32 || avail.y < 32) return;
    float cw = (float)canvas_w(), chh = (float)canvas_h();
    float sc = std::min((avail.x - 8) / cw, (avail.y - 8) / chh);
    ImVec2 sz(cw * sc, chh * sc);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.x += (avail.x - sz.x) / 2; p0.y += (avail.y - sz.y) / 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p0.x + sz.x + 1, p0.y + sz.y + 1), IM_COL32(90, 90, 110, 255));
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##canvas", sz, ImGuiButtonFlags_MouseButtonLeft);
    dl->AddImage((ImTextureID)(intptr_t)g_app.previewSrv, p0, ImVec2(p0.x + sz.x, p0.y + sz.y));

    ImVec2 m = ImGui::GetIO().MousePos;
    float lx = (m.x - p0.x) / sc, ly = (m.y - p0.y) / sc;   // logical coords

    // click select — sticky: if the click lands on the CURRENTLY selected layer,
    // keep it selected even when other layers sit above (makes overlapped layers
    // editable intuitively). Only with no selected layer under the cursor does it
    // fall through to the topmost hit. bg never hit-tests.
    if (ImGui::IsItemClicked(0)) {
        auto& ls = layers();
        auto hits = [&](int i) {
            if (i < 0 || i >= (int)g_app.layerInfo.size() || i >= (int)ls.size()) return false;
            const LayerInfo& li = g_app.layerInfo[i];
            if (li.type == "bg" || li.type.empty()) return false;
            if (jb(ls[i], "hidden", false)) return false;
            return lx >= li.x0 && lx <= li.x1 && ly >= li.y0 && ly <= li.y1;
        };
        if (!hits(g_app.sel)) {
            int hit = -1;
            for (int i = (int)g_app.layerInfo.size() - 1; i >= 0; i--)
                if (hits(i)) { hit = i; break; }
            if (hit >= 0) g_app.sel = hit;
        }
    }
    // drag move
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0) && g_app.sel >= 0 && g_app.sel < (int)layers().size()) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        if (d.x != 0 || d.y != 0) {
            json& L = layers()[g_app.sel];
            float dx = d.x / sc, dy = d.y / sc;
            if (js(L, "type", "") == "shape" && js(L, "shape", "") == "arrow") {
                L["x1"] = jf(L, "x1", 0) + dx; L["y1"] = jf(L, "y1", 0) + dy;
                L["x2"] = jf(L, "x2", 0) + dx; L["y2"] = jf(L, "y2", 0) + dy;
            } else {
                L["x"] = jf(L, "x", canvas_w() / 2.0) + dx;
                L["y"] = jf(L, "y", canvas_h() / 2.0) + dy;
            }
            g_app.undoDirty = true;   // settle happens on mouse release (item deactivates)
        }
    }
    // ctrl+wheel scale on hover
    if (ImGui::IsItemHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0 && g_app.sel >= 0 && g_app.sel < (int)layers().size()) {
        json& L = layers()[g_app.sel];
        float f = ImGui::GetIO().MouseWheel > 0 ? 1.05f : 1 / 1.05f;
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
    // selection bbox
    if (g_app.sel >= 0 && g_app.sel < (int)g_app.layerInfo.size()) {
        const LayerInfo& li = g_app.layerInfo[g_app.sel];
        if (li.x1 > li.x0)
            dl->AddRect(ImVec2(p0.x + li.x0 * sc, p0.y + li.y0 * sc), ImVec2(p0.x + li.x1 * sc, p0.y + li.y1 * sc),
                        IM_COL32(255, 200, 60, 200), 0, 0, 1.5f);
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
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && g_app.sel >= 0 && g_app.sel < (int)layers().size() && !ImGui::GetIO().WantTextInput) {
        layers().erase(layers().begin() + g_app.sel); g_app.sel = -1; g_app.undoDirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_D) && ImGui::GetIO().KeyCtrl && g_app.sel >= 0 && g_app.sel < (int)layers().size()) {
        json c = layers()[g_app.sel]; c["id"] = unique_id((js(c, "id", "layer") + "_").c_str());
        layers().insert(layers().begin() + g_app.sel + 1, c); g_app.sel++; g_app.undoDirty = true;
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
            if (li < info.size()) e["bbox"] = {info[li].x0, info[li].y0, info[li].x1, info[li].y1};
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

// ───────────────────────────── main ─────────────────────────────────────────
int main(int argc, char** argv) {
    std::string docPath, exportPng, proofPng, infoJson, shotPng, brandDir;
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 1);
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
