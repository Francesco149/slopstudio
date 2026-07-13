// slopstudio editor — D3D11 + Dear ImGui NLE: timeline + live compositing preview.
//
// Cross-compiled to a Win64 PE (mingw-w64) and run on Windows via WSLInterop, so the
// interactive UI is native (no WSLg tax). The compositing layer (docs/ARCHITECTURE.md):
// it loads a .slop.json project, renders the timeline of pipeline rows, and composites the
// visual clips active at the playhead into a live preview (textures with per-clip
// transform/opacity) — the instant, GPU-side interaction that is the whole point.
// Generation stays async/cached behind providers; the editor only composites their assets.
//
// Modes:
//   slopstudio.exe [project.slop.json] [--cache DIR] [--time SEC]
//   slopstudio.exe project.slop.json --shot out.png [--frames N] [--time SEC]
//
#include <windows.h>
#include <commdlg.h>   // GetOpenFileNameW — image file picker
#include <shellapi.h>  // WM_DROPFILES — drag images onto the timeline
#include <d3d11.h>
#include <dxgi.h>
#include <winhttp.h>
#include <io.h>
#include <fcntl.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "imgui_stdlib.h"  // ImGui::InputText(label, std::string*) — for long, growable text (code editor)

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// ordered_json: object key order is preserved on parse→patch→dump, so writing a
// project back to disk (generate-on-demand lands a new asset entry) keeps the
// hand-authored field order → clean git diffs, as docs/PROJECT_FORMAT.md promises.
using json = nlohmann::ordered_json;

// ───────────────────────────── project model ──────────────────────────────
// One keyframe on a dotted param path (docs/PROJECT_FORMAT §clips). `t` is TIMELINE seconds
// (absolute), matching the authored examples. `interp` of a keyframe drives the segment that
// LEAVES it: linear · constant · bezier (in/out handles) · spring (critically-/under-damped).
struct KF {
    double t = 0;
    std::vector<double> v;             // scalar → size 1; vec2 (pos/scale/anchor) → size 2
    std::string interp = "linear";
    double stiffness = 300, damping = 24;
    double outh[2] = {0.42, 0.0}, inh[2] = {0.58, 1.0};  // bezier handles (default = ease-in-out)
};
struct Clip {
    std::string id, row, type, label, asset;
    double start = 0, dur = 0;
    double tx_pos[2] = {0, 0}, tx_scale[2] = {1, 1}, tx_rot = 0, tx_opacity = 1, tx_anchor[2] = {0.5, 0.5};
    json params;
    std::map<std::string, std::vector<KF>> keyframes;  // path → ordered keyframes (any numeric param)
};
struct Row {
    std::string id, type, name;
    std::vector<std::string> clips;
    json params;  // row-level generation defaults (e.g. voice_preset, model, lora)
};
struct Track {
    std::string id, name, kind;
    std::vector<std::string> rows;
};
// A `video` asset's decoded PROXY: a dir of decimated JPEG frames (tools/video-proxy.py) the
// compositor samples at the playhead. The editor never decodes — it just maps local time →
// frame index → texture (clip+time → SRV). proxy resolves like any uri (cache://… → --cache).
struct VideoMeta {
    std::string src;            // source media uri (mp4/…) for IN-PROCESS libav decode (preferred)
    std::string proxy;          // decimated-JPEG proxy dir uri (fallback when no libav / can't open src)
    double fps = 0;             // frame rate (proxy's, or filled from the source on first decode)
    int frames = 0;            // frame count (ditto)
    int w = 0, h = 0;          // frame dims (drawn size before transform)
};
struct Project {
    std::string path, title, error;
    int fps = 60;
    double width = 1920, height = 1080;
    double vignette = 0;  // scene-level edge vignette 0..1 (unify bg + host); meta.vignette
    // project-global settings (meta; edited in the docked Project panel). `format` picks the
    // DEFAULT bundle — "1080p" = the locked full-length-video conventions (built-in SFX off,
    // speech 1.0x), "portrait" = shorts (1080x1920 canvas, SFX on, ~1.3x speech). Explicit
    // meta.sfx / meta.speech_rate / meta.resolution always win over the format's defaults.
    std::string format = "1080p";
    bool sfx = false;        // built-in transition SFX one-shots (meta.sfx)
    double masterGainDb = 0; // final-mix gain dB, applied AFTER per-clip loudness normalize (meta.gain_db)
    double speechRate = 1.0; // default tts playback `rate` when a clip has none (meta.speech_rate)
    double speechGainDb = 12.0; // GLOBAL boost added to every tts (speech) clip, ON TOP of its normalize +
                                // per-clip/lane gain (so relative dynamics are unchanged) (meta.speech_gain_db)
    bool songCredits = true;    // auto "now playing" chip (♪ title — artist) at each song's start (meta.song_credits)
    double songCreditSecs = 10; // how long the chip holds before fading (meta.song_credit_secs)
    std::string songCreditCorner = "tl";  // tl/tr/bl/br — where the chip sits (meta.song_credit_corner)
    // per-project tunable position anchors (meta.anchors): category key → base [x,y]. A clip that
    // names one (params.anchor, e.g. "bust"/"code_host"/"tr_room") renders at anchor + transform.pos,
    // so its pos is an OFFSET and one Project-panel knob nudges the whole category in THIS project
    // only (owner idea 2026-07-05). Clips without the param keep absolute pos — full back-compat.
    std::map<std::string, std::array<double, 2>> anchors;
    std::vector<Track> tracks;
    std::map<std::string, Row> rows;
    std::map<std::string, Clip> clips;
    std::map<std::string, std::string> asset_uri;  // asset key → uri (from the assets map)
    std::map<std::string, VideoMeta> asset_video;  // asset key → proxy meta (video assets only)
    json doc;  // the raw parsed document, kept so generate-on-demand can patch + persist it
    bool ok = false;

    double duration() const {
        double d = 0;
        for (auto& kv : clips) d = std::max(d, kv.second.start + kv.second.dur);
        return d > 0 ? d : 10.0;
    }
};

static std::string snippet(const std::string& s, size_t n = 30) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

static void parse_xform(const json& tr, Clip& c) {
    if (!tr.is_object()) return;
    if (tr.contains("pos") && tr["pos"].is_array() && tr["pos"].size() == 2) {
        c.tx_pos[0] = tr["pos"][0].get<double>();
        c.tx_pos[1] = tr["pos"][1].get<double>();
    }
    if (tr.contains("scale") && tr["scale"].is_array() && tr["scale"].size() == 2) {
        c.tx_scale[0] = tr["scale"][0].get<double>();
        c.tx_scale[1] = tr["scale"][1].get<double>();
    }
    c.tx_rot = tr.value("rot", 0.0);
    c.tx_opacity = tr.value("opacity", 1.0);
    if (tr.contains("anchor") && tr["anchor"].is_array() && tr["anchor"].size() == 2) {
        c.tx_anchor[0] = tr["anchor"][0].get<double>();
        c.tx_anchor[1] = tr["anchor"][1].get<double>();
    }
}

// Build the in-memory Project model from an already-parsed JSON doc (no disk I/O). Split out of
// load_project so a STRUCTURAL edit (split/delete) can rebuild the model from the patched doc
// WITHOUT a write+reread — a reread reloads the on-disk file, clobbering unsaved in-memory edits
// (timeline moves/trims/param tweaks). Pair with sync_to_doc to fold those edits in first.
// Dirs the open project's asset files live in — scanned into the Media pane as project items
// (filled at parse; the periodic library rescan picks changes up).
static std::vector<std::string> g_projAssetDirs;
static std::string g_projDir;        // dir of the loaded .slop.json (set at startup; project-relative uri root)
static std::string g_repoRoot;       // code-repo root (holds library/, cache/, presets/); CWD-independent

static Project parse_project_json(json j, const std::string& path) {
    Project p;
    p.path = path;
    try {
        json meta = j.value("meta", json::object());
        p.title = meta.value("title", std::string("untitled"));
        p.fps = meta.value("fps", 60);
        p.vignette = meta.value("vignette", 0.0);  // scene vignette, drawn after the composite
        p.format = meta.value("format", std::string("1080p"));
        bool portrait = (p.format == "portrait");
        if (portrait) { p.width = 1080; p.height = 1920; }        // format default; explicit resolution wins
        if (meta.contains("resolution") && meta["resolution"].is_array() && meta["resolution"].size() == 2) {
            p.width = meta["resolution"][0].get<double>();
            p.height = meta["resolution"][1].get<double>();
        }
        p.sfx = meta.value("sfx", portrait);                      // 1080p: built-in SFX OFF by default
        p.masterGainDb = meta.value("gain_db", 0.0);
        p.speechRate = meta.value("speech_rate", portrait ? 1.3 : 1.0);
        p.speechGainDb = meta.value("speech_gain_db", 12.0);      // global speech boost (+12 default)
        p.songCredits = meta.value("song_credits", true);         // auto on-screen now-playing chip
        p.songCreditSecs = meta.value("song_credit_secs", 10.0);
        p.songCreditCorner = meta.value("song_credit_corner", std::string("tl"));
        if (meta.contains("anchors") && meta["anchors"].is_object())
            for (auto& kv : meta["anchors"].items())
                if (kv.value().is_array() && kv.value().size() == 2)
                    p.anchors[kv.key()] = {kv.value()[0].get<double>(), kv.value()[1].get<double>()};
        json assetsj = j.value("assets", json::object());  // key → {uri, status, …}
        for (auto& kv : assetsj.items()) {
            const json& a = kv.value();
            p.asset_uri[kv.key()] = a.value("uri", std::string());
            if (a.value("type", std::string()) == "video" || a.contains("proxy")) {
                VideoMeta vm;
                // The source media (the asset's own uri) is the in-process decode input; `proxy`
                // (if present) is the JPEG fallback. `src` lets a future asset point elsewhere.
                vm.src = a.value("src", a.value("uri", std::string()));
                vm.proxy = a.value("proxy", std::string());
                vm.fps = a.value("fps", 0.0);
                vm.frames = a.value("frames", 0);
                vm.w = a.value("w", 0); vm.h = a.value("h", 0);
                p.asset_video[kv.key()] = vm;
            }
        }
        {   // the dirs this project's asset files live in → browsable in the Media pane
            // (scan_library adds them as project items; a project whose assets live in
            // examples/assets/luckymas while the stem-dir is …/luckymas2 showed NONE of them).
            std::vector<std::string> dirs;
            for (auto& kv : p.asset_uri) {
                std::string u = kv.second;
                if (u.empty() || u.rfind("cache://", 0) == 0 || u.rfind("http", 0) == 0) continue;
                if (u.rfind("file://", 0) == 0) u = u.substr(7);
                for (auto& ch : u) if (ch == '\\') ch = '/';
                size_t sl = u.find_last_of('/');
                if (sl != std::string::npos && sl > 0) {
                    std::string d = u.substr(0, sl);
                    // absolute-ise project-relative dirs against the project root, so the Media-pane
                    // scan finds them from ANY launch CWD (the dashboard launches at repo-root, where
                    // a bare `assets/foo` would resolve to nothing → the "empty library" bug).
                    if (!d.empty() && d[0] != '/' && d.find(':') == std::string::npos &&
                        !g_projDir.empty() && g_projDir != ".")
                        d = g_projDir + "/" + d;
                    dirs.push_back(d);
                }
            }
            std::sort(dirs.begin(), dirs.end());
            dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
            g_projAssetDirs = dirs;
        }
        for (auto& t : j.value("tracks", json::array())) {
            Track tr;
            tr.id = t.value("id", std::string());
            tr.name = t.value("name", tr.id);
            tr.kind = t.value("kind", std::string("video"));
            for (auto& r : t.value("rows", json::array())) tr.rows.push_back(r.get<std::string>());
            p.tracks.push_back(tr);
        }
        json rowsj = j.value("rows", json::object());  // bind: .items() on a temporary dangles
        for (auto& kv : rowsj.items()) {
            Row r;
            r.id = kv.key();
            r.type = kv.value().value("type", std::string());
            r.name = kv.value().value("name", r.id);
            r.params = kv.value().value("params", json::object());
            for (auto& c : kv.value().value("clips", json::array())) r.clips.push_back(c.get<std::string>());
            p.rows[r.id] = r;
        }
        json clipsj = j.value("clips", json::object());  // bind: .items() on a temporary dangles
        for (auto& kv : clipsj.items()) {
            Clip c;
            c.id = kv.key();
            const json& cj = kv.value();
            c.row = cj.value("row", std::string());
            c.start = cj.value("start", 0.0);
            c.dur = cj.value("dur", 0.0);
            if (cj.contains("asset") && cj["asset"].is_string()) c.asset = cj["asset"].get<std::string>();
            c.params = cj.value("params", json::object());
            if (cj.contains("transform")) parse_xform(cj["transform"], c);
            if (cj.contains("keyframes") && cj["keyframes"].is_object())
                for (auto& kk : cj["keyframes"].items()) {
                    if (!kk.value().is_array()) continue;
                    std::vector<KF> ks;
                    for (auto& k : kk.value()) {
                        KF kf;
                        kf.t = k.value("t", 0.0);
                        kf.interp = k.value("interp", std::string("linear"));
                        if (k.contains("v")) {
                            if (k["v"].is_array()) for (auto& x : k["v"]) kf.v.push_back(x.get<double>());
                            else if (k["v"].is_number()) kf.v.push_back(k["v"].get<double>());
                        }
                        if (k.contains("spring") && k["spring"].is_object()) {
                            kf.stiffness = k["spring"].value("stiffness", 300.0);
                            kf.damping = k["spring"].value("damping", 24.0);
                        }
                        if (k.contains("out") && k["out"].is_array() && k["out"].size() == 2) { kf.outh[0] = k["out"][0].get<double>(); kf.outh[1] = k["out"][1].get<double>(); }
                        if (k.contains("in") && k["in"].is_array() && k["in"].size() == 2) { kf.inh[0] = k["in"][0].get<double>(); kf.inh[1] = k["in"][1].get<double>(); }
                        ks.push_back(kf);
                    }
                    std::sort(ks.begin(), ks.end(), [](const KF& a, const KF& b) { return a.t < b.t; });
                    if (!ks.empty()) c.keyframes[kk.key()] = ks;
                }
            if (p.rows.count(c.row)) c.type = p.rows[c.row].type;
            std::string lbl;
            auto pstr = [&](const char* k) { return (c.params.is_object() && c.params.contains(k) && c.params[k].is_string()) ? c.params[k].get<std::string>() : std::string(); };
            if (!(lbl = pstr("text")).empty()) {}
            else if (!(lbl = pstr("prompt")).empty()) {}
            else if (!(lbl = pstr("title")).empty()) {}   // code/shape cards
            else if (!(lbl = pstr("code")).empty()) {}
            else lbl = cj.value("notes", c.id);
            c.label = snippet(lbl);
            p.clips[c.id] = c;
        }
        p.doc = std::move(j);  // keep the raw doc for patch + persist on generate
        p.ok = true;
    } catch (std::exception& e) {
        p.error = e.what();
    }
    return p;
}

static Project load_project(const std::string& path) {
    Project p; p.path = path;
    std::ifstream f(path);
    if (!f) { p.error = "cannot open " + path; return p; }
    try { json j; f >> j; return parse_project_json(std::move(j), path); }
    catch (std::exception& e) { p.error = e.what(); return p; }
}
static bool sync_to_doc(Project& p);   // fwd-decl: apply_generations (above its definition) folds edits in first

// ───────────────────────────── D3D11 plumbing ─────────────────────────────
static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_sc  = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11BlendState*       g_blendAdd = nullptr;   // additive: dst += src.rgb * src.a (avatar light-up)

static void CreateRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
}
static void CleanupRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool CreateDeviceD3D(HWND hwnd, int w, int h) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // RGBA8 → screenshot is a direct copy
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, lvls, 2,
                                               D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &got, &g_ctx);
    if (hr != S_OK)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, lvls, 2,
                                           D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &got, &g_ctx);
    if (hr != S_OK) return false;
    CreateRTV();
    // additive blend for the avatar "light-up" (talk → brighten): dst.rgb += src.rgb * src.a.
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, &g_blendAdd);
    return true;
}
static void CleanupDeviceD3D() {
    CleanupRTV();
    if (g_blendAdd) { g_blendAdd->Release(); g_blendAdd = nullptr; }
    if (g_sc)  { g_sc->Release();  g_sc  = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static void save_png_backbuffer(const char* path) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sdesc = d;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sdesc.MiscFlags = 0;
    ID3D11Texture2D* stag = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&sdesc, nullptr, &stag))) {
        g_ctx->CopyResource(stag, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(stag, 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<unsigned char> px((size_t)d.Width * d.Height * 4);
            for (UINT y = 0; y < d.Height; ++y)
                memcpy(&px[(size_t)y * d.Width * 4],
                       (unsigned char*)m.pData + (size_t)y * m.RowPitch,
                       (size_t)d.Width * 4);
            g_ctx->Unmap(stag, 0);
            stbi_write_png(path, (int)d.Width, (int)d.Height, 4, px.data(), (int)d.Width * 4);
        }
        stag->Release();
    }
    bb->Release();
}

// ───────────────────────────── textures / assets ──────────────────────────
struct Tex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };
static std::map<std::string, Tex> g_texCache;
static std::string g_cacheDir = "cache";

// Derive the repo root from the running exe (<root>/build/slopstudio.exe) so `library/…` beds,
// `cache://` gens, and presets/ resolve regardless of the launch CWD — the dashboard launches at
// repo-root, the standalone launcher at the project dir, and those two want OPPOSITE roots for the
// two asset families. Marker-verified; falls back to "." (CWD, the legacy behaviour) if unsure.
static std::string derive_repo_root() {
    char buf[1024];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return ".";
    std::string exe(buf, n);
    for (auto& ch : exe) if (ch == '\\') ch = '/';
    size_t s1 = exe.find_last_of('/');                       // .../build/slopstudio.exe → .../build
    if (s1 == std::string::npos || s1 == 0) return ".";
    size_t s2 = exe.find_last_of('/', s1 - 1);               // .../build → .../<root>
    if (s2 == std::string::npos) return ".";
    std::string root = exe.substr(0, s2);
    if (GetFileAttributesA((root + "/library").c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA((root + "/flake.nix").c_str()) != INVALID_FILE_ATTRIBUTES)
        return root;
    return ".";
}

static std::string resolve_asset(const std::string& uri) {
    if (uri.rfind("file://", 0) == 0) return uri.substr(7);
    if (uri.rfind("cache://", 0) == 0) return g_cacheDir + "/" + uri.substr(8);
    // plain path: CWD first (the repo-root convention), else PROJECT-relative (a project dir is
    // portable — it can live outside the code repo), else REPO-relative (library/ beds, presets/).
    // The three roots cover both families from any launch CWD.
    if (!uri.empty() && uri[0] != '/' && uri.find(':') == std::string::npos) {
        if (GetFileAttributesA(uri.c_str()) != INVALID_FILE_ATTRIBUTES) return uri;  // CWD
        if (!g_projDir.empty()) {
            std::string pj = g_projDir + "/" + uri;
            if (GetFileAttributesA(pj.c_str()) != INVALID_FILE_ATTRIBUTES) return pj;
        }
        if (!g_repoRoot.empty() && g_repoRoot != ".") {
            std::string rr = g_repoRoot + "/" + uri;
            if (GetFileAttributesA(rr.c_str()) != INVALID_FILE_ATTRIBUTES) return rr;
        }
    }
    return uri;  // plain path
}

// Project file last-write time (uint64) — drives live auto-reload when the .slop.json is
// hand-edited (or LLM-edited) OUTSIDE the editor, so external param tweaks reflect instantly
// (the doc promised this; this is the actual watch). A partial mid-save parse just retries.
static unsigned long long file_mtime(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    return ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
}
static unsigned long long g_projMtime = 0;

// Undo/redo state (declared early so apply_generations + the timeline drop handler can flag it;
// the functions that drive it live by g_bufFor, further down). See the block there for the model.
static std::deque<std::string> g_undo, g_redo;   // past states (undo) · undone states (redo)
static std::string g_undoBase;                    // the current committed doc, compact dump
static bool g_undoDirty = false;                  // a non-widget edit happened → force a settle-check
static bool g_undoActivePrev = false;             // ImGui::IsAnyItemActive() last frame (gesture-end edge)
static const size_t UNDO_CAP = 300;               // plenty of RAM: 300 × a compact doc dump

// Loads + caches a texture by asset uri. Returns nullptr if it can't resolve (yet) —
// the compositor then draws a typed placeholder, so a missing/regenerating asset never
// blocks the preview.
// A library item's `<file>.meta.json` may carry a `removebg` block → key its flat background to
// alpha ON THE FLY here (non-destructive: the source file is untouched; the matte is what gets
// cached + composited). Defined later (needs the sprite color-key + json). Returns true + fills
// `out` with a keyed copy when active; false → use the raw pixels.
static bool item_removebg(const std::string& path, const unsigned char* data, int w, int h, std::vector<unsigned char>& out);
// A painted alpha MASK (<file>.mask.png; 255=keep, 0=erased — brush/box-fill cleanup of what
// removebg misses) and a CROP rect both ride the item's sidecar, applied here after removebg so
// they're non-destructive + flow to grid/timeline/export. Defined after the sprite crop/key core.
static void item_apply_mask(const std::string& path, std::vector<unsigned char>& buf, int w, int h);
static bool item_apply_crop(const std::string& path, std::vector<unsigned char>& buf, int& w, int& h);

// Create an IMMUTABLE RGBA shader-resource view from a tightly-packed w*h*4 buffer — the one
// upload path shared by the still cache, the proxy-frame cache, and the libav decode cache.
static ID3D11ShaderResourceView* make_rgba_srv(const unsigned char* px, int w, int h) {
    if (!px || w <= 0 || h <= 0) return nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = px; sd.SysMemPitch = (UINT)w * 4;
    ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &tex)) && tex) {
        g_dev->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
    }
    return srv;
}

static Tex* get_texture(const std::string& uri) {
    if (uri.empty()) return nullptr;
    auto it = g_texCache.find(uri);
    if (it != g_texCache.end()) return it->second.srv ? &it->second : nullptr;
    Tex t;
    int n = 0;
    std::string path = resolve_asset(uri);
    unsigned char* data = stbi_load(path.c_str(), &t.w, &t.h, &n, 4);
    if (!data)
        fprintf(stderr, "tex load FAILED uri=%s path=%s reason=%s\n",
                uri.c_str(), path.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "?");
    if (data) {
        // non-destructive sidecar pipeline (library items): bg removal → painted mask → crop.
        // The processed buffer is what's cached + composited; the source PNG is never touched.
        std::vector<unsigned char> buf;
        if (!item_removebg(path, data, t.w, t.h, buf)) buf.assign(data, data + (size_t)t.w * t.h * 4);
        item_apply_mask(path, buf, t.w, t.h);   // brush/box-fill cleanup (multiply alpha)
        item_apply_crop(path, buf, t.w, t.h);   // crop rect (updates t.w/t.h)
        t.srv = make_rgba_srv(buf.data(), t.w, t.h);
        stbi_image_free(data);
    }
    g_texCache[uri] = t;
    return t.srv ? &g_texCache[uri] : nullptr;
}
// Drop a cached texture so the next get_texture reloads from disk — used when a library item's
// bytes change under a stable path (a library regen overwrites library/<sub>/<name>.<ext>).
static void invalidate_texture(const std::string& uri) {
    auto it = g_texCache.find(uri);
    if (it == g_texCache.end()) return;
    if (it->second.srv) it->second.srv->Release();
    g_texCache.erase(it);
}

// ──────────────────────── video proxy frames (uri+time → SRV) ──────────────
// A `video` clip's PROXY is a dir of decimated JPEG frames (tools/video-proxy.py). The
// compositor maps the playhead → the clip's local time → a frame index, and this loads THAT
// frame as a texture — the editor's whole "decode" path. Unlike the still cache it's LRU-
// bounded: scrubbing a 1800-frame clip must not pin 1800 textures in VRAM, so we keep only the
// most-recently-touched ~kFrameCacheMax. A miss loads one JPEG (stb) → immutable texture.
// (A future in-process libav/NVDEC decoder can replace the frame SOURCE behind this same
// signature without touching the compositor.)
struct FrameTex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; unsigned long long lru = 0; };
static std::map<std::string, FrameTex> g_frameCache;   // key = "<proxydir>/f00001.jpg" OR "<srcuri>#<idx>"
static unsigned long long g_frameClock = 0;
static const size_t kFrameCacheMax = 96;               // ~96 resident frames (1080p RGBA ≈ 760MB worst case)

// Evict the least-recently-used resident frame when at cap (called on a cache miss). Shared by
// the proxy-JPEG loader and the libav decode loader — one bounded pool across both sources.
static void evict_one_frame_if_full() {
    if (g_frameCache.size() < kFrameCacheMax) return;
    auto victim = g_frameCache.end();
    for (auto i = g_frameCache.begin(); i != g_frameCache.end(); ++i)
        if (victim == g_frameCache.end() || i->second.lru < victim->second.lru) victim = i;
    if (victim != g_frameCache.end()) { if (victim->second.srv) victim->second.srv->Release(); g_frameCache.erase(victim); }
}

static FrameTex* get_frame_tex(const std::string& proxyDir, int idx) {
    char fn[24];
    snprintf(fn, sizeof fn, "f%05d.jpg", idx + 1);     // proxy frames are 1-based (ffmpeg)
    std::string key = proxyDir + "/" + fn;
    auto it = g_frameCache.find(key);
    if (it != g_frameCache.end()) { it->second.lru = ++g_frameClock; return it->second.srv ? &it->second : nullptr; }
    evict_one_frame_if_full();
    FrameTex ft; ft.lru = ++g_frameClock;
    int n = 0;
    std::string path = resolve_asset(key);
    unsigned char* data = stbi_load(path.c_str(), &ft.w, &ft.h, &n, 4);
    if (data) { ft.srv = make_rgba_srv(data, ft.w, ft.h); stbi_image_free(data); }
    g_frameCache[key] = ft;
    return ft.srv ? &g_frameCache[key] : nullptr;
}

// Resolve a video clip's frame index at timeline time T (playhead) from its proxy meta +
// the clip's `in` (source in-point s) / `speed` / `loop` params. Decimated proxy ⇒ multiple
// 60fps export frames hold one proxy frame (fine for B-roll). Clamped to [0, frames-1].
static int video_frame_index(const VideoMeta& vm, const Clip& c, double T) {
    if (vm.frames <= 0 || vm.fps <= 0) return 0;
    double local = T - c.start;                               // timeline-local seconds
    double in    = c.params.value("in", 0.0);
    double speed = c.params.value("speed", 1.0);
    // params.loop: true = wrap at source EOF (default — extending a short clip repeats it) ·
    // false = hold the last frame · "pingpong" = bounce forward/backward over the source, so
    // moving b-roll loops without the hard rewind seam.
    bool loop = true, pingpong = false;
    if (c.params.is_object() && c.params.contains("loop")) {
        const json& lj = c.params["loop"];
        if (lj.is_boolean())     loop = lj.get<bool>();
        else if (lj.is_string()) pingpong = (lj.get<std::string>() == "pingpong");
    }
    double proxyDur = vm.frames / vm.fps;                     // seconds covered by the proxy
    double span = proxyDur - in;
    double t = (local < 0 ? 0 : local) * speed;
    double srcT = in + t;
    if (pingpong && span > 1e-6) {
        double ph = std::fmod(t, span * 2.0);
        srcT = in + (ph < span ? ph : span * 2.0 - ph);
    } else if (loop && span > 1e-6) srcT = in + std::fmod(t, span);
    int idx = (int)std::lround(srcT * vm.fps);
    if (idx < 0) idx = 0;
    if (idx >= vm.frames) idx = vm.frames - 1;
    return idx;
}

// ─────────────────────── in-process video decode (libav) ───────────────────
// Decode B-roll FRAMES on demand inside the editor — no proxy extraction step. This is the
// real "decode" leaf behind the clip+time→SRV contract: the compositor still asks for a frame
// index and gets a texture; only the SOURCE of the bytes changes (libav seek+decode+swscale
// instead of a pre-baked JPEG). Built only when the flake's mingw-cross ffmpeg is present
// (-DSLOP_LIBAV); otherwise the proxy path (get_frame_tex) is the whole story. NVDEC/d3d11va
// hardware decode can later replace the software decode inside VideoDecoder without touching
// the compositor or the frame cache.
#ifdef SLOP_LIBAV
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}
static bool g_videoDirect = true;   // --no-video-decode forces the proxy fallback (debugging/A-B)

// One open source file: format + codec + swscale contexts kept RESIDENT so scrubbing never
// reopens/reparses. decode_index(i) does the standard accurate seek — jump to the keyframe
// at/just-before the target, then decode forward to it — and converts the frame to RGBA.
struct VideoDecoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext*  dec = nullptr;
    SwsContext*      sws = nullptr;
    AVFrame*         frame = nullptr;
    AVPacket*        pkt = nullptr;
    int        vstream = -1;
    AVRational tb{0, 1};        // stream time_base
    double     fps = 0;
    int        frames = 0, w = 0, h = 0;
    int        cur_idx = -1;    // index of the frame currently held in `frame` (forward-decode reuse)
    bool       ok = false;
    unsigned long long lru = 0;

    bool open(const std::string& path) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vstream < 0) return false;
        AVStream* st = fmt->streams[vstream];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) return false;
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) return false;
        dec->thread_count = 0;                       // auto multithreaded decode
        if (avcodec_open2(dec, codec, nullptr) < 0) return false;
        tb = st->time_base;
        AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        fps = r.num ? av_q2d(r) : 30.0;
        w = dec->width; h = dec->height;
        if (st->nb_frames > 0) frames = (int)st->nb_frames;
        else {
            double dur = (st->duration > 0 && st->duration != AV_NOPTS_VALUE) ? st->duration * av_q2d(tb)
                       : (fmt->duration != AV_NOPTS_VALUE ? fmt->duration / (double)AV_TIME_BASE : 0);
            frames = dur > 0 ? (int)(dur * fps + 0.5) : 0;
        }
        frame = av_frame_alloc(); pkt = av_packet_alloc();
        ok = frame && pkt && w > 0 && h > 0;
        return ok;
    }
    void close() {
        if (sws) sws_freeContext(sws);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
        sws = nullptr; frame = nullptr; pkt = nullptr; dec = nullptr; fmt = nullptr; ok = false;
    }
    // Convert the frame currently in `frame` → RGBA into `out` (w*h*4). False if none held.
    bool to_rgba(std::vector<unsigned char>& out) {
        if (!frame || !frame->data[0]) return false;
        sws = sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format,
                                   w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        out.resize((size_t)w * h * 4);
        unsigned char* dst[4] = { out.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { w * 4, 0, 0, 0 };
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        return true;
    }
    // Decode the frame nearest `idx` (at source fps) → RGBA `out`. Reuses the held frame on a
    // small forward step; seeks to the prior keyframe otherwise.
    bool decode_index(int idx, std::vector<unsigned char>& out, bool retried = false) {
        if (!ok) return false;
        if (idx < 0) idx = 0;
        if (frames > 0 && idx >= frames) idx = frames - 1;
        if (idx == cur_idx) {                                          // already held…
            if (to_rgba(out)) return true;
            cur_idx = -1;                                              // …unless a failed receive wiped it
        }
        int64_t target = (int64_t)((double)idx / fps / av_q2d(tb) + 0.5);
        bool needSeek = (cur_idx < 0) || (idx < cur_idx) || (idx - cur_idx > 30);
        if (needSeek) {
            if (av_seek_frame(fmt, vstream, target, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(fmt, vstream, target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
            avcodec_flush_buffers(dec);
            cur_idx = -1;
        }
        bool reached = false, eof = false;
        while (!reached && !eof) {
            int rp = av_read_frame(fmt, pkt);
            if (rp < 0) { avcodec_send_packet(dec, nullptr); eof = true; }   // flush at EOF
            else if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
            else { avcodec_send_packet(dec, pkt); av_packet_unref(pkt); }
            for (;;) {
                int rr = avcodec_receive_frame(dec, frame);
                if (rr == AVERROR(EAGAIN)) break;
                if (rr < 0) { eof = true; break; }                          // AVERROR_EOF or hard error
                int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                cur_idx = (pts != AV_NOPTS_VALUE) ? (int)(pts * av_q2d(tb) * fps + 0.5) : (cur_idx + 1);
                if (pts == AV_NOPTS_VALUE || cur_idx >= idx || (frames > 0 && cur_idx >= frames - 1)) { reached = true; break; }
            }
        }
        // Container metadata can OVERPROMISE (nb_frames/duration past the real stream — e.g. a
        // 16.5s demo declaring 1130 frames with 990 decodable): hitting EOF short of the target
        // used to leave a "can't decode source" placeholder for the whole phantom tail before the
        // loop wrapped. Learn the REAL count so the loop wraps at true EOF, and a failed receive
        // (which unrefs the held frame) retries the last real frame instead of drawing nothing.
        if (eof && !reached && cur_idx >= 0 && cur_idx + 1 < frames) frames = cur_idx + 1;
        if (to_rgba(out)) return true;     // best frame held (exact, or the last before EOF)
        cur_idx = -1;                      // whatever was held is gone — don't fake a hit next call
        if (!retried && frames > 0) return decode_index(frames - 1, out, true);
        return false;
    }
};

// Open source files are cached (kept resident) — capped + LRU-closed; a failed open is cached
// as a negative result so we don't re-attempt (and re-log) every frame.
static std::map<std::string, VideoDecoder*> g_decoders;
static const size_t kMaxDecoders = 6;

static VideoDecoder* get_decoder(const std::string& srcPath) {
    auto it = g_decoders.find(srcPath);
    if (it != g_decoders.end()) { it->second->lru = ++g_frameClock; return it->second->ok ? it->second : nullptr; }
    if (g_decoders.size() >= kMaxDecoders) {
        auto v = g_decoders.end();
        for (auto i = g_decoders.begin(); i != g_decoders.end(); ++i)
            if (v == g_decoders.end() || i->second->lru < v->second->lru) v = i;
        if (v != g_decoders.end()) { v->second->close(); delete v->second; g_decoders.erase(v); }
    }
    static bool quieted = false;
    if (!quieted) { av_log_set_level(AV_LOG_ERROR); quieted = true; }
    VideoDecoder* d = new VideoDecoder();
    d->lru = ++g_frameClock;
    if (!d->open(srcPath)) { d->close(); fprintf(stderr, "video decode: can't open %s\n", srcPath.c_str()); }
    g_decoders[srcPath] = d;
    return d->ok ? d : nullptr;
}

// Decode-backed analogue of get_frame_tex: the (src,idx) frame as a texture, via the SAME LRU
// frame pool (distinct key namespace "<srcuri>#<idx>"). Decodes one frame on a miss.
static FrameTex* get_decoded_frame_tex(const std::string& srcUri, int idx, VideoDecoder* d) {
    char suf[24]; snprintf(suf, sizeof suf, "#%d", idx);
    std::string key = srcUri + suf;
    auto it = g_frameCache.find(key);
    if (it != g_frameCache.end()) { it->second.lru = ++g_frameClock; return it->second.srv ? &it->second : nullptr; }
    evict_one_frame_if_full();
    FrameTex ft; ft.lru = ++g_frameClock;
    std::vector<unsigned char> rgba;
    if (d->decode_index(idx, rgba) && !rgba.empty()) {
        ft.w = d->w; ft.h = d->h; ft.srv = make_rgba_srv(rgba.data(), ft.w, ft.h);
    }
    g_frameCache[key] = ft;
    return ft.srv ? &g_frameCache[key] : nullptr;
}
#endif // SLOP_LIBAV

// ──────────────────────────── asset library ───────────────────────────────
// A GLOBAL, cross-project library of golden reusable media + (later) a per-PROJECT view of
// generated assets. Backed by a `library/<images|audio|video>/` dir the editor SCANS — drop a
// file in on disk OR via the panel and it appears. Filename = display name (rename = move the
// file); search = substring; generation metadata rides in a `<file>.meta.json` sidecar (L5).
// draw_library_window() browses/searches/previews + drag-drops onto the timeline.
enum LibType { LIB_IMAGE, LIB_AUDIO, LIB_VIDEO, LIB_AVATAR };  // AVATAR = a rig def (library/avatars/<name>.avatar.json)
struct LibItem { std::string name, path; LibType type; bool gen = false; bool preset = false; bool proj = false; };  // gen = .meta.json recipe sidecar; preset = read-only presets/avatars/<name> rig; proj = per-PROJECT library item
static std::string g_libraryDir = "library";
static std::string g_projLibDir;     // per-project library: <projdir>/assets/<stem>/ (set at load; scanned recursively)
static int g_libScope = 0;           // Media pane scope: 0=all · 1=project · 2=common
static std::vector<LibItem> g_library;
static bool g_libraryDirty = true;   // re-scan requested (set on import/rename/delete; also every ~2.5s so on-disk drops just appear)

static std::wstring to_w(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string from_w(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static std::string lower_str(const std::string& s) { std::string o; for (char c : s) o += (char)tolower((unsigned char)c); return o; }
static std::string ext_lower(const std::string& fn) {
    size_t d = fn.find_last_of('.'); return d == std::string::npos ? "" : lower_str(fn.substr(d + 1));
}
static LibType lib_type_of(const std::string& e) {
    if (e == "wav" || e == "mp3" || e == "ogg" || e == "flac" || e == "m4a") return LIB_AUDIO;
    if (e == "mp4" || e == "mov" || e == "webm" || e == "mkv" || e == "avi") return LIB_VIDEO;
    return LIB_IMAGE;
}
static bool has_media_ext(const std::string& e) {
    static const char* ok[] = {"png","jpg","jpeg","bmp","gif","wav","mp3","ogg","flac","m4a",
                               "mp4","mov","webm","mkv","avi", nullptr};
    for (int i = 0; ok[i]; i++) if (e == ok[i]) return true;
    return false;
}
static void ensure_dir(const std::string& d) { CreateDirectoryW(to_w(d).c_str(), nullptr); }
static const char* lib_subdir(LibType t) { return t == LIB_IMAGE ? "images" : t == LIB_AUDIO ? "audio" : t == LIB_AVATAR ? "avatars" : "video"; }

// Library avatar-rig defs (library/avatars/<name>.avatar.json). A rig = a `prefix` (emotion E →
// library/images/<prefix>E.png) + optional `emotions` overrides (a named emotion → a specific
// library image). get_rig() resolves it for the compositor; these helpers author/edit it.
static const char* AVATAR_RIG_EXT = ".avatar.json";
static std::string avatar_rig_path(const std::string& name) { return g_libraryDir + "/avatars/" + name + AVATAR_RIG_EXT; }
static json load_avatar_rig(const std::string& name) {
    std::ifstream f(avatar_rig_path(name));
    if (f) { try { json j; f >> j; return j; } catch (...) {} }
    return json::object();
}
static void invalidate_rig(const std::string& name);   // defined by get_rig (drops the rig cache so edits show live)
static void save_avatar_rig(const std::string& name, const json& j) {
    ensure_dir(g_libraryDir + "/avatars");
    std::ofstream o(avatar_rig_path(name));
    if (o) { o << j.dump(2) << "\n"; o.close(); }
    invalidate_rig(name);
    g_libraryDirty = true;
}

static json lib_load_sidecar(const std::string& itemPath);   // defined below (sidecar helpers)
static bool lib_is_gen(const json& side);
static void invalidate_texture(const std::string& path);     // defined with the texture cache

// The periodic rescan makes new on-disk files JUST APPEAR — but a file an external tool
// OVERWROTE (same path) must also drop its cached texture, or the grid/timeline shows the
// stale pixels forever. Track write-times across scans and invalidate on change.
static std::map<std::string, unsigned long long> g_libMtime;
static unsigned long long ft64(const FILETIME& ft) { return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
static void lib_track_mtime(const std::string& path, const FILETIME& ft) {
    unsigned long long t = ft64(ft);
    auto it = g_libMtime.find(path);
    if (it != g_libMtime.end() && it->second != t) invalidate_texture(path);
    g_libMtime[path] = t;
}

// Recursive media scan for the per-project library (<projdir>/assets/<stem>/ — the existing
// examples/assets/luckymas convention). Display name = path relative to the root, so subdir
// items (e.g. "sygnas/imas-credits.png") stay unambiguous.
static void scan_media_dir_recursive(const std::string& dir, const std::string& rel, bool recurse = true) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(to_w(dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string fn = from_w(fd.cFileName);
        if (fn == "." || fn == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse) scan_media_dir_recursive(dir + "/" + fn, rel.empty() ? fn : rel + "/" + fn);
            continue;
        }
        std::string e = ext_lower(fn);
        if (!has_media_ext(e)) continue;
        if (fn.size() > 9 && fn.compare(fn.size() - 9, 9, ".mask.png") == 0) continue;
        std::string full = dir + "/" + fn;
        bool gen = GetFileAttributesW(to_w(full + ".meta.json").c_str()) != INVALID_FILE_ATTRIBUTES
                   && lib_is_gen(lib_load_sidecar(full));
        lib_track_mtime(full, fd.ftLastWriteTime);
        g_library.push_back({rel.empty() ? fn : rel + "/" + fn, full, lib_type_of(e), gen, false, true});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scan_library() {
    g_library.clear();
    ensure_dir(g_libraryDir);
    // music/ holds the stock BGM beds (library/music/catalogue.json → tools/fetch-stock-music.py);
    // they browse as plain audio items so a bed drags onto the timeline like any clip.
    static const std::pair<const char*, LibType> COMMON_DIRS[] = {
        {"images", LIB_IMAGE}, {"audio", LIB_AUDIO}, {"video", LIB_VIDEO}, {"music", LIB_AUDIO}};
    for (auto& [sub, t] : COMMON_DIRS) {
        std::string dir = g_libraryDir + "/" + std::string(sub);
        ensure_dir(dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(to_w(dir + "/*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string fn = from_w(fd.cFileName);
            std::string e = ext_lower(fn);
            if (!has_media_ext(e)) continue;            // skip .meta.json sidecars, etc.
            if (fn.size() > 9 && fn.compare(fn.size() - 9, 9, ".mask.png") == 0) continue;  // painted-mask sidecar, not a library item
            std::string full = dir + "/" + fn;
            // gen item = a sidecar with kind:"gen" (a removebg-only sidecar doesn't count as a gen)
            bool gen = GetFileAttributesW(to_w(full + ".meta.json").c_str()) != INVALID_FILE_ATTRIBUTES
                       && lib_is_gen(lib_load_sidecar(full));
            lib_track_mtime(full, fd.ftLastWriteTime);
            g_library.push_back({fn, full, lib_type_of(e), gen});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // per-PROJECT library (recursive) — that video's own assets, browsable next to the common ones
    if (!g_projLibDir.empty()) scan_media_dir_recursive(g_projLibDir, "");
    // ALSO the dirs the project's assets actually live in (e.g. examples/assets/luckymas under a
    // luckymas2 stem, assets-src regenerables) — non-recursive, skipping dirs already scanned above.
    // Without this the Media pane hid most of what the cut references (user-reported).
    for (auto& d : g_projAssetDirs) {
        if (!g_projLibDir.empty() && d.rfind(g_projLibDir, 0) == 0) continue;
        if (d.rfind(g_libraryDir, 0) == 0) continue;
        size_t sl = d.find_last_of('/');
        scan_media_dir_recursive(d, sl == std::string::npos ? d : d.substr(sl + 1), false);
    }
    // avatar rigs (library/avatars/<name>.avatar.json) — a separate pass (not a media ext). The
    // display/reference name is the base WITHOUT the .avatar.json suffix (= the row's `rig` value).
    {
        std::string dir = g_libraryDir + "/avatars";
        ensure_dir(dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(to_w(dir + "/*" + AVATAR_RIG_EXT).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string fn = from_w(fd.cFileName);
                std::string base = fn.substr(0, fn.size() - strlen(AVATAR_RIG_EXT));   // strip .avatar.json
                g_library.push_back({base, dir + "/" + fn, LIB_AVATAR, false, false});
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    // PRESET rigs (presets/avatars/<name>/manifest.json) — read-only, but browsable + placeable so
    // the bundled rigs (e.g. gemma-gpt-static) show up in the Library alongside authored ones.
    {
        std::string dir = "presets/avatars";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(to_w(dir + "/*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string name = from_w(fd.cFileName);
                if (name == "." || name == "..") continue;
                std::string mani = dir + "/" + name + "/manifest.json";
                WIN32_FILE_ATTRIBUTE_DATA ad;
                if (!GetFileAttributesExW(to_w(mani).c_str(), GetFileExInfoStandard, &ad)) continue;
                {   // manifest edited on disk (agent/tool) → drop the cached rig so it hot-reloads
                    unsigned long long t = ft64(ad.ftLastWriteTime);
                    auto mi = g_libMtime.find(mani);
                    if (mi != g_libMtime.end() && mi->second != t) invalidate_rig(name);
                    g_libMtime[mani] = t;
                }
                bool dup = false;                                  // a library def of the same name wins
                for (auto& li : g_library) if (li.type == LIB_AVATAR && li.name == name) { dup = true; break; }
                if (!dup) g_library.push_back({name, mani, LIB_AVATAR, false, true});
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::sort(g_library.begin(), g_library.end(),
              [](const LibItem& a, const LibItem& b) {          // project items group first, then a-z
                  if (a.proj != b.proj) return a.proj;
                  return lower_str(a.name) < lower_str(b.name);
              });
    g_libraryDirty = false;
}

// The destination root for new items (imports + gens): the per-project library when a project
// is open and the pane isn't scoped to "common" — a video's new assets belong to that video.
static std::string lib_dest_root() {
    return (!g_projLibDir.empty() && g_libScope != 2) ? g_projLibDir : g_libraryDir;
}

// Copy an external file into the library (into the subdir for its type); de-dupes the name.
// Returns the library path of the copy ("" if not a media file) so callers can reference the
// managed copy instead of the external original.
static std::string library_import(const std::string& srcPath) {
    size_t sl = srcPath.find_last_of("/\\");
    std::string fn = sl == std::string::npos ? srcPath : srcPath.substr(sl + 1);
    std::string e = ext_lower(fn);
    if (!has_media_ext(e)) return "";
    std::string dir = lib_dest_root() + "/" + lib_subdir(lib_type_of(e));
    ensure_dir(dir);
    size_t dot = fn.find_last_of('.');
    std::string base = dot == std::string::npos ? fn : fn.substr(0, dot);
    std::string dst = dir + "/" + fn;
    for (int n = 1; GetFileAttributesW(to_w(dst).c_str()) != INVALID_FILE_ATTRIBUTES; n++)
        dst = dir + "/" + base + "-" + std::to_string(n) + "." + e;
    if (!CopyFileW(to_w(srcPath).c_str(), to_w(dst).c_str(), TRUE)) return "";
    g_libraryDirty = true;
    return dst;
}
static void library_rename(const LibItem& it, const std::string& newBase) {
    if (newBase.empty()) return;
    size_t dot = it.name.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : it.name.substr(dot);
    size_t sl = it.path.find_last_of("/\\");
    std::string dst = it.path.substr(0, sl) + "/" + newBase + ext;
    if (to_w(dst) != to_w(it.path) && MoveFileW(to_w(it.path).c_str(), to_w(dst).c_str())) {
        // move the gen sidecar alongside (if any) so the recipe + history follow the rename
        std::wstring sFrom = to_w(it.path + ".meta.json"), sTo = to_w(dst + ".meta.json");
        if (GetFileAttributesW(sFrom.c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileW(sFrom.c_str(), sTo.c_str());
        g_libraryDirty = true;
    }
}
static void library_delete(const LibItem& it) {
    DeleteFileW(to_w(it.path + ".meta.json").c_str());     // drop the gen-recipe sidecar too (if any)
    if (DeleteFileW(to_w(it.path).c_str())) g_libraryDirty = true;
}

// ── per-item gen sidecar (L4): a `<file>.meta.json` next to a library item records HOW it was
// generated (provider/cap/params + content hash) + a `history` of past gens, so a library item
// can be re-generated in place and earlier gens restored. Plain import has no sidecar; a gen item
// (made via "Add gen item" or Regenerate) does. scan_library skips the .json (not a media ext). ──
static std::string strip_ext(const std::string& p) {
    size_t d = p.find_last_of('.'), s = p.find_last_of("/\\");
    return (d == std::string::npos || (s != std::string::npos && d < s)) ? p : p.substr(0, d);
}
static std::string lib_sidecar_path(const std::string& itemPath) { return itemPath + ".meta.json"; }
static json lib_load_sidecar(const std::string& itemPath) {
    std::ifstream f(lib_sidecar_path(itemPath), std::ios::binary);
    if (!f) return json::object();
    try { json j; f >> j; return j.is_object() ? j : json::object(); } catch (...) { return json::object(); }
}
static bool lib_save_sidecar(const std::string& itemPath, const json& j) {
    std::ofstream o(lib_sidecar_path(itemPath), std::ios::binary);
    if (!o) return false;
    o << j.dump(2) << "\n";
    return true;
}
static bool lib_is_gen(const json& side) { return side.is_object() && side.value("kind", std::string()) == "gen"; }

// ── Sprite-sheet processor core: cut a (GPT) sheet into background-keyed, auto-trimmed library
// PNGs. Flat-colour key removal (picker + fuzziness) + per-rect crop + auto-crop to the alpha
// bbox (minimum size). Simple rectangles, no masking. Shared by the panel + --sprite-cut. ──
struct SpriteRect { int x, y, w, h; };

// Alpha-out pixels within `fuzz` (RGB euclidean distance, 0..441) of the key colour, in place.
static void sprite_color_key(std::vector<unsigned char>& px, int kr, int kg, int kb, float fuzz) {
    float f2 = fuzz * fuzz;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        int dr = (int)px[i] - kr, dg = (int)px[i + 1] - kg, db = (int)px[i + 2] - kb;
        if ((float)(dr * dr + dg * dg + db * db) <= f2) px[i + 3] = 0;
    }
}

// On-the-fly background removal for a library item (forward-declared above get_texture). A
// `<path>.meta.json` `removebg` block keys the bg to alpha — two methods:
//   {method:"colorkey", key:[r,g,b], fuzz}        flat-bg colour key, computed here (instant)
//   {method:"rembg",    model, cache:"cache://…"} a provider matte (isnet-anime/…) — the editor
//                                                 submits the source to the rembg provider once
//                                                 and records the cached CUTOUT here; we just load
//                                                 it in place of the source (segments soft/gradient
//                                                 bgs the colour key can't).
// Non-destructive either way (source untouched; the matte is cached + composited like any texture,
// so it flows to the grid/Viewer/timeline/export). Cheap fast-reject when there's no sidecar.
static bool item_removebg(const std::string& path, const unsigned char* data, int w, int h, std::vector<unsigned char>& out) {
    if (GetFileAttributesW(to_w(path + ".meta.json").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    json side = lib_load_sidecar(path);
    if (!side.contains("removebg") || !side["removebg"].is_object()) return false;
    const json& rb = side["removebg"];
    if (!rb.value("enabled", true)) return false;
    if (rb.value("method", std::string("colorkey")) == "rembg") {
        // load the provider-generated cutout (RGBA) in place of the source. Best-effort: a missing
        // cutout (cache gitignored / not yet cut) or drifted dims → false (show the raw source).
        std::string cut = rb.value("cache", std::string());
        if (cut.empty()) return false;
        int cw = 0, ch = 0, cn = 0;
        unsigned char* cd = stbi_load(resolve_asset(cut).c_str(), &cw, &ch, &cn, 4);
        if (!cd) return false;
        bool okdim = (cw == w && ch == h);
        if (okdim) out.assign(cd, cd + (size_t)w * h * 4);
        stbi_image_free(cd);
        return okdim;
    }
    out.assign(data, data + (size_t)w * h * 4);
    const json& k = rb.contains("key") && rb["key"].is_array() ? rb["key"] : json::array({255, 0, 255});
    int kr = k.size() > 0 ? k[0].get<int>() : 255, kg = k.size() > 1 ? k[1].get<int>() : 0, kb = k.size() > 2 ? k[2].get<int>() : 255;
    sprite_color_key(out, kr, kg, kb, (float)rb.value("fuzz", 60.0));
    return true;
}
// Tight bbox of pixels with alpha > athresh; false if the region is fully transparent.
static bool sprite_alpha_bbox(const unsigned char* px, int w, int h,
                              int& x0, int& y0, int& x1, int& y1, int athresh = 8) {
    x0 = w; y0 = h; x1 = -1; y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (px[((size_t)y * w + x) * 4 + 3] > athresh) {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
    return x1 >= x0 && y1 >= y0;
}
static bool sprite_alpha_bbox(const std::vector<unsigned char>& px, int w, int h,
                              int& x0, int& y0, int& x1, int& y1, int athresh = 8) {
    return sprite_alpha_bbox(px.data(), w, h, x0, y0, x1, y1, athresh);
}
// Crop [rx,ry,rw,rh] (clamped to the image) from an RGBA buffer → a new buffer + its dims.
static std::vector<unsigned char> sprite_crop(const std::vector<unsigned char>& px, int w, int h,
                                              int rx, int ry, int rw, int rh, int& ow, int& oh) {
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > w) rw = w - rx;
    if (ry + rh > h) rh = h - ry;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    ow = rw; oh = rh;
    std::vector<unsigned char> out((size_t)rw * rh * 4);
    for (int y = 0; y < rh; y++)
        memcpy(&out[(size_t)y * rw * 4], &px[((size_t)(ry + y) * w + rx) * 4], (size_t)rw * 4);
    return out;
}
// Cut one rect from an already-keyed buffer → crop → auto-trim to the alpha bbox → write a PNG.
static bool sprite_cut_one(const std::vector<unsigned char>& keyed, int w, int h,
                           const SpriteRect& r, const std::string& outPath) {
    int ow, oh;
    std::vector<unsigned char> c = sprite_crop(keyed, w, h, r.x, r.y, r.w, r.h, ow, oh);
    int x0, y0, x1, y1;
    if (sprite_alpha_bbox(c, ow, oh, x0, y0, x1, y1)) {
        int tw, th;
        std::vector<unsigned char> t = sprite_crop(c, ow, oh, x0, y0, x1 - x0 + 1, y1 - y0 + 1, tw, th);
        return stbi_write_png(outPath.c_str(), tw, th, 4, t.data(), tw * 4) != 0;
    }
    return stbi_write_png(outPath.c_str(), ow, oh, 4, c.data(), ow * 4) != 0;  // fully transparent → as-is
}
// Export rects → library/images/<prefix>-NN.png (keying applied to a copy first). Returns the count written.
static int sprite_export_to_library(const std::vector<unsigned char>& src, int w, int h,
                                    int kr, int kg, int kb, float fuzz,
                                    const std::vector<SpriteRect>& rects, const std::string& prefix) {
    std::vector<unsigned char> keyed = src;
    sprite_color_key(keyed, kr, kg, kb, fuzz);
    std::string dir = g_libraryDir + "/images";
    ensure_dir(dir);
    int n = 0;
    for (size_t i = 0; i < rects.size(); i++) {
        char fn[160];
        snprintf(fn, sizeof fn, "%s/%s-%02d.png", dir.c_str(), prefix.c_str(), (int)i + 1);
        if (sprite_cut_one(keyed, w, h, rects[i], fn)) n++;
    }
    if (n) g_libraryDirty = true;
    return n;
}

// Sibling path of a source item's painted mask (default <base>.mask.png next to the source).
static std::string item_mask_path(const std::string& path) {
    json side = lib_load_sidecar(path);
    std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    std::string def = strip_ext(path.substr(path.find_last_of("/\\") + 1)) + ".mask.png";
    std::string mf = (side.contains("mask") && side["mask"].is_object()) ? side["mask"].value("file", def) : def;
    return dir + mf;
}
// Multiply each pixel's alpha by the painted mask (grayscale PNG, 255=keep / 0=erased) — the
// brush + box-fill cleanup the colour key / rembg can't do. Non-destructive: rides the sidecar.
static void item_apply_mask(const std::string& path, std::vector<unsigned char>& buf, int w, int h) {
    json side = lib_load_sidecar(path);
    if (!side.contains("mask") || !side["mask"].is_object() || !side["mask"].value("enabled", true)) return;
    int mw, mh, mn; unsigned char* md = stbi_load(item_mask_path(path).c_str(), &mw, &mh, &mn, 1);  // 1 chan
    if (!md) return;
    if (mw == w && mh == h)
        for (size_t i = 0; i < (size_t)w * h; i++) buf[i * 4 + 3] = (unsigned char)((int)buf[i * 4 + 3] * md[i] / 255);
    stbi_image_free(md);
}
// Crop the buffer to the sidecar crop rect [x,y,w,h] (source px), updating w,h. After the mask so
// the mask is authored at full source dims. Non-destructive (the rect lives in the sidecar).
static bool item_apply_crop(const std::string& path, std::vector<unsigned char>& buf, int& w, int& h) {
    json side = lib_load_sidecar(path);
    if (!side.contains("crop") || !side["crop"].is_array() || side["crop"].size() != 4) return false;
    int rx = side["crop"][0].get<int>(), ry = side["crop"][1].get<int>(), rw = side["crop"][2].get<int>(), rh = side["crop"][3].get<int>();
    if (rw <= 0 || rh <= 0 || (rx == 0 && ry == 0 && rw >= w && rh >= h)) return false;
    int ow, oh; std::vector<unsigned char> c = sprite_crop(buf, w, h, rx, ry, rw, rh, ow, oh);
    buf.swap(c); w = ow; h = oh;
    return true;
}

// ── painted-mask authoring (shared by the Viewer paint tool + headless --lib-mask) ──
// The mask is a grayscale buffer at SOURCE dims: 255=keep, 0=erased. Brush dabs feather to 0/255.
static std::vector<unsigned char> mask_load_or_blank(const std::string& path, int w, int h) {
    int mw, mh, mn; unsigned char* md = stbi_load(item_mask_path(path).c_str(), &mw, &mh, &mn, 1);
    std::vector<unsigned char> m((size_t)w * h, 255);
    if (md) { if (mw == w && mh == h) m.assign(md, md + (size_t)w * h); stbi_image_free(md); }
    return m;
}
static void mask_save(const std::string& path, const std::vector<unsigned char>& m, int w, int h) {
    std::string mp = item_mask_path(path);
    stbi_write_png(mp.c_str(), w, h, 1, m.data(), w);
    json side = lib_load_sidecar(path);
    json mb = side.value("mask", json::object());
    mb["enabled"] = true;
    mb["file"] = mp.substr(mp.find_last_of("/\\") + 1);
    side["mask"] = mb;
    lib_save_sidecar(path, side);
}
// Soft round brush dab → blend toward `value` (0 erase / 255 restore) by coverage; feather is the
// 0..1 fraction of the radius that fades (0 = hard edge, 1 = fully soft).
static void mask_dab(std::vector<unsigned char>& m, int w, int h, float cx, float cy, float radius, float feather, int value) {
    int x0 = std::max(0, (int)(cx - radius)), x1 = std::min(w - 1, (int)(cx + radius));
    int y0 = std::max(0, (int)(cy - radius)), y1 = std::min(h - 1, (int)(cy + radius));
    float inner = radius * (1.0f - std::min(1.0f, std::max(0.0f, feather)));
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float d = std::hypot((float)x - cx, (float)y - cy);
            if (d > radius) continue;
            float a = (d <= inner) ? 1.0f : 1.0f - (d - inner) / std::max(1e-3f, radius - inner);   // 1 center → 0 edge
            unsigned char& mv = m[(size_t)y * w + x];
            mv = (unsigned char)(mv + (value - (int)mv) * a + 0.5f);   // blend toward target by coverage
        }
}
static void mask_box(std::vector<unsigned char>& m, int w, int h, int rx, int ry, int rw, int rh, int value) {
    for (int y = std::max(0, ry); y < std::min(h, ry + rh); y++)
        for (int x = std::max(0, rx); x < std::min(w, rx + rw); x++) m[(size_t)y * w + x] = (unsigned char)value;
}

// ───────────────────────── defocus blur (footage backdrop) ─────────────────
// A keyframeable `blur` on a textured clip swaps in a cached, low-res Gaussian-blurred
// copy of its source — the "text over bright footage = blur the bg, NOT a dim scrim" rule.
// CPU separable Gaussian over a downsampled copy → cheap HEAVY blur, upscaled for free by
// the compositor's bilinear AddImage. Cached by (uri | sigma-bucket) — the project's
// param-hash texture-cache idea — so a static hold is free and a smooth blur-in only pays
// for the few buckets it passes through. Pure CPU ⇒ identical in preview + export, with no
// render-target/device-state juggling (a live shader pass can come later if effects need it).
struct SrcPix { std::vector<unsigned char> px; int w = 0, h = 0; };
static std::map<std::string, SrcPix> g_srcCache;
static std::map<std::string, ID3D11ShaderResourceView*> g_blurCache;
static std::string canon_emotion(const std::string& e);

static const SrcPix* get_src_pixels(const std::string& uri) {
    auto it = g_srcCache.find(uri);
    if (it != g_srcCache.end()) return it->second.w ? &it->second : nullptr;
    SrcPix s;
    int n = 0;
    std::string path = resolve_asset(uri);
    unsigned char* d = stbi_load(path.c_str(), &s.w, &s.h, &n, 4);
    if (d) { s.px.assign(d, d + (size_t)s.w * s.h * 4); stbi_image_free(d); }
    g_srcCache[uri] = std::move(s);
    SrcPix& r = g_srcCache[uri];
    return r.w ? &r : nullptr;
}

// ── Avatar framing (face-anchored) ──────────────────────────────────────────────────────────────
// The sprite ALWAYS composites whole — framing NEVER pixel-crops it (a previous attempt UV-cropped
// to a "bust" and she rendered as a torso sliced at the thigh with a hard flat edge). Instead we
// DETECT THE FACE and anchor every framing on it: "bust"/"closeup" scale the full sprite by the
// detected face size and place the eye-line at a fixed spot down the frame, so the lower body runs
// off the BOTTOM frame edge (cut by the canvas clip-rect — the only "crop" is the natural frame
// boundary). Anchoring on the face (not the alpha bbox) is robust across poses: the head can sit
// anywhere (front / 3-4 turn / floating) and the framing tracks it. "full"/"floating" keep the
// authored transform verbatim; manual pos/scale tweaks layer on top for in-between framings.

static std::string avatar_framing(const json& params, const std::string& emotion) {
    std::string f = (params.is_object() && params.contains("framing") && params["framing"].is_string())
                  ? params["framing"].get<std::string>() : std::string();
    if (!f.empty()) return f;
    if (canon_emotion(emotion) == "floating" || (params.is_object() && params.value("floating", false)))
        return "floating";
    return "raw";   // unset → authored transform verbatim (legacy/manual beats). bust/closeup/full are
                    // the face-anchored modes; new clips are stamped "bust" at creation.
}

// Detected face for an avatar sprite, in SOURCE px: center-x, eye-line y, and face width — the
// anchor for ALL framings + the emotion-picker thumbnails. Detection keys on this art's pale-pink
// SKIN (bright, low-saturation, green is the min channel — distinct from the purple hair/horns and
// the saturated eyes) inside the head band, taking the 8/92 percentile box so stray skin (hands)
// doesn't skew it. Works on front / 3-4 / floating poses because it follows the skin. Falls back to
// a bbox estimate when no skin is found (e.g. a non-character sprite). Cached per uri.
struct AvatarFace { bool ok = false; float cx = 0, eyeY = 0, w = 1; };
// Detect the face box in a decoded RGBA sprite (raw px, w×h) in SOURCE px. Shared by avatar_face (UI
// thread, uri-cached) and the async emotion-thumbnail loader — ONE skin-scan, so the two never drift.
static AvatarFace face_from_pixels(const unsigned char* px, int w, int h) {
    AvatarFace fa;
    int x0, y0, x1, y1;
    if (px && w > 0 && sprite_alpha_bbox(px, w, h, x0, y0, x1, y1, 8)) {
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        int headBot = y0 + (int)(bh * 0.58f);                    // faces live in the upper body
        std::vector<int> sxv, syv; sxv.reserve(8192); syv.reserve(8192);
        for (int y = y0; y <= y1 && y <= headBot; y++) {
            const unsigned char* row = &px[(size_t)y * w * 4];
            for (int x = x0; x <= x1; x++) {
                const unsigned char* p = row + (size_t)x * 4;
                int r = p[0], g = p[1], b = p[2];
                if (p[3] <= 40) continue;
                int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                if (mx > 222 && (mx - mn) * 100 < 20 * mx && g <= r + 2 && g <= b + 2) {   // bright, low-sat, pink
                    sxv.push_back(x); syv.push_back(y);
                }
            }
        }
        if (sxv.size() >= 30) {
            std::sort(sxv.begin(), sxv.end()); std::sort(syv.begin(), syv.end());   // 8/92 pct box (robust)
            size_t n = sxv.size(), lo = (size_t)(n * 0.08), hi = std::min(n - 1, (size_t)(n * 0.92));
            float fx0 = (float)sxv[lo], fx1 = (float)sxv[hi], fy0 = (float)syv[lo], fy1 = (float)syv[hi];
            fa.cx = (fx0 + fx1) * 0.5f;
            fa.eyeY = fy0 + 0.62f * (fy1 - fy0);                 // eyes sit below the forehead-heavy skin top
            fa.w = std::max(8.0f, fx1 - fx0);
            fa.ok = true;
        }
        if (!fa.ok) { fa.cx = x0 + bw * 0.5f; fa.eyeY = y0 + bh * 0.20f; fa.w = bw * 0.34f; fa.ok = true; }  // fallback
    }
    return fa;
}
// Per-sprite face-box OVERRIDE: a sidecar "face":{cx,eyeY,w} (SOURCE px), authored by the inspector
// "Tune face box" gizmo, WINS over the pale-skin auto-detector. The detector measures the horizontal
// skin spread over the upper body, so poses that bare more shoulder/chest/arm skin (smug, neutral)
// report a WIDER "face" than a hands-in pose (explaining) → the face-anchored fit (scl ∝ 1/faceW)
// renders them at DIFFERENT apparent sizes at the same clip scale. A hand-tuned box per sprite pins
// the face width to a CONSISTENT value ⇒ every room-shot pose matches at scale 1.0.
static std::map<std::string, AvatarFace> g_faceCache;    // uri → face (override or detected)
static void invalidate_face_cache() { g_faceCache.clear(); }   // after a gizmo edit (cheap; rebuilt lazily)
static bool sidecar_face_override(const std::string& uri, AvatarFace& fa) {
    json side = lib_load_sidecar(resolve_asset(uri));
    if (!side.contains("face") || !side["face"].is_object()) return false;
    const json& fj = side["face"];
    fa.cx = fj.value("cx", 0.0f); fa.eyeY = fj.value("eyeY", 0.0f);
    fa.w = std::max(8.0f, fj.value("w", 0.0f)); fa.ok = fa.w > 8.0f;
    return fa.ok;
}
static AvatarFace avatar_face(const std::string& uri) {
    auto it = g_faceCache.find(uri);
    if (it != g_faceCache.end()) return it->second;
    AvatarFace fa;
    if (!sidecar_face_override(uri, fa)) {
        const SrcPix* sp = uri.empty() ? nullptr : get_src_pixels(uri);
        fa = (sp && sp->w > 0) ? face_from_pixels(sp->px.data(), sp->w, sp->h) : AvatarFace{};
    }
    g_faceCache[uri] = fa;
    return fa;
}

// On-screen rect for the FULL sprite under a "bust"/"closeup" framing (else ok=false → the caller
// uses the authored transform). The detected face WIDTH is scaled to a target fraction of the frame
// HEIGHT and the eye-line is pinned at a target spot down the frame; the clip's pos nudges placement
// and its scale multiplies the fit (1 = base framing). Negative sclX keeps the mirror (a.x > b.x).
struct AvatarFit { bool ok = false; ImVec2 a, b; };
static AvatarFit avatar_fit(const std::string& uri, Tex* tex, const std::string& framing,
                            ImVec2 f0, float fw, float fh, float posX, float posY,
                            float sclX, float sclY, bool solo = false, bool overFootage = false) {
    AvatarFit f;
    bool closeup = (framing == "closeup" || framing == "face");
    bool bust = (framing == "bust");
    bool full = (framing == "full" || framing == "floating");   // full-body, face-anchored (consistent placement)
    if ((!closeup && !bust && !full) || !tex || tex->w <= 0 || tex->h <= 0) return f;
    AvatarFace fc = avatar_face(uri);
    if (!fc.ok) return f;
    float faceFrac  = closeup ? 0.42f : bust ? 0.30f : 0.34f;   // detected face width as a fraction of the frame WIDTH
    float eyeFrameY = closeup ? 0.45f : bust ? 0.48f : 0.24f;   // eye-line down the frame (bust sits lower — host default)
    if (overFootage) {          // FULLSCREEN media under her: the footage is the shot — she floats
                                // over it as a small cornered commentator (feet off-frame, low),
                                // never a center-frame presenter head covering the hero footage.
        faceFrac  = closeup ? 0.27f : bust ? 0.19f : 0.22f;
        eyeFrameY = 0.66f;
    }
    if (fh > fw * 1.2f) {   // PORTRAIT (shorts): the tall frame fits her whole body, so the landscape
                            // fractions read enormous + centered — smaller face, eye-line in the lower
                            // third → she presents from the BOTTOM band under the content.
        if (solo) {         // ROOM shot, nothing else on screen: she OWNS the tall frame — but NOT edge to
                            // edge: YouTube-Shorts wants the top (status/back) + bottom (caption/controls
                            // strip) clear, so she's sized + eye-lined to sit in the MIDDLE band with her
                            // horns below the top-unsafe strip and her body fading above the bottom one.
            faceFrac  = closeup ? 0.50f : bust ? 0.58f : 0.52f;
            eyeFrameY = closeup ? 0.48f : bust ? 0.50f : 0.44f;
        } else {
            // CONTENT beat: content sits in the TOP band (below), so the host is BIG at the BOTTOM
            // (feet off-frame) instead of a small strip — she shares the frame vertically, not shrunken.
            faceFrac  = closeup ? 0.40f : bust ? 0.40f : 0.36f;
            eyeFrameY = closeup ? 0.50f : bust ? 0.72f : 0.60f;
        }
    }
    float scl = (faceFrac * fw) / fc.w * 0.5f * (fabsf(sclX) + fabsf(sclY));   // clip scale nudges the fit
    if (scl <= 0) return f;
    float figW = tex->w * scl, figH = tex->h * scl;
    float ax = f0.x + fw * 0.5f + posX - fc.cx * scl;          // face centered horizontally + pos.x
    float ay = f0.y + eyeFrameY * fh + posY - fc.eyeY * scl;   // eye-line at eyeFrameY down the frame + pos.y
    f.ok = true;
    if (sclX < 0) { f.a = ImVec2(ax + figW, ay); f.b = ImVec2(ax, ay + figH); }   // mirror
    else          { f.a = ImVec2(ax, ay);        f.b = ImVec2(ax + figW, ay + figH); }
    return f;
}

// ── async emotion-picker thumbnails ─────────────────────────────────────────────────────────────
// The avatar inspector's "Emotion poses" grid + the preset-rig viewer show ~a dozen face-cropped pose
// thumbnails. Loading them inline (decode PNG → matte → face-scan, ×N) froze the UI on the FIRST click
// of an avatar clip. This loads each on a worker thread into an ISOLATED store (own cache + lock, so it
// never races the UI-thread g_texCache/g_srcCache); the pickers poll it each frame and show a "…"
// placeholder until the thumb pops in. Device-side SRV creation is thread-safe (the device is created
// without D3D11_CREATE_DEVICE_SINGLETHREADED), so the worker can build the texture itself.
struct EmoThumb {
    ID3D11ShaderResourceView* srv = nullptr;
    ImVec2 uv0 = ImVec2(0, 0), uv1 = ImVec2(1, 1);   // face-zoom window (matches avatar_face_uv)
    int state = 0;                                    // 0 unseen · 1 loading · 2 ready (srv may be null on failure)
};
static std::map<std::string, EmoThumb> g_emoThumb;   // sprite uri → thumbnail
static CRITICAL_SECTION g_emoThumbCS;

static DWORD WINAPI emo_thumb_worker(LPVOID arg) {
    std::string uri = *reinterpret_cast<std::string*>(arg);
    delete reinterpret_cast<std::string*>(arg);
    EmoThumb th;
    int w = 0, h = 0, n = 0;
    std::string rp = resolve_asset(uri);
    unsigned char* d = stbi_load(rp.c_str(), &w, &h, &n, 4);
    if (d) {
        AvatarFace fc = face_from_pixels(d, w, h);       // face-zoom UV from the RAW source (like avatar_face_uv)
        if (fc.ok) {
            float half = fc.w * 0.95f, cx = fc.cx, cy = fc.eyeY - fc.w * 0.12f;
            float rx0 = std::max(0.0f, cx - half), ry0 = std::max(0.0f, cy - half);
            float rx1 = std::min((float)w, cx + half), ry1 = std::min((float)h, cy + half);
            if (rx1 > rx0 + 1 && ry1 > ry0 + 1) { th.uv0 = ImVec2(rx0 / w, ry0 / h); th.uv1 = ImVec2(rx1 / w, ry1 / h); }
        }
        int tw = w, tht = h;                             // processed pixels → SRV (same sidecar pipeline as get_texture)
        std::vector<unsigned char> buf;
        if (!item_removebg(rp, d, tw, tht, buf)) buf.assign(d, d + (size_t)tw * tht * 4);
        item_apply_mask(rp, buf, tw, tht);
        item_apply_crop(rp, buf, tw, tht);
        th.srv = make_rgba_srv(buf.data(), tw, tht);
        stbi_image_free(d);
    }
    EnterCriticalSection(&g_emoThumbCS);
    EmoThumb& slot = g_emoThumb[uri];
    slot.srv = th.srv; slot.uv0 = th.uv0; slot.uv1 = th.uv1; slot.state = 2;
    LeaveCriticalSection(&g_emoThumbCS);
    return 0;
}

// Ready thumbnail for `uri` → true + fills `out`; false while it loads (kicks the async load exactly once).
static bool get_emo_thumb(const std::string& uri, EmoThumb& out) {
    if (uri.empty()) return false;
    bool kick = false, ready = false;
    EnterCriticalSection(&g_emoThumbCS);
    auto it = g_emoThumb.find(uri);
    if (it == g_emoThumb.end()) { g_emoThumb[uri].state = 1; kick = true; }
    else if (it->second.state == 2) { out = it->second; ready = true; }
    LeaveCriticalSection(&g_emoThumbCS);
    if (kick) {
        auto* a = new std::string(uri);
        HANDLE hth = CreateThread(nullptr, 0, emo_thumb_worker, a, 0, nullptr);
        if (hth) CloseHandle(hth);
        else { delete a; EnterCriticalSection(&g_emoThumbCS); g_emoThumb[uri].state = 2; LeaveCriticalSection(&g_emoThumbCS); }
    }
    return ready;
}

// Blurred SRV for `uri` at the given source-space gaussian sigma (px), or null. Low-res +
// cached; the compositor sizes the quad from the ORIGINAL Tex dims, so the small blurred
// texture just upscales (bilinear) into place.
// CPU-processed copy of a source image: optional defocus blur AND optional color grade
// (saturation + contrast), param-hash cached (key = uri|blur|sat|contrast). Blur downsamples to
// a working res; grade-only runs at full res. Returns null when nothing to do (caller keeps the
// original SRV). Identical in preview + export (both walk composite_frame). Temperature/tint and
// dim are NOT here — they're a cheap per-channel tint at draw time.
static ID3D11ShaderResourceView* get_processed_srv(const std::string& uri, float sigma,
                                                   float sat, float con) {
    bool doBlur  = sigma > 0.4f;
    bool doGrade = (sat < 0.999f || sat > 1.001f || con < 0.999f || con > 1.001f);
    if (!doBlur && !doGrade) return nullptr;
    int bucket = (int)lroundf(sigma * 0.5f);          // ~2px-sigma buckets: smooth + reusable
    if (bucket < 1) bucket = 1;
    int si = (int)lroundf(sat * 50.0f), ci = (int)lroundf(con * 50.0f);  // 0.02 grade buckets
    char key[700];
    snprintf(key, sizeof key, "%s|b%d|s%d|c%d", uri.c_str(), doBlur ? bucket : 0, si, ci);
    auto it = g_blurCache.find(key);
    if (it != g_blurCache.end()) return it->second;   // hit (may be a negative-cached null)
    g_blurCache[key] = nullptr;
    const SrcPix* sp = get_src_pixels(uri);
    if (!sp) return nullptr;
    int lw, lh;
    std::vector<float> ch[4];
    if (doBlur) {
        float sig = bucket * 2.0f;
        int ds = (int)lroundf(sig / 2.5f);
        if (ds < 2) ds = 2;
        if (ds > 10) ds = 10;                         // working-res downsample factor
        lw = std::max(1, sp->w / ds); lh = std::max(1, sp->h / ds);
        int N = lw * lh;
        for (int c = 0; c < 4; c++) ch[c].assign(N, 0.0f);
        // box-average downsample (sp->w×sp->h → lw×lh)
        for (int y = 0; y < lh; y++)
            for (int x = 0; x < lw; x++) {
                int sx0 = x * sp->w / lw, sx1 = std::max(sx0 + 1, (x + 1) * sp->w / lw);
                int sy0 = y * sp->h / lh, sy1 = std::max(sy0 + 1, (y + 1) * sp->h / lh);
                float a0 = 0, a1 = 0, a2 = 0, a3 = 0; int cnt = 0;
                for (int yy = sy0; yy < sy1; yy++)
                    for (int xx = sx0; xx < sx1; xx++) {
                        const unsigned char* px = &sp->px[((size_t)yy * sp->w + xx) * 4];
                        a0 += px[0]; a1 += px[1]; a2 += px[2]; a3 += px[3]; cnt++;
                    }
                int i = y * lw + x; float inv = cnt ? 1.0f / cnt : 0.0f;
                ch[0][i] = a0 * inv; ch[1][i] = a1 * inv; ch[2][i] = a2 * inv; ch[3][i] = a3 * inv;
            }
        // separable gaussian at working res
        float sw = sig / ds; if (sw < 0.5f) sw = 0.5f;
        int rad = std::max(1, (int)ceilf(sw * 3.0f));
        std::vector<float> k(rad + 1); float ksum = 0;
        for (int i = 0; i <= rad; i++) { k[i] = expf(-(float)(i * i) / (2.0f * sw * sw)); ksum += (i == 0 ? k[i] : 2 * k[i]); }
        for (float& v : k) v /= ksum;
        std::vector<float> tmp(N);
        for (int c = 0; c < 4; c++)
            for (int pass = 0; pass < 2; pass++) {
                bool horiz = (pass == 0);
                for (int y = 0; y < lh; y++)
                    for (int x = 0; x < lw; x++) {
                        float acc = ch[c][y * lw + x] * k[0];
                        for (int i = 1; i <= rad; i++) {
                            int xp, xn, yp, yn;
                            if (horiz) { xp = std::max(0, x - i); xn = std::min(lw - 1, x + i); yp = yn = y; }
                            else       { yp = std::max(0, y - i); yn = std::min(lh - 1, y + i); xp = xn = x; }
                            acc += (ch[c][yp * lw + xp] + ch[c][yn * lw + xn]) * k[i];
                        }
                        tmp[y * lw + x] = acc;
                    }
                ch[c].swap(tmp);
            }
    } else {
        // grade-only: full resolution, no downsample/blur
        lw = sp->w; lh = sp->h;
        int N = lw * lh;
        for (int c = 0; c < 4; c++) ch[c].assign(N, 0.0f);
        for (int i = 0; i < N; i++) {
            const unsigned char* px = &sp->px[(size_t)i * 4];
            ch[0][i] = px[0]; ch[1][i] = px[1]; ch[2][i] = px[2]; ch[3][i] = px[3];
        }
    }
    int N = lw * lh;
    if (doGrade)                                       // saturation around luma, then contrast around mid-gray
        for (int i = 0; i < N; i++) {
            float r = ch[0][i], g = ch[1][i], b = ch[2][i];
            float l = 0.299f * r + 0.587f * g + 0.114f * b;
            r = l + (r - l) * sat; g = l + (g - l) * sat; b = l + (b - l) * sat;
            r = (r - 127.5f) * con + 127.5f; g = (g - 127.5f) * con + 127.5f; b = (b - 127.5f) * con + 127.5f;
            ch[0][i] = r; ch[1][i] = g; ch[2][i] = b;
        }
    std::vector<unsigned char> out((size_t)N * 4);
    for (int i = 0; i < N; i++)
        for (int c = 0; c < 4; c++) {
            float v = ch[c][i]; out[(size_t)i * 4 + c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = lw; d.Height = lh; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = out.data(); sd.SysMemPitch = (UINT)lw * 4;
    ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &tex)) && tex) {
        g_dev->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
    }
    g_blurCache[key] = srv;
    return srv;
}

// ───────────────────────────── audio (wav) ────────────────────────────────
// Minimal RIFF/PCM16 WAV reader. `read_wav16` decodes to mono float (channels averaged) +
// sample rate; the timeline waveform builds a peak envelope from it and the transport mixer
// (below) resamples it for playback. (Our providers write 16-bit PCM via soundfile.)
static bool read_wav16(const std::string& path, std::vector<float>& mono, int& srate) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(sz > 0 ? (size_t)sz : 0);
    bool ok = false;
    if (sz > 44 && fread(buf.data(), 1, (size_t)sz, f) == (size_t)sz &&
        memcmp(buf.data(), "RIFF", 4) == 0 && memcmp(buf.data() + 8, "WAVE", 4) == 0) {
        int channels = 1, sr = 24000, bits = 16;
        size_t off = 12, dataOff = 0, dataLen = 0;
        while (off + 8 <= (size_t)sz) {
            const unsigned char* p = buf.data() + off;
            unsigned len; memcpy(&len, p + 4, 4);
            if (memcmp(p, "fmt ", 4) == 0 && off + 24 <= (size_t)sz) {
                memcpy(&channels, p + 10, 2); channels &= 0xffff;
                memcpy(&sr, p + 12, 4);
                memcpy(&bits, p + 22, 2); bits &= 0xffff;
            } else if (memcmp(p, "data", 4) == 0) {
                dataOff = off + 8; dataLen = len; break;
            }
            off += 8 + len + (len & 1);
        }
        if (dataOff && bits == 16 && channels >= 1) {
            if (dataOff + dataLen > (size_t)sz) dataLen = (size_t)sz - dataOff;
            size_t nframes = dataLen / (2 * (size_t)channels);
            const int16_t* s = (const int16_t*)(buf.data() + dataOff);
            mono.resize(nframes);
            for (size_t i = 0; i < nframes; ++i) {
                int acc = 0;
                for (int c = 0; c < channels; ++c) acc += s[i * channels + c];
                mono[i] = (float)acc / (channels * 32768.0f);
            }
            srate = sr;
            ok = true;
        }
    }
    fclose(f);
    return ok;
}

// Full decoded PCM (mono float + rate) for transport playback — lazy + cached, separate from
// the downsampled waveform envelope. WAV natively; anything else (the mp3 music beds, ogg, …)
// falls back to a whole-file libav decode, so non-WAV audio previews too (export always
// hands the source file to ffmpeg directly).
struct Pcm { std::vector<float> mono; int srate = 0; double dur = 0; float rms = 0; bool ok = false; };
static std::map<std::string, Pcm> g_pcmCache;
static bool decode_audio_libav(const std::string& srcPath, Pcm& pc);   // below (SLOP_LIBAV; stub without)
static Pcm* get_pcm(const std::string& uri);                            // below — WAV + libav fallback

struct Wave { std::vector<float> env; double dur = 0; bool ok = false; };
static std::map<std::string, Wave> g_waveCache;

static Wave* get_wave(const std::string& uri, int buckets = 1024) {
    if (uri.empty()) return nullptr;
    auto it = g_waveCache.find(uri);
    if (it != g_waveCache.end()) return it->second.ok ? &it->second : nullptr;
    Wave w;
    // envelope from the shared decoded-PCM cache (so non-WAV audio gets a waveform too)
    Pcm* pc = get_pcm(uri);
    if (pc) {
        const std::vector<float>& mono = pc->mono;
        size_t nframes = mono.size();
        w.dur = pc->dur;
        w.env.assign(buckets, 0.f);
        for (int bk = 0; bk < buckets; ++bk) {
            size_t a = (size_t)((double)bk / buckets * nframes);
            size_t b = (size_t)((double)(bk + 1) / buckets * nframes);
            float pk = 0.f;
            for (size_t i = a; i < b && i < nframes; ++i) {
                float v = mono[i] < 0 ? -mono[i] : mono[i];
                if (v > pk) pk = v;
            }
            w.env[bk] = pk;
        }
        w.ok = true;
    }
    g_waveCache[uri] = w;
    return w.ok ? &g_waveCache[uri] : nullptr;
}

static Pcm* get_pcm(const std::string& uri) {
    if (uri.empty()) return nullptr;
    auto it = g_pcmCache.find(uri);
    if (it != g_pcmCache.end()) return it->second.ok ? &it->second : nullptr;
    Pcm pc;
    bool got = read_wav16(resolve_asset(uri), pc.mono, pc.srate);
    if (!got) got = decode_audio_libav(resolve_asset(uri), pc);        // non-WAV fallback
    if (got && pc.srate > 0 && !pc.mono.empty()) {
        pc.dur = (double)pc.mono.size() / (double)pc.srate;
        // gated loudness for normalization: RMS over the SPEECH only (samples above a peak-relative
        // gate + an absolute floor) so a line with long pauses/quiet tails isn't mis-measured —
        // the fix behind "one line is way too quiet" auto-leveling.
        float pk = 0; for (float s : pc.mono) { float a = s < 0 ? -s : s; if (a > pk) pk = a; }
        float gate = std::max(pk * 0.15f, 0.02f);
        double s2 = 0; size_t gn = 0;
        for (float s : pc.mono) { float a = s < 0 ? -s : s; if (a >= gate) { s2 += (double)s * s; gn++; } }
        pc.rms = gn ? (float)std::sqrt(s2 / (double)gn) : 0.f;    // ≈ perceived speech loudness
        pc.ok = true;
    } else pc.ok = false;
    g_pcmCache[uri] = pc;
    return pc.ok ? &g_pcmCache[uri] : nullptr;
}

// Decode a VIDEO file's audio track → mono float Pcm (native sample rate), lazy + cached. Mirrors
// the VideoDecoder open path but pulls the best AUDIO stream and resamples to mono float via swr.
// Returns null if the file has no audio / can't decode. Lets video clips carry their own sound —
// mixed at a low default volume in collect_audio (see there). Whole-track decode (one-time cost).
#ifdef SLOP_LIBAV
// Decode ANY audio libav can open (mp3/ogg/m4a, or a video file's audio track) → mono float
// Pcm at the source rate. Shared by get_video_pcm and the get_pcm non-WAV fallback (the mp3
// music beds used to be silent in preview / export-only). Whole-file decode; callers cache.
static bool decode_audio_libav(const std::string& srcPath, Pcm& pc) {
    if (srcPath.empty()) return false;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, srcPath.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (as >= 0) {
            AVStream* st = fmt->streams[as];
            const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
            AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
            if (dec && avcodec_parameters_to_context(dec, st->codecpar) >= 0 && avcodec_open2(dec, codec, nullptr) >= 0) {
                int srate = dec->sample_rate > 0 ? dec->sample_rate : 48000;
                pc.srate = srate;
                AVChannelLayout outLay; av_channel_layout_default(&outLay, 1);    // mono out
                AVChannelLayout inLay; av_channel_layout_copy(&inLay, &dec->ch_layout);
                if (inLay.nb_channels <= 0) { av_channel_layout_uninit(&inLay); av_channel_layout_default(&inLay, 2); }
                SwrContext* swr = nullptr;
                if (swr_alloc_set_opts2(&swr, &outLay, AV_SAMPLE_FMT_FLT, srate, &inLay, dec->sample_fmt, srate, 0, nullptr) >= 0
                    && swr && swr_init(swr) >= 0) {
                    AVPacket* pkt = av_packet_alloc(); AVFrame* fr = av_frame_alloc();
                    auto pull = [&](const uint8_t** in, int inN) {              // append converted mono floats
                        int outMax = (int)swr_get_out_samples(swr, inN);
                        if (outMax <= 0) return 0;
                        size_t base = pc.mono.size(); pc.mono.resize(base + outMax);
                        uint8_t* outp = (uint8_t*)(pc.mono.data() + base);
                        int got = swr_convert(swr, &outp, outMax, in, inN);
                        pc.mono.resize(base + (got > 0 ? got : 0));
                        return got;
                    };
                    while (av_read_frame(fmt, pkt) >= 0) {
                        if (pkt->stream_index == as && avcodec_send_packet(dec, pkt) >= 0)
                            while (avcodec_receive_frame(dec, fr) >= 0) pull((const uint8_t**)fr->extended_data, fr->nb_samples);
                        av_packet_unref(pkt);
                    }
                    avcodec_send_packet(dec, nullptr);                          // flush decoder
                    while (avcodec_receive_frame(dec, fr) >= 0) pull((const uint8_t**)fr->extended_data, fr->nb_samples);
                    while (pull(nullptr, 0) > 0) {}                             // flush resampler
                    av_frame_free(&fr); av_packet_free(&pkt);
                    pc.dur = pc.srate ? (double)pc.mono.size() / pc.srate : 0;
                    pc.ok = !pc.mono.empty();
                }
                if (swr) swr_free(&swr);
                av_channel_layout_uninit(&outLay); av_channel_layout_uninit(&inLay);
            }
            if (dec) avcodec_free_context(&dec);
        }
    }
    avformat_close_input(&fmt);
    return pc.ok;
}
static std::map<std::string, Pcm> g_videoPcmCache;
static Pcm* get_video_pcm(const std::string& srcPath) {
    if (srcPath.empty()) return nullptr;
    auto it = g_videoPcmCache.find(srcPath);
    if (it != g_videoPcmCache.end()) return it->second.ok ? &it->second : nullptr;
    Pcm pc;
    decode_audio_libav(srcPath, pc);
    if (pc.ok && !pc.mono.empty()) {   // decode_audio_libav doesn't set rms (get_pcm does) — compute a level for silence detection
        double s2 = 0; for (float v : pc.mono) s2 += (double)v * v;
        pc.rms = (float)std::sqrt(s2 / (double)pc.mono.size());
    }
    g_videoPcmCache[srcPath] = std::move(pc);
    Pcm& r = g_videoPcmCache[srcPath];
    return r.ok ? &r : nullptr;
}
#else
static bool decode_audio_libav(const std::string&, Pcm&) { return false; }
static Pcm* get_video_pcm(const std::string&) { return nullptr; }
#endif

// ─────────────────────── playback rate (time-stretch) ───────────────────────
// Pitch-preserving time-stretch (WSOLA) of a mono signal by `rate` (output ≈ input/rate):
// overlap-add windowed analysis frames at a rate-scaled hop, picking each frame within a small
// search window to best continue the previous one (the "waveform-similarity" of WSOLA → no
// chipmunk pitch shift, minimal warble). Used by the PREVIEW mixer so a sped/slowed clip
// auditions at the same pitch the ffmpeg `atempo` export produces. Cached per (uri|rate-bucket).
static std::vector<float> wsola_stretch(const std::vector<float>& in, double rate, int srate) {
    size_t N = in.size();
    if (rate <= 0.001 || std::fabs(rate - 1.0) < 1e-3 || N < 2048) return in;   // identity
    int frame = std::max(512, (srate / 40) | 1);     // ~25 ms analysis frame (odd)
    int Hs = frame / 2, ov = frame / 2, seek = std::max(16, frame / 6);
    std::vector<float> win(frame);
    for (int i = 0; i < frame; i++) win[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (frame - 1));  // Hann
    size_t outLen = (size_t)(N / rate) + frame + 2;
    std::vector<float> out(outLen, 0.f), wsum(outLen, 0.f);
    double anaIdeal = 0.0;
    int p = 0;             // chosen analysis start of the current frame
    size_t synPos = 0;
    while (synPos + frame < outLen) {
        for (int k = 0; k < frame; k++) {            // overlap-add this windowed frame
            int si = p + k;
            if (si >= 0 && si < (int)N) { out[synPos + k] += in[si] * win[k]; wsum[synPos + k] += win[k]; }
        }
        int target = p + Hs;                         // where the NEXT frame should continue from
        synPos += Hs;
        anaIdeal += rate * Hs;
        int center = (int)anaIdeal;
        if (center + frame >= (int)N) break;
        int best = center; double bestS = -1e30;     // WSOLA: search ±seek for the best continuation
        for (int d = -seek; d <= seek; d++) {
            int cand = center + d;
            if (cand < 0 || cand + ov >= (int)N || target + ov >= (int)N) continue;
            double sc = 0;
            for (int k = 0; k < ov; k += 2) sc += (double)in[cand + k] * in[target + k];
            if (sc > bestS) { bestS = sc; best = cand; }
        }
        p = best;
    }
    for (size_t i = 0; i < outLen; i++) out[i] = (wsum[i] > 1e-6f) ? out[i] / wsum[i] : 0.f;
    size_t realLen = (size_t)(N / rate);
    if (realLen > 0 && realLen < out.size()) out.resize(realLen);
    return out;
}
static std::map<std::string, Pcm> g_stretchCache;
static Pcm* get_stretched_pcm(const std::string& uri, double rate) {
    if (std::fabs(rate - 1.0) < 1e-3) return get_pcm(uri);
    int rb = (int)lround(rate * 100);                // 0.01 buckets
    char key[700]; snprintf(key, sizeof key, "%s|r%d", uri.c_str(), rb);
    auto it = g_stretchCache.find(key);
    if (it != g_stretchCache.end()) return it->second.ok ? &it->second : nullptr;
    Pcm s; Pcm* base = get_pcm(uri);
    if (base) {
        s.srate = base->srate;
        s.mono = wsola_stretch(base->mono, rate, base->srate);
        s.dur = (double)s.mono.size() / (double)s.srate;
        s.rms = base->rms;                           // loudness is unchanged by a time-stretch
        s.ok = !s.mono.empty();
    }
    g_stretchCache[key] = std::move(s);
    Pcm& r = g_stretchCache[key];
    return r.ok ? &r : nullptr;
}

// Per-clip loudness normalization (basic, RMS-based): the gain (dB) to bring a clip's RMS to
// `target_db` dBFS, clamped so near-silence isn't blown up. Editor-measured → applied identically
// in the preview mixer AND the export plan, so a whispered/quiet clip lands at the same loudness
// as the rest in both. 0 dB if silent/unmeasurable. (RMS ≈ perceived loudness — enough for "basic";
// a true LUFS/EBU-R128 pass can come later.)
static double normalize_gain_db(const Pcm* pc, double target_db) {
    if (!pc || pc->rms < 1e-5f) return 0.0;
    double g = target_db - 20.0 * std::log10((double)pc->rms);
    return g > 24.0 ? 24.0 : (g < -12.0 ? -12.0 : g);
}

// ───────────────────────── audio transport (waveOut mixer) ─────────────────────
// Timeline PREVIEW playback: on Play the playhead advances by the wall clock (drives the
// visuals) while this mixer streams the timeline's WAV audio in sync via winmm waveOut. All
// audio clips are summed at their start offset + per-clip gain — the SAME recipe as export
// (so preview and export agree). mp3/non-WAV clips are skipped here (no in-editor decoder;
// export hands those to ffmpeg). Decode is lazy + cached (get_pcm); sources are linearly
// resampled to the device rate. Streaming, not pre-rendered, so it handles any length + live
// edits: each UI frame we refill whatever device buffers have drained.
static const int AUD_RATE = 48000;
static const int AUD_CH = 2;
static const int AUD_FRAMES = 2048;            // per buffer (~43 ms)
static const int AUD_NBUF = 8;                 // ~340 ms queued — ample for a ~60 fps refill
static HWAVEOUT  g_wo = nullptr;
static WAVEHDR   g_woHdr[AUD_NBUF];
static std::vector<int16_t> g_woBuf[AUD_NBUF];
static bool      g_woFree[AUD_NBUF];
static bool      g_audioOpen = false;
static bool      g_audioActive = false;        // playing (pumping) vs idle
static double    g_audioFillT = 0.0;           // next project-time (s) to synthesize

static double eval_kf(const std::vector<KF>& ks, double T, int comp, double fb);   // defined below
struct MixSrc { const Pcm* pcm; double start; double dur; float gain; double in_off;
                bool duck = false;
                const std::vector<KF>* gainKf = nullptr;   // keyframed gain_db ramp (project time) — the music
                float gainOtherDb = 0.f; };                // volume automation; gainOtherDb = lane + normalize atop it

// built-in transition SFX events (defined by the transition rules, below clip_trans_info)
struct SfxEvent { double t; std::string wav; double gainDb; };
static std::vector<SfxEvent> collect_sfx_events(Project& p);
// music-duck windows around authored gag cues ("fade the music out for the punchline")
static const double DUCK_OUT = 0.3, DUCK_IN = 0.6, DUCK_FLOOR = 0.07;   // fade-out s / fade-back s / SFX-punchline floor
static const double DUCK_VIDEO_FLOOR = 0.42;   // gentler whole-span dip for a video clip's own audio (~-7.5 dB)
static const float  VIDEO_AUDIO_SILENCE_RMS = 0.004f;   // RMS below this = a silent/near-silent track (treat as audio-less: no play, no duck)
struct DuckWin { double a, b, floor; };        // [a,b] window + the bed's floor inside it (0=near-silent … 1=untouched)
static std::vector<DuckWin> collect_duck_windows(Project& p);
static float duck_factor(double t, const std::vector<DuckWin>& wins) {
    double d = 0;   // deepest ducking depth (1-floor)*ramp across all windows overlapping t
    for (auto& w : wins) {
        double down = (t - w.a) / DUCK_OUT;              // 0→1 entering the window
        double up   = (w.b + DUCK_IN - t) / DUCK_IN;     // 1→0 leaving it
        double m = std::min(down, up);
        if (m < 0) m = 0; if (m > 1) m = 1;
        double dep = (1.0 - w.floor) * m;                // this window's dip at t
        if (dep > d) d = dep;
    }
    if (d < 0) d = 0; if (d > 1) d = 1;
    return (float)(1.0 - d);
}

// Audio clips = clips on rows under a track of kind "audio" that have a ready, decodable asset
// (WAV natively; mp3/ogg/… through the libav fallback in get_pcm).
static std::vector<MixSrc> collect_audio(Project& p) {
    std::vector<MixSrc> v;
    std::vector<std::string> arows;
    for (auto& tk : p.tracks) if (tk.kind == "audio") for (auto& r : tk.rows) arows.push_back(r);
    for (auto& kv : p.clips) {
        const Clip& c = kv.second;
        if (c.asset.empty()) continue;
        bool isAudio = false;
        for (auto& r : arows) if (r == c.row) { isAudio = true; break; }
        if (!isAudio) continue;
        auto au = p.asset_uri.find(c.asset);
        if (au == p.asset_uri.end()) continue;
        const std::string& uri = au->second;
        if (uri.size() < 4) continue;
        Pcm* pc = get_pcm(uri);                               // WAV, or the libav fallback (mp3 beds)
        if (!pc) continue;
        double gdb = c.params.value("gain_db", 0.0);
        // loudness normalization (clip param, or inherited from the row): bring this clip to a
        // target RMS so quiet/whispered lines match the rest (same recipe as the export plan).
        bool norm = c.params.value("normalize", false);
        double ntgt = c.params.value("normalize_db", -20.0);
        auto rit = p.rows.find(c.row);
        if (rit != p.rows.end()) {
            if (!c.params.contains("normalize")) norm = rit->second.params.value("normalize", false);
            if (!c.params.contains("normalize_db")) ntgt = rit->second.params.value("normalize_db", -20.0);
            gdb += rit->second.params.value("gain_db", 0.0);    // per-lane/track volume (adds to the clip's)
        }
        if (norm) gdb += normalize_gain_db(pc, ntgt);
        // GLOBAL speech boost: a fixed dB on every tts clip, on top of normalize + clip/lane gain (so the
        // relative dynamics between lines are untouched — just louder overall). Project panel; +8 default.
        if (rit != p.rows.end() && rit->second.type == "tts") gdb += p.speechGainDb;
        float gain = (float)std::pow(10.0, gdb / 20.0);
        // keyframed gain_db = a volume RAMP (the music-lane automation): the keyframes REPLACE the
        // static clip gain; lane volume + normalize + speech-boost still add on top (gainOtherDb).
        const std::vector<KF>* gainKf = nullptr; float gainOtherDb = 0.f;
        auto gkf = c.keyframes.find("params.gain_db");
        if (gkf != c.keyframes.end() && gkf->second.size() >= 2) {
            gainKf = &gkf->second; gainOtherDb = (float)(gdb - c.params.value("gain_db", 0.0));
        }
        bool duck = (rit != p.rows.end() && rit->second.type == "music");   // beds duck around gag cues
        double in_off = c.params.value("in", 0.0);   // asset in-point (s) — set by clip split
        // playback speed (pitch-preserved time-stretch). A tts clip with NO explicit rate takes the
        // project default (meta.speech_rate — shorts run ~1.3x); everything else defaults 1.0.
        double defRate = (rit != p.rows.end() && rit->second.type == "tts") ? p.speechRate : 1.0;
        double rate = c.params.value("rate", defRate);
        if (rate < 0.5) rate = 0.5; if (rate > 2.0) rate = 2.0;
        if (std::fabs(rate - 1.0) > 1e-3) {           // swap in the stretched buffer (in-point is in stretched time)
            Pcm* sp = get_stretched_pcm(uri, rate);
            if (sp) { pc = sp; in_off = in_off / rate; }
        }
        MixSrc ms{pc, c.start, c.dur, gain, in_off, duck};
        ms.gainKf = gainKf; ms.gainOtherDb = gainOtherDb;
        v.push_back(ms);
    }
    // Video clips carry their OWN audio by default — NOT shown as a separate audio track. Mixed at a
    // low default volume (`video_volume`, 0..1, default 0.12 = 12%); `mute_audio:true` (e.g. after a
    // "split to audio track") drops it. Decoded from the mp4 via libav (get_video_pcm), cached.
    for (auto& kv : p.clips) {
        const Clip& c = kv.second;
        if (c.asset.empty() || c.params.value("mute_audio", false)) continue;
        // retimed footage (speed ≠ 1 / pingpong loop) plays its frames off the audio clock —
        // its own sound would desync, so it goes silent (it's 12%-volume b-roll ambience anyway)
        if (std::fabs(c.params.value("speed", 1.0) - 1.0) > 1e-3) continue;
        if (c.params.is_object() && c.params.contains("loop") && c.params["loop"].is_string()) continue;
        auto rit = p.rows.find(c.row);
        if (rit == p.rows.end() || rit->second.type != "video") continue;
        double vol = c.params.value("video_volume", 0.12);
        if (vol <= 0.0) continue;
        auto vmi = p.asset_video.find(c.asset);
        std::string src = (vmi != p.asset_video.end() && !vmi->second.src.empty()) ? vmi->second.src : std::string();
        if (src.empty()) { auto au2 = p.asset_uri.find(c.asset); if (au2 != p.asset_uri.end()) src = au2->second; }
        if (src.empty()) continue;
        Pcm* pc = get_video_pcm(resolve_asset(src));
        if (!pc) continue;   // (a silent track mixes to nothing anyway; don't RMS-gate the mixer — a clip with only sparse audio must still play)
        double gdb = c.params.value("gain_db", 0.0) + 20.0 * std::log10(std::max(1e-4, vol))
                   + rit->second.params.value("gain_db", 0.0);   // per-lane volume too
        v.push_back({pc, c.start, c.dur, (float)std::pow(10.0, gdb / 20.0), c.params.value("in", 0.0)});
    }
    // built-in transition SFX one-shots (meta.sfx gates inside collect_sfx_events)
    for (auto& e : collect_sfx_events(p)) {
        Pcm* pc = get_pcm("library/sfx/" + e.wav);
        if (!pc) continue;                                    // stock pack not fetched → silently none
        v.push_back({pc, e.t, pc->dur, (float)std::pow(10.0, e.gainDb / 20.0), 0.0});
    }
    return v;
}

// Synthesize `frames` stereo samples for project-time [t0, t0+frames/RATE): sum every source
// (linear-resampled, gained, music ducked around gag cues), apply the project master gain
// (meta.gain_db — AFTER the per-clip loudness normalization, mirroring export.sh's post-amix
// volume), clip, write interleaved 16-bit.
static void audio_fill(const std::vector<MixSrc>& srcs, int16_t* out, int frames, double t0, float master = 1.f,
                       const std::vector<DuckWin>* ducks = nullptr) {
    std::vector<float> mix(frames, 0.f);
    for (auto& s : srcs) {
        const std::vector<float>& m = s.pcm->mono;
        bool duck = s.duck && ducks && !ducks->empty();
        for (int j = 0; j < frames; ++j) {
            double T = t0 + (double)j / AUD_RATE;
            double tau = T - s.start;                             // time since the clip's start
            if (tau < 0 || tau >= s.dur) continue;                // within the clip's time range
            double local = tau + s.in_off;                        // → position in the asset (in-point)
            if (local < 0 || local >= s.pcm->dur) continue;       // within the asset
            double si = local * s.pcm->srate;
            size_t i0 = (size_t)si; float fr = (float)(si - i0);
            float a = m[i0], b = (i0 + 1 < m.size()) ? m[i0 + 1] : a;
            float g = s.gainKf ? (float)std::pow(10.0, (eval_kf(*s.gainKf, T, 0, 0.0) + s.gainOtherDb) / 20.0)
                               : s.gain;                          // keyframed volume ramp, else static
            if (duck) g *= duck_factor(T, *ducks);                // music dips for the punchline
            mix[j] += (a + (b - a) * fr) * g;
        }
    }
    for (int j = 0; j < frames; ++j) {
        float v = mix[j] * master; v = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
        int16_t s16 = (int16_t)lrintf(v * 32767.f);
        out[j * 2] = s16; out[j * 2 + 1] = s16;
    }
}

static bool audio_open() {
    if (g_audioOpen) return true;
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = AUD_CH; wf.nSamplesPerSec = AUD_RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = AUD_CH * 2; wf.nAvgBytesPerSec = AUD_RATE * wf.nBlockAlign;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_wo = nullptr; return false; }
    for (int i = 0; i < AUD_NBUF; ++i) {
        g_woBuf[i].assign((size_t)AUD_FRAMES * AUD_CH, 0);
        memset(&g_woHdr[i], 0, sizeof(WAVEHDR));
        g_woHdr[i].lpData = (LPSTR)g_woBuf[i].data();
        g_woHdr[i].dwBufferLength = AUD_FRAMES * AUD_CH * sizeof(int16_t);
        waveOutPrepareHeader(g_wo, &g_woHdr[i], sizeof(WAVEHDR));
        g_woFree[i] = true;
    }
    g_audioOpen = true;
    return true;
}

static void audio_reset_buffers() {
    if (!g_audioOpen) return;
    waveOutReset(g_wo);                                       // stop + reclaim all queued buffers
    for (int i = 0; i < AUD_NBUF; ++i) g_woFree[i] = true;
}

static void audio_start(double t0) { if (audio_open()) { audio_reset_buffers(); g_audioFillT = t0; g_audioActive = true; } }
static void audio_seek(double t)   { if (g_audioActive) { audio_reset_buffers(); g_audioFillT = t; } }
static void audio_stop()           { g_audioActive = false; audio_reset_buffers(); }

// Called every UI frame while playing: refill + re-queue any buffer that has drained, keeping
// AUD_NBUF buffers worth of audio queued ahead of the device's play cursor.
static void audio_pump(Project& p) {
    if (!g_audioActive) return;
    std::vector<MixSrc> srcs = collect_audio(p);
    std::vector<DuckWin> ducks = collect_duck_windows(p);
    float master = (float)std::pow(10.0, p.masterGainDb / 20.0);
    for (int i = 0; i < AUD_NBUF; ++i) {
        if (!g_woFree[i]) {
            if (g_woHdr[i].dwFlags & WHDR_DONE) g_woFree[i] = true;  // device finished this one
            else continue;                                          // still playing/queued
        }
        audio_fill(srcs, g_woBuf[i].data(), AUD_FRAMES, g_audioFillT, master, &ducks);
        g_audioFillT += (double)AUD_FRAMES / AUD_RATE;
        g_woFree[i] = false;
        waveOutWrite(g_wo, &g_woHdr[i], sizeof(WAVEHDR));           // clears DONE, sets INQUEUE
    }
}

static void audio_shutdown() {
    if (!g_audioOpen) return;
    waveOutReset(g_wo);
    for (int i = 0; i < AUD_NBUF; ++i) waveOutUnprepareHeader(g_wo, &g_woHdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wo);
    g_wo = nullptr; g_audioOpen = false; g_audioActive = false;
}

// ── one-shot audition (Library Viewer): play a decoded clip FROM a scrub offset on its own
// waveOut handle (mono @ native rate), and expose the play cursor so the Viewer advances the
// marker. Separate from the timeline mixer above — this is "audition this one library clip from
// where I scrubbed." The sample window is copied, so it's independent of the Pcm cache lifetime.
static HWAVEOUT g_audWo = nullptr;
static WAVEHDR  g_audHdr;
static std::vector<int16_t> g_audBuf;
static int      g_audSrate = 0;
static double   g_audStartSec = 0.0;   // clip offset the buffer began at (marker mapping)
static bool     g_audPlaying = false;

static void audition_stop() {
    if (!g_audWo) return;
    waveOutReset(g_audWo);
    if (g_audHdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_audWo, &g_audHdr, sizeof g_audHdr);
    waveOutClose(g_audWo);
    g_audWo = nullptr; g_audPlaying = false; g_audBuf.clear();
}
static void audition_play(const Pcm* pc, double fromSec) {
    audition_stop();
    if (!pc || pc->mono.empty() || pc->srate <= 0) return;
    if (fromSec < 0) fromSec = 0;
    size_t i0 = (size_t)(fromSec * pc->srate);
    if (i0 >= pc->mono.size()) return;
    g_audBuf.resize(pc->mono.size() - i0);
    for (size_t i = 0; i < g_audBuf.size(); ++i) {
        float v = pc->mono[i0 + i]; v = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
        g_audBuf[i] = (int16_t)lrintf(v * 32767.f);
    }
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = pc->srate;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = pc->srate * 2;
    if (waveOutOpen(&g_audWo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_audWo = nullptr; return; }
    memset(&g_audHdr, 0, sizeof g_audHdr);
    g_audHdr.lpData = (LPSTR)g_audBuf.data();
    g_audHdr.dwBufferLength = (DWORD)(g_audBuf.size() * sizeof(int16_t));
    waveOutPrepareHeader(g_audWo, &g_audHdr, sizeof g_audHdr);
    waveOutWrite(g_audWo, &g_audHdr, sizeof g_audHdr);
    g_audSrate = pc->srate; g_audStartSec = fromSec; g_audPlaying = true;
}
// Play cursor (seconds into the clip) while auditioning, or -1 when idle/finished.
static double audition_pos() {
    if (!g_audWo || !g_audPlaying) return -1;
    if (g_audHdr.dwFlags & WHDR_DONE) { g_audPlaying = false; return -1; }
    MMTIME mt; memset(&mt, 0, sizeof mt); mt.wType = TIME_SAMPLES;
    if (waveOutGetPosition(g_audWo, &mt, sizeof mt) == MMSYSERR_NOERROR) {
        double samp = mt.wType == TIME_SAMPLES ? (double)mt.u.sample
                    : mt.wType == TIME_BYTES   ? (double)mt.u.cb / 2.0   // mono 16-bit
                    : -1.0;                                              // unsupported unit
        if (samp >= 0 && g_audSrate > 0) return g_audStartSec + samp / (double)g_audSrate;
    }
    return -1;
}

// ───────────────────────────── visemes (align) ────────────────────────────
// The align provider's viseme asset is a json file: {visemes:[{viseme,t0,t1,openness}],…}.
// We load + cache the openness track and sample it at the playhead to drive the pngtuber
// mouth — pure compositing, instant. Times are relative to the (driving) audio start.
struct Viseme { double t0 = 0, t1 = 0, openness = 0; };
struct VisemeTrack { std::vector<Viseme> cues; bool ok = false; };
static std::map<std::string, VisemeTrack> g_visemeCache;

static VisemeTrack* get_visemes(const std::string& uri) {
    if (uri.empty()) return nullptr;
    auto it = g_visemeCache.find(uri);
    if (it != g_visemeCache.end()) return it->second.ok ? &it->second : nullptr;
    VisemeTrack t;
    std::ifstream f(resolve_asset(uri));
    if (f) {
        try {
            json j; f >> j;
            for (auto& v : j.value("visemes", json::array())) {
                Viseme vi;
                vi.t0 = v.value("t0", 0.0);
                vi.t1 = v.value("t1", 0.0);
                vi.openness = v.value("openness", 0.0);
                t.cues.push_back(vi);
            }
            t.ok = !t.cues.empty();
        } catch (...) {}
    }
    g_visemeCache[uri] = t;
    return t.ok ? &g_visemeCache[uri] : nullptr;
}

// Smoothed "talk level" at `local` with asymmetric attack/decay (one-pole, rates in 1/s).
// Computed by integrating the openness cues from clip start → DETERMINISTIC (a pure function of
// the track + time), so scrubbing and out-of-order export render identically (no frame state).
// Raw viseme openness is piecewise-constant (it snaps); this eases the avatar's talk-reactive
// bob/scale/brighten in and out. Higher rate = snappier.
static double viseme_talk_at(const VisemeTrack* t, double local, double atk, double dec) {
    if (!t || t->cues.empty() || local <= 0.0) return 0.0;
    double lvl = 0.0, prev = 0.0;
    auto approach = [&](double target, double upto) {
        if (upto <= prev) return;
        double rate = (target > lvl) ? atk : dec;
        lvl += (target - lvl) * (1.0 - exp(-rate * (upto - prev)));
        prev = upto;
    };
    for (auto& c : t->cues) {
        if (prev >= local) break;
        if (c.t0 > prev) approach(0.0, std::min(c.t0, local));   // gap → toward silence
        if (prev >= local) break;
        approach(c.openness, std::min(c.t1, local));             // within cue → toward its openness
    }
    if (prev < local) approach(0.0, local);                      // past last cue → toward silence
    return lvl < 0 ? 0 : (lvl > 1 ? 1 : lvl);
}
// Same attack/decay smoothing, but over the active VO clip's AUDIO ENVELOPE (peak per bucket).
// Driving the avatar bob from the audio (not a baked viseme) makes it FOLLOW the audio when a
// VO clip is moved/restructured — and go silent (0) when no VO is playing under the host.
static double audio_talk_at(const Wave* w, double local, double atk, double dec) {
    if (!w || w->env.empty() || w->dur <= 0.0 || local <= 0.0) return 0.0;
    int N = (int)w->env.size(); double bdur = w->dur / N;
    double lvl = 0.0, prev = 0.0;
    auto approach = [&](double target, double upto) {
        if (upto <= prev) return;
        double rate = (target > lvl) ? atk : dec;
        lvl += (target - lvl) * (1.0 - exp(-rate * (upto - prev)));
        prev = upto;
    };
    int last = std::min(N, (int)(local / bdur) + 1);
    for (int k = 0; k < last; k++) {
        approach(std::min(1.0, w->env[k] * 1.8), std::min((k + 1) * bdur, local));  // ×1.8: speech rarely peaks at 1
        if (prev >= local) break;
    }
    return lvl < 0 ? 0 : (lvl > 1 ? 1 : lvl);
}

// ══════════════════════ generation client (NEXT #1) ═══════════════════════
// Generate-on-demand: a clip's "Generate" button submits a provider job on a
// worker thread (so the interactive compositing loop never blocks), polls it,
// downloads the produced asset into the local cache, then the UI thread patches
// + persists the project and reloads. The last good asset keeps showing/playing
// until the new one is ready — generation never sits in the interactive loop.
// Protocol: docs/PROVIDER_PROTOCOL.md (POST /jobs → poll GET /jobs/{id} → GET
// /assets/{hash}.{ext}). Provider URLs come from config.toml.

struct ProviderCfg { std::string url; bool enabled = true; };
static std::map<std::string, ProviderCfg> g_providers;  // config key (tts/image/…) → cfg

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::string jstr(const json& o, const char* k) {
    if (o.is_object() && o.contains(k) && o[k].is_string()) return o[k].get<std::string>();
    return "";
}
static std::string ext_of(const std::string& uri);  // defined below; used by resolve_driven_audio

// the clip's project anchor base (params.anchor → meta.anchors[key]); [0,0] when unanchored.
// Applied wherever transform.pos lands on screen, so pos stays a pure offset from it.
static ImVec2 anchor_off(const Project& p, const Clip& c) {
    std::string k = jstr(c.params, "anchor");
    if (!k.empty()) {
        auto it = p.anchors.find(k);
        if (it != p.anchors.end()) return ImVec2((float)it->second[0], (float)it->second[1]);
    }
    return ImVec2(0, 0);
}

// Minimal TOML-subset reader: just enough to pull [providers.<name>] url/enabled.
// We don't need a full TOML parser — the config is small and this keeps the editor
// dependency-free (the design doc explicitly scopes config to provider URLs + keys).
static std::string toml_unquote(const std::string& v) {
    size_t q1 = v.find('"');
    if (q1 != std::string::npos) {
        size_t q2 = v.find('"', q1 + 1);
        if (q2 != std::string::npos) return v.substr(q1 + 1, q2 - q1 - 1);
    }
    std::string s = v;
    size_t h = s.find('#');
    if (h != std::string::npos) s = s.substr(0, h);
    return trim(s);
}
static bool load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line, section;
    while (std::getline(f, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (s[0] == '[') {
            size_t e = s.find(']');
            section = (e != std::string::npos) ? s.substr(1, e - 1) : "";
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq)), val = s.substr(eq + 1);
        if (section.rfind("providers.", 0) == 0) {
            std::string name = section.substr(10);
            if (key == "url") g_providers[name].url = toml_unquote(val);
            else if (key == "enabled") g_providers[name].enabled = (val.find("true") != std::string::npos);
        }
    }
    return true;
}

// ── HTTP via WinHTTP ───────────────────────────────────────────────────────
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
struct Url { bool https = false; std::wstring host, path; INTERNET_PORT port = 80; };
static bool parse_url(const std::string& u, Url& out) {
    std::string s = u, scheme = "http";
    size_t p = s.find("://");
    if (p != std::string::npos) { scheme = s.substr(0, p); s = s.substr(p + 3); }
    out.https = (scheme == "https");
    out.port = out.https ? 443 : 80;
    size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : s.substr(slash);
    size_t colon = hostport.find(':');
    std::string host = hostport;
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        out.port = (INTERNET_PORT)atoi(hostport.substr(colon + 1).c_str());
    }
    out.host = widen(host);
    out.path = widen(path);
    return !host.empty();
}
// One HTTP request. Reads the body into *bodyOut (text) OR streams it to `sink`
// (binary asset download). Returns false on transport failure; *statusOut gets the
// HTTP status. Each call owns its own WinHTTP session → safe to run per worker thread.
static bool http_do(const char* method, const std::string& url, const std::string& body,
                    const char* contentType, long* statusOut, std::string* bodyOut, FILE* sink) {
    if (statusOut) *statusOut = 0;
    Url u;
    if (!parse_url(url, u)) return false;
    bool ok = false;
    HINTERNET hS = WinHttpOpen(L"slopstudio/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return false;
    WinHttpSetTimeouts(hS, 10000, 10000, 30000, 120000);  // resolve/connect/send/recv ms
    HINTERNET hC = WinHttpConnect(hS, u.host.c_str(), u.port, 0);
    if (hC) {
        DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hR = WinHttpOpenRequest(hC, widen(method).c_str(), u.path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hR) {
            std::wstring headers;
            if (contentType && *contentType) headers = L"Content-Type: " + widen(contentType) + L"\r\n";
            BOOL sent = WinHttpSendRequest(
                hR, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : (DWORD)-1L,
                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                (DWORD)body.size(), (DWORD)body.size(), 0);
            if (sent && WinHttpReceiveResponse(hR, nullptr)) {
                DWORD code = 0, len = sizeof(code);
                WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                if (statusOut) *statusOut = (long)code;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(hR, &avail) || avail == 0) break;
                    std::vector<char> buf(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(hR, buf.data(), avail, &read) || read == 0) break;
                    if (sink) fwrite(buf.data(), 1, read, sink);
                    else if (bodyOut) bodyOut->append(buf.data(), read);
                }
                ok = true;
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    }
    WinHttpCloseHandle(hS);
    return ok;
}

// ── job state shared between worker threads and the UI thread ───────────────
// state: 0 none · 1 active(submitting/queued/running) · 2 done(awaiting apply) · 3 error
struct GenState {
    int state = 0;
    float progress = 0;
    std::string message;
    json assetEntry;   // ready to drop into doc["assets"][hash] (set when state==2)
    json clipParams;   // the clip's own (edited) params, persisted back to the clip
    std::string hash;
};
struct GenLite { int state = 0; float progress = 0; std::string message; };  // display snapshot
static std::map<std::string, GenState> g_gen;  // clipId → state
static CRITICAL_SECTION g_genCS;
static std::map<std::string, int> g_health;    // provider key → 0 unknown · 1 up · 2 down
static CRITICAL_SECTION g_healthCS;

static std::map<std::string, GenLite> gen_snapshot() {
    std::map<std::string, GenLite> m;
    EnterCriticalSection(&g_genCS);
    for (auto& kv : g_gen) m[kv.first] = {kv.second.state, kv.second.progress, kv.second.message};
    LeaveCriticalSection(&g_genCS);
    return m;
}
static void gen_set(const std::string& clipId, int state, float pr, const std::string& msg) {
    EnterCriticalSection(&g_genCS);
    auto& g = g_gen[clipId];
    g.state = state; g.progress = pr; g.message = msg;
    LeaveCriticalSection(&g_genCS);
}

// Row type → (provider config key, provider capability type). avatar/caption/effect
// are composited locally, not generated through a provider here.
static bool map_type(const std::string& rowType, std::string& provKey, std::string& capType) {
    if (rowType == "tts")    { provKey = "tts";   capType = "speech";     return true; }
    if (rowType == "image")  { provKey = "image"; capType = "text2image"; return true; }
    if (rowType == "music")  { provKey = "music"; capType = "search";     return true; }
    if (rowType == "video")  { provKey = "video"; capType = "motion";     return true; }
    if (rowType == "avatar") { provKey = "align"; capType = "visemes";    return true; }
    return false;
}

static json load_voice_preset(const std::string& name) {
    json j;
    std::ifstream f("presets/voices/" + name + ".json");
    if (f) { try { f >> j; } catch (...) {} }
    return j;
}

// List the available voice presets (presets/voices/*.json) for the editor's voice switcher.
static std::vector<std::string> list_voice_presets() {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"presets/voices/*.json", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char nm[260];
            int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, nullptr, nullptr);
            std::string s = (n > 0) ? std::string(nm) : "";
            if (s.size() > 5 && s.substr(s.size() - 5) == ".json") out.push_back(s.substr(0, s.size() - 5));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Build the provider `params` for a clip: row defaults overlaid with clip params,
// then capability-specific resolution. For TTS we resolve voice_preset →
// voice/seed/language from presets/voices/<name>.json (mirrors tools/demo-cache.sh),
// because the provider's `speech` capability requires the full `voice` instruct.
static std::string read_file_str(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}
static std::string b64_encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned n = (unsigned)(unsigned char)in[i] << 16 | (unsigned)(unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += T[(n >> 18) & 63]; out += T[(n >> 12) & 63]; out += T[(n >> 6) & 63]; out += T[n & 63];
    }
    if (i < in.size()) {
        unsigned n = (unsigned)(unsigned char)in[i] << 16;
        if (i + 1 < in.size()) n |= (unsigned)(unsigned char)in[i + 1] << 8;
        out += T[(n >> 18) & 63]; out += T[(n >> 12) & 63];
        out += (i + 1 < in.size()) ? T[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Build a `speech` job's params from a merged param object ({text, voice_preset, emotion, seed,
// language}). Resolves the voice preset → the full `voice` instruct + (clone mode) its golden ref
// clip. Shared by the clip generate path (build_params) AND the library voice-gen form.
// Spoken-form normalization for the OUTGOING TTS text only (the clip/asset keep the authored
// text, so adopt/lint matching and captions are untouched). Years are the reliable win: the
// model reads a bare "2007" as "two zero zero seven", so expand standalone 4-digit years.
static std::string spoken_two(int n) {   // 0..99 in words ("oh five" handled by the caller)
    static const char* ones[] = {"zero","one","two","three","four","five","six","seven","eight","nine","ten",
        "eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen"};
    static const char* tens[] = {"","","twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety"};
    if (n < 20) return ones[n];
    return std::string(tens[n / 10]) + (n % 10 ? std::string("-") + ones[n % 10] : "");
}
static std::string spoken_year(int y) {
    int hi = y / 100, lo = y % 100;
    if (y >= 2000 && y < 2010) return lo ? "two thousand " + spoken_two(lo) : "two thousand";
    if (lo == 0)  return spoken_two(hi) + " hundred";                       // 1900 → nineteen hundred
    if (lo < 10)  return spoken_two(hi) + " oh " + spoken_two(lo);          // 1905 → nineteen oh five
    return spoken_two(hi) + " " + spoken_two(lo);                           // 1997 → nineteen ninety-seven
}
static std::string tts_normalize(const std::string& s) {
    std::string out; out.reserve(s.size());
    size_t i = 0;
    auto alnum = [](char c) { return isalnum((unsigned char)c) != 0; };
    while (i < s.size()) {
        if (isdigit((unsigned char)s[i])) {
            size_t j = i; while (j < s.size() && isdigit((unsigned char)s[j])) j++;
            bool lone = (i == 0 || !alnum(s[i - 1])) && (j >= s.size() || !alnum(s[j]));
            if (j - i == 4 && lone && (s[i] == '1' || s[i] == '2')) {   // a standalone 4-digit year
                out += spoken_year(atoi(s.substr(i, 4).c_str()));
                i = j; continue;
            }
            out.append(s, i, j - i); i = j; continue;
        }
        out += s[i++];
    }
    return out;
}

static json speech_params(const json& merged) {
    json out = json::object();
    std::string presetName = jstr(merged, "voice_preset");
    json preset = presetName.empty() ? json::object() : load_voice_preset(presetName);
    out["text"] = tts_normalize(jstr(merged, "text"));
    std::string voice = jstr(merged, "voice");
    if (voice.empty()) voice = jstr(preset, "voice");
    out["voice"] = voice;
    std::string emo = jstr(merged, "emotion");
    if (emo.empty()) emo = jstr(merged, "instruct");
    out["seed"] = merged.contains("seed") ? merged.value("seed", 0) : preset.value("seed", 0);
    std::string lang = jstr(merged, "language");
    if (lang.empty()) lang = preset.contains("language") ? jstr(preset, "language") : "English";
    out["language"] = lang;
    if (!presetName.empty()) out["voice_preset"] = presetName;  // volatile label
    // CLONE mode: if the preset ships a golden reference clip (baked by tools/bake-voice-ref.py),
    // send it + its transcript so the provider clones a STABLE timbre across lines instead of
    // re-deriving from the instruct each call. Current cloned voices do not support per-line
    // delivery emotion, so `emotion` remains script/avatar metadata and is not sent in clone mode.
    std::string ref = jstr(preset, "ref"), refText = jstr(preset, "ref_text");
    if (!ref.empty() && !refText.empty()) {
        std::string bytes = read_file_str("presets/voices/" + ref);
        if (!bytes.empty()) { out["ref_b64"] = b64_encode(bytes); out["ref_text"] = refText; }
    } else if (!emo.empty()) {
        out["emotion"] = emo;  // design/non-clone backends may support delivery emotion.
    }
    return out;
}

static json build_params(Project& p, Clip& c, const std::string& capType) {
    json merged = json::object();
    auto rit = p.rows.find(c.row);
    if (rit != p.rows.end() && rit->second.params.is_object()) merged = rit->second.params;
    if (c.params.is_object()) merged.update(c.params);

    if (capType == "speech") return speech_params(merged);
    if (capType == "visemes") {
        json out = json::object();
        if (merged.contains("recognizer")) out["recognizer"] = merged["recognizer"];
        return out;  // `dialog` (the driven line's text) is added by start_generate
    }
    return merged;  // image/music/video: pass row+clip params straight through
}

// For an align-driven clip (avatar), find the audio asset that drives it: the `driven_by`
// row's clip overlapping this clip's time. The asset KEY is the provider content hash, which
// align resolves zero-copy from the sibling cache; we also pass a fetchable uri as fallback.
static bool resolve_driven_audio(Project& p, Clip& c, const std::string& clipId,
                                 json& inputs, std::string& dialog, std::string& err) {
    auto rit = p.rows.find(c.row);
    std::string drivenRow = (rit != p.rows.end()) ? jstr(rit->second.params, "driven_by") : "";
    if (drivenRow.empty()) { err = "avatar row has no 'driven_by' (the VO row to lip-sync)"; return false; }
    auto drit = p.rows.find(drivenRow);
    if (drit == p.rows.end()) { err = "driven_by row '" + drivenRow + "' not found"; return false; }
    Clip* best = nullptr;
    // the compiler pairs beats by id (bNN_av ↔ bNN_vo) — honor that first. Time overlap
    // lies before retime: a freshly generated neighbour VO grows to its raw take length
    // and swallows the estimated spans of the beats after it.
    if (clipId.size() > 3 && clipId.compare(clipId.size() - 3, 3, "_av") == 0) {
        std::string vid = clipId.substr(0, clipId.size() - 3) + "_vo";
        auto dc = p.clips.find(vid);
        if (dc != p.clips.end() && !dc->second.asset.empty() && dc->second.row == drivenRow)
            best = &dc->second;
    }
    if (!best) {  // fallback: the driven-row clip with the MOST overlap (ties → closest start)
        double bov = -1.0, bdist = 1e18;
        Clip* first = nullptr;
        for (auto& cid : drit->second.clips) {
            auto dc = p.clips.find(cid);
            if (dc == p.clips.end() || dc->second.asset.empty()) continue;
            Clip& d = dc->second;
            if (!first) first = &d;
            double ov = std::min(d.start + d.dur, c.start + c.dur) - std::max(d.start, c.start);
            double dist = std::abs(d.start - c.start);
            if (ov > bov + 1e-9 || (ov > bov - 1e-9 && dist < bdist)) { bov = ov; bdist = dist; best = &d; }
        }
        if (!best) best = first;  // nothing overlaps at all → first generated clip on the row
    }
    if (!best) { err = "drive row '" + drivenRow + "' has no generated audio yet — generate the VO first"; return false; }
    std::string hash = best->asset, ext = "wav", uri;
    auto au = p.asset_uri.find(hash);
    if (au != p.asset_uri.end()) { std::string e = ext_of(au->second); if (!e.empty()) ext = e; }
    std::string dprov, dcap;  // a provider-fetchable uri (driven row's provider) as fallback
    if (map_type(drit->second.type, dprov, dcap)) {
        auto dpc = g_providers.find(dprov);
        if (dpc != g_providers.end() && !dpc->second.url.empty())
            uri = dpc->second.url + "/assets/" + hash + "." + ext;
    }
    inputs = json::array();
    inputs.push_back(json{{"hash", hash}, {"uri", uri}});
    dialog = jstr(best->params, "text");
    return true;
}

static std::string ext_of(const std::string& uri) {
    size_t slash = uri.find_last_of("/\\");
    size_t dot = uri.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return uri.substr(dot + 1);
}
static std::string ext_for_kind(const std::string& k) {
    if (k == "audio") return "wav";
    if (k == "image") return "png";
    if (k == "video") return "mp4";
    return "bin";
}
static void make_dirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            if (!cur.empty() && cur != ".") CreateDirectoryA(cur.c_str(), nullptr);
            if (i < path.size()) cur.push_back(path[i]);
        } else cur.push_back(path[i]);
    }
}

// What the worker thread carries. Heap-allocated; the worker deletes it.
struct GenReq {
    std::string clipId, providerUrl, providerKey, capType, cacheDir;
    json body;        // {type, params, inputs, preset}
    json clipParams;  // the clip's own params (edited) → persisted back to the clip
};

// Submit `body` to <url>/jobs, poll to completion, and download the first result asset's bytes to
// <destDir>/<hash>.<ext>. `prog(pct, msg)` reports live status (queued/cached/progress/download).
// On success fills hash/ext/outPath + the full `result` and returns true; on any failure returns
// false with `err` set. Shared by the clip generate path AND the library gen path — no shared
// editor↔provider storage on this topology, so bytes are always HTTP-fetched.
static bool run_provider_job(const std::string& url, const json& body, const std::string& destDir,
                             const std::function<void(float, const std::string&)>& prog,
                             std::string& hash, std::string& ext, std::string& outPath,
                             json& result, std::string& err) {
    long st = 0;
    std::string resp, bodyStr = body.dump();
    if (!http_do("POST", url + "/jobs", bodyStr, "application/json", &st, &resp, nullptr) || st / 100 != 2) {
        err = "submit failed (" + std::to_string(st) + ") " + snippet(resp, 80); return false;
    }
    json jr;
    try { jr = json::parse(resp); } catch (...) { err = "bad submit response"; return false; }
    std::string jobId = jr.value("job_id", std::string());
    hash = jr.value("hash", std::string());
    if (jr.value("cached", false) || jr.value("status", std::string()) == "done") {
        result = jr.value("result", json::object());
        prog(1.0f, "cached");
    } else {
        prog(0.0f, "queued");
        bool terminal = false;
        for (int i = 0; i < 600 && !terminal; ++i) {  // poll up to ~3 min @ 300ms
            Sleep(300);
            long ps = 0; std::string pr;
            if (!http_do("GET", url + "/jobs/" + jobId, "", nullptr, &ps, &pr, nullptr) || ps / 100 != 2) continue;
            json pj;
            try { pj = json::parse(pr); } catch (...) { continue; }
            std::string s = pj.value("status", std::string());
            prog((float)pj.value("progress", 0.0),
                 pj.contains("message") && pj["message"].is_string() ? pj["message"].get<std::string>() : s);
            if (s == "done") { result = pj.value("result", json::object()); hash = pj.value("hash", hash); terminal = true; }
            else if (s == "error") {
                json e = pj.value("error", json::object());
                err = "job error: " + (e.is_object() ? e.value("message", std::string("unknown")) : std::string("unknown"));
                return false;
            }
        }
        if (!terminal) { err = "timed out waiting for job"; return false; }
    }

    json assets = result.value("assets", json::array());
    if (!assets.is_array() || assets.empty()) { err = "job returned no assets"; return false; }
    json a0 = assets[0];
    ext = ext_of(a0.value("uri", std::string()));
    if (ext.empty()) ext = ext_for_kind(a0.value("kind", std::string("data")));

    make_dirs(destDir);
    outPath = destDir + "/" + hash + "." + ext;
    prog(0.97f, "downloading asset");
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) { err = "cannot write " + outPath; return false; }
    long ds = 0;
    bool dok = http_do("GET", url + "/assets/" + hash + "." + ext, "", nullptr, &ds, nullptr, f);
    fclose(f);
    if (!dok || ds / 100 != 2) { err = "asset download failed (" + std::to_string(ds) + ")"; return false; }
    return true;
}

static DWORD WINAPI gen_worker(LPVOID arg) {
    GenReq* req = (GenReq*)arg;
    std::string hash, ext, outPath, err; json result;
    auto prog = [&](float p, const std::string& m) { gen_set(req->clipId, 1, p, m); };
    if (!run_provider_job(req->providerUrl, req->body, req->cacheDir + "/" + req->providerKey,
                          prog, hash, ext, outPath, result, err)) {
        gen_set(req->clipId, 3, 0, err); delete req; return 0;
    }
    json a0 = result.value("assets", json::array())[0];

    // Asset entry for the project's cache index (docs/PROJECT_FORMAT.md §assets).
    json entry;
    entry["provider"] = req->providerKey;
    entry["type"] = req->capType;
    entry["params"] = req->body.value("params", json::object());
    if (entry["params"].is_object()) entry["params"].erase("ref_b64");  // don't persist the ~400KB ref blob
    json inHashes = json::array();  // record consumed input asset hashes (§assets inputs)
    for (auto& in : req->body.value("inputs", json::array()))
        if (in.is_object() && in.contains("hash") && in["hash"].is_string()) inHashes.push_back(in["hash"]);
    entry["inputs"] = inHashes;
    entry["status"] = "ready";
    entry["uri"] = "cache://" + req->providerKey + "/" + hash + "." + ext;
    entry["meta"] = a0.value("meta", json::object());
    if (result.contains("provenance") && !result["provenance"].is_null())
        entry["meta"]["attribution"] = result["provenance"];

    EnterCriticalSection(&g_genCS);
    auto& g = g_gen[req->clipId];
    g.state = 2; g.progress = 1.0f; g.message = "done";
    g.assetEntry = entry; g.hash = hash; g.clipParams = req->clipParams;
    LeaveCriticalSection(&g_genCS);
    delete req;
    return 0;
}

// UI-thread: resolve the job for a clip and kick off a worker thread.
static void start_generate(Project& p, const std::string& clipId) {
    auto cit = p.clips.find(clipId);
    if (cit == p.clips.end()) return;
    Clip& c = cit->second;
    std::string provKey, capType;
    if (!map_type(c.type, provKey, capType)) { gen_set(clipId, 3, 0, "no provider for type '" + c.type + "'"); return; }
    auto pc = g_providers.find(provKey);
    if (pc == g_providers.end() || !pc->second.enabled || pc->second.url.empty()) {
        gen_set(clipId, 3, 0, "provider '" + provKey + "' not configured (config.toml)");
        return;
    }
    json params = build_params(p, c, capType);
    json inputs = json::array();
    if (capType == "visemes" || capType == "word_timing") {  // align: needs the driven audio
        std::string dialog, err;
        if (!resolve_driven_audio(p, c, clipId, inputs, dialog, err)) { gen_set(clipId, 3, 0, err); return; }
        if (!dialog.empty()) params["dialog"] = dialog;  // improves Rhubarb accuracy
    }
    GenReq* req = new GenReq{clipId, pc->second.url, provKey, capType, g_cacheDir,
                             json{{"type", capType}, {"params", params}, {"inputs", inputs}, {"preset", nullptr}},
                             c.params.is_object() ? c.params : json::object()};
    gen_set(clipId, 1, 0, "submitting");
    HANDLE h = CreateThread(nullptr, 0, gen_worker, req, 0, nullptr);
    if (h) CloseHandle(h);
    else { gen_set(clipId, 3, 0, "could not start worker thread"); delete req; }
}

// ── library gen items (L4): generate a NEW library entry (image or voice) via a provider, OR
// regenerate one in place. Unlike the clip path, the result lands as a library FILE (library/
// <sub>/<name>.<ext>) + a `.meta.json` sidecar recording the recipe + a history of past gens. Runs
// on a worker thread; the UI thread watches g_libGen[key] (status) + g_libGenDirty (rescan). ──
struct LibGenState { int state = 0; float progress = 0; std::string message; };  // 0 idle 1 active 2 done 3 err
static std::map<std::string, LibGenState> g_libGen;   // key (libDir/baseName, no ext) → status
static volatile bool g_libGenDirty = false;           // a lib gen finished → rescan + drop caches
static std::string g_libGenLast;                      // last-written library file (texture to invalidate)

static void libgen_set(const std::string& key, int state, float pr, const std::string& msg) {
    EnterCriticalSection(&g_genCS);
    auto& s = g_libGen[key]; s.state = state; s.progress = pr; s.message = msg;
    LeaveCriticalSection(&g_genCS);
}
static LibGenState libgen_get(const std::string& key) {
    EnterCriticalSection(&g_genCS);
    LibGenState s; auto it = g_libGen.find(key); if (it != g_libGen.end()) s = it->second;
    LeaveCriticalSection(&g_genCS);
    return s;
}
// In-flight library gens (state==1), for placeholder cells in the grid (a NEW gen has no file yet).
struct PendingGen { std::string key, name, message; float progress; LibType type; };
static std::vector<PendingGen> libgen_pending() {
    std::vector<PendingGen> out;
    EnterCriticalSection(&g_genCS);
    for (auto& kv : g_libGen) if (kv.second.state == 1) {
        const std::string& k = kv.first;
        size_t s = k.find_last_of('/');
        LibType t = k.find("/audio/") != std::string::npos ? LIB_AUDIO
                  : k.find("/video/") != std::string::npos ? LIB_VIDEO : LIB_IMAGE;
        out.push_back({k, s == std::string::npos ? k : k.substr(s + 1), kv.second.message, kv.second.progress, t});
    }
    LeaveCriticalSection(&g_genCS);
    return out;
}

// Build a provider job body from a saved/edited recipe (the sidecar's editable params). For speech
// the recipe ({text, voice_preset, emotion, seed, language}) is resolved → the full instruct + ref;
// image params pass straight through. The recipe stays small + editable; the resolved body carries
// the big ref blob the provider needs (never persisted in the sidecar).
static json lib_job_body(const std::string& capType, const json& recipe) {
    json params = (capType == "speech") ? speech_params(recipe) : recipe;
    return json{{"type", capType}, {"params", params}, {"inputs", json::array()}, {"preset", nullptr}};
}

struct LibGenReq {
    std::string url, providerKey, capType, cacheDir, libDir, baseName, key;
    json body;          // {type, params, inputs, preset}
    json metaParams;    // the editable recipe recorded in the sidecar (prompt/seed | text/voice)
    bool regen;         // push the existing current gen into history before replacing it
};

static DWORD WINAPI lib_gen_worker(LPVOID arg) {
    LibGenReq* req = (LibGenReq*)arg;
    std::string hash, ext, cachePath, err; json result;
    auto prog = [&](float p, const std::string& m) { libgen_set(req->key, 1, p, m); };
    if (!run_provider_job(req->url, req->body, req->cacheDir + "/" + req->providerKey,
                          prog, hash, ext, cachePath, result, err)) {
        libgen_set(req->key, 3, 0, err); delete req; return 0;
    }
    std::string finalPath = req->libDir + "/" + req->baseName + "." + ext;

    // sidecar: on regen, demote the existing current gen into history (newest-first) before replacing
    json side = lib_load_sidecar(finalPath);
    json hist = side.value("history", json::array());
    std::string prevHash = side.value("hash", std::string());
    if (req->regen && !prevHash.empty() && prevHash != hash) {
        hist.insert(hist.begin(), json{{"hash", prevHash}, {"ext", side.value("ext", std::string("png"))},
                                       {"provider", side.value("provider", req->providerKey)},
                                       {"params", side.value("params", json::object())}});
        if (hist.size() > 24) hist.erase(hist.begin() + 24, hist.end());  // cap the strip
    }

    CopyFileW(to_w(cachePath).c_str(), to_w(finalPath).c_str(), FALSE);  // overwrite the library file

    json out = json::object();
    out["kind"] = "gen";
    out["provider"] = req->providerKey;
    out["cap"] = req->capType;
    out["params"] = req->metaParams;
    out["hash"] = hash;
    out["ext"] = ext;
    out["history"] = hist;
    json a0 = result.value("assets", json::array())[0];
    if (a0.is_object() && a0.contains("meta")) out["meta"] = a0["meta"];
    if (result.contains("provenance") && !result["provenance"].is_null()) out["attribution"] = result["provenance"];
    lib_save_sidecar(finalPath, out);

    EnterCriticalSection(&g_genCS); g_libGenLast = finalPath; LeaveCriticalSection(&g_genCS);
    libgen_set(req->key, 2, 1.0f, "done");
    g_libGenDirty = true;
    delete req;
    return 0;
}

// filename-safe base name (alnum/-/_/space/dot kept, others → '-'; trims trailing space/dot)
static std::string lib_safe_name(const std::string& n) {
    std::string o;
    for (char c : n) o += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ' || c == '.') ? c : '-';
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    return o.empty() ? "gen" : o;
}
// A library base name not already taken (for NEW gen items — regen reuses the same name). Checks
// the type's canonical ext (png/wav/mp4) since that's what the provider returns.
static std::string lib_unique_base(LibType t, const std::string& baseIn) {
    std::string dir = lib_dest_root() + "/" + lib_subdir(t);
    std::string ext = (t == LIB_AUDIO) ? "wav" : (t == LIB_VIDEO) ? "mp4" : "png";
    std::string base = lib_safe_name(baseIn), cand = base;
    for (int n = 1; GetFileAttributesW(to_w(dir + "/" + cand + "." + ext).c_str()) != INVALID_FILE_ATTRIBUTES; n++)
        cand = base + "-" + std::to_string(n);
    return cand;
}

// UI-thread: kick a library-gen worker. type picks the subdir; metaParams is the editable recipe;
// `body` is the full provider job. regen=true keeps the prior gen in the sidecar history.
static void start_lib_gen(LibType type, const std::string& nameIn, const std::string& providerKey,
                          const std::string& capType, const json& body, const json& metaParams, bool regen,
                          const std::string& destDir = std::string()) {   // regen passes the item's OWN dir (never re-homes it)
    std::string libDir = !destDir.empty() ? destDir : lib_dest_root() + "/" + lib_subdir(type);
    std::string base = lib_safe_name(nameIn);
    std::string key = libDir + "/" + base;
    auto pc = g_providers.find(providerKey);
    if (pc == g_providers.end() || !pc->second.enabled || pc->second.url.empty()) {
        libgen_set(key, 3, 0, "provider '" + providerKey + "' not configured (config.toml)");
        return;
    }
    ensure_dir(libDir);
    LibGenReq* req = new LibGenReq{pc->second.url, providerKey, capType, g_cacheDir, libDir, base, key,
                                   body, metaParams, regen};
    libgen_set(key, 1, 0, "submitting");
    HANDLE h = CreateThread(nullptr, 0, lib_gen_worker, req, 0, nullptr);
    if (h) CloseHandle(h);
    else { libgen_set(key, 3, 0, "could not start worker thread"); delete req; }
}

// UI-thread: when a library gen finishes, drop the regenerated file's stale texture/PCM (its path
// is stable across a regen) and request a rescan (a NEW item appears in the grid). Call per-frame.
static void lib_poll_gen_done() {
    if (!g_libGenDirty) return;
    g_libGenDirty = false;
    std::string last;
    EnterCriticalSection(&g_genCS); last = g_libGenLast; LeaveCriticalSection(&g_genCS);
    if (!last.empty()) { invalidate_texture(last); g_pcmCache.erase(last); }
    g_libraryDirty = true;
}

// ───────────────────────── rembg cut-out (provider bg removal) ──────────────
// The Viewer's "rembg" method submits the SELECTED library image to the rembg provider on a
// worker thread; the cutout is cached + recorded in the item's sidecar removebg block, so
// item_removebg loads it (non-destructive). Segments soft/gradient bgs the colour key can't.
// One op at a time (driven by a single Viewer selection); g_genCS guards the shared state.
static std::string g_vSideFor;             // (defined here for rembg's live refresh) Viewer's loaded-sidecar selection
static volatile long g_rembgState = 0;     // 0 idle/applied · 1 running · 2 done(pending apply) · 3 error
static float g_rembgProg = 0;
static std::string g_rembgMsg, g_rembgPath;

static void rembg_set(int state, float pr, const std::string& msg, const std::string& path) {
    EnterCriticalSection(&g_genCS);
    g_rembgState = state; g_rembgProg = pr; g_rembgMsg = msg; if (!path.empty()) g_rembgPath = path;
    LeaveCriticalSection(&g_genCS);
}

// Submit a library image → cutout, then patch its sidecar removebg block. Synchronous — the worker
// thread AND the headless --lib-removebg path both call this; `prog` reports submit/poll progress.
static bool rembg_cut_sync(const std::string& url, const std::string& cacheDir, const std::string& libPath,
                           const std::string& model, const std::function<void(float, const std::string&)>& prog,
                           std::string& err) {
    std::string bytes = read_file_str(libPath);
    if (bytes.empty()) { err = "cannot read " + libPath; return false; }
    json body = {{"type", "remove_bg"},
                 {"params", json{{"image_b64", b64_encode(bytes)}, {"model", model}}},
                 {"inputs", json::array()}, {"preset", nullptr}};
    std::string hash, ext, cutPath; json result;
    if (!run_provider_job(url, body, cacheDir + "/rembg", prog, hash, ext, cutPath, result, err)) return false;
    json side = lib_load_sidecar(libPath);
    json rb = side.value("removebg", json::object());
    rb.erase("key"); rb.erase("fuzz");                 // drop colour-key-only fields
    rb["method"] = "rembg"; rb["model"] = model; rb["enabled"] = true;
    rb["cache"] = "cache://rembg/" + hash + "." + ext; rb["hash"] = hash;
    side["removebg"] = rb;
    lib_save_sidecar(libPath, side);
    return true;
}

struct RembgReq { std::string url, cacheDir, libPath, model; };
static DWORD WINAPI rembg_worker(LPVOID arg) {
    RembgReq* req = (RembgReq*)arg;
    std::string err;
    auto prog = [&](float p, const std::string& m) { rembg_set(1, p, m, req->libPath); };
    if (rembg_cut_sync(req->url, req->cacheDir, req->libPath, req->model, prog, err))
        rembg_set(2, 1.0f, "done", req->libPath);
    else
        rembg_set(3, 0, err, req->libPath);
    delete req;
    return 0;
}

// UI-thread: kick a rembg cut-out worker for a library image. Sets an error if unconfigured.
static void start_rembg(const std::string& libPath, const std::string& model) {
    auto pc = g_providers.find("rembg");
    if (pc == g_providers.end() || !pc->second.enabled || pc->second.url.empty()) {
        rembg_set(3, 0, "provider 'rembg' not configured (config.toml)", libPath); return;
    }
    rembg_set(1, 0, "submitting", libPath);
    RembgReq* req = new RembgReq{pc->second.url, g_cacheDir, libPath, model};
    HANDLE h = CreateThread(nullptr, 0, rembg_worker, req, 0, nullptr);
    if (h) CloseHandle(h);
    else { rembg_set(3, 0, "could not start worker thread", libPath); delete req; }
}

// UI-thread per-frame: when a cut-out finishes, drop the stale texture so item_removebg re-keys
// with the new cutout (live), rescan the grid, and force the Viewer to reload the patched sidecar.
static void rembg_poll_done() {
    EnterCriticalSection(&g_genCS);
    bool done = (g_rembgState == 2); std::string path = g_rembgPath;
    if (done) g_rembgState = 0;
    LeaveCriticalSection(&g_genCS);
    if (done) { invalidate_texture(path); g_vSideFor.clear(); g_libraryDirty = true; }
}

// ── voice-preset preview: one-off TTS synth (no clip / no project patch), auto-played ──
// For the voice editor — design an instruct, hear it. Async worker; status under g_genCS.
static std::string g_previewMsg;
static volatile bool g_previewBusy = false;
struct PreviewReq { std::string url, cacheDir; json body; };

static void preview_set(const std::string& m, bool busy) {
    EnterCriticalSection(&g_genCS); g_previewMsg = m; g_previewBusy = busy; LeaveCriticalSection(&g_genCS);
}

static DWORD WINAPI preview_worker(LPVOID arg) {
    PreviewReq* req = (PreviewReq*)arg;
    long st = 0; std::string resp;
    if (!http_do("POST", req->url + "/jobs", req->body.dump(), "application/json", &st, &resp, nullptr) || st / 100 != 2) {
        preview_set("submit failed (" + std::to_string(st) + ")", false); delete req; return 0;
    }
    json jr; try { jr = json::parse(resp); } catch (...) { preview_set("bad submit response", false); delete req; return 0; }
    std::string jobId = jr.value("job_id", std::string()), hash = jr.value("hash", std::string());
    json result;
    if (jr.value("cached", false) || jr.value("status", std::string()) == "done") {
        result = jr.value("result", json::object());
    } else {
        bool terminal = false;
        for (int i = 0; i < 600 && !terminal; ++i) {
            Sleep(300);
            long ps = 0; std::string pr;
            if (!http_do("GET", req->url + "/jobs/" + jobId, "", nullptr, &ps, &pr, nullptr) || ps / 100 != 2) continue;
            json pj; try { pj = json::parse(pr); } catch (...) { continue; }
            std::string s = pj.value("status", std::string());
            preview_set("synthesizing… " + s, true);
            if (s == "done") { result = pj.value("result", json::object()); hash = pj.value("hash", hash); terminal = true; }
            else if (s == "error") { preview_set("job error", false); delete req; return 0; }
        }
        if (!terminal) { preview_set("timed out", false); delete req; return 0; }
    }
    json assets = result.value("assets", json::array());
    if (!assets.is_array() || assets.empty()) { preview_set("no audio returned", false); delete req; return 0; }
    std::string srcUri = assets[0].value("uri", std::string()), ext = ext_of(srcUri);
    if (ext.empty()) ext = "wav";
    std::string outDir = req->cacheDir + "/tts"; make_dirs(outDir);
    std::string outPath = outDir + "/preview_" + hash + "." + ext;
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) { preview_set("cannot write preview", false); delete req; return 0; }
    long ds = 0; bool dok = http_do("GET", req->url + "/assets/" + hash + "." + ext, "", nullptr, &ds, nullptr, f);
    fclose(f);
    if (!dok || ds / 100 != 2) { preview_set("download failed", false); delete req; return 0; }
    PlaySoundA(outPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);  // winmm async ok off-thread
    preview_set("playing preview", false);
    delete req; return 0;
}

// Launch a voice preview (designed `voice` instruct → a spoken sample line). No-op if busy.
static void voice_preview(const std::string& voice, int seed, const std::string& lang, const std::string& text) {
    if (g_previewBusy) return;
    auto pc = g_providers.find("tts");
    if (pc == g_providers.end() || !pc->second.enabled || pc->second.url.empty()) { preview_set("tts provider not configured", false); return; }
    json params = {{"text", text}, {"voice", voice}, {"seed", seed}, {"language", lang.empty() ? "English" : lang}};
    PreviewReq* req = new PreviewReq{pc->second.url, g_cacheDir, json{{"type", "speech"}, {"params", params}, {"inputs", json::array()}}};
    preview_set("submitting…", true);
    HANDLE h = CreateThread(nullptr, 0, preview_worker, req, 0, nullptr);
    if (h) CloseHandle(h); else { preview_set("worker failed", false); delete req; }
}

// Write a voice preset to presets/voices/<name>.json. Returns false on bad name / write error.
static bool save_voice_preset(const std::string& name, const std::string& voice, int seed, const std::string& lang) {
    if (name.empty()) return false;
    for (char ch : name) if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_')) return false;  // safe filename
    json j = {{"schema", "slopstudio.voice/1"}, {"name", name}, {"provider", "tts"},
              {"voice", voice}, {"language", lang.empty() ? "English" : lang}, {"seed", seed},
              {"description", "Authored in the editor's voice preset editor."}};
    make_dirs("presets/voices");
    std::ofstream f("presets/voices/" + name + ".json");
    if (!f) return false;
    f << j.dump(2) << "\n";
    return true;
}

// Bake a golden REFERENCE clip for a preset (design once via VoiceDesign) → presets/voices/
// <name>.ref.wav + write the preset with ref/ref_text, so the editor clones it (stable timbre)
// for every line. The same as tools/bake-voice-ref.py, in-editor. Async (synth ~5-7s).
static volatile bool g_voicesDirty = false;  // a bake finished → UI refreshes the preset list
struct BakeReq { std::string url, name, voice, language, refText; int seed; };

static DWORD WINAPI bake_worker(LPVOID arg) {
    BakeReq* r = (BakeReq*)arg;
    json body = {{"type", "speech"}, {"params", {{"text", r->refText}, {"voice", r->voice},
                 {"seed", r->seed}, {"language", r->language.empty() ? "English" : r->language}}},
                 {"inputs", json::array()}};
    long st = 0; std::string resp;
    if (!http_do("POST", r->url + "/jobs", body.dump(), "application/json", &st, &resp, nullptr) || st / 100 != 2) {
        preview_set("bake submit failed", false); delete r; return 0;
    }
    json jr; try { jr = json::parse(resp); } catch (...) { preview_set("bake: bad response", false); delete r; return 0; }
    std::string jobId = jr.value("job_id", std::string()), hash = jr.value("hash", std::string());
    json result;
    if (jr.value("cached", false) || jr.value("status", std::string()) == "done") {
        result = jr.value("result", json::object());
    } else {
        bool term = false;
        for (int i = 0; i < 600 && !term; ++i) {
            Sleep(300);
            long ps = 0; std::string pr;
            if (!http_do("GET", r->url + "/jobs/" + jobId, "", nullptr, &ps, &pr, nullptr) || ps / 100 != 2) continue;
            json pj; try { pj = json::parse(pr); } catch (...) { continue; }
            std::string s = pj.value("status", std::string());
            preview_set("baking ref... " + s, true);
            if (s == "done") { result = pj.value("result", json::object()); hash = pj.value("hash", hash); term = true; }
            else if (s == "error") { preview_set("bake: job error", false); delete r; return 0; }
        }
        if (!term) { preview_set("bake timed out", false); delete r; return 0; }
    }
    json assets = result.value("assets", json::array());
    if (!assets.is_array() || assets.empty()) { preview_set("bake: no audio", false); delete r; return 0; }
    std::string ext = ext_of(assets[0].value("uri", std::string())); if (ext.empty()) ext = "wav";
    make_dirs("presets/voices");
    std::string refName = r->name + ".ref." + ext, outPath = "presets/voices/" + refName;
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) { preview_set("bake: cannot write ref", false); delete r; return 0; }
    long ds = 0; bool dok = http_do("GET", r->url + "/assets/" + hash + "." + ext, "", nullptr, &ds, nullptr, f);
    fclose(f);
    if (!dok || ds / 100 != 2) { preview_set("bake: download failed", false); delete r; return 0; }
    json pj = {{"schema", "slopstudio.voice/1"}, {"name", r->name}, {"provider", "tts"}, {"voice", r->voice},
               {"language", r->language.empty() ? "English" : r->language}, {"seed", r->seed},
               {"description", "Authored + baked in the editor's voice preset editor."},
               {"ref", refName}, {"ref_text", r->refText}};
    std::ofstream("presets/voices/" + r->name + ".json") << pj.dump(2) << "\n";
    g_voicesDirty = true;
    preview_set("baked golden ref -> " + outPath, false);
    delete r; return 0;
}

static void voice_bake(const std::string& name, const std::string& voice, int seed,
                       const std::string& lang, const std::string& refText) {
    if (g_previewBusy) return;
    if (name.empty()) { preview_set("bake: name required", false); return; }
    for (char ch : name) if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_')) {
        preview_set("bake: name must be alnum/-/_", false); return; }
    if (refText.empty()) { preview_set("bake: ref/preview text required", false); return; }
    auto pc = g_providers.find("tts");
    if (pc == g_providers.end() || !pc->second.enabled || pc->second.url.empty()) {
        preview_set("tts provider not configured", false); return; }
    BakeReq* r = new BakeReq{pc->second.url, name, voice, lang, refText, seed};
    preview_set("baking golden ref...", true);
    HANDLE h = CreateThread(nullptr, 0, bake_worker, r, 0, nullptr);
    if (h) CloseHandle(h); else { preview_set("bake worker failed", false); delete r; }
}

// UI-thread: land any finished jobs into the project, persist it, and reload.
// The project file is the source of truth (docs/PROJECT_FORMAT.md), so we patch the
// raw doc (new assets[hash] + clips[id].asset/params) and write it back pretty-printed.
static void apply_generations(Project& p, std::string& bufFor) {
    struct A { std::string clipId, hash; json entry, params; };
    std::vector<A> apps;
    EnterCriticalSection(&g_genCS);
    std::vector<std::string> done;
    for (auto& kv : g_gen)
        if (kv.second.state == 2) { apps.push_back({kv.first, kv.second.hash, kv.second.assetEntry, kv.second.clipParams}); done.push_back(kv.first); }
    for (auto& id : done) g_gen.erase(id);  // asset now lands → clear the badge
    LeaveCriticalSection(&g_genCS);
    if (apps.empty()) return;

    sync_to_doc(p);   // fold unsaved arrangement edits into the doc before patching (no rollback)
    for (auto& a : apps) {
        p.doc["assets"][a.hash] = a.entry;
        if (p.doc.contains("clips") && p.doc["clips"].contains(a.clipId)) {
            p.doc["clips"][a.clipId]["asset"] = a.hash;
            if (a.params.is_object() && !a.params.empty())
                p.doc["clips"][a.clipId]["params"] = a.params;  // persist edited text/emotion/prompt
            // snap the clip to the new asset's natural length (audio/viseme duration) so it FITS —
            // switching a VO voice changes its length, and a stale dur cuts the line off / desyncs.
            // …but only for AUDIO clips (a VO's length defines its dur). An avatar clip's asset is a
            // viseme TRACK, not its displayed content — snapping it would clobber the on-screen
            // duration and open a gap between avatar clips. So snap only "speech" assets.
            double md = a.entry.value("meta", json::object()).value("duration", 0.0);
            if (md > 0.01 && a.entry.value("type", std::string()) == "speech") {
                // played dur = RAW audio dur ÷ playback rate (explicit params.rate, else the
                // project default meta.speech_rate on tts rows — shorts run 1.3x). Snapping to
                // the raw length left 1.3x clips ~30% long after an in-editor regen, so they
                // overlapped the next line / left dead air (fixed 2026-07-07).
                auto& cl = p.doc["clips"][a.clipId];
                double rate = 1.0;
                {
                    std::string rw = cl.value("row", std::string());
                    if (p.doc.contains("rows") && p.doc["rows"].contains(rw)
                        && p.doc["rows"][rw].value("type", std::string()) == "tts")
                        rate = p.speechRate;
                    if (cl.contains("params") && cl["params"].is_object() && cl["params"].contains("rate"))
                        rate = cl["params"].value("rate", rate);
                    if (rate < 0.5) rate = 0.5;
                    if (rate > 2.0) rate = 2.0;
                }
                cl["dur"] = md / rate;
                // A regenerated speech asset is a FRESH full recording of the line, so any `in` (source
                // in-point — e.g. left over from a clip SPLIT, which offsets the right half into the OLD
                // shared audio) now seeks PAST the new audio → silence (the user's split-then-regen bug).
                if (p.doc["clips"][a.clipId].contains("params") && p.doc["clips"][a.clipId]["params"].is_object())
                    p.doc["clips"][a.clipId]["params"].erase("in");
            }
        }
        fprintf(stderr, "generated %s → asset %s\n", a.clipId.c_str(), a.hash.c_str());
    }
    // Rebuild from the patched doc in-memory — NO reread (it would clobber unsaved arrangement
    // edits we just folded in) and NO forced save. The new asset is an unsaved change like any
    // edit (Ctrl+S or the headless --generate path persists it); the undo checkpoint captures it.
    std::string path = p.path;
    p = parse_project_json(std::move(p.doc), path);
    bufFor.clear();          // force the properties param buffers to refresh
    g_undoDirty = true;      // async landing (outside ImGui) → make the generation an undo step
}

// Sync the edited in-memory clips (timeline drag/trim + inspector transform/params) back into
// the raw doc and write the project file. Generate persists only the clip it touched; this
// persists the whole ARRANGEMENT so it survives reload + reaches --export (which reads the file).
// Fold the edited in-memory clips (timeline drag/trim + inspector transform/params/keyframes) +
// track reorder back into the raw doc — WITHOUT writing to disk. save_project (then writes) and
// the structural edits (split/delete/generate, which then patch + rebuild the doc) all call this
// FIRST, so unsaved arrangement edits are never lost. False if there's no clips map to sync into.
static bool sync_to_doc(Project& p) {
    if (!p.doc.is_object() || !p.doc.contains("clips") || !p.doc["clips"].is_object()) return false;
    for (auto& kv : p.clips) {
        const Clip& c = kv.second;
        if (!p.doc["clips"].contains(c.id)) continue;
        json& cj = p.doc["clips"][c.id];
        cj["row"] = c.row;          // a clip dragged to another (same-type) row changes its row + membership
        cj["start"] = c.start;
        cj["dur"] = c.dur;
        json tr = (cj.contains("transform") && cj["transform"].is_object()) ? cj["transform"] : json::object();
        tr["pos"] = json::array({c.tx_pos[0], c.tx_pos[1]});
        tr["scale"] = json::array({c.tx_scale[0], c.tx_scale[1]});
        tr["rot"] = c.tx_rot;
        tr["opacity"] = c.tx_opacity;
        tr["anchor"] = json::array({c.tx_anchor[0], c.tx_anchor[1]});
        cj["transform"] = tr;
        if (c.params.is_object()) cj["params"] = c.params;
        if (!c.asset.empty()) cj["asset"] = c.asset;
        // keyframes — the inspector's "Keyframes" panel edits these in-memory; serialize for Save
        if (!c.keyframes.empty()) {
            json kfj = json::object();
            for (auto& kp : c.keyframes) {
                json arr = json::array();
                for (auto& k : kp.second) {
                    json o = json{{"t", k.t}};
                    if (k.v.size() == 1) o["v"] = k.v[0]; else o["v"] = k.v;
                    if (k.interp != "linear") o["interp"] = k.interp;
                    if (k.interp == "spring") o["spring"] = json{{"stiffness", k.stiffness}, {"damping", k.damping}};
                    arr.push_back(o);
                }
                kfj[kp.first] = arr;
            }
            cj["keyframes"] = kfj;
        } else cj.erase("keyframes");
    }
    // rebuild row→clips membership (a clip moved between rows changes which row lists it + the
    // compositor walks row.clips for z-order, so the doc must match the model's membership)
    if (p.doc.contains("rows") && p.doc["rows"].is_object())
        for (auto& kv : p.rows) {
            if (!p.doc["rows"].contains(kv.first) || !p.doc["rows"][kv.first].is_object()) continue;
            json arr = json::array();
            for (auto& cid : kv.second.clips) arr.push_back(cid);
            p.doc["rows"][kv.first]["clips"] = arr;
            if (kv.second.params.is_object())                       // persist lane params (volume, normalize, …)
                p.doc["rows"][kv.first]["params"] = kv.second.params;
        }
    // persist track reorder (▲▼) — reflect p.tracks order into the doc by id
    if (p.doc.contains("tracks") && p.doc["tracks"].is_array()) {
        json reordered = json::array();
        for (auto& tk : p.tracks)
            for (auto& dj : p.doc["tracks"])
                if (dj.is_object() && dj.value("id", std::string()) == tk.id) { reordered.push_back(dj); break; }
        if (reordered.size() == p.doc["tracks"].size()) p.doc["tracks"] = reordered;
    }
    return true;
}

// Keep only the newest `keep` timestamped backups for a project stem in `.backups/`.
static void prune_backups(const std::string& bdir, const std::string& stem, int keep) {
    std::vector<std::string> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(to_w(bdir + "/" + stem + ".*.bak").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(from_w(fd.cFileName)); }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    if ((int)names.size() <= keep) return;
    std::sort(names.begin(), names.end());                        // timestamp names sort chronologically
    for (size_t i = 0; i + (size_t)keep < names.size(); i++)      // delete the oldest overflow
        DeleteFileW(to_w(bdir + "/" + names[i]).c_str());
}

// Rotate a backup BEFORE overwriting a project file. Undo is in-memory only, so this is the on-disk
// safety net: a bad save (or an accidentally-dropped track — the kirby music-loss incident) is
// recoverable from `<path>.bak` (the immediately-prior save) or a timestamped ring in `<dir>/.backups/`
// (survives several bad saves in a row). Never lets an empty/failed read clobber a good backup.
static void backup_before_save(const std::string& path) {
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;   // first save — nothing to keep
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (cur.empty()) return;
    { std::ofstream b(path + ".bak", std::ios::binary); if (b) b << cur; }
    size_t sl = path.find_last_of("/\\");
    std::string dir  = sl == std::string::npos ? std::string(".") : path.substr(0, sl);
    std::string stem = sl == std::string::npos ? path : path.substr(sl + 1);
    std::string bdir = dir + "/.backups";
    ensure_dir(bdir);
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d%02d%02d-%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    { std::ofstream b(bdir + "/" + stem + "." + ts + ".bak", std::ios::binary); if (b) b << cur; }
    prune_backups(bdir, stem, 12);                                // keep the last dozen
}

static bool save_project(Project& p) {
    if (!sync_to_doc(p)) return false;
    backup_before_save(p.path);
    std::ofstream out(p.path);
    if (!out) return false;
    out << p.doc.dump(2) << "\n";
    return true;
}

// Move a clip to another row. Only succeeds between rows of the SAME type (a tts clip can't live on
// an image row) — that's the constraint when dragging a clip vertically onto a different lane.
// Updates both the clip's back-reference (c.row) and the rows' membership lists (the compositor
// walks row.clips for z-order); persistence is via sync_to_doc. Returns true if the move happened.
static bool move_clip_to_row(Project& p, const std::string& clipId, const std::string& newRow) {
    auto cit = p.clips.find(clipId);
    auto nit = p.rows.find(newRow);
    if (cit == p.clips.end() || nit == p.rows.end()) return false;
    Clip& c = cit->second;
    if (c.row == newRow) return false;
    if (nit->second.type != c.type) return false;          // same-type lanes only
    auto oit = p.rows.find(c.row);
    if (oit != p.rows.end()) {
        auto& v = oit->second.clips;
        v.erase(std::remove(v.begin(), v.end(), clipId), v.end());
    }
    nit->second.clips.push_back(clipId);
    c.row = newRow;
    return true;
}

// ── source-anchored trims ───────────────────────────────────────────────────
// play-seconds → source-seconds factor for a clip whose asset is a TIMED medium: tts/music
// play at `rate` (tts defaults to the project speech rate — shorts run ~1.3x), video at
// `speed`. 0 for untimed clips (image/caption/code/…) — they have no in-point to anchor.
static double src_time_factor(const Project& p, const std::string& row, const std::string& type, const json& params) {
    auto rit = p.rows.find(row);
    const std::string& rt = (rit != p.rows.end()) ? rit->second.type : type;
    if (rt == "tts" || rt == "music") {
        double def = (rt == "tts") ? p.speechRate : 1.0;
        double r = params.is_object() ? params.value("rate", def) : def;
        return std::min(2.0, std::max(0.5, r));   // the mixer's stretch clamp
    }
    if (rt == "video") return params.is_object() ? params.value("speed", 1.0) : 1.0;
    return 0.0;
}

// Advance an asset in-point by `adv` SOURCE-seconds. A looping video (loop defaults true)
// folds a forward advance across its loop seam — the repeating segment is [in, EOF) — so the
// frame shown at the fold point is exactly what the unsplit clip showed there. Non-video
// assets aren't in asset_video and just add. Floored at 0: no media before the source start.
static double advance_in_folded(const Project& p, const std::string& asset, const json& params, double in, double adv) {
    bool loops = true;   // video default; "pingpong" (string) folds like loop — fine for b-roll
    if (params.is_object() && params.contains("loop") && params["loop"].is_boolean()) loops = params["loop"].get<bool>();
    double nin = in + adv;
    auto vmi = p.asset_video.find(asset);
    if (loops && adv > 0 && vmi != p.asset_video.end() && vmi->second.fps > 0 && vmi->second.frames > 0) {
        double span = vmi->second.frames / vmi->second.fps - in;
        if (span > 1e-6) nin = in + std::fmod(adv, span);
    }
    return std::max(0.0, nin);
}

// Split the clip at timeline time t into two: the original keeps [start, t); a NEW clip covers
// [t, end). Timed media (tts/music/video) gives the right half an advanced asset `in`-point so
// it CONTINUES from the split point instead of restarting (rate/speed-aware; honored by the
// mixer, the compositor and export). Keyframes are absolute-time, so
// both halves keep them and sample correctly. Patches the doc, persists, and reloads p.
// (Reloads p → the caller must not hold a Clip& into p afterwards; callers defer this.)
static void split_clip(Project& p, const std::string& clipId, double t) {
    auto it = p.clips.find(clipId);
    if (it == p.clips.end()) return;
    const Clip& c = it->second;
    if (t <= c.start + 0.02 || t >= c.start + c.dur - 0.02) return;   // playhead must be inside
    // Fold unsaved edits (a dragged start, a trim, param tweaks) into the doc FIRST. Otherwise we
    // split against the STALE on-disk geometry: the left half keeps the old start while the right
    // is placed at the live playhead → the halves don't line up (the "overlap"), and the reread
    // below would snap every other clip back to its last-saved spot (the "rollback"). After the
    // sync the doc's clip matches the in-memory clip, so the cut is exact.
    sync_to_doc(p);
    if (!p.doc.contains("clips") || !p.doc["clips"].contains(clipId)) return;
    std::string row = c.row, type = c.type;
    double cstart = c.start, cend = c.start + c.dur, splitLocal = t - cstart;
    std::string nid; int n = 2;
    do { nid = clipId + "_" + std::to_string(n++); } while (p.doc["clips"].contains(nid));
    json right = p.doc["clips"][clipId];                   // clone (params/transform/keyframes/asset)
    p.doc["clips"][clipId]["dur"] = splitLocal;            // left half  = [start, t)
    right["start"] = t;
    right["dur"] = cend - t;                               // right half = [t, end) — adjacent, no overlap, no ripple
    {   // timed media continues from the split point (play→source: × rate for audio, × speed for video)
        json rp = (right.contains("params") && right["params"].is_object()) ? right["params"] : json::object();
        double k = src_time_factor(p, row, type, rp);
        if (k > 1e-6) {
            rp["in"] = advance_in_folded(p, right.value("asset", std::string()), rp, rp.value("in", 0.0), splitLocal * k);
            right["params"] = rp;
        }
    }
    p.doc["clips"][nid] = right;
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips")
        && p.doc["rows"][row]["clips"].is_array())
        p.doc["rows"][row]["clips"].push_back(nid);
    // Rebuild the model from the patched doc — NO disk write (don't force a save) and NO reread
    // (which clobbers unsaved edits). The split is now an unsaved change like a move/trim; Ctrl+S persists it.
    std::string path = p.path; p = parse_project_json(std::move(p.doc), path);
    fprintf(stderr, "split %s at %.2fs → %s (in-memory)\n", clipId.c_str(), t, nid.c_str());
}

// Duplicate a clip — a full copy (params/transform/keyframes/asset) placed right AFTER the original
// on the same row. Returns the new id (the caller selects it). No ripple: the user drags it where
// they want. In-memory like split/move (Ctrl+S persists). A quick alternative to split-and-edit.
// Ctrl+drag-to-duplicate state: the clip a copy was already spawned for THIS drag (so it fires once),
// and where to place that copy — the original's start, so the copy stays put while the original drags
// off (its InvisibleButton id is stable, so ImGui keeps the drag alive across the p rebuild).
static std::string g_ctrlDupDragId;      // clip a Ctrl+drag copy was already spawned for (fires once/drag)
static std::string g_dragDupReq;         // DrawTimeline → DrawUI: request a duplicate (deferred; p rebuild)
static double g_dupAtStart = -1e18;      // where to place the pending copy (-1e18 = default: right after)
// Placement tool: click empty timeline space to add a clip of the armed type (drag = draw its length).
static std::string g_placeType;          // armed clip type ("" = off); set by the palette, cleared by Esc
static std::string g_placeReq;           // DrawTimeline → DrawUI: a placement to commit (deferred)
static std::string g_placeReqRow;        // the lane the cursor was over (row-selection prefers it)
static double g_placeReqT = 0, g_placeReqDur = -1;

static std::string duplicate_clip(Project& p, const std::string& clipId, double atStart = -1e18) {
    auto it = p.clips.find(clipId);
    if (it == p.clips.end()) return "";
    double newStart = (atStart > -1e17) ? atStart : it->second.start + it->second.dur;
    std::string row = it->second.row;
    sync_to_doc(p);                                         // fold unsaved edits in first (like split)
    if (!p.doc.contains("clips") || !p.doc["clips"].contains(clipId)) return "";
    std::string nid = clipId + "_copy"; int n = 2;
    while (p.doc["clips"].contains(nid) || p.clips.count(nid)) nid = clipId + "_copy" + std::to_string(n++);
    json dup = p.doc["clips"][clipId];                      // clone params/transform/keyframes/asset
    dup["start"] = newStart;                                // placed right after the original
    p.doc["clips"][nid] = dup;
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips")
        && p.doc["rows"][row]["clips"].is_array())
        p.doc["rows"][row]["clips"].push_back(nid);
    std::string path = p.path; p = parse_project_json(std::move(p.doc), path);
    fprintf(stderr, "duplicate %s → %s @ %.2fs (in-memory)\n", clipId.c_str(), nid.c_str(), newStart);
    return nid;
}

// Delete a clip from the project (doc + its row's list), persist, reload p. (Reloads p.)
static void delete_clip(Project& p, const std::string& clipId) {
    if (!p.doc.contains("clips") || !p.doc["clips"].contains(clipId)) return;
    sync_to_doc(p);   // fold unsaved edits in first so the rest of the timeline doesn't roll back
    std::string row = p.clips.count(clipId) ? p.clips[clipId].row : std::string();
    p.doc["clips"].erase(clipId);
    if (!row.empty() && p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips")) {
        json keep = json::array();
        for (auto& v : p.doc["rows"][row]["clips"]) if (v.is_string() && v.get<std::string>() != clipId) keep.push_back(v);
        p.doc["rows"][row]["clips"] = keep;
    }
    std::string path = p.path; p = parse_project_json(std::move(p.doc), path);   // in-memory, no disk write/reread
    fprintf(stderr, "deleted clip %s (in-memory)\n", clipId.c_str());
}

// Delete a whole track: its rows and every clip on them. Same in-memory doc-patch + rebuild pattern as
// delete_clip (Ctrl+S persists). Keeps at least one track. (Reloads p → callers must defer + not hold a Clip&.)
static void delete_track(Project& p, const std::string& trackId) {
    if (p.tracks.size() <= 1) return;                       // keep at least one track
    auto tit = std::find_if(p.tracks.begin(), p.tracks.end(), [&](const Track& t){ return t.id == trackId; });
    if (tit == p.tracks.end()) return;
    std::vector<std::string> rowIds = tit->rows;
    sync_to_doc(p);                                         // fold unsaved edits in first
    if (p.doc.contains("tracks") && p.doc["tracks"].is_array()) {   // drop the track entry (by id)
        json keep = json::array();
        for (auto& tj : p.doc["tracks"]) if (!(tj.is_object() && tj.value("id", std::string()) == trackId)) keep.push_back(tj);
        p.doc["tracks"] = keep;
    }
    for (const auto& rid : rowIds) {                        // drop each row + all its clips
        if (p.doc.contains("rows") && p.doc["rows"].contains(rid) && p.doc["rows"][rid].contains("clips")
            && p.doc["rows"][rid]["clips"].is_array() && p.doc.contains("clips"))
            for (auto& cj : p.doc["rows"][rid]["clips"]) if (cj.is_string()) p.doc["clips"].erase(cj.get<std::string>());
        if (p.doc.contains("rows")) p.doc["rows"].erase(rid);
    }
    std::string path = p.path; p = parse_project_json(std::move(p.doc), path);
    fprintf(stderr, "deleted track %s (%zu rows) (in-memory)\n", trackId.c_str(), rowIds.size());
}

// Background: poll every provider's /healthz so the UI can show a status dot.
static DWORD WINAPI health_worker(LPVOID) {
    for (;;) {
        for (auto& kv : g_providers) {
            if (!kv.second.enabled || kv.second.url.empty()) continue;
            long st = 0; std::string b;
            bool up = http_do("GET", kv.second.url + "/healthz", "", nullptr, &st, &b, nullptr) && st == 200;
            EnterCriticalSection(&g_healthCS);
            g_health[kv.first] = up ? 1 : 2;
            LeaveCriticalSection(&g_healthCS);
        }
        Sleep(5000);
    }
    return 0;
}

// ─────────────────────────────── UI ───────────────────────────────────────
struct UIState {
    double playhead = 0.0;
    std::string selected;
    bool playing = false;     // transport running (playhead advances on the wall clock)
    bool scrubbed = false;    // playhead was dragged this frame (→ reseek audio while playing)
    double tlZoom = 0.0;      // timeline px/sec; 0 = auto-fit the whole project to the panel
    double tlScroll = 0.0;    // timeline horizontal scroll offset (seconds, left edge)
};

static float g_tlVScroll = -1.f;   // --tl-vscroll <px>: pin the timeline lane scroll (headless shots/verify)

static ImU32 type_color(const std::string& t) {
    if (t == "tts")     return IM_COL32( 80, 160, 240, 255);
    if (t == "avatar")  return IM_COL32(220, 120, 200, 255);
    if (t == "image")   return IM_COL32(120, 200, 120, 255);
    if (t == "video")   return IM_COL32(230, 170,  90, 255);
    if (t == "music")   return IM_COL32(180, 140, 230, 255);
    if (t == "caption") return IM_COL32(200, 200, 110, 255);
    if (t == "gradient") return IM_COL32(110, 130, 175, 255);
    if (t == "blur")    return IM_COL32(150, 185, 215, 255);
    if (t == "anchor")  return IM_COL32( 95, 220, 185, 255);   // caption anchor — the caption-move handle
    if (t == "diagram") return IM_COL32(120, 205, 235, 255);
    if (t == "filler")  return IM_COL32( 90, 110, 130, 255);
    return IM_COL32(150, 150, 150, 255);
}

static bool clip_active(const Clip& c, double t) { return t >= c.start && t < c.start + c.dur; }

// ── pngtuber avatar (the Tier-A state machine, docs/ARCHITECTURE.md §7) ──────
// A procedural chibi driven by the align viseme track (mouth openness) + the clip's emotion
// tag (expression) + procedural idle (blink, breathing). All pure GPU compositing — instant
// at the playhead. This is now the FALLBACK: when a rig manifest + sprites are present the
// compositor renders real Gemma sprites instead (avatar_sprite, below), driven by the same
// openness+emotion state machine; this draw stands in when a rig is missing.
struct Expr { ImU32 accent; float brow; float eyeOpen; };
static Expr expr_for(const std::string& e) {
    if (e == "smug")                                   return {IM_COL32(220,120,200,255), -0.5f, 0.65f};
    if (e == "happy" || e == "happy-sparkle" || e == "excited") return {IM_COL32(255,180,90,255), 0.4f, 1.0f};
    if (e == "annoyed" || e == "annoyed-puff" || e == "angry")  return {IM_COL32(230,90,90,255), -1.0f, 0.8f};
    if (e == "surprised")                              return {IM_COL32(120,200,230,255), 1.0f, 1.25f};
    if (e == "confused")                               return {IM_COL32(200,200,110,255), 0.3f, 0.9f};
    if (e == "sad")                                    return {IM_COL32(120,150,210,255), 0.7f, 0.7f};
    return {IM_COL32(185,150,225,255), 0.0f, 1.0f};  // neutral
}
static void add_ellipse(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col) {
    if (rx < 0.5f || ry < 0.5f) return;
    ImVec2 p[24];
    const int n = 22;
    for (int i = 0; i < n; ++i) { float a = (float)i / n * 6.2831853f; p[i] = ImVec2(c.x + cosf(a) * rx, c.y + sinf(a) * ry); }
    dl->AddConvexPolyFilled(p, n, col);
}
static void draw_pngtuber(ImDrawList* dl, ImVec2 a, ImVec2 b, double openness, const std::string& emotion, double t) {
    Expr ex = expr_for(emotion);
    float w = b.x - a.x, h = b.y - a.y, cx = (a.x + b.x) * 0.5f;
    float bob = sinf((float)t * 1.8f) * h * 0.012f;             // breathing
    float top = a.y + bob;
    const ImU32 skin = IM_COL32(247,226,234,255), hair = IM_COL32(150,90,200,255),
                horn = IM_COL32(34,30,40,255), ink = IM_COL32(70,46,82,255), body = IM_COL32(72,62,96,255);

    dl->AddRectFilled(ImVec2(cx - w*0.30f, top + h*0.56f), ImVec2(cx + w*0.30f, top + h*0.99f), body, 12.f);
    dl->AddRectFilled(ImVec2(cx - w*0.30f, top + h*0.56f), ImVec2(cx + w*0.30f, top + h*0.60f),
                      (ex.accent & 0x00FFFFFF) | 0x90000000, 12.f);  // accent collar

    float R = w * 0.30f;
    ImVec2 hc(cx, top + h*0.34f);
    dl->AddTriangleFilled(ImVec2(hc.x-R*0.78f,hc.y-R*0.55f), ImVec2(hc.x-R*0.5f,hc.y-R*1.45f), ImVec2(hc.x-R*0.18f,hc.y-R*0.85f), horn);
    dl->AddTriangleFilled(ImVec2(hc.x+R*0.78f,hc.y-R*0.55f), ImVec2(hc.x+R*0.5f,hc.y-R*1.45f), ImVec2(hc.x+R*0.18f,hc.y-R*0.85f), horn);
    dl->AddCircleFilled(hc, R, skin, 40);
    dl->AddCircleFilled(ImVec2(hc.x, hc.y - R*0.62f), R*0.98f, hair, 36);     // hair crown
    dl->AddCircleFilled(ImVec2(hc.x, hc.y + R*0.14f), R*0.95f, skin, 40);     // face over hair

    bool blink = fmodf((float)t, 3.4f) < 0.12f;
    float eyeY = hc.y + R*0.06f, eyeDX = R*0.44f, erx = R*0.17f, ery = R*0.23f * ex.eyeOpen;
    for (int s = -1; s <= 1; s += 2) {
        ImVec2 e(hc.x + s*eyeDX, eyeY);
        if (blink) dl->AddLine(ImVec2(e.x-erx, e.y), ImVec2(e.x+erx, e.y), ink, 2.5f);
        else { add_ellipse(dl, e, erx, ery, IM_COL32(255,255,255,255)); add_ellipse(dl, e, erx*0.62f, ery*0.72f, ex.accent); add_ellipse(dl, e, erx*0.3f, ery*0.4f, ink); }
        float inx = e.x - s*erx*1.3f, outx = e.x + s*erx*1.3f, by = e.y - ery - R*0.16f, dy = -ex.brow * R*0.16f;
        dl->AddLine(ImVec2(outx, by), ImVec2(inx, by + dy), ink, 3.0f);       // eyebrow (emotion tilt)
    }

    float mo = (float)std::max(0.0, std::min(1.0, openness));                 // mouth = viseme openness
    ImVec2 mc(hc.x, hc.y + R*0.52f);
    float mrx = R*0.24f, mry = R*0.035f + mo * R*0.28f;
    add_ellipse(dl, mc, mrx, mry, IM_COL32(150,52,74,255));
    if (mo > 0.18f) add_ellipse(dl, ImVec2(mc.x, mc.y + mry*0.35f), mrx*0.6f, mry*0.5f, IM_COL32(210,110,120,255));  // tongue
    if (mo < 0.08f) dl->AddLine(ImVec2(mc.x-mrx, mc.y), ImVec2(mc.x+mrx, mc.y), IM_COL32(150,52,74,255), 2.5f);

    dl->AddCircle(hc, R, ex.accent, 40, 2.0f);
    char lbl[64]; snprintf(lbl, sizeof lbl, "%s  open=%.2f", emotion.c_str(), mo);
    dl->AddText(ImVec2(a.x + 4, top + 2), ex.accent, lbl);
}

// ── avatar rig (real Gemma sprites) ─────────────────────────────────────────
// A rig manifest (presets/avatars/<rig>/manifest.json) maps each emotion/pose to a sprite.
// Static `sprite` entries are the active pngtuber path; legacy mouth/blink fields are still
// parsed only so older experiments load. Loaded+cached once; sprites go through get_texture.
struct AvatarRig {
    std::string dir;                 // presets/avatars/<rig>
    std::string fallback = "neutral";
    float openThreshold = 0.45f;
    float blinkPeriod = 3.4f, blinkDur = 0.12f;             // a blink every period, lasting dur
    std::map<std::string, std::vector<std::string>> mouths;       // emotion → eyes-OPEN mouth frames
    std::map<std::string, std::vector<std::string>> mouthsBlink;  // emotion → eyes-CLOSED mouth frames (grid rig)
    std::map<std::string, std::string> blink;               // emotion → single eyes-closed sprite (simple rig)
    bool ok = false;
};
static std::map<std::string, AvatarRig> g_rigCache;

// The canonical pngtuber expression set (mirrors expr_for/canon_emotion). A library rig resolves
// each of these from its prefix (emotion E → library/images/<prefix>E.png); explicit overrides add
// or replace any of them — including a manually-named emotion outside this set.
static const char* const AVATAR_EMOS[] = {"neutral", "happy", "smug", "confused", "annoyed", "surprised", "sad"};
static const int AVATAR_EMO_N = 7;

static const AvatarRig* get_rig(const std::string& rig) {
    if (rig.empty()) return nullptr;
    auto it = g_rigCache.find(rig);
    if (it != g_rigCache.end()) return it->second.ok ? &it->second : nullptr;
    AvatarRig r;
    r.dir = "presets/avatars/" + rig;
    // repo-root presets first; else the PROJECT's own presets (a portable project dir can
    // carry legacy/one-off rigs the code repo no longer ships)
    if (!g_projDir.empty() && GetFileAttributesA((r.dir + "/manifest.json").c_str()) == INVALID_FILE_ATTRIBUTES
        && GetFileAttributesA((g_projDir + "/" + r.dir + "/manifest.json").c_str()) != INVALID_FILE_ATTRIBUTES)
        r.dir = g_projDir + "/" + r.dir;
    std::ifstream f(r.dir + "/manifest.json");
    if (f) {
        try {
            json j; f >> j;
            r.fallback = j.value("fallback", std::string("neutral"));
            r.openThreshold = (float)j.value("mouth_open_threshold", 0.45);
            json bt = j.value("blink", json::object());  // optional rig-level blink timing
            r.blinkPeriod = (float)bt.value("period", 3.4);
            r.blinkDur = (float)bt.value("dur", 0.12);
            json exprs = j.value("expressions", json::object());  // bind: .items() on a temporary dangles
            for (auto& kv : exprs.items()) {
                std::vector<std::string> frames;
                // STATIC rig: one sprite per pose — `"emotion": {"sprite": "x.png"}`.
                // Legacy manifests may still contain ordered `mouths`/`blink` fields; they are
                // parsed for compatibility, but current pngtuber UX uses static sprites only.
                std::string single = kv.value().value("sprite", std::string());
                if (!single.empty()) {
                    frames.push_back(r.dir + "/" + single);
                } else {
                    json ms = kv.value().value("mouths", json::array());
                    for (auto& m : ms)
                        if (m.is_string()) frames.push_back(r.dir + "/" + m.get<std::string>());
                }
                if (!frames.empty()) r.mouths[kv.key()] = frames;
                // ANIMATED grid (authored sheets): the eyes-CLOSED row at the same mouth levels,
                // so a blink closes the eyes without the mouth dropping.
                std::vector<std::string> bframes;
                for (auto& m : kv.value().value("mouths_blink", json::array()))
                    if (m.is_string()) bframes.push_back(r.dir + "/" + m.get<std::string>());
                if (!bframes.empty()) r.mouthsBlink[kv.key()] = bframes;
                std::string bl = kv.value().value("blink", std::string());  // single eyes-closed frame (simple rig)
                if (!bl.empty()) r.blink[kv.key()] = r.dir + "/" + bl;
            }
            r.ok = !r.mouths.empty();
        } catch (...) {}
    }
    // Library-authored rig (the user's "avatar item in the library"): a tiny def at
    // library/avatars/<rig>.avatar.json = a `prefix` (emotion E → library/images/<prefix>E.png) plus
    // optional `emotions` overrides (a manually-named emotion → a specific library image). Each
    // resolves to a single static pose (openness no-ops, like the SD rig). Authored/edited in the
    // Library + Viewer; placing it spawns an avatar clip whose row references this rig by name.
    if (!r.ok) {
        std::ifstream lf(g_libraryDir + "/avatars/" + rig + ".avatar.json");
        if (lf) {
            try {
                json j; lf >> j;
                r.fallback = j.value("fallback", std::string("neutral"));
                std::string prefix = j.value("prefix", std::string());
                json em = j.value("emotions", json::object());                 // explicit overrides
                std::vector<std::string> keys(AVATAR_EMOS, AVATAR_EMOS + AVATAR_EMO_N);
                for (auto& kv : em.items())                                     // + any manually-named emotion
                    if (std::find(keys.begin(), keys.end(), kv.key()) == keys.end()) keys.push_back(kv.key());
                for (auto& emo : keys) {
                    std::string file = em.is_object() ? em.value(emo, std::string()) : std::string();  // override wins
                    if (file.empty() && !prefix.empty()) file = prefix + emo + ".png";                 // else prefix convention
                    if (file.empty()) continue;
                    std::string full = g_libraryDir + "/images/" + file;
                    if (GetFileAttributesW(to_w(full).c_str()) != INVALID_FILE_ATTRIBUTES)
                        r.mouths[emo] = {full};                                 // 1-frame static pose (full path → get_texture)
                }
                r.ok = !r.mouths.empty();
                if (r.ok) r.dir = g_libraryDir + "/avatars";                    // frames already hold full paths
            } catch (...) {}
        }
    }
    if (!r.ok) fprintf(stderr, "avatar rig '%s' unavailable (presets/avatars/%s/manifest.json | %s/avatars/%s.avatar.json) — procedural fallback\n",
                       rig.c_str(), rig.c_str(), g_libraryDir.c_str(), rig.c_str());
    g_rigCache[rig] = r;
    return r.ok ? &g_rigCache[rig] : nullptr;
}
static void invalidate_rig(const std::string& name) { g_rigCache.erase(name); }   // forward-declared up top; live re-resolve after an edit

// Canonicalize an emotion tag to a rig key (mirrors expr_for's alias set).
static std::string canon_emotion(const std::string& e) {
    if (e == "happy-sparkle" || e == "excited") return "happy";
    if (e == "annoyed-puff" || e == "angry")    return "annoyed";
    if (e == "shock" || e == "shocked")          return "surprised";
    if (e == "teacher" || e == "teaching" || e == "explain" || e == "explaining") return "teaching";
    if (e == "deadpan" || e == "flat")           return "deadpan";
    if (e == "laugh" || e == "laughing")         return "laugh";
    if (e == "impressed")                        return "impressed";
    if (e == "sweet")                            return "sweet";
    if (e == "wonderful")                        return "wonderful";
    if (e == "floating" || e == "float")         return "floating";
    return e;  // neutral/happy/smug/confused/annoyed/surprised/sad pass through
}

// Map a free-text emotion/instruct string (e.g. a VO line's "smug, teasing, warm") to a rig
// expression key by keyword — so an avatar's expression can auto-follow the driven line. "" if
// nothing matches. More specific cues first; substring match catches "annoyed"/"irritated"/etc.
static std::string emotion_from_text(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) t += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
    static const struct { const char* kw; const char* emo; } kws[] = {
        {"teacher","teaching"}, {"teaching","teaching"}, {"explain","teaching"}, {"lecture","teaching"},
        {"laugh","laugh"}, {"giggle","laugh"},
        {"wonderful","wonderful"}, {"sweet","sweet"}, {"impressed","impressed"},
        {"sparkle","happy"}, {"excited","happy"}, {"cheer","happy"}, {"happy","happy"},
        {"smile","happy"},   {"joy","happy"},
        {"smug","smug"}, {"smirk","smug"}, {"teasing","smug"}, {"sly","smug"}, {"confiden","smug"}, {"mischiev","smug"},
        {"annoy","annoyed"}, {"angry","annoyed"}, {"mad","annoyed"}, {"pout","annoyed"}, {"irritat","annoyed"}, {"puff","annoyed"},
        {"surprise","surprised"}, {"shock","surprised"}, {"gasp","surprised"}, {"amaze","surprised"},
        {"confus","confused"}, {"puzzl","confused"}, {"question","confused"}, {"unsure","confused"},
        {"sad","sad"}, {"cry","sad"}, {"sorrow","sad"}, {"melanch","sad"}, {"down","sad"}, {"gloom","sad"},
        {"deadpan","deadpan"}, {"flat","deadpan"}, {"neutral","neutral"}, {"calm","neutral"},
    };
    for (auto& k : kws) if (t.find(k.kw) != std::string::npos) return k.emo;
    return "";
}

// Pick the sprite for an emotion/pose under a rig. nullptr ⇒ caller draws procedural.
// Static one-frame entries are the intended path; multi-frame legacy entries collapse to the
// resting frame because the compositor passes openness=0 for pngtuber rigs.
// The resting-frame sprite uri for an emotion (openness 0, no blink) — what the pickers thumbnail.
// Resolves like avatar_sprite but WITHOUT decoding, so the async thumbnail loader can key on the path.
// An EXACT rig key always wins over canonicalization: canon_emotion collapses the script
// vocabulary onto the small legacy set (explaining→teaching, shocked→surprised, …), which is
// right only when the rig LACKS the richer key — a rig that ships a real `explaining` sprite
// must get it (the strict-teacher sprite hijacked every "explaining" beat otherwise).
static std::map<std::string, std::vector<std::string>>::const_iterator
rig_emotion_lookup(const AvatarRig* rig, const std::string& emotion) {
    auto it = rig->mouths.find(emotion);
    if (it == rig->mouths.end()) it = rig->mouths.find(canon_emotion(emotion));
    if (it == rig->mouths.end()) it = rig->mouths.find(rig->fallback);
    return it;
}
static std::string avatar_sprite_path(const AvatarRig* rig, const std::string& emotion) {
    if (!rig) return "";
    auto it = rig_emotion_lookup(rig, emotion);
    if (it == rig->mouths.end() || it->second.empty()) return "";
    return it->second[0];
}
static Tex* avatar_sprite(const AvatarRig* rig, const std::string& emotion, double openness, bool blink,
                          std::string* outPath = nullptr) {
    if (!rig) return nullptr;
    auto it = rig_emotion_lookup(rig, emotion);
    if (it == rig->mouths.end() || it->second.empty()) return nullptr;
    const std::string& mkey = it->first;          // the key that actually matched (after fallback)
    const auto& frames = it->second;
    // Quantize openness over the ordered mouth frames (closed→wide). Viseme openness is
    // piecewise-constant per cue, so a hard threshold doesn't flutter within a phoneme.
    size_t idx;
    if (frames.size() <= 1)        idx = 0;
    else if (frames.size() == 2)   idx = (openness >= rig->openThreshold) ? 1 : 0;
    else { double o = std::max(0.0, std::min(1.0, openness)); idx = (size_t)(o * (frames.size() - 1) + 0.5); }
    if (blink) {
        auto mb = rig->mouthsBlink.find(mkey);    // grid rig: eyes-closed at the SAME mouth level
        if (mb != rig->mouthsBlink.end() && idx < mb->second.size())
            if (Tex* t = get_texture(mb->second[idx])) { if (outPath) *outPath = mb->second[idx]; return t; }
        auto b = rig->blink.find(mkey);           // simple rig: a single eyes-closed frame
        if (b != rig->blink.end() && !b->second.empty())
            if (Tex* t = get_texture(b->second)) { if (outPath) *outPath = b->second; return t; }
    }
    if (outPath) *outPath = frames[idx];
    return get_texture(frames[idx]);
}

// Cached alpha bbox per source uri (the foot-shadow position). ImVec4 = (x0,y0,x1,y1).
static std::map<std::string, ImVec4> g_spriteBbox;
static bool sprite_bbox(const std::string& uri, ImVec4& bb) {
    auto it = g_spriteBbox.find(uri);
    if (it == g_spriteBbox.end()) {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        if (const SrcPix* sp = uri.empty() ? nullptr : get_src_pixels(uri))
            if (sp->w > 0) sprite_alpha_bbox(sp->px, sp->w, sp->h, x0, y0, x1, y1, 8);
        it = g_spriteBbox.emplace(uri, ImVec4((float)x0, (float)y0, (float)x1, (float)y1)).first;
    }
    bb = it->second;
    return bb.z >= bb.x && bb.w >= bb.y;
}

// Resolve a POSE VARIANT of `emo` (front / front34 / viewer34 / float_front / float34 /
// float_viewer34) → texture + path. The standing front is the bare emotion key; the others are
// "<base>_<pose>" with <base> = the canonical front file's stem (so aliased emotions like
// surprised→shocked find their shocked_<pose> variants). Falls back to the canonical front when a
// variant is absent. The sprite always composites whole (never cropped).
static Tex* avatar_variant(const AvatarRig* rig, const std::string& emo, const std::string& pose,
                           std::string* outPath) {
    std::string frontPath;
    Tex* front = avatar_sprite(rig, emo, 0.0, false, &frontPath);
    if (!front || !rig || pose.empty() || pose == "front" || pose == "auto") {
        if (outPath) *outPath = frontPath;
        return front;
    }
    std::string fn = frontPath;                                   // → base = front stem minus "_front"
    size_t sl = fn.find_last_of("/\\"); if (sl != std::string::npos) fn = fn.substr(sl + 1);
    size_t dot = fn.rfind('.'); if (dot != std::string::npos) fn = fn.substr(0, dot);
    if (fn.size() > 6 && fn.compare(fn.size() - 6, 6, "_front") == 0) fn = fn.substr(0, fn.size() - 6);
    std::string key = fn + "_" + pose;
    if (rig->mouths.count(key)) {
        std::string vp; Tex* v = avatar_sprite(rig, key, 0.0, false, &vp);
        if (v) { if (outPath) *outPath = vp; return v; }
    }
    if (outPath) *outPath = frontPath;                            // variant missing → canonical front
    return front;
}

// ImDrawList callback: switch the device to additive blend for the avatar "light-up" redraw
// (dst += sprite.rgb * alpha → the host brightens while talking). Pair with a
// ImDrawCallback_ResetRenderState afterwards to restore ImGui's normal alpha blend.
static void DrawCb_Additive(const ImDrawList*, const ImDrawCmd*) {
    if (g_blendAdd) { const float bf[4] = {0, 0, 0, 0}; g_ctx->OMSetBlendState(g_blendAdd, bf, 0xffffffff); }
}

// ───────────────────────── keyframe animation (the motion-graphics layer) ───
// Any numeric param (transform.pos/scale/opacity, or a clip param like code.typewrite)
// animates via a per-path keyframe curve (docs/PROJECT_FORMAT §clips). Evaluated at the
// TIMELINE playhead → pure compositing, instant. This is what gives Ken Burns (animated
// transform), pop-ins (spring scale) and typewriter reveals (animated typewrite).
static double spring_response(double tau, double k, double c) {  // normalized 0→1 step response
    if (tau <= 0) return 0.0;
    double w = sqrt(k > 1e-6 ? k : 1e-6), zeta = c / (2.0 * sqrt(k > 1e-6 ? k : 1e-6));
    if (zeta < 1.0) {                                  // underdamped → overshoots, then settles
        double wd = w * sqrt(1.0 - zeta * zeta), e = exp(-zeta * w * tau);
        return 1.0 - e * (cos(wd * tau) + (zeta * w / wd) * sin(wd * tau));
    }
    double e = exp(-w * tau);                          // critically/over-damped
    return 1.0 - e * (1.0 + w * tau);
}
static double bezier_ease(double u, const double out[2], const double in[2]) {  // cubic-bezier(out,in)
    double x1 = out[0], y1 = out[1], x2 = in[0], y2 = in[1], t = u;
    for (int k = 0; k < 8; ++k) {                      // Newton-solve t for Bx(t)=u, return By(t)
        double mt = 1 - t, x = 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t * t * t - u;
        double d = 3 * mt * mt * x1 + 6 * mt * t * (x2 - x1) + 3 * t * t * (1 - x2);
        if (fabs(d) < 1e-6) break;
        t -= x / d; t = t < 0 ? 0 : (t > 1 ? 1 : t);
    }
    double mt = 1 - t; return 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t;
}
// Sample a keyframe track at timeline time T (component comp for vec params). The LEFT
// keyframe's interp drives each segment; a trailing spring keeps settling past its target.
static double eval_kf(const std::vector<KF>& ks, double T, int comp, double fb) {
    if (ks.empty()) return fb;
    auto V = [&](const KF& k) -> double { return comp < (int)k.v.size() ? k.v[comp] : (k.v.empty() ? fb : k.v.back()); };
    if (T <= ks.front().t) return V(ks.front());
    size_t i = 0;
    for (size_t j = 0; j + 1 < ks.size(); ++j) if (T >= ks[j].t) i = j;
    const KF& a = ks[i]; const KF& b = ks[i + 1];
    double av = V(a), bv = V(b);
    if (a.interp == "spring") return av + (bv - av) * spring_response(T - a.t, a.stiffness, a.damping);
    if (T >= b.t) return bv;
    double dt = b.t - a.t, u = dt > 1e-9 ? (T - a.t) / dt : 1.0;
    if (a.interp == "constant") return av;
    if (a.interp == "bezier") u = bezier_ease(u, a.outh, b.inh);
    u = u < 0 ? 0 : (u > 1 ? 1 : u);
    return av + (bv - av) * u;
}
// Effective transform component (keyframed → fallback to the static transform field).
static double anim_xform(Clip& c, const char* path, int comp, double T, double fb) {
    auto it = c.keyframes.find(path);
    return it == c.keyframes.end() ? fb : eval_kf(it->second, T, comp, fb);
}
// Effective clip param: keyframed (params.<name>) → static params value → fallback.
static double anim_param(Clip& c, const std::string& name, double T, double fb) {
    auto it = c.keyframes.find("params." + name);
    if (it != c.keyframes.end()) return eval_kf(it->second, T, 0, fb);
    if (c.params.is_object() && c.params.contains(name) && c.params[name].is_number()) return c.params[name].get<double>();
    return fb;
}

// ── transform SIZE helpers (inspector) ──────────────────────────────────────
// A media clip's on-screen size is native_px × transform.scale. The inspector lets you set the
// ACTUAL output size in px (default) OR the scale factor — both drive transform.scale. Crucially,
// when scale is KEYFRAMED (the CRT pop-in, a Fufu zoom) the edit rescales the whole keyframe track
// instead of a dead static field — that was the "resizing the CRT clip does nothing" bug, since
// anim_xform ignores the static transform.scale whenever a keyframe track exists.

// Native (pre-scale) px size of a clip's SOURCE: image texture · video frame · avatar sprite.
// False for native-compositing clips (code/caption/shape/gradient) — their scale is a pure
// multiplier with no intrinsic px, so the inspector shows only the scale factor for those.
// per-clip source crop: params.crop = [x, y, w, h] as fractions of the source (0..1) — zoom a beat
// into part of a screenshot/footage (the copy dialog in a full desktop grab) with no pre-cropped
// derived asset. Composes with layout (which fits the CROPPED region), transform, and the mirror.
static bool clip_crop(const Clip& c, float& x0, float& y0, float& w, float& h) {
    x0 = 0; y0 = 0; w = 1; h = 1;
    if (!c.params.is_object() || !c.params.contains("crop")) return false;
    const json& cr = c.params["crop"];
    if (!cr.is_array() || cr.size() != 4) return false;
    auto cl = [](double v, double lo, double hi){ return v < lo ? lo : v > hi ? hi : v; };
    x0 = (float)cl(cr[0].get<double>(), 0.0, 1.0);
    y0 = (float)cl(cr[1].get<double>(), 0.0, 1.0);
    w  = (float)cl(cr[2].get<double>(), 0.01, 1.0 - x0);
    h  = (float)cl(cr[3].get<double>(), 0.01, 1.0 - y0);
    return true;
}
static bool clip_native_size(Project& p, Clip& c, float& nw, float& nh) {
    float cx0, cy0, cw, ch; bool cropped = clip_crop(c, cx0, cy0, cw, ch);   // layout fits the visible crop
    if (c.type == "image") {
        auto au = p.asset_uri.find(c.asset);
        if (au != p.asset_uri.end()) { Tex* t = get_texture(resolve_asset(au->second)); if (t && t->w > 0) { nw = (float)t->w * cw; nh = (float)t->h * ch; return true; } }
    } else if (c.type == "video") {
        auto vm = p.asset_video.find(c.asset);
        if (vm != p.asset_video.end()) {
            VideoMeta& m = vm->second;          // dims may not be probed yet (no composite run) → open the source
#ifdef SLOP_LIBAV
            if (m.w <= 0 && g_videoDirect && !m.src.empty()) { VideoDecoder* d = get_decoder(resolve_asset(m.src)); if (d && d->w > 0) { m.w = d->w; m.h = d->h; if (m.fps <= 0) { m.fps = d->fps; m.frames = d->frames; } } }
#endif
            if (m.w <= 0 && !m.proxy.empty()) { FrameTex* ft = get_frame_tex(resolve_asset(m.proxy), 0); if (ft && ft->w > 0) { m.w = ft->w; m.h = ft->h; } }
            if (m.w > 0) { nw = (float)m.w * cw; nh = (float)m.h * ch; return true; }
        }
    } else if (c.type == "avatar") {
        auto rit = p.rows.find(c.row);
        std::string rig = (rit != p.rows.end()) ? jstr(rit->second.params, "rig") : std::string();
        if (rig.empty()) rig = "gemma-big";
        std::string emo = jstr(c.params, "emotion"); if (emo.empty() || emo == "auto") emo = "neutral";
        Tex* sp = avatar_sprite(get_rig(rig), emo, 0.0, false, nullptr);
        if (sp && sp->w > 0) { nw = (float)sp->w; nh = (float)sp->h; return true; }
    }
    return false;
}
// The render-time `layout` scale multiplier (mirror of the layout block in the composite loop) —
// the inspector px readout must fold it in, or two same-res sources read as equal px while the
// inset-laid one renders ~1.3x bigger on screen (user-reported "both say 800x600").
static float layout_scale_mul(Project& p, Clip& c) {
    if (c.type != "image" && c.type != "video") return 1.0f;
    std::string lay = jstr(c.params, "layout");
    if (lay.empty() || lay == "none") return 1.0f;
    float nw = 0, nh = 0;
    if (!clip_native_size(p, c, nw, nh) || nw <= 1 || nh <= 1) return 1.0f;
    float FW = (float)p.width, FH = (float)p.height;
    bool porFrame = FH > FW * 1.2f;
    if (lay == "cover") return std::max(FW / nw, FH / nh);   // always cover (backdrops)
    if (lay == "fullscreen") {
        float ar = (nw / nh) / (FW / FH);
        float visible = ar > 1 ? 1.f / ar : ar;
        return visible < 0.45f ? std::min(FW / nw, FH / nh) : std::max(FW / nw, FH / nh);
    }
    if (lay == "fit") return std::min(FW / nw, FH / nh);
    if (lay.rfind("inset", 0) == 0) {
        float L = porFrame ? std::min(0.86f * FW / nw, 0.42f * FH / nh)
                           : std::min(0.55f * FW / nw, 0.72f * FH / nh);
        return std::max(0.2f, std::min(2.0f, L));
    }
    return 1.0f;
}
// Representative scale for the size readout (comp 0=x 1=y): the effective (keyframed) scale at the
// playhead, but if that's ~0 at a pop-in/out frame, fall back to the track's peak so dividing
// through it to compute a px size never blows the element up.
static double clip_rep_scale(Clip& c, int comp, double T) {
    double stat = c.tx_scale[comp < 1 ? 0 : 1];
    auto it = c.keyframes.find("transform.scale");
    if (it == c.keyframes.end()) return stat;
    double e = eval_kf(it->second, T, comp, stat), peak = 0;
    for (auto& k : it->second) if ((int)k.v.size() > comp) peak = std::max(peak, std::fabs(k.v[comp]));
    return (std::fabs(e) > 0.02 * peak || peak < 1e-9) ? e : peak;
}
// Multiply a clip's scale by (rx,ry): every keyframe value when scale is animated (keeps the
// pop/zoom intact, just bigger/smaller), else the static transform.scale field.
static void clip_rescale(Clip& c, double rx, double ry) {
    auto it = c.keyframes.find("transform.scale");
    if (it != c.keyframes.end()) { for (auto& k : it->second) { if (k.v.size() >= 1) k.v[0] *= rx; if (k.v.size() >= 2) k.v[1] *= ry; } }
    else { c.tx_scale[0] *= rx; c.tx_scale[1] *= ry; }
}
// Mirror a clip on one axis (0=x horizontal, 1=y vertical): set the SIGN of its scale (negative =
// flipped), preserving magnitude. Flips both the static scale AND every keyframe value on that axis
// (an animated zoom stays a zoom, just mirrored) — so the flip checkbox works on animated clips too.
// The compositor draws a negative scale as a UV mirror (image/video) or a reversed quad (avatar).
static void clip_flip(Clip& c, int axis, bool on) {
    double sgn = on ? -1.0 : 1.0;
    c.tx_scale[axis] = sgn * std::fabs(c.tx_scale[axis]);
    auto it = c.keyframes.find("transform.scale");
    if (it != c.keyframes.end()) for (auto& k : it->second) if ((int)k.v.size() > axis) k.v[axis] = sgn * std::fabs(k.v[axis]);
}

// ───────────────────────── code cards (native syntax highlight) ─────────────
// A `code` clip renders a syntax-highlighted monospace card — decompilation / source
// excerpts as a LIVE, instantly-editable timeline layer (not a generated image): the
// video brief's first reusable primitive (docs/video/001-luckymaster.md). Everything is
// pure compositing: a C++ tokenizer colours keyword/type/string/number/comment/preproc/
// func, themed dark; a title bar, line numbers, per-line highlight, a typewriter reveal
// (`typewrite` 0..1) and vertical `scroll` — all instant + keyframe-animatable.
static ImFont* g_monoFont = nullptr;   // Consolas, loaded in main(); null → ImGui default

enum CodeLang { LANG_C, LANG_LUA, LANG_TOML, LANG_TEXT };
static CodeLang lang_of(const std::string& s) {
    if (s == "lua") return LANG_LUA;
    if (s == "toml" || s == "ini") return LANG_TOML;
    if (s == "text" || s == "txt") return LANG_TEXT;
    return LANG_C;  // c/cpp/h/decompilation share the C lexer (the default)
}
enum TokClass { TK_DEFAULT, TK_KEYWORD, TK_TYPE, TK_STRING, TK_NUMBER, TK_COMMENT, TK_PREPROC, TK_FUNC, TK_PUNCT };

struct CodeTheme { ImU32 bg, titlebar, border, gutter, lineno, dot[3]; ImU32 col[9]; ImU32 hlBar; ImU32 caret; };
static CodeTheme default_code_theme() {
    CodeTheme t;
    t.bg = IM_COL32(22, 24, 32, 255); t.titlebar = IM_COL32(34, 38, 50, 255);
    t.border = IM_COL32(74, 84, 112, 255); t.gutter = IM_COL32(28, 30, 40, 255);
    t.lineno = IM_COL32(98, 106, 132, 255);
    t.dot[0] = IM_COL32(237, 106, 94, 255); t.dot[1] = IM_COL32(244, 191, 79, 255); t.dot[2] = IM_COL32(91, 192, 99, 255);
    t.col[TK_DEFAULT] = IM_COL32(208, 214, 230, 255);
    t.col[TK_KEYWORD] = IM_COL32(198, 120, 221, 255);  // purple — control flow / storage
    t.col[TK_TYPE]    = IM_COL32( 86, 182, 194, 255);  // teal   — types (HWND, undefined4…)
    t.col[TK_STRING]  = IM_COL32(152, 195, 121, 255);  // green
    t.col[TK_NUMBER]  = IM_COL32(209, 154, 102, 255);  // orange — incl. hex (0x8800c6)
    t.col[TK_COMMENT] = IM_COL32(108, 118, 140, 255);  // grey
    t.col[TK_PREPROC] = IM_COL32(224, 108, 117, 255);  // red    — #include / #define
    t.col[TK_FUNC]    = IM_COL32( 97, 175, 239, 255);  // blue   — call targets
    t.col[TK_PUNCT]   = IM_COL32(171, 178, 191, 255);
    t.hlBar = IM_COL32(255, 214, 90, 38); t.caret = IM_COL32(255, 214, 90, 255);
    return t;
}

// The decompilation vocabulary of the LuckyMaster RE (Win32 / Ghidra) gets first-class colours.
static const char* C_KW[] = {
    "if","else","for","while","do","return","switch","case","break","continue","goto","default",
    "sizeof","struct","union","enum","typedef","static","const","volatile","extern","register","auto",
    "inline","class","public","private","protected","namespace","template","typename","this","operator",
    "using","virtual","override","new","delete","true","false","nullptr","NULL","TRUE","FALSE", nullptr };
static const char* C_TY[] = {
    "void","int","char","short","long","float","double","bool","unsigned","signed","wchar_t","size_t",
    "uint","undefined","undefined1","undefined2","undefined4","undefined8","byte","word","dword",
    "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
    "HWND","HDC","HHOOK","HINSTANCE","HANDLE","HBITMAP","HBRUSH","HMENU","HOOKPROC","HFONT","HGDIOBJ",
    "DWORD","WORD","BYTE","UINT","INT","BOOL","CHAR","WCHAR","LPSTR","LPCSTR","LPWSTR","LPVOID","LPARAM",
    "WPARAM","LRESULT","ATOM","RECT","POINT","COLORREF","SECURITY_ATTRIBUTES","LPSECURITY_ATTRIBUTES","MSG", nullptr };
static const char* LUA_KW[] = {
    "function","end","local","if","then","else","elseif","return","nil","true","false","and","or","not",
    "for","in","do","while","repeat","until","break","goto", nullptr };
static bool in_set(const std::string& w, const char** set) {
    for (int i = 0; set[i]; ++i) if (w == set[i]) return true;
    return false;
}

struct CTok { int line, col, len; TokClass cls; };
// Stateful lexer over the whole text (block comments span lines). Tokens are grouped by line;
// whitespace is left as gaps (monospace ⇒ column N sits at x = textX + N*cellw).
static void tokenize_code(const std::vector<std::string>& lines, CodeLang lang, std::vector<CTok>& out) {
    bool inBlock = false;
    for (int li = 0; li < (int)lines.size(); ++li) {
        const std::string& s = lines[li];
        int n = (int)s.size(), i = 0;
        auto push = [&](int col, int len, TokClass cls) { if (len > 0) out.push_back({li, col, len, cls}); };
        while (i < n) {
            char c = s[i];
            if (inBlock) {                                   // inside a /* … */ that opened earlier
                int start = i;
                while (i < n) { if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; inBlock = false; break; } if (++i < n) c = s[i]; }
                push(start, i - start, TK_COMMENT); continue;
            }
            if (c == ' ' || c == '\t') { i++; continue; }
            if (lang == LANG_C   && c == '/' && i + 1 < n && s[i + 1] == '/') { push(i, n - i, TK_COMMENT); break; }
            if (lang == LANG_LUA && c == '-' && i + 1 < n && s[i + 1] == '-') { push(i, n - i, TK_COMMENT); break; }
            if (lang == LANG_TOML && c == '#') { push(i, n - i, TK_COMMENT); break; }
            if (lang == LANG_C && c == '/' && i + 1 < n && s[i + 1] == '*') {  // block comment open
                int start = i; i += 2; inBlock = true;
                while (i < n) { if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; inBlock = false; break; } i++; }
                push(start, i - start, TK_COMMENT); continue;
            }
            if (c == '"' || ((lang == LANG_C || lang == LANG_LUA) && c == '\'')) {  // string / char literal
                char q = c; int start = i; i++;
                while (i < n) { if (s[i] == '\\') { i += 2; continue; } if (s[i] == q) { i++; break; } i++; }
                push(start, i - start, TK_STRING); continue;
            }
            if (lang == LANG_C && c == '#') {                // preprocessor (only if line-leading)
                bool first = true; for (int k = 0; k < i; k++) if (s[k] != ' ' && s[k] != '\t') { first = false; break; }
                if (first) { push(i, n - i, TK_PREPROC); break; }
            }
            if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {  // number (incl 0x…)
                int start = i;
                if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) { i += 2; while (i < n && isxdigit((unsigned char)s[i])) i++; }
                else { while (i < n && (isdigit((unsigned char)s[i]) || s[i] == '.')) i++; }
                while (i < n && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L' || s[i] == 'f' || s[i] == 'F')) i++;
                push(start, i - start, TK_NUMBER); continue;
            }
            if (isalpha((unsigned char)c) || c == '_') {     // identifier → keyword / type / func / default
                int start = i; while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
                std::string w = s.substr(start, i - start);
                TokClass cls = TK_DEFAULT;
                bool typeName = (w.size() > 2 && w.substr(w.size() - 2) == "_t");
                if (lang == LANG_C) {
                    if (in_set(w, C_KW)) cls = TK_KEYWORD;
                    else if (typeName || in_set(w, C_TY)) cls = TK_TYPE;
                } else if (lang == LANG_LUA) {
                    if (in_set(w, LUA_KW)) cls = TK_KEYWORD;
                } else if (lang == LANG_TOML) {
                    if (w == "true" || w == "false") cls = TK_KEYWORD;
                }
                if (cls == TK_DEFAULT) { int j = i; while (j < n && s[j] == ' ') j++; if (j < n && s[j] == '(') cls = TK_FUNC; }
                push(start, i - start, cls); continue;
            }
            if ((unsigned char)c >= 0x80) {                  // UTF-8 sequence → ONE token
                // (the per-byte fallthrough drew each byte alone → an invalid span → a '?'
                // per BYTE; a code card literally demonstrating Shift-JIS text needs this)
                unsigned char uc = (unsigned char)c;
                int len = uc >= 0xF0 ? 4 : uc >= 0xE0 ? 3 : uc >= 0xC0 ? 2 : 1;
                if (i + len > n) len = n - i;
                push(i, len, TK_DEFAULT); i += len; continue;
            }
            push(i, 1, TK_PUNCT); i++;                       // single-char punctuation
        }
    }
}

static inline ImU32 mul_alpha(ImU32 c, float f) {
    int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * f + 0.5f);
    a = a < 0 ? 0 : (a > 255 ? 255 : a);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}
// A large proportional CJK font for captions/text overlays (the UI font is 17px — too small
// to upscale for big on-screen text). Loaded in main() at 48px with JP ranges → crisp Latin
// AND Japanese (the mini-lessons). null → ImGui default font.
static ImFont* g_captionFont = nullptr;
// Param colour: [r,g,b] / [r,g,b,a] (0..255) → ImU32, else the default.
static ImU32 parse_color(const json& j, ImU32 def) {
    if (j.is_array() && j.size() >= 3)
        return IM_COL32(j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j.size() >= 4 ? j[3].get<int>() : 255);
    return def;
}

// Draw a code clip centred at (cx,cy). Auto-sizes to its content; scale comes from the clip
// transform (font_px · scale · preview-scale). `t` is the playhead (caret blink, deterministic).
static void draw_code_clip(ImDrawList* dl, float cx, float cy, float s, Clip& c, int alpha, double t,
                           ImVec2 f0 = ImVec2(0, 0), float fw = 0, float fh = 0) {
    json& P = c.params;
    if (!P.is_object()) return;
    std::string src = jstr(P, "code"); if (src.empty()) src = jstr(P, "text");
    CodeLang lang = lang_of(jstr(P, "lang"));
    CodeTheme th = default_code_theme();
    float scaleX = (float)anim_xform(c, "transform.scale", 0, t, c.tx_scale[0]);
    float anchorX = (float)anim_xform(c, "transform.anchor", 0, t, c.tx_anchor[0]);
    float anchorY = (float)anim_xform(c, "transform.anchor", 1, t, c.tx_anchor[1]);
    ImFont* font = g_monoFont ? g_monoFont : ImGui::GetFont();
    float pad = (float)anim_param(c, "pad", t, 26.0) * scaleX * s;

    std::vector<std::string> lines; { std::string cur; for (char ch : src) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else if (ch != '\r') cur += ch; } lines.push_back(cur); }
    int totalLines = (int)lines.size();
    std::vector<CTok> toks; tokenize_code(lines, lang, toks);

    bool lineNos = P.value("line_numbers", false);
    int firstLineNo = P.value("first_line", 1);             // absolute number to label line 0 (decompile refs)
    int gutterDigits = (int)std::to_string(firstLineNo + totalLines).size();
    int gutterChars = lineNos ? gutterDigits + 2 : 0;
    int maxcolsFit = 0; for (auto& L : lines) maxcolsFit = std::max(maxcolsFit, (int)L.size());
    // AUTO-FIT (default when no explicit font_px): the biggest font whose longest line spans the
    // frame width minus a small margin — the code card owns the width instead of hugging a corner.
    float screenFont;
    bool autoFit = fw > 1 && !P.contains("font_px") && !c.keyframes.count("params.font_px");
    if (autoFit) {
        float kM = font->CalcTextSizeA(100.0f, 1e30f, 0.0f, "M").x / 100.0f;   // mono advance / font px
        if (kM < 0.1f) kM = 0.6f;
        float availW = fw * 0.90f - 2.0f * pad;
        screenFont = availW / (std::max(8, maxcolsFit + gutterChars) * kM);
        screenFont = std::max(10.0f * s, std::min(42.0f * s, screenFont)) * scaleX;
    } else {
        screenFont = (float)anim_param(c, "font_px", t, 30.0) * scaleX * s;
    }
    if (screenFont < 4.0f) screenFont = 4.0f;
    float cellw = font->CalcTextSizeA(screenFont, 1e30f, 0.0f, "M").x;
    if (cellw < 1.0f) cellw = screenFont * 0.6f;
    float lineH = screenFont * 1.34f;
    float gutterW = gutterChars * cellw;
    std::string title = jstr(P, "title");
    bool hasTitle = !title.empty();
    float titleH = hasTitle ? screenFont * 1.7f : 0.0f;

    double scroll = anim_param(c, "scroll", t, 0.0);        // first visible line (fractional)
    int maxLines = P.value("max_lines", 0);
    int viewLines = (maxLines > 0 && maxLines < totalLines) ? maxLines : totalLines;
    int maxcols = 0; for (auto& L : lines) maxcols = std::max(maxcols, (int)L.size());

    float W = pad + gutterW + maxcols * cellw + pad;
    float H = titleH + pad + viewLines * lineH + pad;
    float x0 = cx - W * anchorX;
    float y0 = cy - H * anchorY;
    // dock:"top" (the skeleton's code default): center the card horizontally and pin it near the
    // top edge — the host sits small in the lower-right corner below it. transform.pos stays a
    // NUDGE from the docked spot (cy carries pos.y; default pos.y=0 ⇒ cy = frame center ⇒ offset 0)
    // — a docked card used to ignore vertical moves entirely.
    if (fw > 1 && jstr(P, "dock") == "top") { x0 = cx - W * 0.5f; y0 = f0.y + fh * 0.055f + (cy - (f0.y + fh * 0.5f)); }
    float aF = alpha / 255.0f;
    float rad = 10.0f * scaleX * s;

    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + W, y0 + H), mul_alpha(th.bg, aF), rad);
    if (lineNos) dl->AddRectFilled(ImVec2(x0, y0 + titleH), ImVec2(x0 + pad + gutterW, y0 + H), mul_alpha(th.gutter, aF), rad, ImDrawFlags_RoundCornersLeft);
    if (hasTitle) {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + W, y0 + titleH), mul_alpha(th.titlebar, aF), rad, ImDrawFlags_RoundCornersTop);
        float dotR = screenFont * 0.18f, dy = y0 + titleH * 0.5f, dx = x0 + pad * 0.55f;
        for (int d = 0; d < 3; ++d) dl->AddCircleFilled(ImVec2(dx + d * dotR * 2.6f, dy), dotR, mul_alpha(th.dot[d], aF));
        dl->AddText(font, screenFont * 0.84f, ImVec2(dx + 3 * dotR * 2.6f + dotR, y0 + titleH * 0.5f - screenFont * 0.42f),
                    mul_alpha(th.col[TK_DEFAULT], aF * 0.92f), title.c_str());
    }
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + W, y0 + H), mul_alpha(th.border, aF), rad, 0, std::max(1.0f, 1.5f * scaleX * s));

    // typewriter budget: characters revealed so far across the whole text (incl. newlines).
    std::vector<int> charsBefore(totalLines, 0);
    int totalChars = 0;
    for (int li = 0; li < totalLines; ++li) { charsBefore[li] = totalChars; totalChars += (int)lines[li].size() + 1; }
    double tw = anim_param(c, "typewrite", t, 1.0);
    // DEFAULT typewriter: a code clip with no typewrite keyframes and no explicit static value
    // animates its reveal over the clip head (the signature look — no authoring needed).
    bool twAuto = !c.keyframes.count("params.typewrite")
                  && !(c.params.is_object() && c.params.contains("typewrite"));
    double twAutoDur = std::min(2.2, c.dur * 0.45);
    if (twAuto) tw = twAutoDur > 1e-3 ? std::max(0.0, std::min(1.0, (t - c.start) / twAutoDur)) : 1.0;
    long budget = (tw >= 1.0) ? (1L << 30) : (long)(tw * totalChars + 0.5);
    // line-highlight set (1-based, relative to the clip's own lines)
    std::vector<int> hl; if (P.contains("highlight") && P["highlight"].is_array()) for (auto& v : P["highlight"]) if (v.is_number()) hl.push_back((int)v.get<double>());
    bool hasHL = !hl.empty();
    // highlight reveal: hold off until the typewriter finishes, then sweep the bar in + dim the rest (smooth).
    double twDone = twAuto ? c.start + twAutoDur : c.start;
    auto twkf = c.keyframes.find("params.typewrite");
    if (twkf != c.keyframes.end() && !twkf->second.empty()) {
        twDone = twkf->second.back().t;                       // default: last typewrite keyframe
        for (auto& k : twkf->second) if (!k.v.empty() && k.v[0] >= 0.999) { twDone = k.t; break; }
    }
    float hlR = 1.0f;
    if (hasHL) { float e = (float)std::max(0.0, std::min(1.0, (t - (twDone + 0.12)) / 0.45));
                 hlR = e * e * (3.0f - 2.0f * e); }          // smoothstep ease

    float textX = x0 + pad + gutterW, contentTop = y0 + titleH + pad;
    dl->PushClipRect(ImVec2(x0, y0 + titleH), ImVec2(x0 + W, y0 + H), true);
    float caretX = -1, caretY = 0;
    for (int li = 0; li < totalLines; ++li) {
        float y = contentTop + (float)((li - scroll) * lineH);
        if (y + lineH < y0 + titleH || y > y0 + H) continue;     // outside viewport
        bool isHL = false; for (int hn : hl) if (hn == li + 1) isHL = true;
        float dim = (hasHL && !isHL) ? (1.0f - 0.60f * hlR) : 1.0f;   // others dim AS the highlight reveals
        if (isHL && hlR > 0.001f) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + W * hlR, y + lineH), mul_alpha(th.hlBar, aF));  // bar sweeps in L→R
        if (lineNos) {
            char num[24]; snprintf(num, sizeof num, "%*d", gutterDigits, firstLineNo + li);
            dl->AddText(font, screenFont, ImVec2(x0 + pad * 0.5f, y + (lineH - screenFont) * 0.5f), mul_alpha(th.lineno, aF * dim), num);
        }
        for (auto& tk : toks) {
            if (tk.line != li) continue;
            long absStart = charsBefore[li] + tk.col;
            if (absStart >= budget) continue;                    // not yet typed
            int drawLen = tk.len;
            if (absStart + tk.len > budget) {
                drawLen = (int)(budget - absStart); caretX = textX + (tk.col + drawLen) * cellw; caretY = y;
                // never split a UTF-8 sequence mid-codepoint (draws a '?' for one frame)
                while (drawLen > 0 && ((unsigned char)lines[li][tk.col + drawLen] & 0xC0) == 0x80) drawLen--;
            }
            const char* b = lines[li].c_str() + tk.col;
            dl->AddText(font, screenFont, ImVec2(textX + tk.col * cellw, y + (lineH - screenFont) * 0.5f),
                        mul_alpha(th.col[tk.cls], aF * dim), b, b + drawLen);
        }
    }
    if (caretX > 0 && fmod(t, 0.8) < 0.5)                          // blinking caret at the reveal point
        dl->AddRectFilled(ImVec2(caretX, caretY + lineH * 0.12f), ImVec2(caretX + cellw * 0.12f + 1.5f, caretY + lineH * 0.88f), mul_alpha(th.caret, aF));
    dl->PopClipRect();
}

// Screen-space rects (x0,y0,x1,y1) of every TEXT overlay drawn this composite (captions/plates/
// transcript). Cleared at the top of composite_frame, appended by draw_caption_clip, read by
// draw_song_credit so the now-playing chip dodges the ACTUAL boxes on screen (a hand-moved plate
// included — no style/place guessing).
static std::vector<ImVec4> g_frameTextBoxes;

// ── caption-anchor clips (`anchor` rows) ─────────────────────────────────────
// A MOVE HANDLE for captions: every caption/text clip whose START falls inside an anchor
// clip's span renders offset by that anchor's transform.pos — one clip moves a whole time
// range of captions at once, and because regeneration (slop.py transcript) rewrites only
// the caption clips themselves, the offset SURVIVES a caption regen. Overlapping anchors
// sum; params.rows (array of row ids) narrows the targets (default: every caption/text
// clip in the span). The clip draws nothing — the timeline rect + inspector are its UI.
static ImVec2 caption_anchor_off(Project& p, const Clip& cap, double T) {
    ImVec2 off(0, 0);
    for (auto& kv : p.clips) {
        Clip& a = kv.second;
        if (a.type != "anchor") continue;
        if (cap.start < a.start - 1e-4 || cap.start >= a.start + a.dur - 1e-4) continue;
        if (a.params.is_object() && a.params.contains("rows") && a.params["rows"].is_array()) {
            bool m = false;
            for (auto& r : a.params["rows"]) if (r.is_string() && r.get<std::string>() == cap.row) { m = true; break; }
            if (!m) continue;
        }
        off.x += (float)anim_xform(a, "transform.pos", 0, T, a.tx_pos[0]);
        off.y += (float)anim_xform(a, "transform.pos", 1, T, a.tx_pos[1]);
    }
    return off;
}

// ───────────────────────── captions / text overlays ─────────────────────────
// A `caption` (or `text`) clip draws styled on-screen text: lower-thirds, term pop-ups, and
// the JP mini-lessons (docs/video/001-luckymaster.md). Up to three stacked lines — `text`
// (primary), `sub` (secondary), `gloss` (tertiary) — over an optional rounded panel with an
// optional accent stripe. Uses the large CJK caption font so Japanese renders. `style` picks
// sensible defaults (plain · lower_third · term · jp_lesson); explicit params override. Pure
// compositing → instant + keyframe-animatable (animate transform.opacity for a fade, etc.).
static void draw_caption_clip(ImDrawList* dl, float cx, float cy, float s, Clip& c, int alpha, double t,
                              ImVec2 f0 = ImVec2(0, 0), float fw = 0, float fh = 0, int autoCorner = -1) {
    json& P = c.params;
    if (!P.is_object()) return;
    std::string text = jstr(P, "text"), sub = jstr(P, "sub"), gloss = jstr(P, "gloss");
    std::string style = jstr(P, "style"); if (style.empty()) style = "plain";
    if (text.empty() && sub.empty() && gloss.empty()) return;
    bool lower = (style == "lower_third"), jp = (style == "jp_lesson"), term = (style == "term");

    float scaleX = (float)anim_xform(c, "transform.scale", 0, t, c.tx_scale[0]);
    float ancX = (float)anim_xform(c, "transform.anchor", 0, t, c.tx_anchor[0]);
    float ancY = (float)anim_xform(c, "transform.anchor", 1, t, c.tx_anchor[1]);
    float fpx = (float)anim_param(c, "font_px", t, lower ? 52.0 : jp ? 106.0 : 56.0);   // jp lessons ARE the shot — big by default (distilled from the user's 1.4x etymology tweak)
    float screenFont = fpx * scaleX * s; if (screenFont < 4.0f) screenFont = 4.0f;
    ImFont* font = g_captionFont ? g_captionFont : ImGui::GetFont();
    float aF = alpha / 255.0f;

    bool box = P.value("box", style != "plain");
    bool multi = !sub.empty() || !gloss.empty();
    float boxPad = (float)P.value("box_pad", term ? (multi ? 22.0 : 16.0) : 22.0) * scaleX * s;
    ImU32 textCol  = parse_color(P.value("color", json()), IM_COL32(245, 247, 252, 255));
    ImU32 accent   = parse_color(P.value("accent", json()), IM_COL32(220, 120, 200, 255));   // Gemma purple-pink
    ImU32 boxCol   = parse_color(P.value("box_color", json()), IM_COL32(16, 16, 22, 214));
    ImU32 subCol   = parse_color(P.value("sub_color", json()), jp ? accent : IM_COL32(202, 208, 222, 255));
    ImU32 glossCol = parse_color(P.value("gloss_color", json()), IM_COL32(150, 158, 176, 255));
    int align = 1;                                                  // 0 left · 1 center · 2 right
    { std::string a = jstr(P, "align"); if (a == "left") align = 0; else if (a == "right") align = 2; else if (a == "center") align = 1; else if (lower) align = 0; }

    float wrap = (float)P.value("wrap_px", 0.0) * scaleX * s;
    float f1 = screenFont, f2 = screenFont * 0.58f, f3 = screenFont * 0.46f, gap = screenFont * 0.16f;
    auto meas = [&](const std::string& str, float fs) -> ImVec2 { return str.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(fs, wrap > 0 ? wrap : 1e30f, wrap > 0 ? wrap : 0.0f, str.c_str()); };
    ImVec2 m1 = meas(text, f1), m2 = meas(sub, f2), m3 = meas(gloss, f3);
    float blockW = std::max(std::max(m1.x, m2.x), m3.x);
    float blockH = m1.y + (sub.empty() ? 0 : gap + m2.y) + (gloss.empty() ? 0 : gap + m3.y);
    float stripeW = (lower || jp) ? screenFont * 0.14f : 0.0f;
    float stripeGap = stripeW > 0 ? boxPad * 0.6f : 0.0f;
    float H = blockH + (box ? boxPad * 2 : 0);
    float radEarly = term ? H * 0.5f : screenFont * 0.14f;
    // a term plate is a PILL — its sides curve into the text block, so the widest line
    // (usually the sub) needs extra horizontal margin to clear the curve
    float boxPadX = boxPad + (term && box ? radEarly * 0.5f : 0.0f);
    float W = blockW + (box ? boxPadX * 2 : 0) + stripeW + stripeGap;
    float x0 = cx - W * ancX, y0 = cy - H * ancY;
    // place:"auto" (the skeleton's plate default): pin the measured box to the least-busy CORNER
    // decided by the composite loop (0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right) with a
    // frame-relative margin — plates never sit mid-frame over content or her face. Bottom corners
    // are the lower-third STRAP default (dodges a centered host).
    if (autoCorner >= 0 && fw > 1) {
        bool por = fh > fw * 1.2f;
        float mx = fw * 0.035f, my = fh * 0.06f;
        // portrait bottom strap sits well ABOVE the bottom edge — the YouTube-Shorts title/description +
        // action buttons live in the bottom ~16%, so a strap at the very bottom is hidden by them.
        float botMargin = por ? 0.16f : 0.045f;
        float cornerX = ((autoCorner & 1) == 0) ? f0.x + mx : f0.x + fw - W - mx;
        float cornerY = (autoCorner >= 2) ? f0.y + fh - H - fh * botMargin : f0.y + my;
        // `pos` is an OFFSET from the auto-placed corner, NOT an absolute that snaps the plate to center:
        // the caller bakes tx_pos into cx,cy, so (cx,cy − frame-center) IS the authored offset. Add it.
        // → default pos [0,0] = the clean corner; nudging tweaks FROM there (no jump). (user request)
        x0 = cornerX + (cx - (f0.x + fw * 0.5f));
        y0 = cornerY + (cy - (f0.y + fh * 0.5f));
    }
    float rad = radEarly;                                          // term = pill

    if (box) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + W, y0 + H), mul_alpha(boxCol, aF), rad);
    if (stripeW > 0) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + stripeW, y0 + H), mul_alpha(accent, aF), rad, ImDrawFlags_RoundCornersLeft);

    float tx0 = x0 + (box ? boxPadX : 0) + stripeW + stripeGap, ty = y0 + (box ? boxPad : 0);
    auto line = [&](const std::string& str, float fs, ImU32 col, ImVec2 m) {
        if (str.empty()) return;
        float lx = (align == 0) ? tx0 : (align == 2) ? tx0 + blockW - m.x : tx0 + (blockW - m.x) * 0.5f;
        dl->AddText(font, fs, ImVec2(lx, ty), mul_alpha(col, aF), str.c_str(), nullptr, wrap > 0 ? wrap : 0.0f);
        ty += m.y + gap;
    };
    line(text, f1, textCol, m1);
    line(sub, f2, subCol, m2);
    line(gloss, f3, glossCol, m3);
    if (alpha > 24) g_frameTextBoxes.push_back(ImVec4(x0, y0, x0 + W, y0 + H));  // for now-playing chip avoidance
}

// ───────────────────────── shapes / callouts ────────────────────────────────
// A `shape` clip draws a vector callout: box (frame a code line / UI element), ellipse,
// line, arrow, bracket. Stroke `color`/`thickness`, optional `fill`; box/ellipse sized by
// `w`/`h` (project px) at the transform pos; line/arrow run `from`→`to` (centre-relative px)
// with an animatable `grow` 0..1 so it can draw itself in. Pure compositing + keyframeable
// (pop a box in via transform.scale; sweep an arrow via params.grow).
static void draw_shape_clip(ImDrawList* dl, float cx, float cy, float s, Clip& c, int alpha, double t) {
    json& P = c.params;
    if (!P.is_object()) return;
    std::string shape = jstr(P, "shape"); if (shape.empty()) shape = "box";
    float scaleX = (float)anim_xform(c, "transform.scale", 0, t, c.tx_scale[0]);
    float scaleY = (float)anim_xform(c, "transform.scale", 1, t, c.tx_scale[1]);
    float ancX = (float)anim_xform(c, "transform.anchor", 0, t, c.tx_anchor[0]);
    float ancY = (float)anim_xform(c, "transform.anchor", 1, t, c.tx_anchor[1]);
    float aF = alpha / 255.0f;
    ImU32 col = mul_alpha(parse_color(P.value("color", json()), IM_COL32(255, 214, 90, 255)), aF);  // accent yellow
    ImU32 fill = parse_color(P.value("fill", json()), 0);
    bool hasFill = ((fill >> IM_COL32_A_SHIFT) & 0xFF) > 0;
    float th = (float)anim_param(c, "thickness", t, 6.0) * scaleX * s; if (th < 1.0f) th = 1.0f;

    if (shape == "box") {
        float w = (float)anim_param(c, "w", t, 400.0) * scaleX * s, h = (float)anim_param(c, "h", t, 120.0) * scaleY * s;
        float round = (float)P.value("round", 10.0) * scaleX * s;
        ImVec2 a(cx - w * ancX, cy - h * ancY), b(a.x + w, a.y + h);
        if (hasFill) dl->AddRectFilled(a, b, mul_alpha(fill, aF), round);
        dl->AddRect(a, b, col, round, 0, th);
    } else if (shape == "ellipse" || shape == "circle") {
        float w = (float)anim_param(c, "w", t, 300.0) * scaleX * s, h = (float)anim_param(c, "h", t, 220.0) * scaleY * s;
        ImVec2 cen(cx + w * (0.5f - ancX), cy + h * (0.5f - ancY)), r(w * 0.5f, h * 0.5f);
        if (hasFill) dl->AddEllipseFilled(cen, r, mul_alpha(fill, aF));
        dl->AddEllipse(cen, r, col, 0.0f, 0, th);
    } else if (shape == "line" || shape == "arrow") {
        double fr[2] = {0, 0}, to[2] = {220, 0};
        if (P.contains("from") && P["from"].is_array() && P["from"].size() == 2) { fr[0] = P["from"][0].get<double>(); fr[1] = P["from"][1].get<double>(); }
        if (P.contains("to") && P["to"].is_array() && P["to"].size() == 2) { to[0] = P["to"][0].get<double>(); to[1] = P["to"][1].get<double>(); }
        float grow = (float)anim_param(c, "grow", t, 1.0);
        ImVec2 A(cx + (float)fr[0] * s, cy + (float)fr[1] * s);
        ImVec2 B(cx + (float)(fr[0] + (to[0] - fr[0]) * grow) * s, cy + (float)(fr[1] + (to[1] - fr[1]) * grow) * s);
        if (shape == "arrow") {                                  // filled arrowhead at the moving end
            float ang = atan2f(B.y - A.y, B.x - A.x), hl = th * 2.8f + 10.0f * scaleX * s;
            float len = sqrtf((B.x - A.x) * (B.x - A.x) + (B.y - A.y) * (B.y - A.y));
            ImVec2 w1(B.x + cosf(ang + 2.70f) * hl, B.y + sinf(ang + 2.70f) * hl),
                   w2(B.x + cosf(ang - 2.70f) * hl, B.y + sinf(ang - 2.70f) * hl);
            // the shaft must STOP at the head's base — running it to B pokes its square
            // butt-cap out of the narrowing tip
            float back = hl * cosf(3.14159265f - 2.70f);
            if (len > back + 1.0f) dl->AddLine(A, ImVec2(B.x - cosf(ang) * back, B.y - sinf(ang) * back), col, th);
            dl->AddTriangleFilled(B, w1, w2, col);
        } else dl->AddLine(A, B, col, th);
    } else if (shape == "bracket") {                             // a "[" marking a region (opens right)
        float h = (float)anim_param(c, "h", t, 220.0) * scaleY * s, tick = (float)P.value("tick", 28.0) * scaleX * s;
        ImVec2 top(cx, cy - h * ancY), bot(cx, top.y + h);
        dl->AddLine(top, bot, col, th);
        dl->AddLine(top, ImVec2(top.x + tick, top.y), col, th);
        dl->AddLine(bot, ImVec2(bot.x + tick, bot.y), col, th);
    }
}

// ───────────────────────── diagram (reusable boxes + arrows) ─────────────────
// A `diagram` clip draws a labeled box-and-arrow figure — for explainer flows (the gcalsrv
// network path, Act 7) and relationship figures (the font -> window-size finding, Act 9). Two
// authoring modes, both pure compositing + keyframe-animatable:
//   • flow:  ["XP app","WinINet","Schannel @ localhost","Lua EVENTS/MAIL"]  -> an auto-laid-out
//            chain of rounded boxes with arrows between (dir: "h" default | "v"). A flow item may
//            be an object {label, sub} for a two-line box.
//   • nodes/edges: nodes=[{id,label,sub?,x,y,w?,h?,color?}] (x,y center-relative project px) +
//            edges=[{from,to,label?}] (from/to = node id or array index) for an arbitrary graph.
// `reveal` (animatable 0..1) stages it in item-by-item — flow reveals node,arrow,node,arrow…;
// explicit reveals all nodes then edges (the typewriter-style draw-in the user likes). `title`
// adds a header above the figure; `accent` colors borders+arrows; `font_px` sizes labels.
static void draw_diagram_clip(ImDrawList* dl, float cx, float cy, float s, Clip& c, int alpha, double t,
                              ImVec2 f0 = ImVec2(0, 0), float fw = 0, float fh = 0, float sclMul = 1.0f) {
    json& P = c.params;
    if (!P.is_object()) return;
    float scaleX = (float)anim_xform(c, "transform.scale", 0, t, c.tx_scale[0]) * sclMul;   // sclMul = the pop spring
    float ancX = (float)anim_xform(c, "transform.anchor", 0, t, c.tx_anchor[0]);
    float ancY = (float)anim_xform(c, "transform.anchor", 1, t, c.tx_anchor[1]);
    ImFont* font = g_captionFont ? g_captionFont : ImGui::GetFont();
    float aF = alpha / 255.0f;
    ImU32 accent  = parse_color(P.value("accent", json()),    IM_COL32(120, 205, 235, 255));  // techy cyan
    ImU32 boxFill = parse_color(P.value("fill", json()),      IM_COL32( 26,  30,  40, 235));
    ImU32 txtCol  = parse_color(P.value("color", json()),     IM_COL32(238, 244, 252, 255));
    ImU32 subCol  = parse_color(P.value("sub_color", json()), IM_COL32(150, 200, 220, 255));
    double reveal = anim_param(c, "reveal", t, 1.0);
    float fpx = 5, subF = 3, padX = 0, padY = 0, gap = 0, rad = 0, th = 1.5f;

    struct DNode { std::string label, sub; float x, y, w, h; ImU32 col; };
    std::vector<DNode> nodes;
    struct DEdge { int a, b; std::string label; };
    std::vector<DEdge> edges;
    auto meas = [&](const std::string& str, float fs) -> ImVec2 {
        return str.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(fs, 1e30f, 0.0f, str.c_str());
    };

    bool isFlow = false;
    // Build the layout, then AUTO-FIT: if the figure runs wider than the frame, rebuild with the
    // scale knocked down so a wide node graph never brushes the frame edge (or the host).
    for (int fitPass = 0; fitPass < 2; ++fitPass) {
    nodes.clear(); edges.clear(); isFlow = false;
    fpx = (float)anim_param(c, "font_px", t, 30.0) * scaleX * s; if (fpx < 5.0f) fpx = 5.0f;
    subF = fpx * 0.62f;
    padX = 18.0f * scaleX * s; padY = 12.0f * scaleX * s;
    gap  = (float)P.value("gap", 56.0) * scaleX * s;            // space between boxes (holds the arrow)
    rad  = 9.0f * scaleX * s;
    th   = std::max(1.5f, 2.4f * scaleX * s);
    if (P.contains("nodes") && P["nodes"].is_array()) {
        for (auto& jn : P["nodes"]) {
            DNode n; n.label = jn.value("label", std::string()); n.sub = jn.value("sub", std::string());
            ImVec2 m1 = meas(n.label, fpx), m2 = meas(n.sub, subF);
            float defW = std::max(m1.x, m2.x) + padX * 2;
            float defH = m1.y + (n.sub.empty() ? 0 : m2.y + padY * 0.5f) + padY * 2;
            n.w = jn.contains("w") ? (float)jn["w"].get<double>() * scaleX * s : defW;
            n.h = jn.contains("h") ? (float)jn["h"].get<double>() * scaleX * s : defH;
            float nx = jn.contains("x") ? (float)jn["x"].get<double>() : 0.0f;
            float ny = jn.contains("y") ? (float)jn["y"].get<double>() : 0.0f;
            n.x = cx + nx * scaleX * s - n.w * 0.5f; n.y = cy + ny * scaleX * s - n.h * 0.5f;  // authored coords scale WITH the figure
            n.col = parse_color(jn.value("color", json()), accent);
            nodes.push_back(n);
        }
        auto idxOf = [&](const json& v) -> int {
            if (v.is_number()) return (int)v.get<double>();
            if (v.is_string()) { std::string id = v.get<std::string>();
                for (size_t i = 0; i < P["nodes"].size(); ++i) if (P["nodes"][i].value("id", std::string()) == id) return (int)i; }
            return -1;
        };
        if (P.contains("edges") && P["edges"].is_array())
            for (auto& je : P["edges"]) {
                int a = idxOf(je.value("from", json())), b = idxOf(je.value("to", json()));
                if (a >= 0 && b >= 0 && a < (int)nodes.size() && b < (int)nodes.size())
                    edges.push_back({a, b, je.value("label", std::string())});
            }
    } else if (P.contains("flow") && P["flow"].is_array()) {
        isFlow = true;
        std::string dir = P.value("dir", std::string("h"));
        bool vert = (dir == "v" || dir == "vertical");
        float maxW = 0, maxH = 0;
        for (auto& jn : P["flow"]) {
            DNode n;
            if (jn.is_string()) n.label = jn.get<std::string>();
            else { n.label = jn.value("label", std::string()); n.sub = jn.value("sub", std::string()); }
            ImVec2 m1 = meas(n.label, fpx), m2 = meas(n.sub, subF);
            n.w = std::max(m1.x, m2.x) + padX * 2;
            n.h = m1.y + (n.sub.empty() ? 0 : m2.y + padY * 0.5f) + padY * 2;
            n.col = accent;
            maxW = std::max(maxW, n.w); maxH = std::max(maxH, n.h);
            nodes.push_back(n);
        }
        int N = (int)nodes.size();
        float totW, totH;
        if (vert) { for (auto& n : nodes) n.w = maxW; totW = maxW; totH = N * maxH + gap * std::max(0, N - 1); }
        else      { for (auto& n : nodes) n.h = maxH; totH = maxH; totW = gap * std::max(0, N - 1); for (auto& n : nodes) totW += n.w; }
        float x0 = cx - totW * ancX, y0 = cy - totH * ancY;
        float cur = vert ? y0 : x0;
        for (auto& n : nodes) {
            if (vert) { n.x = x0 + (totW - n.w) * 0.5f; n.y = cur; cur += n.h + gap; }
            else      { n.x = cur; n.y = y0 + (totH - n.h) * 0.5f; cur += n.w + gap; }
        }
        for (int i = 0; i + 1 < N; ++i) edges.push_back({i, i + 1, std::string()});
    }
    if (nodes.empty()) return;
    if (fitPass == 0 && fw > 1) {
        float minX = 1e9f, maxX = -1e9f;
        for (auto& n : nodes) { minX = std::min(minX, n.x); maxX = std::max(maxX, n.x + n.w); }
        float need = fw * 0.92f;
        if (maxX - minX > need) { scaleX *= need / (maxX - minX); continue; }
    }
    break;
    }   // fit loop

    // reveal staging: flow interleaves node,arrow,node…(slot 2i / 2i+1); explicit = nodes then edges.
    int nItems = isFlow ? std::max(1, 2 * (int)nodes.size() - 1) : (int)(nodes.size() + edges.size());
    double prog = (reveal >= 1.0 ? 1e9 : reveal * nItems);
    auto itemA = [&](int slot) -> float { double a = prog - slot; return (float)(a < 0 ? 0 : a > 1 ? 1 : a); };

    // dead:[id | index | label-substring] — endpoints that no longer exist (a dead server): muted
    // fill, red border, a red ✕; `traffic` packets into them die at the door (see the edge loop).
    std::set<int> deadIdx;
    if (P.contains("dead") && P["dead"].is_array())
        for (auto& dv : P["dead"]) {
            if (dv.is_number()) { int di = (int)dv.get<double>(); if (di >= 0 && di < (int)nodes.size()) deadIdx.insert(di); }
            else if (dv.is_string()) {
                std::string ds = dv.get<std::string>();
                bool hit = false;
                if (P.contains("nodes") && P["nodes"].is_array())               // explicit mode: id match first
                    for (size_t i = 0; i < P["nodes"].size() && !hit; ++i)
                        if (P["nodes"][i].value("id", std::string()) == ds) { deadIdx.insert((int)i); hit = true; }
                for (size_t i = 0; i < nodes.size() && !hit; ++i)
                    if (nodes[i].label.find(ds) != std::string::npos) { deadIdx.insert((int)i); hit = true; }
            }
        }

    // overall figure bbox (nodes + title space) — used by the card and the title
    std::string title = jstr(P, "title");
    float bbMinX = 1e9f, bbMinY = 1e9f, bbMaxX = -1e9f, bbMaxY = -1e9f;
    for (auto& n : nodes) {
        bbMinX = std::min(bbMinX, n.x); bbMaxX = std::max(bbMaxX, n.x + n.w);
        bbMinY = std::min(bbMinY, n.y); bbMaxY = std::max(bbMaxY, n.y + n.h);
    }
    float titleF = fpx * 1.04f;
    float bbTop = title.empty() ? bbMinY : bbMinY - titleF * 1.9f;

    // backdrop card (default ON): one soft dark panel behind the whole figure so the diagram
    // reads as a unit over any scene — the same treatment as the code card. card:false opts out.
    if (P.value("card", true)) {
        float cp = fpx * 0.85f;
        ImVec2 ca(bbMinX - cp, bbTop - cp), cb(bbMaxX + cp, bbMaxY + cp);
        dl->AddRectFilled(ca, cb, mul_alpha(IM_COL32(15, 17, 26, 228), aF), rad * 1.8f);
        dl->AddRect(ca, cb, mul_alpha(IM_COL32(88, 96, 120, 130), aF), rad * 1.8f, 0, std::max(1.0f, 1.2f * s));
    }

    // title centered above the union bounding box of all nodes
    if (!title.empty()) {
        ImVec2 mt = meas(title, titleF);
        dl->AddText(font, titleF, ImVec2((bbMinX + bbMaxX) * 0.5f - mt.x * 0.5f, bbMinY - titleF * 1.5f), mul_alpha(accent, aF), title.c_str());
    }

    // edges (drawn first, under the boxes): clip the segment to each box edge; arrowhead at 'to'.
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        int slot = isFlow ? (2 * edges[ei].a + 1) : ((int)nodes.size() + (int)ei);
        float ea = itemA(slot); if (ea <= 0.001f) continue;
        DNode& A = nodes[edges[ei].a]; DNode& B = nodes[edges[ei].b];
        ImVec2 ca(A.x + A.w * 0.5f, A.y + A.h * 0.5f), cb(B.x + B.w * 0.5f, B.y + B.h * 0.5f);
        ImVec2 d(cb.x - ca.x, cb.y - ca.y); float len = sqrtf(d.x * d.x + d.y * d.y); if (len < 1) continue;
        ImVec2 u(d.x / len, d.y / len);
        auto edgePt = [&](DNode& n, ImVec2 cc, int sgn) -> ImVec2 {
            float tx = u.x != 0 ? (n.w * 0.5f) / fabsf(u.x) : 1e9f, ty = u.y != 0 ? (n.h * 0.5f) / fabsf(u.y) : 1e9f;
            float tt = std::min(tx, ty);
            return ImVec2(cc.x + sgn * u.x * tt, cc.y + sgn * u.y * tt);
        };
        ImVec2 p0 = edgePt(A, ca, +1), p1 = edgePt(B, cb, -1);
        ImU32 ec = mul_alpha(accent, aF * ea);
        dl->AddLine(p0, p1, ec, th);
        float ang = atan2f(p1.y - p0.y, p1.x - p0.x), hl = th * 2.6f + 9.0f * scaleX * s;
        dl->AddTriangleFilled(p1, ImVec2(p1.x + cosf(ang + 2.618f) * hl, p1.y + sinf(ang + 2.618f) * hl),
                              ImVec2(p1.x + cosf(ang - 2.618f) * hl, p1.y + sinf(ang - 2.618f) * hl), ec);
        if (!edges[ei].label.empty()) {
            ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), ml = meas(edges[ei].label, subF);
            dl->AddRectFilled(ImVec2(mid.x - ml.x * 0.5f - 4, mid.y - ml.y * 0.5f - 2), ImVec2(mid.x + ml.x * 0.5f + 4, mid.y + ml.y * 0.5f + 2), mul_alpha(IM_COL32(16, 18, 26, 235), aF * ea), 4.0f);
            dl->AddText(font, subF, ImVec2(mid.x - ml.x * 0.5f, mid.y - ml.y * 0.5f), mul_alpha(subCol, aF * ea), edges[ei].label.c_str());
        }
        // traffic:true — little packets travel each edge on a loop; packets into a DEAD node never
        // arrive (they flash red at the door and vanish) — "the exe talks to a ghost", animated.
        if (P.value("traffic", false)) {
            bool toDead = deadIdx.count(edges[ei].b) > 0;
            float speed = (float)P.value("traffic_speed", 0.5);
            const int nd = 3;
            for (int k = 0; k < nd; ++k) {
                float ph = (float)fmod(t * speed + k / (double)nd, 1.0);
                float rr = th * 1.35f;
                if (toDead && ph > 0.80f) {                        // impact: an expanding, fading red ring
                    float u2 = (ph - 0.80f) / 0.20f;
                    ImVec2 ip(p0.x + (p1.x - p0.x) * 0.80f, p0.y + (p1.y - p0.y) * 0.80f);
                    dl->AddCircle(ip, rr * (1.0f + u2 * 2.2f), mul_alpha(IM_COL32(235, 90, 90, 220), aF * ea * (1.0f - u2)), 0, std::max(1.5f, th * 0.7f));
                } else {
                    ImVec2 dp(p0.x + (p1.x - p0.x) * ph, p0.y + (p1.y - p0.y) * ph);
                    float fadeIn = ph < 0.08f ? ph / 0.08f : 1.0f;
                    ImU32 dc2 = (toDead && ph > 0.55f) ? IM_COL32(235, 150, 105, 255) : IM_COL32(190, 230, 250, 255);
                    dl->AddCircleFilled(dp, rr, mul_alpha(dc2, aF * ea * fadeIn));
                }
            }
        }
    }
    // boxes
    for (size_t i = 0; i < nodes.size(); ++i) {
        int slot = isFlow ? (2 * (int)i) : (int)i;
        float na = itemA(slot); if (na <= 0.001f) continue;
        DNode& n = nodes[i];
        bool dead = deadIdx.count((int)i) > 0;
        ImVec2 a(n.x, n.y), b(n.x + n.w, n.y + n.h);
        dl->AddRectFilled(a, b, mul_alpha(dead ? IM_COL32(32, 22, 26, 235) : boxFill, aF * na), rad);
        dl->AddRect(a, b, mul_alpha(dead ? IM_COL32(225, 95, 95, 255) : n.col, aF * na), rad, 0, th);
        ImVec2 m1 = meas(n.label, fpx), m2 = meas(n.sub, subF);
        float blockH = m1.y + (n.sub.empty() ? 0 : m2.y + padY * 0.5f);
        float ty = n.y + (n.h - blockH) * 0.5f;
        dl->AddText(font, fpx, ImVec2(n.x + (n.w - m1.x) * 0.5f, ty), mul_alpha(txtCol, aF * na * (dead ? 0.6f : 1.0f)), n.label.c_str());
        if (!n.sub.empty()) dl->AddText(font, subF, ImVec2(n.x + (n.w - m2.x) * 0.5f, ty + m1.y + padY * 0.5f), mul_alpha(subCol, aF * na * (dead ? 0.6f : 1.0f)), n.sub.c_str());
        if (dead) {                                                 // the red ✕ — unmissable at video size
            float inset = std::min(n.w, n.h) * 0.18f;
            ImU32 xc = mul_alpha(IM_COL32(235, 82, 82, 225), aF * na);
            float xth = th * 1.9f;
            dl->AddLine(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), xc, xth);
            dl->AddLine(ImVec2(a.x + inset, b.y - inset), ImVec2(b.x - inset, a.y + inset), xc, xth);
        }
    }
}

// gradient/vignette overlay — a full-frame wash to tame a busy background or fade an edge.
// kind: "vignette" (darken toward the anchor, edges biased away from it) · "linear" (directional
// fade: dir = down/up/left/right). color + keyframeable `strength` (0..1) + `feather` (band size);
// `anchor` [0..1,0..1] biases the vignette's bright spot. Pure ImDrawList (no asset, instant).
static void draw_gradient_clip(ImDrawList* dl, ImVec2 f0, float fw, float fh, Clip& c, int baseAlpha, double t) {
    json& P = c.params;
    std::string kind = P.value("gradient", std::string("vignette"));
    ImU32 base = parse_color(P.value("color", json()), IM_COL32(0, 0, 0, 255));
    double strength = std::max(0.0, std::min(1.0, anim_param(c, "strength", t, 0.6)));
    int srcA = (base >> IM_COL32_A_SHIFT) & 0xFF;
    int maxA = (int)(srcA * strength * (baseAlpha / 255.0));
    if (maxA <= 0) return;
    ImU32 cFull = (base & 0x00FFFFFF) | ((ImU32)maxA << IM_COL32_A_SHIFT);
    ImU32 cZero =  base & 0x00FFFFFF;                                   // same hue, alpha 0
    ImVec2 q0 = f0, q1 = ImVec2(f0.x + fw, f0.y + fh);
    auto axis = [&](const char* k, double d){ if (P.contains("anchor") && P["anchor"].is_array() && P["anchor"].size() == 2) {
        return std::max(0.0, std::min(1.0, P["anchor"][k[0] == 'x' ? 0 : 1].get<double>())); } return d; };
    if (kind == "linear") {
        std::string dir = P.value("dir", std::string("down"));
        if (dir == "up")         dl->AddRectFilledMultiColor(q0, q1, cFull, cFull, cZero, cZero);
        else if (dir == "left")  dl->AddRectFilledMultiColor(q0, q1, cFull, cZero, cZero, cFull);
        else if (dir == "right") dl->AddRectFilledMultiColor(q0, q1, cZero, cFull, cFull, cZero);
        else                     dl->AddRectFilledMultiColor(q0, q1, cZero, cZero, cFull, cFull);  // down
        return;
    }
    // vignette / radial: darken each edge toward the anchor (deeper on the side away from it)
    double ax = axis("x", 0.5), ay = axis("y", 0.5);
    double feather = std::max(0.05, std::min(1.0, anim_param(c, "feather", t, 0.45)));
    float L = (float)(fw * feather * 2 * ax),       R = (float)(fw * feather * 2 * (1 - ax));
    float Tp = (float)(fh * feather * 2 * ay),       B = (float)(fh * feather * 2 * (1 - ay));
    if (Tp > 1) dl->AddRectFilledMultiColor(q0, ImVec2(q1.x, q0.y + Tp), cFull, cFull, cZero, cZero);
    if (B  > 1) dl->AddRectFilledMultiColor(ImVec2(q0.x, q1.y - B), q1, cZero, cZero, cFull, cFull);
    if (L  > 1) dl->AddRectFilledMultiColor(q0, ImVec2(q0.x + L, q1.y), cFull, cZero, cZero, cFull);
    if (R  > 1) dl->AddRectFilledMultiColor(ImVec2(q1.x - R, q0.y), q1, cZero, cFull, cFull, cZero);
}

// Composite the frame at the playhead into the preview rect [f0, f0+(fw,fh)].
// Visual clips (image/video/avatar) active at the playhead are drawn bottom-track-first,
// each at canvas-center + pos (project px), sized native*scale, anchored, opacity-tinted.
// This is pure GPU compositing — instant, the latency target.
// Mean RGB of an image (alpha-skipping, subsampled, cached) — for auto bg→host color matching.
struct ImgMean { float r = 0, g = 0, b = 0; bool ok = false; };
static std::map<std::string, ImgMean> g_meanCache;
static ImgMean image_mean(const std::string& uri) {
    auto it = g_meanCache.find(uri);
    if (it != g_meanCache.end()) return it->second;
    ImgMean m;
    const SrcPix* sp = get_src_pixels(uri);
    if (sp && sp->w > 0 && sp->h > 0) {
        long total = (long)sp->w * sp->h;
        long step = std::max(1L, total / 4096);              // ~4k samples
        double ar = 0, ag = 0, ab = 0; long n = 0;
        for (long i = 0; i < total; i += step) {
            const unsigned char* px = &sp->px[(size_t)i * 4];
            if (px[3] < 8) continue;                         // skip transparent
            ar += px[0]; ag += px[1]; ab += px[2]; n++;
        }
        if (n) { m.r = (float)(ar / n); m.g = (float)(ag / n); m.b = (float)(ab / n); m.ok = true; }
    }
    g_meanCache[uri] = m;
    return m;
}

// Does this image carry REAL transparency (a cut-out like the konata PNG)? Cached, subsampled.
// A transparent showcase composites straight onto the scene — a rectangular border/shadow around
// its invisible bounding box reads as a bug, so the default inset frame skips such sources.
static std::map<std::string, bool> g_alphaCache;
static bool image_has_alpha(const std::string& uri) {
    auto it = g_alphaCache.find(uri);
    if (it != g_alphaCache.end()) return it->second;
    bool has = false;
    const SrcPix* sp = get_src_pixels(uri);
    if (sp && sp->w > 0 && sp->h > 0) {
        long total = (long)sp->w * sp->h, step = std::max(1L, total / 8192), n = 0, seen = 0;
        for (long i = 0; i < total; i += step) { seen++; if (sp->px[(size_t)i * 4 + 3] < 240) n++; }
        has = seen > 0 && n * 50 > seen;      // >2% translucent samples = a real cutout, not edge noise
    }
    g_alphaCache[uri] = has;
    return has;
}

// ── default clip transitions (fade in/out + slide types) ───────────────────
// Every visual clip gets a smooth eased fade in + out BY DEFAULT (the user's "use the default for
// most clips" ask) — applied to the effective alpha/pos before the per-type draw, so it covers
// image/avatar/code/caption/shape alike. Skipped where it would fight content: (1) a clip with
// MANUAL transform.opacity keyframes (don't double a hand-authored fade); (2) an edge that's
// CONTIGUOUS with another clip on the same row (back-to-back → hard cut, not a dip to black; a real
// OVERLAP still cross-dissolves since both ends fade). Per-clip `transition` param overrides: false
// = none; {in,out} each "fade"|"slide_down"|"slide_up"|"slide_left"|"slide_right"|"rise"|"none" or
// {type,dur}. Manual transform keyframes remain the escape hatch for ad-hoc motion.
struct TransFx { float alphaMul = 1.0f; float dx = 0, dy = 0; float sclMul = 1.0f;      // dx/dy in project px
                 float vx = 0, vy = 0; };   // instantaneous velocity (project px/s) — drives motion-blur ghosts

// Stock idle motion (default Ken Burns) — a slow zoom + gentle drift over the clip's duration,
// applied to image/video clips BY DEFAULT (the user's "make the slow floating/zoom a stock animation,
// not hand-keyframed per clip"). Skipped if the clip has manual scale/pos keyframes (hand-tuned wins)
// or motion:false / "none". `motion`: zoom_in(default) | zoom_out | pan_left|right|up|down, or
// {type, amount}. Returns a scale multiplier + a pos drift (project px) to fold onto the transform.
struct MotionFx { float sclMul = 1.0f; float dx = 0, dy = 0; };
static MotionFx clip_motion(Clip& c, double T) {
    MotionFx m;
    const json& mo = (c.params.is_object() && c.params.contains("motion")) ? c.params["motion"] : json();
    bool isMedia = (c.type == "image" || c.type == "video");
    std::string type; double amount = 0.06;
    if (mo.is_null()) { if (!isMedia) return m; type = "zoom_in"; }    // default: media zoom_in, others none
    else if (mo.is_boolean()) { if (!mo.get<bool>()) return m; type = "zoom_in"; }
    else if (mo.is_string()) type = mo.get<std::string>();
    else if (mo.is_object()) { type = mo.value("type", std::string("zoom_in")); amount = mo.value("amount", 0.06); }
    if (type == "none") return m;
    if (c.keyframes.count("transform.scale") || c.keyframes.count("transform.pos")) return m;   // manual Ken Burns wins
    double prog = c.dur > 1e-3 ? (T - c.start) / c.dur : 0.0;
    float e = (float)(prog < 0 ? 0 : prog > 1 ? 1 : prog);
    if (type == "zoom_in")       { m.sclMul = 1.0f + (float)amount * e;        m.dy = -(float)amount * 110 * e; }  // zoom + gentle rise
    else if (type == "zoom_out") { m.sclMul = 1.0f + (float)amount * (1.0f - e); m.dy =  (float)amount * 110 * (1.0f - e); }
    else if (type == "pan_left")  m.dx = -(float)amount * 560 * e;
    else if (type == "pan_right") m.dx =  (float)amount * 560 * e;
    else if (type == "pan_up")    m.dy = -(float)amount * 380 * e;
    else if (type == "pan_down")  m.dy =  (float)amount * 380 * e;
    return m;
}

// Avatar pose-swap slide constants — shared by the transition math and the SFX scheduler.
static const double AV_SWAP_SW = 0.20, AV_SWAP_DIST = 640.0, AV_GLIDE = 0.35;

// fwd (defined below with the other span queries): the pose-swap signature includes the avatar's
// AUTO-PLACEMENT state so a placement flip at a contiguous seam fires the slide.
static void content_centroid_span(const Project& p, double t0, double t1,
                                  ImVec2 center, float s, float fw, bool& has, float& cx);
static bool span_has_content(const Project& p, double t0, double t1);
static bool span_has_fullscreen_content(const Project& p, double t0, double t1);
static std::string avatar_place_sig(const Project& p, const Clip& c);

// The DECISION half of clip_transition — which transition fires on each edge, edge contiguity,
// and the avatar pose-swap/glide detection — pulled out so the built-in SFX scheduler can ask
// "what plays when" without duplicating the rules (the sounds always match the compositor).
struct TransInfo {
    std::string inType = "fade", outType = "fade"; double inDur = 0.3, outDur = 0.3;
    bool disabled = false;                 // transition:false
    bool skipIn = false, skipOut = false;  // contiguous edge → hard cut
    const Clip* nbIn = nullptr; const Clip* nbOut = nullptr;   // the contiguous neighbours
    bool avatarRow = false;
    bool swapIn = false, swapOut = false;  // pose-swap slide fires on that edge
    bool glideIn = false;                  // same-sprite reposition glide
    bool popThrough = false;               // contiguous media swap still pops (fade suppressed)
};
static TransInfo clip_trans_info(Project& p, Clip& c) {
    TransInfo ti;
    const json& tr = (c.params.is_object() && c.params.contains("transition")) ? c.params["transition"] : json();
    if (tr.is_boolean() && !tr.get<bool>()) { ti.disabled = true; return ti; }   // transition:false
    auto spec = [&](const char* side, std::string& type, double& dur) {
        type = "fade"; dur = 0.3;
        if (tr.is_object() && tr.contains(side)) {
            const json& sp = tr[side];
            if (sp.is_string()) type = sp.get<std::string>();
            else if (sp.is_object()) { type = sp.value("type", std::string("fade")); dur = sp.value("dur", 0.3); }
            else if (sp.is_boolean() && !sp.get<bool>()) type = "none";
        }
    };
    spec("in", ti.inType, ti.inDur); spec("out", ti.outType, ti.outDur);
    // Non-fullscreen media showcases POP in by default (the user's pick): an inset/fit image or
    // video with no explicit `transition.in` enters with the spring pop instead of a plain fade.
    if (ti.inType == "fade" && !(tr.is_object() && tr.contains("in"))) {
        if (c.type == "image" || c.type == "video") {
            std::string lay = jstr(c.params, "layout");
            if (lay == "fit" || lay.rfind("inset", 0) == 0) ti.inType = "pop";
        } else if (c.type == "diagram") ti.inType = "pop";   // diagram cards enter like image showcases
    }
    double half = c.dur * 0.5;
    ti.inDur = std::min(ti.inDur, half); ti.outDur = std::min(ti.outDur, half);   // short clip → proportional
    // Contiguity: an edge that's BACK-TO-BACK with another clip on the SAME row gets a HARD CUT (no
    // fade/slide) rather than a dip-to-black + slide-out-and-in "bounce". This is the behaviour the
    // header comment always described but was never implemented — a host segmented per VO line kept
    // sliding out and back in on every line (and briefly emptied the frame, so the whole-frame bg
    // filler had a void to fill). A real OVERLAP (edge far from a neighbour) still cross-dissolves.
    const double CONTIG = 0.06;
    auto rit = p.rows.find(c.row);
    ti.avatarRow = (rit != p.rows.end() && rit->second.type == "avatar");
    // Avatar rows get a MUCH wider adjacency window: the host is segmented per VO line and hand
    // trims/retimes routinely leave a small gap or overlap at the seam — that must not silently
    // demote the pose-swap slide to a fade/teleport (the user's "resized it and the slide+motion
    // blur is gone"). A sub-⅓s seam is still "the same host, next beat".
    double ctol = ti.avatarRow ? 0.35 : CONTIG;
    if (rit != p.rows.end())
        for (auto& oid : rit->second.clips) {
            if (oid == c.id) continue;
            auto oit = p.clips.find(oid);
            if (oit == p.clips.end()) continue;
            double os = oit->second.start, oe = os + oit->second.dur;
            if (std::fabs(oe - c.start) < ctol) { ti.skipIn = true; ti.nbIn = &oit->second; }            // neighbour ends ~at our start
            if (std::fabs(os - (c.start + c.dur)) < ctol) { ti.skipOut = true; ti.nbOut = &oit->second; } // neighbour starts ~at our end
        }
    if (ti.avatarRow) {
        // The swap signature is the RESOLVED SPRITE (through the rig's alias/canon/fallback chain),
        // not the raw emotion text — adjacent beats tagged "smug" vs "amused" that land on the same
        // sprite must NOT slide out and back into an identical pose (reads as a pointless dip).
        std::string rigName = jstr(rit->second.params, "rig"); if (rigName.empty()) rigName = "gemma-big";
        const AvatarRig* arig = get_rig(rigName);
        auto sig = [&](const Clip& k) {
            std::string e = jstr(k.params, "emotion");
            if (e.empty() || e == "auto") e = "neutral";
            std::string sp = arig ? avatar_sprite_path(arig, e) : emotion_from_text(e);
            // + the auto-placement state: a placement flip (over-footage corner/shrink, presenter
            // side-step, …) at a seam is a visible re-staging even when the sprite is identical —
            // it must slide, not teleport.
            return sp + "|" + jstr(k.params, "framing") + "|" +
                   jstr(k.params, "pose") + "|" + jstr(k.params, "face") + "|" + avatar_place_sig(p, k);
        };
        std::string mySig = sig(c);
        ti.swapOut = ti.nbOut && sig(*ti.nbOut) != mySig;
        ti.swapIn  = ti.nbIn  && sig(*ti.nbIn)  != mySig;
        // explicit override: transition in/out = "swap" FORCES the pose-swap slide on that edge —
        // for a beat where sprite AND placement are identical (the auto detection rightly sees no
        // change) but the cut wants a deliberate re-entrance dip (e.g. a final-beat punchline).
        // RECIPROCAL across the seam: the dip needs both halves (out through the floor, then in
        // from it) — a one-sided force would leave the neighbour standing until a 1-frame vanish.
        auto forcesSwap = [](const Clip* nb, const char* side) {
            if (!nb || !nb->params.is_object() || !nb->params.contains("transition")) return false;
            const json& tr = nb->params["transition"];
            if (!tr.is_object() || !tr.contains(side)) return false;
            const json& sp = tr[side];
            return (sp.is_string() && sp.get<std::string>() == "swap") ||
                   (sp.is_object() && sp.value("type", std::string()) == "swap");
        };
        if (ti.inType  == "swap" || forcesSwap(ti.nbIn,  "out")) ti.swapIn  = true;
        if (ti.outType == "swap" || forcesSwap(ti.nbOut, "in"))  ti.swapOut = true;
        // SAME sprite, DIFFERENT spot → glide (see clip_transition)
        if (ti.nbIn && !ti.swapIn) {
            double gdx = (double)ti.nbIn->tx_pos[0] - c.tx_pos[0];
            double gdy = (double)ti.nbIn->tx_pos[1] - c.tx_pos[1];
            ti.glideIn = std::fabs(gdx) + std::fabs(gdy) > 2.0;
        }
    }
    // Media swaps POP THROUGH a contiguous edge: back-to-back image/video clips are a content
    // swap, and the spring pop is the read of "new thing arrived" — only the FADE is suppressed
    // (no dip to black; the neighbour hard-cuts out underneath the pop).
    ti.popThrough = ti.skipIn && ti.inType == "pop" &&
                    (c.type == "image" || c.type == "video" || c.type == "caption");   // caption: transcript chunks pop through each other
    return ti;
}

static TransFx clip_transition(Project& p, Clip& c, double T) {
    TransFx fx;
    TransInfo ti = clip_trans_info(p, c);
    if (ti.disabled) return fx;                                        // transition:false
    bool hasOpKf = c.keyframes.count("transform.opacity") > 0;
    auto ease = [](double x) { x = x < 0 ? 0 : (x > 1 ? 1 : x); return x * x * (3 - 2 * x); };
    auto slide = [](const std::string& ty, float d, TransFx& f, bool out) {
        float s = out ? -1.f : 1.f;   // OUT exits the way it came in (mirror)
        if (ty == "slide_down")      f.dy -= s * d * 120;
        else if (ty == "slide_up")   f.dy += s * d * 120;
        else if (ty == "slide_left") f.dx += s * d * 160;
        else if (ty == "slide_right")f.dx -= s * d * 160;
        else if (ty == "rise")       f.dy += s * d * 100;
    };
    // "pop": a SPRING bounce on the way IN (same feel as the chiyo's-key pop — POP_K/POP_C set the curve
    // AND the speed, via the real elapsed time), a clean smoothstep SHRINK on the way OUT. POP_MIN = the
    // scale at the edge. The in spring runs on its OWN timescale (settles to 1.0), DECOUPLED from the
    // fade's inDur so the bounce isn't clipped. Resize-robust (recomputed from the clip edge every frame)
    // — unlike baked scale keyframes, which strand mid-clip when the clip is resized (the wallpaper bug).
    const float  POP_MIN = 0.5f;                  // scale at the edge (0.5 = the classic pop)
    const double POP_K = 240.0, POP_C = 15.0;     // spring stiffness/damping — matches cf_v_h7 (chiyo's key)
    double tin = T - c.start, tout = (c.start + c.dur) - T;
    // HOST POSE SWAP: contiguous host clips that change sprite (emotion/framing/pose/face) get a
    // quick eased slide-out → slide-in through the bottom edge instead of the invisible hard cut,
    // so the pose change reads as a deliberate move. The velocity (vx/vy) is exported so the avatar
    // draw can trail motion-blur ghosts along it; blur peaks mid-slide (velocity is the ease's
    // derivative) and settles to zero — the "motion blur with easing" look.
    if (ti.avatarRow) {
        const double SW = AV_SWAP_SW, DIST = AV_SWAP_DIST;   // per-side duration (s); slide depth (project px)
        if (ti.swapOut && tout < SW) {                     // slide OUT — accelerates toward the edge
            double u = 1.0 - tout / SW;
            fx.dy += (float)(DIST * u * u);
            fx.vy += (float)(DIST * 2.0 * u / SW);
            if (u > 0.55) fx.alphaMul *= (float)(1.0 - (u - 0.55) / 0.45);     // fade only near the end
        }
        if (ti.swapIn && tin < SW) {                       // slide IN — decelerates into place
            double u = 1.0 - tin / SW;
            fx.dy += (float)(DIST * u * u);
            fx.vy -= (float)(DIST * 2.0 * u / SW);
            if (u > 0.55) fx.alphaMul *= (float)(1.0 - (u - 0.55) / 0.45);
        }
        // SAME sprite, DIFFERENT spot: the slide is suppressed (identical pose dipping out/in reads
        // wrong) but a hard position teleport reads worse — GLIDE from the neighbour's spot into
        // ours instead (decelerating, motion-blurred like the slide).
        if (ti.glideIn && tin < AV_GLIDE) {
            double gdx = (double)ti.nbIn->tx_pos[0] - c.tx_pos[0];
            double gdy = (double)ti.nbIn->tx_pos[1] - c.tx_pos[1];
            double u = 1.0 - tin / AV_GLIDE;
            fx.dx += (float)(gdx * u * u);                fx.dy += (float)(gdy * u * u);
            fx.vx -= (float)(gdx * 2.0 * u / AV_GLIDE);   fx.vy -= (float)(gdy * 2.0 * u / AV_GLIDE);
        }
    }
    if ((!ti.skipIn || ti.popThrough) && ti.inType != "none") {
        if (!ti.skipIn && ti.inDur > 1e-3 && tin < ti.inDur) {
            double e = ease(tin / ti.inDur);
            if (!hasOpKf) fx.alphaMul *= (float)e;                              // fade in
            if (ti.inType != "fade" && ti.inType != "pop") slide(ti.inType, 1.0f - (float)e, fx, false);
        }
        if (ti.inType == "pop")                                                // SPRING pop-in (own timescale)
            fx.sclMul *= (float)(POP_MIN + (1.0 - POP_MIN) * spring_response(tin, POP_K, POP_C));
    }
    if (!ti.skipOut && ti.outType != "none" && ti.outDur > 1e-3 && tout < ti.outDur) {
        double e = ease(tout / ti.outDur);
        if (!hasOpKf) fx.alphaMul *= (float)e;                                 // fade out
        if (ti.outType == "pop")       fx.sclMul *= POP_MIN + (1.0f - POP_MIN) * (float)e;  // clean shrink-out
        else if (ti.outType != "fade") slide(ti.outType, 1.0f - (float)e, fx, true);
    }
    return fx;
}

// ── built-in transition SFX ─────────────────────────────────────────────────
// Tiny synthesized one-shots (library/sfx/, regenerate: tools/gen-sfx.py) scheduled at the
// times the DEFAULT transitions fire: "pop" entrances (incl. pop-through) → pop.wav,
// slide_*/rise + the avatar pose-swap slide → whoosh.wav. Gated by the project-global toggle
// (meta.sfx — Project panel; portrait/shorts defaults ON, 1080p OFF). The same event list
// feeds the preview mixer (collect_audio) AND the export plan, so preview == export.
// (struct SfxEvent + the fwd decl live up by MixSrc — collect_audio consumes these.)
static std::vector<SfxEvent> collect_sfx_events(Project& p) {
    std::vector<SfxEvent> ev;
    // AUTHORED gag cues (params.sfx_cue = "awkward" | "boom" | any library/sfx name): explicit
    // content, so NOT gated by meta.sfx — full-length videos keep their punchline sounds even
    // with the built-in transition SFX off. Optional params.sfx_at (s offset into the clip) and
    // params.sfx_gain_db. The music DUCKS around these (collect_duck_windows).
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        if (!c.params.is_object()) continue;
        std::string cue = jstr(c.params, "sfx_cue");
        if (cue.empty()) continue;
        ev.push_back({c.start + c.params.value("sfx_at", 0.0), cue + ".wav",
                      c.params.value("sfx_gain_db", -3.0)});
    }
    if (!p.sfx) return ev;                             // ↓ the built-in TRANSITION sounds are gated
    std::set<std::string> vrows;                       // visual rows = rows under video-kind tracks
    for (auto& tk : p.tracks) if (tk.kind == "video") for (auto& r : tk.rows) vrows.insert(r);
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        if (!vrows.count(c.row)) continue;
        if (c.params.is_object() && c.params.contains("sfx") &&
            c.params["sfx"].is_boolean() && !c.params["sfx"].get<bool>()) continue;   // per-clip opt-out
        TransInfo ti = clip_trans_info(p, c);
        if (ti.disabled) continue;
        if (ti.inType == "pop" && (!ti.skipIn || ti.popThrough))
            ev.push_back({c.start, "pop.wav", -8.0});
        bool slIn  = ti.inType.rfind("slide", 0) == 0 || ti.inType == "rise";
        bool slOut = ti.outType.rfind("slide", 0) == 0 || ti.outType == "rise";
        if (slIn && !ti.skipIn)   ev.push_back({c.start, "whoosh.wav", -10.0});
        if (slOut && !ti.skipOut) ev.push_back({c.start + c.dur - ti.outDur, "whoosh.wav", -10.0});
        if (ti.swapIn)                                 // one whoosh per pose-swap seam (from the IN side)
            ev.push_back({std::max(0.0, c.start - AV_SWAP_SW), "whoosh.wav", -9.0});
    }
    std::sort(ev.begin(), ev.end(), [](const SfxEvent& a, const SfxEvent& b) { return a.t < b.t; });
    std::vector<SfxEvent> out;                         // a cascade landing several clips on one beat
    for (auto& e : ev) {                               // collapses to ONE sound (not a louder stack)
        if (!out.empty() && out.back().wav == e.wav && std::fabs(out.back().t - e.t) < 0.03) continue;
        out.push_back(e);
    }
    return out;
}

// Duck windows: the music fades out under each authored gag cue and comes back after it —
// "fade the music out + play the sound for the punchline" (the user's awkward-moment brief).
// One window per sfx_cue clip (params.sfx_duck: false opts out): [cue, cue + sound length].
static std::vector<DuckWin> collect_duck_windows(Project& p) {
    std::vector<DuckWin> wins;
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        if (!c.params.is_object()) continue;
        std::string cue = jstr(c.params, "sfx_cue");
        if (cue.empty() || !c.params.value("sfx_duck", true)) continue;
        double t = c.start + c.params.value("sfx_at", 0.0);
        Pcm* pc = get_pcm("library/sfx/" + cue + ".wav");
        double len = pc ? std::min(pc->dur, 2.5) : 1.8;
        wins.push_back({t, t + len, DUCK_FLOOR});           // gag/SFX punchline → deep dip
    }
    // A video clip carrying its OWN audio ducks the bed over its whole span (default on;
    // params.duck_music:false opts out) — mirror the mixer's "video audio is live" gating so we
    // duck exactly when it actually plays, with a gentler floor than an SFX punchline.
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        if (!c.params.is_object()) continue;
        if (c.params.value("mute_audio", false)) continue;
        if (!c.params.value("duck_music", true)) continue;
        if (std::fabs(c.params.value("speed", 1.0) - 1.0) > 1e-3) continue;         // retimed → its audio is muted
        if (c.params.contains("loop") && c.params["loop"].is_string()) continue;    // pingpong → muted
        auto rit = p.rows.find(c.row);
        if (rit == p.rows.end() || rit->second.type != "video") continue;
        if (c.params.value("video_volume", 0.12) <= 0.0) continue;
        auto vmi = p.asset_video.find(c.asset);
        std::string vsrc = (vmi != p.asset_video.end() && !vmi->second.src.empty()) ? vmi->second.src
                           : (p.asset_uri.count(c.asset) ? p.asset_uri[c.asset] : std::string());
        Pcm* vpc = vsrc.empty() ? nullptr : get_video_pcm(resolve_asset(vsrc));
        // Duck when the source has REAL audio (not a silent gameplay track) — a low playback volume is
        // fine (the cat meme rides at 2% because the source is loud, and it's meant to be heard + duck).
        if (!vpc || vpc->dur <= 0.0 || vpc->rms < VIDEO_AUDIO_SILENCE_RMS) continue;
        wins.push_back({c.start, c.start + c.dur, DUCK_VIDEO_FLOOR});
    }
    return wins;
}

// Optional OUTER GLOW behind a clip's rect [a,b] — for contrast between floating clips and the
// background. Concentric rounded rects from the edge outward, accumulating alpha (brightest at the
// edge, soft falloff). `glow` param: true (default cool-white), or {color:[r,g,b], size, strength}.
static void draw_clip_glow(ImDrawList* dl, ImVec2 a, ImVec2 b, const json& g, float s, float op = 1.0f) {
    if (g.is_null()) return;
    if (g.is_boolean() && !g.get<bool>()) return;
    if (op <= 0.003f) return;   // fade the glow WITH the clip (like shadow/frame). It used to draw at full
                                // strength regardless of the clip's animated opacity → when a baked fade
                                // stranded past the clip's old end (extend-then-stale), the faded-to-0 image
                                // left a translucent glow square (the user's 187s idolmaster bug).
    int r = 235, gg = 240, bl = 255; float size = 26.f * s, strength = 0.7f;
    if (g.is_object()) {
        if (g.contains("enabled") && g["enabled"].is_boolean() && !g["enabled"].get<bool>()) return;
        if (g.contains("color") && g["color"].is_array() && g["color"].size() == 3) {
            r = g["color"][0].get<int>(); gg = g["color"][1].get<int>(); bl = g["color"][2].get<int>();
        }
        size = (float)g.value("size", 26.0) * s; strength = (float)g.value("strength", 0.7);
    }
    strength *= op;
    const int layers = 8;
    for (int i = layers; i >= 1; --i) {                       // outer (faint) → inner (bright)
        float exp = size * i / (float)layers;
        int al = (int)(strength * 26.0f * (1.0f - (i - 1) / (float)layers));   // accumulates near the edge
        if (al <= 0) continue;
        dl->AddRectFilled(ImVec2(a.x - exp, a.y - exp), ImVec2(b.x + exp, b.y + exp),
                          IM_COL32(r, gg, bl, al > 255 ? 255 : al), exp);
    }
}

// Is the outer glow ACTUALLY on? A `glow` param left behind as {enabled:false} (the UI's off-
// toggle keeps the styling for a later re-enable) must NOT count as "has glow" — presence-only
// checks suppressed the default inset frame forever after one glow on/off round-trip (the
// user's 220s "border never came back" bug).
static bool glow_enabled(const json& params) {
    if (!params.is_object() || !params.contains("glow")) return false;
    const json& g = params["glow"];
    if (g.is_boolean()) return g.get<bool>();
    if (g.is_object()) return !(g.contains("enabled") && g["enabled"].is_boolean() && !g["enabled"].get<bool>());
    return false;
}

// Soft DROP SHADOW behind a floating inset (drawn BEFORE the image) — offset down/right so the inset
// lifts off the background like a card. Layered fills (no shader). Part of the `frame` treatment.
static void draw_inset_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float radius, float s, float op = 1.0f) {
    if (op <= 0.003f) return;                                      // fully faded → nothing
    float off = 7.0f * s;                                          // offset down + slightly right
    const int layers = 9;
    for (int i = layers; i >= 1; --i) {
        float exp = (off * 1.7f) * i / (float)layers;              // spread (soft penumbra)
        int al = (int)(11.0f * (1.0f - (i - 1) / (float)layers) * op);  // dark, builds toward the edge (× clip opacity)
        if (al <= 0) continue;
        ImVec2 sa(a.x - exp + off * 0.5f, a.y - exp + off), sb(b.x + exp + off * 0.5f, b.y + exp + off);
        dl->AddRectFilled(sa, sb, IM_COL32(0, 0, 0, al), radius + exp);
    }
}

// A clean "card" border around a floating inset (drawn AFTER the image): a darker outer seat (defines
// the edge against a light bg) + the crisp light border + a faint inner highlight (a glassy bevel) —
// a professional inset, not the old barebones single line. `frame`: true, or
// {color:[r,g,b,a], thickness, radius, shadow:bool}.
static void draw_clip_frame(ImDrawList* dl, ImVec2 a, ImVec2 b, const json& f, float s, float op = 1.0f) {
    if (f.is_null()) return;
    if (f.is_boolean() && !f.get<bool>()) return;
    if (op <= 0.003f) return;                       // fully faded → nothing (fade the frame WITH the clip)
    int r = 246, g = 249, bl = 255, al = 235;
    float thickness = 2.5f * s, radius = 9.0f * s;
    if (f.is_object()) {
        if (f.contains("enabled") && f["enabled"].is_boolean() && !f["enabled"].get<bool>()) return;
        if (f.contains("color") && f["color"].is_array() && f["color"].size() >= 3) {
            r = f["color"][0].get<int>(); g = f["color"][1].get<int>(); bl = f["color"][2].get<int>();
            if (f["color"].size() >= 4) al = f["color"][3].get<int>();
        }
        thickness = (float)f.value("thickness", 2.5) * s;
        radius = (float)f.value("radius", 9.0) * s;
    }
    auto oa = [op](int a){ return (int)(a * op); };  // scale every layer's alpha by the clip's opacity
    dl->AddRect(ImVec2(a.x - s, a.y - s), ImVec2(b.x + s, b.y + s), IM_COL32(0, 0, 0, oa(70)), radius + s, 0, 1.5f * s);
    dl->AddRect(a, b, IM_COL32(r, g, bl, oa(al)), radius, 0, thickness);              // crisp light border
    dl->AddRect(ImVec2(a.x + thickness, a.y + thickness), ImVec2(b.x - thickness, b.y - thickness),
                IM_COL32(255, 255, 255, oa(42)), std::max(0.0f, radius - thickness), 0, std::max(1.0f, s));  // inner highlight
}

// Resolve the frame treatment for a media clip: an explicit `frame` param, else default-ON for a
// non-fullscreen INSET (the user's "built-in professional border so it stands out from the bg").
// Returns null = no frame (e.g. a fullscreen plate, or `frame:false`).
static json resolve_frame(const json& params, bool insetClip) {
    if (params.is_object() && params.contains("frame")) {
        const json& f = params["frame"];
        if (f.is_boolean() && !f.get<bool>()) return json();
        if (f.is_object() && f.contains("enabled") && f["enabled"].is_boolean() && !f["enabled"].get<bool>()) return json();
        return f;
    }
    return insetClip ? json(true) : json();
}

// CONTENT side over a TIME SPAN (not a single instant): scale-weighted screen-x of OFF-CENTER
// image/video clips (screenshots, insets, the CRT — not the centered full-frame bg plate) whose
// span OVERLAPS [t0,t1). Used to decide a host clip's presenter alignment ONCE for its whole span:
// evaluating at the single playhead made the host SNAP to the opposite corner the instant a content
// clip appeared partway through her clip. `has`=false (cx=center.x) means "no content → center".
static void content_centroid_span(const Project& p, double t0, double t1,
                                  ImVec2 center, float s, float fw, bool& has, float& cx) {
    float w = 0, x = 0;
    for (auto tk = p.tracks.rbegin(); tk != p.tracks.rend(); ++tk) {
        if (tk->kind != "video") continue;
        for (auto& rid : tk->rows) {
            auto rit = p.rows.find(rid);
            if (rit == p.rows.end()) continue;
            const std::string& rt = rit->second.type;   // code/diagram cards count as content too (the host must dodge them)
            if (rt != "image" && rt != "video" && rt != "code" && rt != "diagram") continue;
            for (auto& cid : rit->second.clips) {
                auto cit = p.clips.find(cid);
                if (cit == p.clips.end()) continue;
                const Clip& c = cit->second;
                if (c.start + c.dur <= t0 || c.start >= t1) continue;   // span overlap, NOT "active at playhead"
                std::string lay = jstr(c.params, "layout");             // layout places at render time — mirror it here
                if (lay == "fullscreen" || lay == "fit") continue;      // full-frame → bg, not side content
                float px = (float)c.tx_pos[0] * s;                      // screen-x offset from center
                if (lay.rfind("inset", 0) == 0) px += ((lay == "inset-left") ? -1.f : 1.f) * 0.1875f * fw;
                if (fabsf(px) < fw * 0.08f) continue;                  // ~centered → bg / full-frame, not "content"
                float wt = (float)fabs(c.tx_scale[0]);
                w += wt; x += (center.x + px) * wt;
            }
        }
    }
    has = w > 0.01f;
    cx = has ? x / w : center.x;
}

// Does ANY content clip (image/video/code/diagram — any layout except a "cover" backdrop) overlap
// the span? Coarser than content_centroid_span (which only counts SIDE content for facing): this
// answers "is she alone in the room?" — portrait solo shots size the host UP (avatar_fit solo).
static bool span_has_content(const Project& p, double t0, double t1) {
    for (auto& tk : p.tracks) {
        if (tk.kind != "video") continue;
        for (auto& rid : tk.rows) {
            auto rit = p.rows.find(rid);
            if (rit == p.rows.end()) continue;
            const std::string& rt = rit->second.type;
            if (rt != "image" && rt != "video" && rt != "code" && rt != "diagram") continue;
            for (auto& cid : rit->second.clips) {
                auto cit = p.clips.find(cid);
                if (cit == p.clips.end()) continue;
                const Clip& c = cit->second;
                if (c.start + c.dur <= t0 || c.start >= t1) continue;
                if (jstr(c.params, "layout") == "cover") continue;   // backdrop, not content
                if (c.params.value("overlay", false)) continue;      // a bottom reaction overlay (meme) — host stays solo-big
                return true;
            }
        }
    }
    return false;
}

// Is there a FULLSCREEN media clip (image/video layout=fullscreen) under the span? Fullscreen
// footage is the shot — the host must float over it small + cornered, not stand center-frame
// at presenter size (the "giant head over the hero footage" bug this default replaces; the
// luckymas3 cut fixed it by hand-nudging every fullscreen beat's host pos).
static bool span_has_fullscreen_content(const Project& p, double t0, double t1) {
    for (auto& tk : p.tracks) {
        if (tk.kind != "video") continue;
        for (auto& rid : tk.rows) {
            if (rid == "r_bg") continue;   // the room BACKDROP is not hero footage — a host over a
                                           // fullscreen roomday bg should stand IN it, not corner over it
            auto rit = p.rows.find(rid);
            if (rit == p.rows.end()) continue;
            const std::string& rt = rit->second.type;
            if (rt != "image" && rt != "video") continue;
            for (auto& cid : rit->second.clips) {
                auto cit = p.clips.find(cid);
                if (cit == p.clips.end()) continue;
                const Clip& c = cit->second;
                if (c.start + c.dur <= t0 || c.start >= t1) continue;
                if (jstr(c.params, "layout") == "fullscreen") return true;
            }
        }
    }
    return false;
}

// The avatar AUTO-PLACEMENT signature over a clip's span — a project-space mirror of the
// draw-time decisions (presenter side-step, over-footage shrink+corner, jp_lesson step-aside,
// portrait solo sizing; see the avatar branch of the media draw). Folded into the pose-swap
// signature: when a fullscreen clip starts/ends EXACTLY at a host seam the placement flips while
// the sprite stays identical — same-sprite detection saw "no change" and the host TELEPORTED
// corner↔center with no transition (recettear b116→b117 / b134→b135). Placement flip ⇒ slide.
static uint64_t g_placeSigEpoch = 1;   // bumped once per UI frame (see undo_checkpoint call site)
static std::string avatar_place_sig(const Project& p, const Clip& c) {
    // Memoized per clip per frame: the SFX scheduler re-runs clip_trans_info over EVERY visual
    // clip each frame while audio plays, and the span scans here are O(clips) each — uncached
    // that's O(clips²) per frame on the interactive loop. An edit shows next frame (sub-frame
    // staleness only). Export/headless never mutate mid-run, so a static epoch is a pure win.
    static std::map<const Clip*, std::pair<uint64_t, std::string>> cache;
    auto cit = cache.find(&c);
    if (cit != cache.end() && cit->second.first == g_placeSigEpoch) return cit->second.second;
    ImVec2 anc = anchor_off(p, c);
    std::string emo = jstr(c.params, "emotion"); if (emo.empty() || emo == "auto") emo = "neutral";
    std::string framing = avatar_framing(c.params, emo);
    bool faceShot = (framing == "bust" || framing == "closeup" || framing == "face");
    bool defPos = c.tx_pos[0] == 0.0 && c.tx_pos[1] == 0.0 &&
                  !c.keyframes.count("transform.pos") && fabsf(anc.x) < 0.5f;
    bool landscape = p.width >= p.height;
    float fw = (float)p.width;
    int side = 0;                                  // mirrors the draw's facing/content-side pick
    std::string faceP = jstr(c.params, "face");
    if (faceP == "left") side = -1;
    else if (faceP == "right") side = 1;
    else {
        bool hasC; float ccx;
        content_centroid_span(p, c.start, c.start + c.dur, ImVec2(0, 0), 1.0f, fw, hasC, ccx);
        if (hasC) {
            float d = ccx - (anc.x + (float)c.tx_pos[0]);
            side = d > fw * 0.04f ? 1 : (d < -fw * 0.04f ? -1 : 0);
        }
    }
    bool overFootage = faceShot && defPos && landscape &&
                       span_has_fullscreen_content(p, c.start, c.start + c.dur);
    float offX = (side != 0 && defPos && landscape) ? -side * 0.24f : 0.0f;   // fraction of fw
    if (overFootage && offX == 0.0f) offX = -0.335f;
    if (offX == 0.0f && side == 0 && defPos && landscape)
        for (auto& kv : p.clips) {                 // the jp_lesson step-aside (host hard left)
            const Clip& oc = kv.second;
            if (oc.type != "caption" || jstr(oc.params, "style") != "jp_lesson") continue;
            if (oc.start + oc.dur <= c.start || oc.start >= c.start + c.dur) continue;
            offX = -0.355f; break;
        }
    // portrait only: solo flips the host between mid-band-big and bottom-band. Landscape solo
    // changes nothing placement-wise — including it there would fire pointless dips.
    bool soloSized = !landscape && !span_has_content(p, c.start, c.start + c.dur);
    char buf[48];
    snprintf(buf, sizeof(buf), "s%d|o%d|x%.3f|p%d", side, overFootage ? 1 : 0, offX, soloSized ? 1 : 0);
    if (cache.size() > 4096) cache.clear();            // reloads strand old Clip addresses — bound it
    cache[&c] = {g_placeSigEpoch, std::string(buf)};
    return buf;
}

// The current sprite of the top-most active AVATAR clip (resolved rig → emotion → front pose), or
// null. Lets the `filler` fill with a blurred copy of the HOST when no image plate is on screen —
// else the fill is pure black the instant she's alone (the user's "not pure black when it's only
// the host"). A representative front sprite is enough: it's cover-scaled + heavily blurred anyway.
static Tex* topmost_avatar_sprite(Project& p, UIState& st, const std::string& excludeId, std::string& outPath) {
    for (auto& tk : p.tracks) {
        if (tk.kind != "video") continue;
        for (auto& rid : tk.rows) {
            auto rit = p.rows.find(rid);
            if (rit == p.rows.end() || rit->second.type != "avatar") continue;
            std::string rig = jstr(rit->second.params, "rig"); if (rig.empty()) rig = "gemma-big";
            const AvatarRig* arig = get_rig(rig);
            if (!arig) continue;
            for (auto& cid : rit->second.clips) {
                if (cid == excludeId) continue;
                auto cit = p.clips.find(cid);
                if (cit == p.clips.end() || !clip_active(cit->second, st.playhead)) continue;
                std::string emo = emotion_from_text(jstr(cit->second.params, "emotion"));
                if (emo.empty() || emo == "auto") emo = "neutral";
                Tex* sp = avatar_variant(arig, emo, "front", &outPath);
                if (sp && sp->w > 0) return sp;
            }
        }
    }
    return nullptr;
}

// ───────────────────────── whole-frame blur transition ────────────────────
// A `blur` clip is NOT a plate — it's a full-frame post-process over the ENTIRE composite (host,
// captions, code, every layer), so one clip over a scene cut blurs A out, hides the hard cut at the
// peak, and sharpens back in on B. (The old version blurred a single image plate below it, so the
// host/text popped through un-blurred and the opaque plate could draw OVER foreground text — the bug
// the user hit at 261.78s.) The strength defaults to an ease-in→out BELL; an explicit `strength`
// param/keyframes override it. Preview: the main loop renders the composite to an RT, CPU-blurs it,
// and shows it in place of the live frame (g_pvBlurSrv). Export: the readback buffer is CPU-blurred.
// Both use blur_rgba → identical preview + export.
static bool                      g_pvBlurActive = false;   // this frame's preview shows the blurred RT (set by the main loop)
static ID3D11ShaderResourceView* g_pvBlurSrv    = nullptr; // the blurred whole-frame texture the preview samples

// ── whole-frame `filler` backdrop (see render_fill_backdrop) ──
// A `filler` clip fills the empty bg with a blurred copy of the on-screen content. It used to guess a
// SINGLE image plate (topmost active image, else the host sprite + a flat colour wash) — so it ignored
// that plate's animation (a banner sliding out showed NO motion) and collapsed to a flat grey/black
// wash the instant that one plate went inactive (the user's 111s "abruptly goes grey" bug). Instead the
// filler now samples g_fillSrv: the FULL foreground composite (every layer EXCEPT fillers), rendered
// fresh each frame at half res and run through the SAME heavy `blur_rgba` gaussian as the `blur` clip
// (see render_fill_backdrop) → a SMOOTH gradient that bleeds the content colours out to fill the frame,
// tracks every layer + its motion, and never flattens to a wash.
static bool                      g_fillReady = false;            // this frame's backdrop RT is valid (set by the main loop / export)
static bool                      g_compositeSkipFillers = false; // true only while rendering that RT → fillers draw nothing (breaks the feedback loop)
static ID3D11ShaderResourceView* g_fillSrv = nullptr;            // the backdrop texture a filler samples
static int                       g_fillW = 0, g_fillH = 0;

// Max blur strength [0..1] over all active `blur` clips at t, + the project-space gaussian sigma (px)
// via sigmaOut. 0 when none is active (the fast path — no post-process, preview draws live).
static float frame_blur_strength(Project& p, double t, float& sigmaOut) {
    float best = 0.f; sigmaOut = 0.f;
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        if (c.type != "blur" || !clip_active(c, t)) continue;
        double lt = c.dur > 1e-3 ? (t - c.start) / c.dur : 0.0; lt = lt < 0 ? 0 : lt > 1 ? 1 : lt;
        double tri = lt < 0.5 ? lt / 0.5 : (1.0 - lt) / 0.5;             // 0→1→0 over the clip
        double bell = tri * tri * (3.0 - 2.0 * tri);                     // smoothstep ease-in/out
        double strength = anim_param(c, "strength", t, bell);           // explicit param/kf overrides the bell
        strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
        if (strength <= best) continue;
        best = (float)strength;
        sigmaOut = (float)(anim_param(c, "blur", t, 30.0) * strength);  // full blur px scaled by the bell
    }
    return best;
}

// In-place heavy separable gaussian over a full RGBA frame (row-major w*h*4): downsample by a
// sigma-derived factor → blur at working res → bilinear upscale back — the same cheap-heavy-blur
// trick as get_processed_srv, applied to the WHOLE composited frame. sigma is in this buffer's px.
static void blur_rgba(std::vector<unsigned char>& buf, int w, int h, float sigma) {
    if (sigma < 0.4f || w < 2 || h < 2 || (int)buf.size() < w * h * 4) return;
    int ds = (int)lroundf(sigma / 2.5f); if (ds < 1) ds = 1; if (ds > 12) ds = 12;
    int lw = std::max(1, w / ds), lh = std::max(1, h / ds), N = lw * lh;
    std::vector<float> ch[4]; for (int c = 0; c < 4; c++) ch[c].assign(N, 0.f);
    for (int y = 0; y < lh; y++)                                          // box-average downsample
        for (int x = 0; x < lw; x++) {
            int sx0 = x * w / lw, sx1 = std::max(sx0 + 1, (x + 1) * w / lw);
            int sy0 = y * h / lh, sy1 = std::max(sy0 + 1, (y + 1) * h / lh);
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0; int cnt = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) { const unsigned char* px = &buf[((size_t)yy * w + xx) * 4]; a0 += px[0]; a1 += px[1]; a2 += px[2]; a3 += px[3]; cnt++; }
            int i = y * lw + x; float inv = cnt ? 1.f / cnt : 0.f;
            ch[0][i] = a0 * inv; ch[1][i] = a1 * inv; ch[2][i] = a2 * inv; ch[3][i] = a3 * inv;
        }
    float sw = sigma / ds; if (sw < 0.5f) sw = 0.5f;
    int rad = std::max(1, (int)ceilf(sw * 3.f));
    std::vector<float> k(rad + 1); float ksum = 0;
    for (int i = 0; i <= rad; i++) { k[i] = expf(-(float)(i * i) / (2.f * sw * sw)); ksum += (i == 0 ? k[i] : 2 * k[i]); }
    for (float& v : k) v /= ksum;
    std::vector<float> tmp(N);
    for (int c = 0; c < 4; c++)
        for (int pass = 0; pass < 2; pass++) {
            bool horiz = (pass == 0);
            for (int y = 0; y < lh; y++)
                for (int x = 0; x < lw; x++) {
                    float acc = ch[c][y * lw + x] * k[0];
                    for (int i = 1; i <= rad; i++) {
                        int xp, xn, yp, yn;
                        if (horiz) { xp = std::max(0, x - i); xn = std::min(lw - 1, x + i); yp = yn = y; }
                        else       { yp = std::max(0, y - i); yn = std::min(lh - 1, y + i); xp = xn = x; }
                        acc += (ch[c][yp * lw + xp] + ch[c][yn * lw + xn]) * k[i];
                    }
                    tmp[y * lw + x] = acc;
                }
            ch[c].swap(tmp);
        }
    for (int y = 0; y < h; y++)                                           // bilinear upscale back into buf
        for (int x = 0; x < w; x++) {
            float fx = (x + 0.5f) * lw / w - 0.5f, fy = (y + 0.5f) * lh / h - 0.5f;
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float tx = fx - x0, ty = fy - y0;
            int x1 = std::min(lw - 1, std::max(0, x0 + 1)), y1 = std::min(lh - 1, std::max(0, y0 + 1));
            x0 = std::min(lw - 1, std::max(0, x0)); y0 = std::min(lh - 1, std::max(0, y0));
            unsigned char* px = &buf[((size_t)y * w + x) * 4];
            for (int c = 0; c < 4; c++) {
                float v00 = ch[c][y0 * lw + x0], v10 = ch[c][y0 * lw + x1], v01 = ch[c][y1 * lw + x0], v11 = ch[c][y1 * lw + x1];
                float v = v00 * (1 - tx) * (1 - ty) + v10 * tx * (1 - ty) + v01 * (1 - tx) * ty + v11 * tx * ty;
                px[c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
}

// ── "now playing" song-credit chip ──────────────────────────────────────────────────────────
// An automatic on-screen credit that fades in at each SONG's start and holds ~10s, showing
// "♪ Title — Artist" from the music asset's meta (the SAME title/artist the export description
// credits read — one source of truth, auto-filled from ID3 on import, editable in the Inspector).
// A song INSTANCE = a contiguous run of same-asset music clips: a LOOPED bed (many back-to-back
// clips of one file) triggers ONCE; the same song re-used later triggers again. Gated by
// meta.song_credits (Project panel); a start-clip's params.credit:false opts that instance out.
// Called from composite_frame so it burns into BOTH the live preview and the export.
static void draw_song_credit(Project& p, UIState& st, ImDrawList* dl, ImVec2 f0, float fw, float fh) {
    double t = st.playhead;
    struct MC { double s, e; std::string asset, clip; };
    std::vector<MC> mcs;
    for (auto& kv : p.clips) {
        const Clip& c = kv.second;
        auto ri = p.rows.find(c.row);
        if (ri == p.rows.end() || ri->second.type != "music" || c.asset.empty()) continue;
        mcs.push_back({c.start, c.start + c.dur, c.asset, c.id});
    }
    if (mcs.empty()) return;
    std::sort(mcs.begin(), mcs.end(), [](const MC& a, const MC& b) { return a.s < b.s; });
    // Fold the sorted clips into song INSTANCES and keep the one whose start is the latest <= t.
    const double GAP = 0.35;                       // a same-asset clip within 0.35s continues the instance
    double bestStart = -1e9, instEnd = 0; std::string bestAsset, bestClip;
    double curStart = -1, curEnd = -1; std::string curAsset, curClip;
    auto consider = [&]() {
        if (curStart >= 0 && curStart <= t && curStart > bestStart) { bestStart = curStart; instEnd = curEnd; bestAsset = curAsset; bestClip = curClip; }
    };
    for (auto& m : mcs) {
        bool newInst = (curStart < 0) || m.asset != curAsset || m.s > curEnd + GAP;
        if (newInst) { consider(); curStart = m.s; curEnd = m.e; curAsset = m.asset; curClip = m.clip; }
        else curEnd = std::max(curEnd, m.e);
    }
    consider();
    if (bestStart < -1e8) return;                  // nothing has started at/before the playhead
    { auto ci = p.clips.find(bestClip);            // per-instance opt-out on the START clip
      if (ci != p.clips.end() && ci->second.params.is_object() && ci->second.params.value("credit", true) == false) return; }
    double hold = std::min(p.songCreditSecs, std::max(0.0, instEnd - bestStart));  // a short sting shows only for its length
    double dt = t - bestStart;
    if (hold < 0.2 || dt < 0 || dt > hold) return;
    double fin = 0.4, fout = 0.7, a = 1.0;         // fade in / out inside [0, hold]
    if (dt < fin) a = dt / fin;
    if (dt > hold - fout) a = std::min(a, (hold - dt) / fout);
    if (a <= 0.02) return;
    // title/artist from the asset meta; fall back to the filename stem so an untagged song still
    // shows SOMETHING (a nudge to fill it in) rather than a blank chip.
    std::string title, artist;
    if (p.doc.contains("assets") && p.doc["assets"].contains(bestAsset) && p.doc["assets"][bestAsset].is_object()) {
        json m = p.doc["assets"][bestAsset].value("meta", json::object());
        title = m.value("title", std::string()); artist = m.value("artist", std::string());
    }
    if (title.empty()) {
        auto uu = p.asset_uri.find(bestAsset);
        if (uu != p.asset_uri.end()) {
            std::string fn = uu->second; size_t sl = fn.find_last_of("/\\"); if (sl != std::string::npos) fn = fn.substr(sl + 1);
            size_t dot = fn.find_last_of('.'); if (dot != std::string::npos) fn = fn.substr(0, dot);
            title = fn;
        }
    }
    if (title.empty()) return;
    // ── size the pill (♪ · title · artist) ──
    ImFont* font = g_captionFont ? g_captionFont : ImGui::GetFont();
    float aF = (float)a;
    bool por = fh > fw * 1.2f;
    float titleFs = fh * 0.030f; if (titleFs < 12) titleFs = 12;   // scales with the (preview or export) frame
    float artFs = titleFs * 0.74f, padX = titleFs * 0.62f, padY = titleFs * 0.42f, noteGap = titleFs * 0.5f, lineGap = titleFs * 0.14f;
    std::string note = "♪";                                        // ♪ (in the caption glyph set) — the accent
    ImVec2 nSz = font->CalcTextSizeA(titleFs * 1.15f, 1e30f, 0, note.c_str());
    ImVec2 tSz = font->CalcTextSizeA(titleFs, 1e30f, 0, title.c_str());
    ImVec2 aSz = artist.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(artFs, 1e30f, 0, artist.c_str());
    float textW = std::max(tSz.x, aSz.x);
    float blockH = tSz.y + (artist.empty() ? 0.f : lineGap + aSz.y);
    float W = padX + nSz.x + noteGap + textW + padX;
    float H = padY + std::max(blockH, nSz.y) + padY;
    float mx = fw * 0.028f, my = fh * (por ? 0.05f : 0.055f);
    // ── TARGET position: stay on the configured corner's SIDE, then shift VERTICALLY until the pill
    //    clears every on-screen text box (captions/plates/transcript — the ACTUAL drawn rects, so a
    //    hand-moved plate is respected, not guessed from style/place). If the whole column is blocked,
    //    shift UP above the topmost blocking box. (the owner's spec.) ──
    int cfg = (p.songCreditCorner == "tr") ? 1 : (p.songCreditCorner == "bl") ? 2 : (p.songCreditCorner == "br") ? 3 : 0;
    bool right = (cfg & 1), startBottom = (cfg >= 2);
    float tgx = right ? f0.x + fw - W - mx : f0.x + mx;
    float yTop = f0.y + my, yBot = f0.y + fh - H - my - fh * (por ? 0.10f : 0.0f);
    if (yBot < yTop) yBot = yTop;
    auto overlaps = [&](float yy) -> bool {
        for (auto& b : g_frameTextBoxes)
            if (tgx < b.z && tgx + W > b.x && yy < b.w && yy + H > b.y) return true;   // AABB (x fixed to the corner)
        return false;
    };
    float startY = startBottom ? yBot : yTop, step = std::max(6.0f, H * 0.35f), dir = startBottom ? -1.f : 1.f;
    float tgy = startY; bool placed = false;
    for (int i = 0; i < 200; i++) {
        float yy = startY + dir * step * i;
        if (yy < yTop || yy > yBot) break;                          // walked past the far edge, still blocked
        if (!overlaps(yy)) { tgy = yy; placed = true; break; }
    }
    if (!placed) {                                                  // column full → shift up above the topmost box
        float top = 1e9f;
        for (auto& b : g_frameTextBoxes) if (tgx < b.z && tgx + W > b.x) top = std::min(top, b.y);
        tgy = (top < 1e8f) ? std::max(yTop, top - H - my * 0.5f) : yTop;
    }
    // ── EASE toward the target: a critically-damped spring integrated over PLAYHEAD time (so it's
    //    identical in preview and DETERMINISTIC in export = fixed 1/fps steps), snapping on a new song
    //    / scrub. Store the position as a FRACTION of the frame so a preview-size vs export-size change
    //    doesn't jump it. Velocity drives motion-blur ghosts, exactly like the host pose-swap slide. ──
    static float cx = 0, cy = 0, vx = 0, vy = 0; static double prevT = -1; static std::string curInst;
    std::string inst = bestAsset + "@" + std::to_string((long long)(bestStart * 1000));
    float tfx = (tgx - f0.x) / fw, tfy = (tgy - f0.y) / fh;
    double ddt = t - prevT;
    if (inst != curInst || prevT < 0 || ddt < 0 || ddt > 0.4) { cx = tfx; cy = tfy; vx = vy = 0; }   // snap
    else {
        const float K = 260.f, C = 33.f;                            // ~0.3s critically-damped settle (host-like)
        for (double rem = ddt; rem > 1e-4;) {                       // sub-step for stability at large dt
            float h = (float)std::min(rem, 0.02); rem -= h;
            float ax = K * (tfx - cx) - C * vx, ay = K * (tfy - cy) - C * vy;
            vx += ax * h; vy += ay * h; cx += vx * h; cy += vy * h;
        }
    }
    prevT = t; curInst = inst;
    float dx = f0.x + cx * fw, dy = f0.y + cy * fh;                 // animated top-left (screen px)
    float velx = vx * fw, vely = vy * fh;                           // velocity (screen px/s) for the ghosts
    // ── paint the pill (rounded box · ♪ accent · title · artist) — no left stripe (it overshot the
    //    rounded corner); the ♪ note IS the accent. Callable at ghost offsets for the motion trail. ──
    float rad = std::min(H * 0.5f, H * 0.44f);
    auto paint = [&](float ox, float oy, float am) {
        float X = dx + ox, Y = dy + oy, m = aF * am;
        ImU32 boxCol   = mul_alpha(IM_COL32(16, 16, 22, 224), m);
        ImU32 accent   = mul_alpha(IM_COL32(226, 128, 206, 255), m);   // Gemma purple-pink
        ImU32 titleCol = mul_alpha(IM_COL32(245, 247, 252, 255), m);
        ImU32 artCol   = mul_alpha(IM_COL32(192, 198, 214, 255), m);
        dl->AddRectFilled(ImVec2(X, Y), ImVec2(X + W, Y + H), boxCol, rad);
        dl->AddText(font, titleFs * 1.15f, ImVec2(X + padX, Y + (H - nSz.y) * 0.5f), accent, note.c_str());
        float tx = X + padX + nSz.x + noteGap, ty = Y + (H - blockH) * 0.5f;
        dl->AddText(font, titleFs, ImVec2(tx, ty), titleCol, title.c_str());
        if (!artist.empty()) dl->AddText(font, artFs, ImVec2(tx, ty + tSz.y + lineGap), artCol, artist.c_str());
    };
    if (fabsf(velx) > 25.f || fabsf(vely) > 25.f) {                 // motion-blur trail (strong mid-move, none at rest)
        const int G = 5; const float GDT = 1.0f / 85.f;
        for (int gi = G; gi >= 1; --gi) paint(-velx * GDT * gi, -vely * GDT * gi, 0.34f * (1.0f - (gi - 1) / (float)G));
    }
    paint(0, 0, 1.0f);
}

static void composite_frame(Project& p, UIState& st, ImDrawList* dl, ImVec2 f0, float fw, float fh) {
    // Black base normally; a soft dark tint while rendering the filler-backdrop pre-pass, so a genuine
    // empty region (a beat with content only on one side) reads as a dim backdrop after the blur, not a
    // stark black hole — without reintroducing the flat bright grey the old filler fell back to.
    dl->AddRectFilled(f0, ImVec2(f0.x + fw, f0.y + fh), g_compositeSkipFillers ? IM_COL32(20, 20, 26, 255) : IM_COL32(0, 0, 0, 255));
    g_frameTextBoxes.clear();             // repopulated by draw_caption_clip; read by draw_song_credit at the end
    const float s = fw / (float)p.width;  // project px → preview px
    const ImVec2 center(f0.x + fw * 0.5f, f0.y + fh * 0.5f);
    dl->PushClipRect(f0, ImVec2(f0.x + fw, f0.y + fh), true);

    // AUTO bg→host match: sample the bottom-most active image clip (the bg plate) so avatar clips
    // grade toward it + get a contact shadow BY DEFAULT (no per-clip params; explicit params win).
    std::string bgUri;
    for (auto tk = p.tracks.rbegin(); tk != p.tracks.rend() && bgUri.empty(); ++tk) {
        if (tk->kind != "video") continue;
        for (auto& rid : tk->rows) {
            auto rit = p.rows.find(rid);
            if (rit == p.rows.end() || rit->second.type != "image") continue;
            for (auto& cid : rit->second.clips) {
                auto cit = p.clips.find(cid);
                if (cit != p.clips.end() && clip_active(cit->second, st.playhead) && !cit->second.asset.empty()) {
                    auto au = p.asset_uri.find(cit->second.asset);
                    if (au != p.asset_uri.end()) { bgUri = au->second; break; }
                }
            }
            if (!bgUri.empty()) break;
        }
    }
    double autoSat = 1.0, autoTemp = 0.0, autoShadow = 0.0;
    if (!bgUri.empty()) {
        ImgMean bm = image_mean(bgUri);
        if (bm.ok) {
            autoSat = 0.85;                                                            // gentle desat → sits in the plate
            autoTemp = std::max(-0.3, std::min(0.3, ((bm.r - bm.b) / 255.0) * 0.7));   // match warm/cool
            autoShadow = 0.45;                                                         // default contact shadow
        }
    }

    // CONTENT side (the host turns toward / sits aside the thing on screen) is decided PER HOST CLIP
    // over that clip's whole span (content_centroid_span, in the avatar branch below) — not here at the
    // single playhead, which made her snap corners the instant a content clip appeared mid-clip.

    for (auto tk = p.tracks.rbegin(); tk != p.tracks.rend(); ++tk) {  // bottom of stack first
        if (tk->kind != "video") continue;
        for (auto& rid : tk->rows) {
            auto rit = p.rows.find(rid);
            if (rit == p.rows.end()) continue;
            for (auto& cid : rit->second.clips) {
                auto cit = p.clips.find(cid);
                if (cit == p.clips.end()) continue;
                Clip& c = cit->second;
                if (!clip_active(c, st.playhead)) continue;

                // effective transform: keyframed (sampled at the playhead) → static fallback.
                // This is the motion-graphics layer — Ken Burns (pos+scale), fades (opacity).
                double T = st.playhead;
                ImVec2 anc = anchor_off(p, c);   // project anchor base; transform.pos is an offset from it
                float cx = center.x + (anc.x + (float)anim_xform(c, "transform.pos", 0, T, c.tx_pos[0])) * s;
                float cy = center.y + (anc.y + (float)anim_xform(c, "transform.pos", 1, T, c.tx_pos[1])) * s;
                float eSclX = (float)anim_xform(c, "transform.scale", 0, T, c.tx_scale[0]);
                float eSclY = (float)anim_xform(c, "transform.scale", 1, T, c.tx_scale[1]);
                float eAncX = (float)anim_xform(c, "transform.anchor", 0, T, c.tx_anchor[0]);
                float eAncY = (float)anim_xform(c, "transform.anchor", 1, T, c.tx_anchor[1]);
                int alpha = (int)(std::max(0.0, std::min(1.0, anim_xform(c, "transform.opacity", 0, T, c.tx_opacity))) * 255.0);
                // default fade in/out (+ optional slide) — applies to every clip type below.
                TransFx fx = clip_transition(p, c, T);
                alpha = (int)(alpha * fx.alphaMul);
                cx += fx.dx * s; cy += fx.dy * s;
                eSclX *= fx.sclMul; eSclY *= fx.sclMul;   // "pop" transition scales the clip in/out
                // stock idle motion (default Ken Burns zoom+drift on media; manual kf / motion:none opt out)
                MotionFx mo = clip_motion(c, T);
                eSclX *= mo.sclMul; eSclY *= mo.sclMul;
                cx += mo.dx * s; cy += mo.dy * s;

                // layout: RENDER-TIME adaptive placement for media — the transform-free primitive an
                // LLM (or a fast human) uses so any source size lands well-framed: "fullscreen" covers
                // the frame; "inset"/"inset-left"/"inset-right" fits ~half the frame beside the host
                // (the distilled content-inset default, computed live so an asset swap or project-res
                // change stays framed). The clip transform layers ON TOP (pos=nudge, scale=multiplier),
                // and Ken Burns / transitions compose as usual. Mirrors avatar `framing` philosophy.
                float layMul = 1.0f;   // layout's uniform scale — the frame/inset decision below
                                       // sizes from the BASE transform (no motion/transition anim)
                if (c.type == "image" || c.type == "video") {
                    std::string lay = jstr(c.params, "layout");
                    float nw = 0, nh = 0;
                    if (!lay.empty() && lay != "none" && clip_native_size(p, c, nw, nh) && nw > 1 && nh > 1) {
                        float FW = fw / s, FH = fh / s;                      // frame in project px
                        bool porFrame = FH > FW * 1.2f;                      // portrait/shorts canvas
                        if (lay == "cover") {
                            // ALWAYS cover-crop, no degrade — for backdrops (the room bg behind a
                            // portrait short: a 16:9 set in a 9:16 frame is a heavy crop ON PURPOSE).
                            float L = std::max(FW / nw, FH / nh);
                            eSclX *= L; eSclY *= L; layMul = L;
                        } else if (lay == "fullscreen") {
                            // Extreme aspect mismatch (a 5:1 site banner, a phone shot): cover would
                            // crop away most of the source — degrade to CONTAIN so the filler backdrop
                            // shows behind instead of a meaningless full-screen crop.
                            float ar = (nw / nh) / (FW / FH);
                            float visible = ar > 1 ? 1.f / ar : ar;          // fraction of the source cover keeps
                            float L = visible < 0.45f ? std::min(FW / nw, FH / nh)
                                                      : std::max(FW / nw, FH / nh);
                            eSclX *= L; eSclY *= L; layMul = L;
                        } else if (lay == "fit") {
                            float L = std::min(FW / nw, FH / nh);            // contain (letterbox on a filler)
                            eSclX *= L; eSclY *= L; layMul = L;
                            if (porFrame) cy -= 0.24f * fh;   // portrait: sit in the TOP band (host is big below), not dead-center
                        } else if (lay.rfind("inset", 0) == 0) {
                            // portrait: the content band sits UP TOP (wide, ~40% tall) with the host
                            // presenting BIG from below; landscape: ~half-frame beside the host.
                            float L = porFrame ? std::min(0.86f * FW / nw, 0.42f * FH / nh)
                                               : std::min(0.55f * FW / nw, 0.72f * FH / nh);
                            L = std::max(0.2f, std::min(2.0f, L));
                            eSclX *= L; eSclY *= L; layMul = L;
                            if (porFrame) cy -= 0.24f * fh;
                            else {
                                float side = (lay == "inset-left") ? -1.f : 1.f; // default: right-of-center (host takes the other corner)
                                cx += side * 0.1875f * fw;
                            }
                        }
                    }
                }

                // code: a live syntax-highlighted decompilation/source card (native compositing,
                // not a generated image) — see draw_code_clip. Self-sizes; transform places it.
                if (c.type == "code") { draw_code_clip(dl, cx, cy, s, c, alpha, st.playhead, f0, fw, fh); continue; }
                // caption/text: styled on-screen text (lower-thirds, term pop-ups, JP lessons).
                // Plates with place:"auto" pick the least busy top corner for their whole span:
                // above the host (opposite the side content), top-left over fullscreen footage,
                // top-right otherwise — never mid-frame.
                if (c.type == "caption" || c.type == "text") {
                    // caption-anchor clips covering this caption's start shift it (see caption_anchor_off) —
                    // composes with auto-corner placement below exactly like the clip's own pos offset does.
                    ImVec2 capA = caption_anchor_off(p, c, st.playhead);
                    cx += capA.x * s; cy += capA.y * s;
                    int autoCorner = -1;
                    std::string place = jstr(c.params, "place");
                    std::string capStyle = jstr(c.params, "style");
                    bool defaultPos = c.tx_pos[0] == 0.0 && c.tx_pos[1] == 0.0 && !c.keyframes.count("transform.pos");
                    // lower_third = the bottom STRAP by DEFAULT (pinned to the corner away from the host/
                    // content). This is now the DEFAULT position and `pos` is an OFFSET from it (see
                    // draw_caption_clip) — so nudging tweaks FROM the strap instead of snapping the plate to
                    // center. Explicit place: still wins the corner choice.
                    if (place.empty() && capStyle == "lower_third") place = "strap";
                    // jp_lesson with a default transform: centered but LOW (the host steps aside; see
                    // the avatar branch) — distilled from the user's etymology-plate tweak (0,+309).
                    if (defaultPos && capStyle == "jp_lesson") cy += 0.286f * fh;
                    // Explicit corner pins (author/agent override): tl/tr/bl/br.
                    if (place == "tl") autoCorner = 0;
                    else if (place == "tr") autoCorner = 1;
                    else if (place == "bl") autoCorner = 2;
                    else if (place == "br") autoCorner = 3;
                    else if (place == "strap") {
                        bool hasC; float ccx;
                        content_centroid_span(p, c.start, c.start + c.dur, center, s, fw, hasC, ccx);
                        float busyX = 0; bool busy = hasC;                    // content side, else host side
                        if (hasC) busyX = ccx - center.x;
                        else
                            for (auto& kv : p.clips) {
                                const Clip& oc = kv.second;
                                if (oc.type != "avatar") continue;
                                if (oc.start + oc.dur <= c.start || oc.start >= c.start + c.dur) continue;
                                busyX += (float)oc.tx_pos[0] * s; busy = true;
                            }
                        autoCorner = (busy && busyX > fw * 0.02f) ? 2 : 3;   // opposite bottom corner (default right)
                    } else if (place == "auto") {
                        bool hasC; float ccx;
                        content_centroid_span(p, c.start, c.start + c.dur, center, s, fw, hasC, ccx);
                        bool fullC = false;
                        for (auto& kv : p.clips) {
                            const Clip& oc = kv.second;
                            if (oc.start + oc.dur <= c.start || oc.start >= c.start + c.dur) continue;
                            auto rt2 = p.rows.find(oc.row);
                            if (rt2 == p.rows.end() || (rt2->second.type != "image" && rt2->second.type != "video")) continue;
                            std::string lay2 = jstr(oc.params, "layout");
                            if (lay2 == "fullscreen" || lay2 == "fit") { fullC = true; break; }
                        }
                        autoCorner = hasC ? (ccx > center.x ? 0 : 1) : (fullC ? 0 : 1);
                        // two auto-placed plates up at once (an act card + a beat plate): the
                        // later-starting one takes the OPPOSITE corner instead of stacking.
                        for (auto& kv : p.clips) {
                            const Clip& oc = kv.second;
                            if (kv.first == cid || (oc.type != "caption" && oc.type != "text")) continue;
                            if (!clip_active(oc, st.playhead) || jstr(oc.params, "place") != "auto") continue;
                            if (oc.start < c.start - 1e-6 || (std::fabs(oc.start - c.start) < 1e-6 && kv.first < cid))
                                autoCorner = 1 - autoCorner;
                        }
                    }
                    draw_caption_clip(dl, cx, cy, s, c, alpha, st.playhead, f0, fw, fh, autoCorner);
                    continue;
                }
                // shape: a vector callout (highlight box, arrow, ellipse, bracket).
                if (c.type == "shape") { draw_shape_clip(dl, cx, cy, s, c, alpha, st.playhead); continue; }
                // diagram: reusable boxes + arrows (flow chains / labeled graphs) with a staged reveal.
                if (c.type == "diagram") { draw_diagram_clip(dl, cx, cy, s, c, alpha, st.playhead, f0, fw, fh, fx.sclMul); continue; }
                // gradient/vignette: a full-frame wash to tame a busy bg or fade an edge.
                if (c.type == "gradient") { draw_gradient_clip(dl, f0, fw, fh, c, alpha, st.playhead); continue; }
                // filler: a heavily-blurred, cover-scaled backdrop that fills black/empty bg with the
                // foreground content (place on a LOW track). `source` = an asset key, or "auto" (the
                // topmost active image) — so a black scene gets a soft blurred version of its own shot.
                if (c.type == "filler") {
                    if (g_compositeSkipFillers) continue;   // rendering the backdrop RT itself → a filler draws nothing (no feedback)
                    std::string srcKey = jstr(c.params, "source");
                    // Preferred: the WHOLE-FRAME backdrop — the gaussian-blurred foreground composite
                    // (g_fillSrv, see render_fill_backdrop), zoomed IN (~4x) so only the central region
                    // shows, enlarged → a smooth colour GRADIENT that fills the frame instead of a
                    // recognisable blurred copy of the scene ("halo"). Dimmed. Tracks every layer's motion;
                    // never a flat wash. (source:"<asset>" still pins to one plate via the legacy path
                    // below; only "auto"/unset uses the whole-frame backdrop. `fill` tunes the zoom.)
                    if (g_fillReady && g_fillSrv && g_fillW > 0 && (srcKey.empty() || srcKey == "auto")) {
                        double dim = anim_param(c, "dim", T, 0.55); double bdF = dim < 0 ? 0 : dim > 1 ? 1 : dim;
                        float cover = std::max(fw / (float)g_fillW, fh / (float)g_fillH) * (float)anim_param(c, "fill", T, 4.0);
                        float w = g_fillW * cover, h = g_fillH * cover;
                        ImVec2 a(center.x - w * 0.5f, center.y - h * 0.5f), b(a.x + w, a.y + h);
                        int bd = (int)(bdF * 255);
                        dl->AddImage((ImTextureID)(intptr_t)g_fillSrv, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(bd, bd, bd, alpha));
                        continue;
                    }
                    // Legacy fallback (source pinned to an asset, or the backdrop pre-pass didn't run):
                    // a single image plate / host sprite, statically cover-scaled + blurred.
                    std::string srcUri;
                    if (!srcKey.empty() && srcKey != "auto") { auto au = p.asset_uri.find(srcKey); if (au != p.asset_uri.end()) srcUri = au->second; }
                    if (srcUri.empty())   // auto: the topmost active IMAGE asset (the foreground)
                        for (auto t2 = p.tracks.begin(); t2 != p.tracks.end() && srcUri.empty(); ++t2)
                            for (auto& r2 : t2->rows) {
                                auto ri2 = p.rows.find(r2);
                                if (ri2 == p.rows.end() || ri2->second.type != "image") continue;
                                for (auto& c2 : ri2->second.clips) {
                                    auto ci2 = p.clips.find(c2);
                                    if (ci2 != p.clips.end() && ci2->first != c.id && clip_active(ci2->second, st.playhead) && !ci2->second.asset.empty()) {
                                        auto au2 = p.asset_uri.find(ci2->second.asset);
                                        if (au2 != p.asset_uri.end()) { srcUri = au2->second; break; }
                                    }
                                }
                                if (!srcUri.empty()) break;
                            }
                    Tex* ft = srcUri.empty() ? nullptr : get_texture(resolve_asset(srcUri));
                    std::string blurUri = srcUri;                    // what get_processed_srv blurs (image asset OR host sprite)
                    bool avatarFill = false;
                    if ((!ft || ft->w <= 0) && (srcKey.empty() || srcKey == "auto")) {
                        // No image plate on screen — fall back to the HOST sprite so the fill isn't pure
                        // BLACK when only the avatar is up (blur an enlarged copy of her as the backdrop).
                        std::string spPath; Tex* sp = topmost_avatar_sprite(p, st, c.id, spPath);
                        if (sp && sp->w > 0) { ft = sp; blurUri = spPath; avatarFill = true; }
                    }
                    if (ft && ft->w > 0) {
                        double blur = anim_param(c, "blur", T, 30.0), dim = anim_param(c, "dim", T, 0.55);
                        double bdF = dim < 0 ? 0 : dim > 1 ? 1 : dim;
                        // The host sprite is TRANSPARENT (a character on nothing), so cover-scaling it alone
                        // leaves the corners black. Back it with a flat wash of her dominant (opaque-pixel)
                        // colour so the whole frame is filled — a soft tinted backdrop, not a black void.
                        if (avatarFill) {
                            ImgMean hm = image_mean(blurUri);
                            int wr = 10, wg = 8, wb = 14;   // dim neutral fallback
                            if (hm.ok) { wr = (int)(hm.r * bdF); wg = (int)(hm.g * bdF); wb = (int)(hm.b * bdF); }
                            dl->AddRectFilled(f0, ImVec2(f0.x + fw, f0.y + fh), IM_COL32(wr, wg, wb, alpha));
                        }
                        ID3D11ShaderResourceView* srv = ft->srv;
                        if (blur > 0.4 && !blurUri.empty()) { auto ps = get_processed_srv(blurUri, (float)blur, 1.0f, 1.0f); if (ps) srv = ps; }
                        float cover = std::max(fw / (float)ft->w, fh / (float)ft->h) * (float)anim_param(c, "fill", T, 1.18);
                        float w = ft->w * cover, h = ft->h * cover;
                        ImVec2 a(center.x - w * 0.5f, center.y - h * 0.5f), b(a.x + w, a.y + h);
                        int bd = (int)(bdF * 255);
                        dl->AddImage((ImTextureID)(intptr_t)srv, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(bd, bd, bd, alpha));
                    }
                    continue;
                }

                // blur: a WHOLE-FRAME defocus transition — draws NOTHING here. It's applied as a
                // post-process over the FULL composited frame (see frame_blur_strength + blur_rgba,
                // driven by the preview pre-pass + the export readback) so it blurs EVERY layer —
                // host, captions, code, insets — not just one image plate. The clip is just the
                // timing/strength source; skip it in the per-clip draw.
                if (c.type == "blur") continue;

                // caption anchor: a move handle for the captions in its span (caption_anchor_off,
                // applied in the caption branch above) — the clip itself draws nothing.
                if (c.type == "anchor") continue;

                // avatar: a STATIC pose per expression + an audio-reactive bob + light-up.
                // (SD and authored-sheet frame ANIMATION aren't stable enough to feel good, so
                // until an Inochi2D mesh rig the host is one PNG per expression sold as "talking"
                // by a bob + brighten driven by viseme openness — the classic pngtuber move, both
                // tweakable. The emotion tag still picks the pose; `draw_pngtuber` is the fallback.)
                if (c.type == "avatar") {
                    // Resolve the driving VO clip active at the playhead — it sets BOTH the
                    // lip-sync TIME ORIGIN and the emotion auto-follow. Sampling visemes relative
                    // to the AUDIO's clip (not the avatar's own start) keeps the mouth in sync even
                    // if the avatar clip is moved/trimmed. (Visemes are timed from the audio start.)
                    double visOrigin = c.start, visRate = 1.0, visIn = 0.0;
                    std::string drivenEmo, voUri; bool haveVO = false;
                    auto drit = p.rows.find(jstr(rit->second.params, "driven_by"));
                    if (drit != p.rows.end())
                        for (auto& dcid : drit->second.clips) {
                            auto dc = p.clips.find(dcid);
                            if (dc != p.clips.end() && clip_active(dc->second, st.playhead)) {
                                visOrigin = dc->second.start;                       // ← bob follows the VO clip's CURRENT position
                                // Visemes/wave are timed to the ORIGINAL 1x audio, but a sped-up clip (shorts run
                                // ~1.3x) plays a TIME-STRETCHED buffer — so the mouth must sample the source AHEAD
                                // by `rate`, else the bob drifts behind the sound. Mirror the mixer's played→source
                                // map exactly (source_t = in + tau*rate; `in` = source in-point of a split take) so
                                // the bob tracks whatever the mixer actually sounds.
                                double defRate = (drit->second.type == "tts") ? p.speechRate : 1.0;
                                visRate = dc->second.params.value("rate", defRate);
                                if (visRate < 0.5) visRate = 0.5; if (visRate > 2.0) visRate = 2.0;
                                visIn = dc->second.params.value("in", 0.0);
                                drivenEmo = emotion_from_text(jstr(dc->second.params, "emotion"));
                                auto dau = p.asset_uri.find(dc->second.asset);
                                if (dau != p.asset_uri.end()) voUri = dau->second;  // the VO's audio → drives the bob
                                haveVO = true;
                                break;
                            }
                        }
                    std::string vuri;
                    auto va = p.asset_uri.find(c.asset);
                    if (va != p.asset_uri.end()) vuri = va->second;
                    const VisemeTrack* vtrack = vuri.empty() ? nullptr : get_visemes(vuri);
                    // emotion: the clip's own tag, or — when unset/"auto" — the driven line's emotion.
                    std::string emo = jstr(c.params, "emotion");
                    if (emo.empty() || emo == "auto") emo = drivenEmo.empty() ? "neutral" : drivenEmo;
                    std::string rig = jstr(rit->second.params, "rig");
                    if (rig.empty()) rig = "gemma-big";
                    const AvatarRig* arig = get_rig(rig);
                    std::string framing = avatar_framing(c.params, emo);
                    bool fullBody = (framing == "full" || framing == "floating");
                    bool faceShot = (framing == "bust" || framing == "closeup" || framing == "face");
                    // POSE VARIANT (overridable via params.pose). Default: full-body → floating sprite;
                    // when off-center content is on screen → the looking-aside ¾ pose facing it ("looks
                    // like she's looking at the thing"); else front. front34/float34 natively face RIGHT.
                    std::string pose = jstr(c.params, "pose");
                    bool faceFlip = false;
                    int side = 0;                                  // facing / content direction: -1 left, +1 right
                    { std::string faceP = jstr(c.params, "face");  // explicit override: "left"/"right"
                      if (faceP == "left") side = -1;
                      else if (faceP == "right") side = 1;
                      else {
                          // decide alignment ONCE over THIS host clip's whole span (stable — no mid-clip snap)
                          bool hasContent; float contentCx;
                          content_centroid_span(p, c.start, c.start + c.dur, center, s, fw, hasContent, contentCx);
                          if (hasContent) {
                              float d = contentCx - (center.x + (anc.x + (float)c.tx_pos[0]) * s);   // content vs THIS host
                              side = d > fw * 0.04f ? 1 : d < -fw * 0.04f ? -1 : 0;
                          }
                      } }
                    if (pose.empty() || pose == "auto") {
                        if (side != 0) { pose = fullBody ? "float34" : "front34"; faceFlip = (side < 0); }
                        else           { pose = fullBody ? "float_front" : "front"; }
                    }
                    // PRESENTER layout: with content on screen and the host at its DEFAULT position,
                    // auto-place it in the OPPOSITE corner (sits aside, facing the thing). Any manual pos
                    // wins. Gating on the FULL default (x AND y both 0, no pos keyframes) — not just x==0 —
                    // kills the old discontinuity where typing X=0 on a Y-nudged host re-triggered the
                    // offset (host jumped between "aside" and "literal" as the slider crossed 0).
                    // An anchor with a HORIZONTAL component (e.g. "code_host" x=+660) is explicit side
                    // placement too — the auto-offsets used to stack on top of it (anchor +660 then
                    // presenter +0.24·fw pushed her clean off-frame right on every anchored code beat).
                    // A purely VERTICAL anchor ("bust" = a y nudge) keeps the smart horizontal defaults
                    // (presenter side-step, over-footage shrink+corner).
                    bool avDefaultPos = c.tx_pos[0] == 0.0 && c.tx_pos[1] == 0.0 &&
                                        !c.keyframes.count("transform.pos") && fabsf(anc.x) < 0.5f;
                    // Presenter side-offset is a LANDSCAPE idea (host beside content). PORTRAIT/shorts stack
                    // content ABOVE a big bottom host, so a horizontal auto-offset just shoves her off-center
                    // with no way to get back to 0 (the user's "pos says 0,0 but she's on the left"). Off in portrait.
                    bool landscape = fw >= fh;
                    // FULLSCREEN media under this host beat: the footage is the shot — tuck her
                    // small into the lower-left instead of center-presenter (see avatar_fit).
                    bool overFootage = faceShot && avDefaultPos && landscape &&
                                       span_has_fullscreen_content(p, c.start, c.start + c.dur);
                    float autoOffX = (side != 0 && avDefaultPos && landscape) ? -side * fw * 0.24f : 0.0f;
                    if (overFootage && autoOffX == 0.0f) autoOffX = -fw * 0.335f;
                    // A centered jp_lesson plate IS the shot — the host steps well aside (hard left)
                    // instead of standing under it (distilled from the user's etymology-beat tweak).
                    if (autoOffX == 0.0f && side == 0 && avDefaultPos && landscape)
                        for (auto& kv : p.clips) {
                            const Clip& oc = kv.second;
                            if (oc.type != "caption" || jstr(oc.params, "style") != "jp_lesson") continue;
                            if (oc.start + oc.dur <= c.start || oc.start >= c.start + c.dur) continue;
                            autoOffX = -0.355f * fw; break;
                        }
                    bool spriteFloating = pose.find("float") != std::string::npos;
                    std::string spPath;
                    Tex* sp = avatar_variant(arig, emo, pose, &spPath);  // resting frame (static; spPath → grade it)
                    // smoothed "talk" level (attack/decay envelope over the visemes) drives the
                    // reactive bob/scale/brighten — eases in/out instead of snapping per cue.
                    // talk level (drives bob/scale/light-up): from the ACTIVE VO clip's audio envelope so it
                    // FOLLOWS the audio when clips are moved; 0 when no VO plays under the host (no stale bob).
                    double atk = c.params.value("talk_attack", 30.0), dec = c.params.value("talk_decay", 10.0);
                    const Wave* voWave = (haveVO && !voUri.empty()) ? get_wave(voUri) : nullptr;
                    double srcT = visIn + (st.playhead - visOrigin) * visRate;   // play-time → source-audio time (rate/in-aware)
                    float talk = !haveVO ? 0.0f
                               : voWave ? (float)audio_talk_at(voWave, srcT, atk, dec)
                                        : (float)viseme_talk_at(vtrack, srcT, atk, dec);
                    float bobAmt  = (float)c.params.value("bob", 1.0);        // idle breathing up/down amplitude
                    float bobSpd  = (float)c.params.value("bob_speed", 1.8);  // idle breathing rate (visible while PLAYING)
                    float talkBob = (float)c.params.value("talk_bob", 0.6);   // talk UP/DOWN amount (translation)
                    float talkScl = (float)c.params.value("talk_scale", 0.0); // talk SCALE amount (separate from bob)
                    float lightup = (float)c.params.value("lightup", 0.35);   // talk BRIGHTNESS only (0=off)
                    float dim     = (float)c.params.value("dim", 1.0);        // SILENT brightness 0..1 (1=no dim)
                    dim = dim < 0 ? 0 : (dim > 1 ? 1 : dim);
                    float bright = dim + (1.0f - dim) * talk;               // dim when silent → full while talking
                    if (sp) {
                        float pulse = 1.0f + talk * talkScl * 0.06f;        // SCALE pop while talking (talk_scale)
                        // Face-anchored framing (closeup/bust/full) places the WHOLE sprite (never crops);
                        // full-body uses the floating variant. `faceFlip` mirrors her to face on-screen
                        // content. A no-face fallback uses the authored transform.
                        float effSclX = eSclX * (faceFlip ? -1.0f : 1.0f);
                        bool soloShot = !span_has_content(p, c.start, c.start + c.dur);
                        AvatarFit fit = avatar_fit(spPath, sp, framing, f0, fw, fh,
                                                   cx - center.x + autoOffX, cy - center.y, effSclX, eSclY,
                                                   soloShot, overFootage);
                        float w, h; ImVec2 a, b;
                        float bobOff = 0.0f;   // current bob displacement (px; <0 = lifted UP). The contact shadow
                                               // stays on the GROUND (undoes this) + grows/fades with the lift.
                        if (fit.ok) {
                            float bw0 = fit.b.x - fit.a.x, bh0 = fit.b.y - fit.a.y;
                            w = bw0 * pulse; h = bh0 * pulse;
                            float yoff = sinf((float)st.playhead * bobSpd) * fabsf(h) * 0.012f * bobAmt
                                         - talk * talkBob * fabsf(h) * 0.05f;
                            bobOff = yoff;
                            a = ImVec2(fit.a.x - (w - bw0) * 0.5f, fit.a.y - (h - bh0) * 0.5f + yoff);  // grow about center
                            b = ImVec2(a.x + w, a.y + h);
                        } else {
                            w = sp->w * effSclX * s * pulse;   // eSclX/Y are keyframed (anim_xform) → the Fufu zoom
                            h = sp->h * eSclY * s * pulse;
                            float yoff = sinf((float)st.playhead * bobSpd) * h * 0.012f * bobAmt  // idle breathing
                                         - talk * talkBob * h * 0.05f;                            // talk UP/DOWN (talk_bob)
                            bobOff = yoff;
                            a = ImVec2(cx - w * eAncX, cy + yoff - h * eAncY);
                            b = ImVec2(a.x + w, a.y + h);
                        }
                        if (c.params.is_object() && c.params.contains("glow"))   // optional outer glow (common look prop)
                            draw_clip_glow(dl, a, b, c.params["glow"], s, alpha / 255.0f);
                        // ── integrate the host into the plate: color grade (match the bg) + contact shadow ──
                        // auto bg→host match by default (sampled above); explicit clip params override,
                        // `auto_grade:false` opts out.
                        bool autoG = c.params.value("auto_grade", true);
                        double avSat = anim_param(c, "saturation", T, autoG ? autoSat : 1.0);
                        double avCon = anim_param(c, "contrast", T, 1.0);
                        double avTemp = anim_param(c, "temperature", T, autoG ? autoTemp : 0.0);
                        double avTintc = anim_param(c, "tint", T, 0.0);
                        double shadow = anim_param(c, "shadow", T, autoG ? autoShadow : 0.0);  // contact shadow
                        ID3D11ShaderResourceView* ssrv = sp->srv;          // saturation/contrast → cached graded copy
                        if (!spPath.empty() && (avSat < 0.999 || avSat > 1.001 || avCon < 0.999 || avCon > 1.001)) {
                            ID3D11ShaderResourceView* gs = get_processed_srv(spPath, 0.0f, (float)avSat, (float)avCon);
                            if (gs) ssrv = gs;
                        }
                        // Contact shadow at her FEET. AUTO (`shadow`, grade-driven): only NON-floating
                        // sprites shown full-body (a float pose hovers; bust/closeup have feet off-frame).
                        // `foot_shadow` (explicit per-clip: bool or intensity) FORCES it on for ANY pose/
                        // framing — e.g. a floating "ghost" grounded by a contact shadow below her hem.
                        // Positioned at the alpha-bbox bottom (her actual feet), not the sprite-quad bottom.
                        bool forceShadow = false; double footAmt = shadow;
                        if (c.params.is_object() && c.params.contains("foot_shadow")) {
                            const json& fsj = c.params["foot_shadow"];
                            forceShadow = fsj.is_boolean() ? fsj.get<bool>() : (fsj.is_number() ? fsj.get<double>() > 0.01 : false);
                            if (forceShadow) footAmt = fsj.is_number() ? fsj.get<double>() : 0.45;
                        }
                        if (footAmt > 0.01 && (forceShadow || (!spriteFloating && !faceShot))) {
                            ImVec4 bb;
                            if (sprite_bbox(spPath, bb) && sp->w > 0 && sp->h > 0) {
                                float fX0 = std::min(a.x, b.x), fW = fabsf(b.x - a.x), fH = b.y - a.y;
                                // The shadow is cast on the FLOOR: it stays put while she bobs (undo bobOff so it
                                // doesn't ride up with her feet), and as she lifts UP it spreads a touch BIGGER +
                                // fades DIMMER (the penumbra grows) — the realistic ground-contact read.
                                float lift = bobOff < 0 ? -bobOff : 0.0f;                        // upward displacement (px)
                                float nl = fH > 0.0f ? lift / (fH * 0.045f) : 0.0f;              // 0..~1 across the bob's up-range
                                if (nl > 1.0f) nl = 1.0f;
                                float liftScl = 1.0f + 0.14f * nl;                               // ever so slightly bigger
                                float liftDim = 1.0f - 0.34f * nl;                               // ever so slightly dimmer
                                float feetY  = a.y - bobOff + (bb.w + 1) / (float)sp->h * fH;    // GROUND line (bob removed)
                                float footCx = fX0 + ((bb.x + bb.z) * 0.5f) / (float)sp->w * fW; // bbox center-x → screen
                                float sw2 = (bb.z - bb.x + 1) / (float)sp->w * fW * 0.78f * liftScl, sh = sw2 * 0.16f;
                                ImVec2 sa(footCx - sw2 * 0.5f, feetY - sh * 0.5f), sb(footCx + sw2 * 0.5f, feetY + sh * 0.5f);
                                int salpha = (int)((footAmt > 1 ? 1 : footAmt) * 0.5 * alpha * liftDim);
                                dl->AddImage((ImTextureID)(intptr_t)sp->srv, sa, sb, ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(0, 0, 0, salpha));                          // squashed silhouette
                            }
                        }
                        auto gcA = [](double v){ return (int)((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0 + 0.5); };
                        ImU32 tintc = IM_COL32(gcA(bright * (1.0 + avTemp * 0.3)), gcA(bright * (1.0 + avTintc * 0.3)),
                                               gcA(bright * (1.0 - avTemp * 0.3)), alpha);  // brightness + white-balance
                        // motion blur on the pose-swap slide: trailing ghosts along the transition
                        // velocity (strong mid-slide, none at rest — the velocity IS the ease's derivative).
                        if (fabsf(fx.vy) > 60.0f || fabsf(fx.vx) > 60.0f) {
                            const int G = 4; const float GDT = 1.0f / 140.0f;      // ghost spacing (s of trail)
                            for (int gi = G; gi >= 1; --gi) {
                                float ox = -fx.vx * s * GDT * gi, oy = -fx.vy * s * GDT * gi;
                                int ga = (int)((alpha / 255.0f) * 45.0f * (1.0f - (gi - 1) / (float)G));
                                if (ga <= 0) continue;
                                dl->AddImage((ImTextureID)(intptr_t)ssrv, ImVec2(a.x + ox, a.y + oy),
                                             ImVec2(b.x + ox, b.y + oy), ImVec2(0, 0), ImVec2(1, 1),
                                             (tintc & 0x00FFFFFF) | ((ImU32)ga << IM_COL32_A_SHIFT));
                            }
                        }
                        dl->AddImage((ImTextureID)(intptr_t)ssrv, a, b, ImVec2(0, 0), ImVec2(1, 1), tintc);
                        int lu = (int)(talk * lightup * 255.0f);            // light-up: additive glow above full
                        if (lu > 2 && g_blendAdd) {
                            dl->AddCallback(DrawCb_Additive, nullptr);
                            dl->AddImage((ImTextureID)(intptr_t)ssrv, a, b, ImVec2(0, 0), ImVec2(1, 1),
                                         IM_COL32(255, 250, 235, (lu * alpha) / 255));
                            dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
                        }
                    } else {
                        float aw = 360.f * eSclX * s, ah = 520.f * eSclY * s;
                        ImVec2 pa(cx - aw * eAncX, cy - ah * eAncY);
                        draw_pngtuber(dl, pa, ImVec2(pa.x + aw, pa.y + ah), talk, emo, st.playhead);
                    }
                    continue;
                }

                // video: a B-roll/footage clip backed by a decoded PROXY (tools/video-proxy.py).
                // Map the playhead → local time → proxy frame → texture (the editor's decode path),
                // then draw like an image with transform + the cheap draw-time tint (dim/temp/tint).
                // (blur/saturation/contrast are texture-PROCESSED + path-keyed → skipped here so a
                // scrub can't grow an unbounded processed-frame cache; add a bounded one if a beat needs it.)
                if (c.type == "video") {
                    auto vmIt = p.asset_video.find(c.asset);
                    FrameTex* ft = nullptr;
                    bool hasSrc = false;
                    if (vmIt != p.asset_video.end()) {
                        VideoMeta& vm = vmIt->second;
                        hasSrc = !vm.src.empty();
#ifdef SLOP_LIBAV
                        // Preferred: decode the source media in-process (no proxy step). Fills the
                        // meta's fps/frames/dims from the file on first open, then maps time→frame.
                        if (g_videoDirect && !vm.src.empty()) {
                            VideoDecoder* d = get_decoder(resolve_asset(vm.src));
                            if (d) {
                                if (vm.fps <= 0) { vm.fps = d->fps; vm.frames = d->frames; vm.w = d->w; vm.h = d->h; }
                                vm.frames = d->frames;   // stays synced when the decoder LEARNS the real EOF (bogus container counts)
                                ft = get_decoded_frame_tex(vm.src, video_frame_index(vm, c, T), d);
                            }
                        }
#endif
                        // Fallback: the decimated JPEG proxy (no libav, or src wouldn't open).
                        if (!ft && !vm.proxy.empty())
                            ft = get_frame_tex(resolve_asset(vm.proxy), video_frame_index(vm, c, T));
                    }
                    if (ft) {
                        float cx0, cy0, cw, ch; clip_crop(c, cx0, cy0, cw, ch);                     // params.crop → source sub-rect
                        float w = ft->w * cw * std::fabs(eSclX) * s, h = ft->h * ch * std::fabs(eSclY) * s;   // |scale| + UV flip = mirror in place
                        ImVec2 a(cx - w * eAncX, cy - h * eAncY), b(a.x + w, a.y + h);
                        ImVec2 uv0(cx0, cy0), uv1(cx0 + cw, cy0 + ch);
                        if (eSclX < 0) std::swap(uv0.x, uv1.x);
                        if (eSclY < 0) std::swap(uv0.y, uv1.y);
                        if (c.params.is_object() && c.params.contains("glow"))   // optional outer glow (common look prop)
                            draw_clip_glow(dl, a, b, c.params["glow"], s, alpha / 255.0f);
                        // frame: explicit param, else default-ON for a non-fullscreen INSET with NO glow (e.g.
                        // the CRT) — a pro border + drop shadow that lifts it off the bg. frame:false opts out.
                        // Inset-ness from the BASE size (authored transform × layout — no motion/transition/
                        // keyframe animation): a motion zoom crossing 90% of the frame used to strip the
                        // border MID-CLIP, and a pop entrance made a fullscreen clip briefly "inset".
                        bool hasGlow = glow_enabled(c.params);
                        float bw = ft->w * cw * fabsf((float)c.tx_scale[0]) * layMul * s;
                        float bh = ft->h * ch * fabsf((float)c.tx_scale[1]) * layMul * s;
                        bool insetClip = (bw < fw * 0.9f || bh < fh * 0.9f) && !hasGlow;
                        json frameP = resolve_frame(c.params, insetClip);
                        if (!frameP.is_null() && (!frameP.is_object() || frameP.value("shadow", true)))
                            draw_inset_shadow(dl, a, b, (frameP.is_object() ? (float)frameP.value("radius", 9.0) : 9.0f) * s, s, alpha / 255.0f);
                        double dim  = anim_param(c, "dim", T, 1.0);
                        double temp = anim_param(c, "temperature", T, 0.0);   // warm(+)/cool(-)
                        double tnt  = anim_param(c, "tint", T, 0.0);          // green(-)/magenta(+)
                        bool autoG = c.params.value("auto_grade", false);     // opt-in white-balance match (sat needs a texture pass → skipped on video)
                        if (autoG) temp += autoTemp;
                        double base = dim < 0 ? 0 : dim > 1 ? 1 : dim;
                        auto gc = [](double v){ return (int)((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0 + 0.5); };
                        dl->AddImage((ImTextureID)(intptr_t)ft->srv, a, b, uv0, uv1,
                                     IM_COL32(gc(base * (1.0 + temp * 0.3)), gc(base * (1.0 + tnt * 0.3)),
                                              gc(base * (1.0 - temp * 0.3)), alpha));
                        if (!frameP.is_null()) draw_clip_frame(dl, a, b, frameP, s, alpha / 255.0f);
                    } else {
                        // No frame: source unreadable, or (no libav build) no proxy extracted yet.
                        float w = 480.f * eSclX * s, h = 270.f * eSclY * s;
                        ImVec2 a(cx - w * eAncX, cy - h * eAncY), b(a.x + w, a.y + h);
                        ImU32 col = type_color(c.type);
                        dl->AddRectFilled(a, b, (col & 0x00FFFFFF) | (60u << 24), 8.0f);
                        dl->AddRect(a, b, col, 8.0f, 0, 2.0f);
                        dl->AddText(ImVec2(a.x + 8, a.y + 8), col,
                                    hasSrc ? "video (can't decode source)" : "video (no source — set the asset uri/proxy)");
                    }
                    continue;
                }

                std::string uri;
                auto au = p.asset_uri.find(c.asset);
                if (au != p.asset_uri.end()) uri = au->second;
                Tex* tex = uri.empty() ? nullptr : get_texture(uri);

                if (tex) {
                    // negative scale = MIRROR: use |scale| for the rect (frame/shadow/glow stay correct,
                    // a<b) and flip the UVs instead → the clip mirrors in place around its anchor.
                    float cx0, cy0, cw, ch; clip_crop(c, cx0, cy0, cw, ch);          // params.crop → source sub-rect
                    float w = tex->w * cw * std::fabs(eSclX) * s;
                    float h = tex->h * ch * std::fabs(eSclY) * s;
                    ImVec2 a(cx - w * eAncX, cy - h * eAncY);
                    ImVec2 b(a.x + w, a.y + h);
                    ImVec2 uv0(cx0, cy0), uv1(cx0 + cw, cy0 + ch);
                    if (eSclX < 0) std::swap(uv0.x, uv1.x);
                    if (eSclY < 0) std::swap(uv0.y, uv1.y);
                    if (c.params.is_object() && c.params.contains("glow"))   // optional outer glow (contrast vs bg)
                        draw_clip_glow(dl, a, b, c.params["glow"], s, alpha / 255.0f);
                    // frame: explicit param, else default-ON for a non-fullscreen INSET with NO glow (a glow
                    // already styles a filler-backed inset; this is the pro border for the bare ones). frame:false opts out.
                    // A source with real transparency (a cutout PNG) gets NO default frame — the border would
                    // outline an invisible rectangle around the figure. Inset-ness from the BASE size
                    // (authored transform × layout — no motion/transition/keyframe animation): a motion
                    // zoom crossing 90% of the frame used to strip the border MID-CLIP.
                    bool hasGlow = glow_enabled(c.params);
                    float bw = tex->w * cw * fabsf((float)c.tx_scale[0]) * layMul * s;
                    float bh = tex->h * ch * fabsf((float)c.tx_scale[1]) * layMul * s;
                    bool insetClip = (bw < fw * 0.9f || bh < fh * 0.9f) && !hasGlow
                                     && !image_has_alpha(uri);
                    json frameP = resolve_frame(c.params, insetClip);
                    if (!frameP.is_null() && (!frameP.is_object() || frameP.value("shadow", true)))
                        draw_inset_shadow(dl, a, b, (frameP.is_object() ? (float)frameP.value("radius", 9.0) : 9.0f) * s, s, alpha / 255.0f);
                    // keyframeable defocus: blur>0 swaps in a cached low-res blurred copy
                    // (source-px sigma) so titles/text stay legible over bright footage.
                    ID3D11ShaderResourceView* srv = tex->srv;
                    // keyframeable defocus blur + color grade (saturation/contrast) → a cached
                    // CPU-processed copy (param-hash by uri|blur|sat|contrast); identical in export.
                    double blur = anim_param(c, "blur", T, 0.0);
                    double sat  = anim_param(c, "saturation", T, 1.0);  // chroma scale (1=neutral; <1 tames oversaturated gens)
                    double con  = anim_param(c, "contrast", T, 1.0);    // contrast around mid-gray (1=neutral; <1 flattens)
                    // opt-in auto color-grade: sit a floating inset in the scene the way the host
                    // does — gentle desat + warm/cool match toward the bottom bg plate.
                    bool autoG = c.params.value("auto_grade", false);
                    if (autoG) sat *= autoSat;
                    if (blur > 0.4 || sat < 0.999 || sat > 1.001 || con < 0.999 || con > 1.001) {
                        ID3D11ShaderResourceView* ps = get_processed_srv(uri, (float)blur, (float)sat, (float)con);
                        if (ps) srv = ps;
                    }
                    // dim (darken) + white-balance (temperature/tint) fold into a per-CHANNEL multiply
                    // tint — cheap + keyframeable, no texture pass. `dim` 1=full bright; temp warm(+)/cool(-).
                    double dim    = anim_param(c, "dim", T, 1.0);
                    double temp   = anim_param(c, "temperature", T, 0.0);  // warm(+) / cool(-)
                    double tintgm = anim_param(c, "tint", T, 0.0);         // green(-) / magenta(+)
                    if (autoG) temp += autoTemp;                            // auto bg→clip white-balance match
                    double base = dim < 0 ? 0 : dim > 1 ? 1 : dim;
                    auto gc = [](double v){ return (int)((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0 + 0.5); };
                    dl->AddImage((ImTextureID)(intptr_t)srv, a, b, uv0, uv1,
                                 IM_COL32(gc(base * (1.0 + temp * 0.3)), gc(base * (1.0 + tintgm * 0.3)),
                                          gc(base * (1.0 - temp * 0.3)), alpha));
                    if (!frameP.is_null()) draw_clip_frame(dl, a, b, frameP, s, alpha / 255.0f);
                } else {
                    // typed placeholder (avatar before sprites, or a not-yet-ready asset)
                    float w = (c.type == "avatar" ? 360.f : 320.f) * eSclX * s;
                    float h = (c.type == "avatar" ? 520.f : 320.f) * eSclY * s;
                    ImVec2 a(cx - w * eAncX, cy - h * eAncY);
                    ImVec2 b(a.x + w, a.y + h);
                    ImU32 col = type_color(c.type);
                    dl->AddRectFilled(a, b, (col & 0x00FFFFFF) | (60u << 24), 8.0f);
                    dl->AddRect(a, b, col, 8.0f, 0, 2.0f);
                    std::string lab = c.type + (c.asset.empty() ? "" : " (loading…)");
                    dl->AddText(ImVec2(a.x + 8, a.y + 8), col, lab.c_str());
                }
            }
        }
    }
    // scene vignette: darken frame edges to unify bg + host (crude 4-edge gradient; meta.vignette).
    if (p.vignette > 0.01) {
        float vg = (float)(p.vignette > 1 ? 1 : p.vignette);
        ImU32 d0 = IM_COL32(0, 0, 0, (int)(vg * 150)), d1 = IM_COL32(0, 0, 0, 0);
        ImVec2 q0 = f0, q1 = ImVec2(f0.x + fw, f0.y + fh);
        float band = (fw < fh ? fw : fh) * 0.30f;
        dl->AddRectFilledMultiColor(q0, ImVec2(q1.x, q0.y + band), d0, d0, d1, d1);            // top
        dl->AddRectFilledMultiColor(ImVec2(q0.x, q1.y - band), q1, d1, d1, d0, d0);            // bottom
        dl->AddRectFilledMultiColor(q0, ImVec2(q0.x + band, q1.y), d0, d1, d1, d0);            // left
        dl->AddRectFilledMultiColor(ImVec2(q1.x - band, q0.y), q1, d1, d0, d0, d1);            // right
    }
    // auto "now playing" song credit — drawn LAST so it sits on top of the vignette + all content
    // (skipped while rendering the filler-backdrop pre-pass, so it never blurs into the backdrop).
    if (p.songCredits && fw > 1 && !g_compositeSkipFillers) draw_song_credit(p, st, dl, f0, fw, fh);
    dl->PopClipRect();
}

static std::string g_dropPath; static POINT g_dropPt{}; static bool g_hasDrop = false;  // pending drag-and-drop file
// Add an image clip referencing an external file at (row, t) — model + doc, so it renders + saves.
static std::string add_image_clip_at(Project& p, const std::string& row, double t, const std::string& path, double dur = 4.0) {
    std::string akey; int n = 1; while (p.asset_uri.count(akey = "ext_img" + std::to_string(n))) n++;
    p.asset_uri[akey] = path;
    if (p.doc.contains("assets") && p.doc["assets"].is_object())
        p.doc["assets"][akey] = json{{"provider", "external"}, {"type", "image"}, {"status", "ready"}, {"uri", path}, {"meta", json::object()}};
    std::string id; int m = 1; while (p.clips.count(id = "c_drop" + std::to_string(m))) m++;
    std::string fn = path; size_t sl = fn.find_last_of("/\\"); if (sl != std::string::npos) fn = fn.substr(sl + 1);
    Clip c; c.id = id; c.row = row; c.type = "image"; c.start = t; c.dur = dur; c.asset = akey; c.label = fn;
    // Content-inset default = the render-time `layout` primitive (was a baked transform distilled
    // from the hand-tuned beats — now computed live in composite_frame, so an asset swap or a
    // project-res change stays framed; the user nudges/rescales per clip ON TOP of the layout).
    c.params = json{{"motion", "zoom_in"}, {"auto_grade", true}, {"layout", "inset"},
                    {"glow", json{{"enabled", true}, {"size", 24.0}, {"strength", 0.55}}},
                    {"frame", json{{"enabled", true}, {"thickness", 2.0}, {"radius", 8.0}, {"color", json::array({244, 248, 255, 220})}}}};
    p.clips[id] = c;
    if (p.rows.count(row)) p.rows[row].clips.push_back(id);
    if (p.doc.contains("clips"))
        p.doc["clips"][id] = json{{"row", row}, {"start", t}, {"dur", dur}, {"asset", akey}, {"params", c.params},
            {"transform", json{{"pos", {0, 0}}, {"scale", {1.0, 1.0}}, {"rot", 0}, {"opacity", 1}, {"anchor", {0.5, 0.5}}}}};
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips"))
        p.doc["rows"][row]["clips"].push_back(id);
    return id;
}

// Best-effort attribution line from tags. The whole stock-music palette is Kevin MacLeod's
// incompetech catalogue (CC BY 4.0) → for that artist emit the canonical credit that matches the
// existing sidecars exactly; any OTHER artist gets a plain "Title" by Artist that the user finishes
// with a license in the asset properties (never ASSERT a license we can't infer). Feeds BOTH the
// export description credits AND the on-screen now-playing chip.
static std::string attribution_for(const std::string& title, const std::string& artist) {
    if (title.empty() && artist.empty()) return "";
    std::string t = title.empty() ? "Untitled" : title;
    std::string al = artist; for (auto& ch : al) ch = (char)tolower((unsigned char)ch);
    if (al == "kevin macleod") return "\"" + t + "\" Kevin MacLeod (incompetech.com) — CC BY 4.0";
    if (!artist.empty()) return "\"" + t + "\" by " + artist;
    return "\"" + t + "\"";
}

// Read embedded audio tags (ID3 / Vorbis-comment / …) into an asset `meta` object:
//   { title, artist, album, year, attribution:{attribution_text} }
// so a dragged-in song auto-fills its credit with ZERO hand-authoring (the recurring pain: new
// songs arrived with no `.meta.json`). A `<file>.meta.json` sidecar (the stock convention) still
// WINS — a hand-tuned credit is authoritative. Falls back to {} when libav is absent / no tags.
static json audio_meta_from_tags(const std::string& path) {
    { std::ifstream f(resolve_asset(path) + ".meta.json", std::ios::binary);   // sidecar wins
      if (f.good()) { try { json j; f >> j; if (j.is_object() && j.contains("meta") && j["meta"].is_object()) return j["meta"]; } catch (...) {} } }
    json meta = json::object();
#ifdef SLOP_LIBAV
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, resolve_asset(path).c_str(), nullptr, nullptr) == 0 && fmt) {
        avformat_find_stream_info(fmt, nullptr);
        auto tag = [&](const char* k) -> std::string {
            AVDictionaryEntry* e = av_dict_get(fmt->metadata, k, nullptr, AV_DICT_IGNORE_SUFFIX);
            if ((!e || !e->value))                                            // some muxers stash tags on the stream
                for (unsigned i = 0; i < fmt->nb_streams && (!e || !e->value); i++)
                    e = av_dict_get(fmt->streams[i]->metadata, k, nullptr, AV_DICT_IGNORE_SUFFIX);
            return (e && e->value) ? std::string(e->value) : std::string();
        };
        std::string title = tag("title"), artist = tag("artist"), album = tag("album");
        std::string date = tag("date"); if (date.empty()) date = tag("year"); if (date.empty()) date = tag("TYER");
        if (!title.empty())  meta["title"]  = title;
        if (!artist.empty()) meta["artist"] = artist;
        if (!album.empty())  meta["album"]  = album;
        if (date.size() >= 4) meta["year"] = date.substr(0, 4);               // just the year
        std::string att = attribution_for(title, artist);
        if (!att.empty()) meta["attribution"] = json{{"attribution_text", att}};
        avformat_close_input(&fmt);
    }
#endif
    return meta;
}

// Drop an external audio file (library sound / SFX / music bed) onto an audio row → a clip at its
// real length (WAV decoded; mp3 → default, trim to taste). Mixed by the same recipe as gen audio.
static std::string add_audio_clip_at(Project& p, const std::string& row, double t, const std::string& path) {
    std::string akey; int n = 1; while (p.asset_uri.count(akey = "ext_aud" + std::to_string(n))) n++;
    p.asset_uri[akey] = path;
    std::string rtype = p.rows.count(row) ? p.rows[row].type : "music";
    // a dragged-in SONG auto-fills its title/artist/credit from the file's ID3 tags (the credit
    // then drives the export description + the on-screen now-playing chip). Editable in the Inspector.
    json ameta = (rtype == "music") ? audio_meta_from_tags(path) : json::object();
    if (p.doc.contains("assets") && p.doc["assets"].is_object())
        p.doc["assets"][akey] = json{{"provider", "external"}, {"type", rtype == "tts" ? "speech" : "music"},
                                     {"status", "ready"}, {"uri", path}, {"meta", ameta}};
    Pcm* pc = get_pcm(path); double dur = (pc && pc->dur > 0.05) ? pc->dur : 4.0;   // real WAV length, else default
    std::string id; int m = 1; while (p.clips.count(id = "c_aud" + std::to_string(m))) m++;
    std::string fn = path; size_t sl = fn.find_last_of("/\\"); if (sl != std::string::npos) fn = fn.substr(sl + 1);
    Clip c; c.id = id; c.row = row; c.type = rtype; c.start = t; c.dur = dur; c.asset = akey; c.label = fn; c.params = json::object();
    p.clips[id] = c;
    if (p.rows.count(row)) p.rows[row].clips.push_back(id);
    if (p.doc.contains("clips"))
        p.doc["clips"][id] = json{{"row", row}, {"start", t}, {"dur", dur}, {"asset", akey}, {"params", json::object()},
            {"transform", json{{"pos", {0, 0}}, {"scale", {1.0, 1.0}}, {"rot", 0}, {"opacity", 1}, {"anchor", {0.5, 0.5}}}}};
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips"))
        p.doc["rows"][row]["clips"].push_back(id);
    return id;
}

static std::string add_track(Project& p, const std::string& type, const std::string& name);  // defined below — returns the new row id

// Drop a video file onto a video row → a clip backed by IN-PROCESS decode (no proxy step). The
// asset is just {type:video, uri}; the libav decoder fills fps/frames/dims (and we probe them here
// to set the clip's real length). The clip type comes from its row, so this needs a `video` row.
static std::string add_video_clip_at(Project& p, const std::string& row, double t, const std::string& path) {
    std::string akey; int n = 1; while (p.asset_uri.count(akey = "ext_vid" + std::to_string(n))) n++;
    p.asset_uri[akey] = path;
    VideoMeta vm; vm.src = path;
    double dur = 6.0;
#ifdef SLOP_LIBAV
    if (VideoDecoder* d = get_decoder(resolve_asset(path))) {
        vm.fps = d->fps; vm.frames = d->frames; vm.w = d->w; vm.h = d->h;
        if (d->fps > 0 && d->frames > 0) dur = d->frames / d->fps;     // real source length
    }
#endif
    p.asset_video[akey] = vm;
    if (p.doc.contains("assets") && p.doc["assets"].is_object())
        p.doc["assets"][akey] = json{{"provider", "external"}, {"type", "video"}, {"status", "ready"}, {"uri", path}, {"meta", json::object()}};
    std::string id; int m = 1; while (p.clips.count(id = "c_vid" + std::to_string(m))) m++;
    std::string fn = path; size_t sl = fn.find_last_of("/\\"); if (sl != std::string::npos) fn = fn.substr(sl + 1);
    json params = json{{"in", 0.0}, {"speed", 1.0}, {"loop", true}, {"video_volume", 0.12},
                       {"motion", "zoom_in"}, {"auto_grade", false}, {"layout", "inset"},
                       {"glow", json{{"enabled", true}, {"size", 24.0}, {"strength", 0.55}}},
                       {"frame", json{{"enabled", true}, {"thickness", 2.0}, {"radius", 8.0}, {"color", json::array({244, 248, 255, 220})}}}};
    Clip c; c.id = id; c.row = row; c.type = "video"; c.start = t; c.dur = dur; c.asset = akey; c.label = fn; c.params = params;
    p.clips[id] = c;
    if (p.rows.count(row)) p.rows[row].clips.push_back(id);
    if (p.doc.contains("clips"))
        p.doc["clips"][id] = json{{"row", row}, {"start", t}, {"dur", dur}, {"asset", akey}, {"params", params},
            {"transform", json{{"pos", {0, 0}}, {"scale", {1.0, 1.0}}, {"rot", 0}, {"opacity", 1}, {"anchor", {0.5, 0.5}}}}};
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips"))
        p.doc["rows"][row]["clips"].push_back(id);
    return id;
}

// Find an existing `video` row, or create a "Footage" one. Returns the row id (drag-drop targets).
static std::string find_or_create_video_row(Project& p) {
    for (auto& kv : p.rows) if (kv.second.type == "video") return kv.first;
    add_track(p, "video", "Footage");
    for (auto& kv : p.rows) if (kv.second.type == "video") return kv.first;
    return "";
}

// Place an avatar clip FROM SCRATCH (the user's "can't add an avatar clip except by splitting"):
// ensure an avatar row that references `rig` (adopt a rig-less avatar row, else create one and point
// it at the first VO row so lip-sync + emotion auto-follow work), then add a static-pose clip. All
// avatar clips on the row inherit the rig (the emotion frames) by sharing the row's `rig` param.
static std::string add_avatar_clip_at(Project& p, const std::string& rig, double t, double dur = 3.0) {
    std::string row;
    for (auto& kv : p.rows) if (kv.second.type == "avatar" && jstr(kv.second.params, "rig") == rig) { row = kv.first; break; }
    if (row.empty()) {
        for (auto& kv : p.rows) if (kv.second.type == "avatar" && jstr(kv.second.params, "rig").empty()) { row = kv.first; break; }
        if (row.empty()) row = add_track(p, "avatar", "Avatar");   // use the NEW row's id (a std::map iter would re-pick an existing avatar row)
        if (!row.empty()) {
            p.rows[row].params["rig"] = rig;
            if (jstr(p.rows[row].params, "driven_by").empty())
                for (auto& kv : p.rows) if (kv.second.type == "tts") { p.rows[row].params["driven_by"] = kv.first; break; }
            if (p.doc.contains("rows") && p.doc["rows"].contains(row)) p.doc["rows"][row]["params"] = p.rows[row].params;
        }
    }
    if (row.empty()) return "";
    bool haveDriven = !jstr(p.rows[row].params, "driven_by").empty();
    std::string id; int m = 1; while (p.clips.count(id = "c_av" + std::to_string(m))) m++;
    json params = json::object();
    params["emotion"] = haveDriven ? "auto" : "neutral";   // auto = follow the driven VO line's emotion
    params["framing"] = "bust";   // host's common framing — half-bust (feet off-frame); change to full/closeup in the panel
    Clip c; c.id = id; c.row = row; c.type = "avatar"; c.start = t; c.dur = dur; c.label = "avatar:" + rig; c.params = params;
    p.clips[id] = c;
    if (p.rows.count(row)) p.rows[row].clips.push_back(id);
    if (p.doc.contains("clips"))
        p.doc["clips"][id] = json{{"row", row}, {"start", t}, {"dur", dur}, {"params", params},
            {"transform", json{{"pos", {0, 0}}, {"scale", {1.0, 1.0}}, {"rot", 0}, {"opacity", 1}, {"anchor", {0.5, 0.5}}}}};
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips"))
        p.doc["rows"][row]["clips"].push_back(id);
    return id;
}

// When a clip's edge is TRIMMED, keep a baked fade (opacity keyframes) anchored to that edge instead
// of stranding it mid-clip. Extending a clip used to leave its fade-OUT at the OLD end → the image
// faded to 0 early and (with the glow drawn independent of opacity) left a translucent square. The
// trailing fade = the last strictly-descending run of opacity keyframes (+ the plateau point it drops
// from); shift it by the same delta the clip's END moved. The leading fade (ascending head) is the
// mirror for the LEFT edge. No-op for clips with no opacity keyframes (default-transition fades already
// track duration; these helpers only matter for BAKED fades, e.g. luckymas' assembled cut).
static double kf_scalar(const KF& k) { return k.v.empty() ? 0.0 : k.v[0]; }
static void reanchor_trailing_fade(Clip& c, double d) {
    auto it = c.keyframes.find("transform.opacity");
    if (it == c.keyframes.end()) return;
    auto& ks = it->second; int n = (int)ks.size();
    if (n < 2) return;
    int i = n - 1;
    while (i > 0 && kf_scalar(ks[i - 1]) > kf_scalar(ks[i])) i--;                 // top of the descending tail
    if (i == 0 || !(kf_scalar(ks[i]) > kf_scalar(ks[n - 1]) + 1e-6)) return;      // not an edge fade-out
    if (d < 0 && ks[i].t + d <= ks[i - 1].t + 1e-3) d = ks[i - 1].t + 1e-3 - ks[i].t;  // don't cross the plateau before it
    for (int j = i; j < n; ++j) ks[j].t += d;
}
static void reanchor_leading_fade(Clip& c, double d) {
    auto it = c.keyframes.find("transform.opacity");
    if (it == c.keyframes.end()) return;
    auto& ks = it->second; int n = (int)ks.size();
    if (n < 2) return;
    int i = 0;
    while (i + 1 < n && kf_scalar(ks[i + 1]) > kf_scalar(ks[i])) i++;             // top of the ascending head
    if (i == n - 1 || !(kf_scalar(ks[i]) > kf_scalar(ks[0]) + 1e-6)) return;      // not an edge fade-in
    if (d > 0 && ks[i].t + d >= ks[i + 1].t - 1e-3) d = ks[i + 1].t - 1e-3 - ks[i].t;  // don't cross the plateau after it
    for (int j = 0; j <= i; ++j) ks[j].t += d;
}

// ───────────────────────── audio gain envelope (volume automation) ─────────
// A music/VO clip's volume RAMP is a keyframe track on `params.gain_db` (values in dB — absolute
// clip gain; the lane volume + loudness-normalize still add on TOP, see the mixer at collect_audio).
// The mixer + export already READ it (≥2 keys ⇒ piecewise-linear volume, composed with the duck);
// these helpers + the widgets below are the missing EDITOR surface for authoring the slope by hand.
// Times are ABSOLUTE timeline seconds (the KF convention), kept sorted; segments are linear.
static const char* GAIN_KF = "params.gain_db";
static const float GAINENV_TOP = 6.0f, GAINENV_BOT = -60.0f;   // dB range shown/edited (−60 ≈ silence, for fades)
static std::vector<KF>* gain_env(Clip& c) {                    // the track (≥1 key), or null
    auto it = c.keyframes.find(GAIN_KF);
    return it == c.keyframes.end() ? nullptr : &it->second;
}
static void gain_env_sort(std::vector<KF>& ks) {
    std::sort(ks.begin(), ks.end(), [](const KF& a, const KF& b) { return a.t < b.t; });
}
// Materialize a flat 2-point ramp (start+end at the current static gain) so a clip with no
// automation becomes an editable line. No-op if a ≥2-point ramp already exists.
static std::vector<KF>* gain_env_ensure(Clip& c) {
    auto* e = gain_env(c);
    if (e && e->size() >= 2) return e;
    double g = c.params.value("gain_db", 0.0);
    std::vector<KF>& ks = c.keyframes[GAIN_KF];
    ks.clear();
    KF a; a.t = c.start;         a.v = {g}; a.interp = "linear";
    KF b; b.t = c.start + c.dur; b.v = {g}; b.interp = "linear";
    ks.push_back(a); ks.push_back(b);
    return &ks;
}
// Insert a breakpoint at absolute time t / level db (materializes the ramp first).
static void gain_env_add(Clip& c, double t, double db) {
    auto* e = gain_env_ensure(c);
    t  = std::max(c.start, std::min(c.start + c.dur, t));
    db = std::max((double)GAINENV_BOT, std::min((double)GAINENV_TOP, db));
    KF k; k.t = t; k.v = {db}; k.interp = "linear";
    e->push_back(k); gain_env_sort(*e);
}
static void gain_env_clear(Clip& c) { c.keyframes.erase(GAIN_KF); }   // → flat static gain_db again
static float gainenv_db2f(double db) { return (float)((GAINENV_TOP - db) / (GAINENV_TOP - GAINENV_BOT)); }  // dB → 0(top)..1(bot)
static double gainenv_f2db(float f)  { return GAINENV_TOP - (double)f * (GAINENV_TOP - GAINENV_BOT); }

// The big precise editor, shown in the Inspector for a selected audio clip: a dB-gridded graph
// with the waveform behind it, draggable breakpoints (x=time, y=dB), click-to-add, right-click a
// middle point to delete, plus fade presets. Undo settles automatically (node drags are ImGui
// active items; discrete edits set g_undoDirty).
static void draw_gain_envelope(Project& p, Clip& c, double playhead) {
    ImGui::SeparatorText("volume envelope");
    auto* e = gain_env(c);
    bool have = e && e->size() >= 2;
    double body = c.params.value("gain_db", 0.0);
    if (!have) {
        if (ImGui::SmallButton("+ add ramp")) { gain_env_ensure(c); g_undoDirty = true; }
        ImGui::SameLine(); ImGui::TextDisabled("flat %+.1f dB — or click the graph to shape it", body);
    } else {
        if (ImGui::SmallButton("fade in")) {                       // silence → body over the first stretch
            gain_env_ensure(c); gain_env_add(c, c.start + std::min(1.5, c.dur * 0.4), body);
            gain_env(c)->front().v[0] = GAINENV_BOT; g_undoDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("fade out")) {                      // body → silence over the last stretch
            gain_env_ensure(c); gain_env_add(c, c.start + c.dur - std::min(1.5, c.dur * 0.4), body);
            gain_env(c)->back().v[0] = GAINENV_BOT; g_undoDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("flat")) { gain_env_clear(c); g_undoDirty = true; }
    }

    ImVec2 size(std::max(120.0f, ImGui::GetContentRegionAvail().x), 150.0f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    double dur = c.dur > 1e-3 ? c.dur : 1.0;
    auto T2x  = [&](double t)  { return p0.x + (float)((t - c.start) / dur) * size.x; };
    auto x2T  = [&](float x)   { return c.start + (double)((x - p0.x) / size.x) * dur; };
    auto db2y = [&](double db) { return p0.y + gainenv_db2f(db) * size.y; };
    auto y2db = [&](float y)   { return gainenv_f2db((y - p0.y) / size.y); };
    e = gain_env(c);
    bool haveRamp = e && e->size() >= 2;

    // ── INTERACTION FIRST: the breakpoint handles are submitted BEFORE the full-canvas "##genv" catch-all,
    // so a press ON a point DRAGS it instead of adding a new one (ImGui gives an overlapping press to the
    // first-submitted item — the same rule that made the timeline points steal-able). Drawing is deferred to
    // below so the dots still render on top. hovIdx/actIdx feed the circle styling. ──
    int delIdx = -1, hovIdx = -1, actIdx = -1; bool anyActive = false;
    if (haveRamp) {
        for (size_t i = 0; i < e->size(); ++i) {
            ImVec2 np(T2x((*e)[i].t), db2y((*e)[i].v[0]));
            ImGui::SetCursorScreenPos(ImVec2(np.x - 6, np.y - 6));
            ImGui::PushID((int)i);
            ImGui::InvisibleButton("##n", ImVec2(12, 12));
            bool nh = ImGui::IsItemHovered(), na = ImGui::IsItemActive();
            if (nh) hovIdx = (int)i;
            bool isEnd = (i == 0 || i + 1 == e->size());
            if (na) {
                actIdx = (int)i; anyActive = true;
                ImVec2 m = ImGui::GetIO().MousePos;
                double ndb = std::max((double)GAINENV_BOT, std::min((double)GAINENV_TOP, y2db(m.y)));
                double nt = (*e)[i].t;
                if (i == 0) nt = c.start;                       // endpoints keep their time; interior slides between neighbors
                else if (i + 1 == e->size()) nt = c.start + c.dur;
                else nt = std::max((*e)[i - 1].t + 1e-3, std::min((*e)[i + 1].t - 1e-3, std::max(c.start, std::min(c.start + c.dur, x2T(m.x)))));
                (*e)[i].t = nt; (*e)[i].v[0] = ndb;
                ImGui::SetTooltip("%.2fs   %+.1f dB", nt - c.start, ndb);
            }
            if (nh && !isEnd && ImGui::IsMouseClicked(1)) delIdx = (int)i;   // right-click a middle point → delete
            ImGui::PopID();
        }
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##genv", size);              // catch-all LAST: clicks that miss every handle add a point
    bool canvasHov = ImGui::IsItemHovered();

    // ── DRAW ──
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(20, 20, 26, 255));
    for (double gl : {6.0, 0.0, -6.0, -12.0, -24.0, -36.0, -48.0, -60.0}) {   // dB gridlines + labels
        float y = db2y(gl);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), gl == 0.0 ? IM_COL32(78, 78, 92, 255) : IM_COL32(42, 42, 50, 255));
        char b[16]; snprintf(b, sizeof b, "%+.0f", gl);
        dl->AddText(ImVec2(p0.x + 2, y - 1), IM_COL32(108, 108, 120, 255), b);
    }
    // waveform behind (faint) — asset time = in + localFrac*dur*rate (rate-aware, like the mixer)
    std::string uri; auto au = p.asset_uri.find(c.asset); if (au != p.asset_uri.end()) uri = au->second;
    Wave* wv = uri.empty() ? nullptr : get_wave(uri);
    if (wv && !wv->env.empty() && wv->dur > 1e-3) {
        double inOff = c.params.value("in", 0.0);
        auto rit = p.rows.find(c.row);
        double rate = c.params.value("rate", (rit != p.rows.end() && rit->second.type == "tts") ? p.speechRate : 1.0);
        int N = (int)wv->env.size(); float mid = p0.y + size.y * 0.5f;
        for (float x = p0.x; x < p0.x + size.x; x += 1.0f) {
            double at = inOff + ((double)(x - p0.x) / size.x) * dur * rate;
            double frac = at / wv->dur; if (frac < 0.0 || frac >= 1.0) continue;
            int bk = (int)(frac * N); if (bk < 0 || bk >= N) continue;
            float ee = wv->env[bk] * size.y * 0.45f;
            dl->AddLine(ImVec2(x, mid - ee), ImVec2(x, mid + ee), IM_COL32(44, 66, 88, 120));
        }
    }
    if (playhead >= c.start && playhead <= c.start + c.dur) {   // where the transport sits, for syncing a ramp to a moment
        float x = T2x(playhead);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), IM_COL32(240, 70, 70, 170));
    }
    const ImU32 LINE = IM_COL32(120, 200, 140, 235);
    if (!haveRamp) {   // no ramp yet: draw the flat static level; a click seeds a ramp through it
        float y = db2y(body);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), IM_COL32(120, 200, 140, 200), 1.5f);
        if (canvasHov && ImGui::IsMouseClicked(0)) { ImVec2 m = ImGui::GetIO().MousePos; gain_env_add(c, x2T(m.x), y2db(m.y)); g_undoDirty = true; }
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y + 4));
        ImGui::TextDisabled("click the graph to add a volume ramp");
        return;
    }
    // envelope polyline + flat extension to the clip edges (eval_kf clamps outside the end keys)
    dl->AddLine(ImVec2(p0.x, db2y(e->front().v[0])), ImVec2(T2x(e->front().t), db2y(e->front().v[0])), IM_COL32(120, 200, 140, 120), 1.5f);
    for (size_t i = 0; i + 1 < e->size(); ++i)
        dl->AddLine(ImVec2(T2x((*e)[i].t), db2y((*e)[i].v[0])), ImVec2(T2x((*e)[i + 1].t), db2y((*e)[i + 1].v[0])), LINE, 2.0f);
    dl->AddLine(ImVec2(T2x(e->back().t), db2y(e->back().v[0])), ImVec2(p0.x + size.x, db2y(e->back().v[0])), IM_COL32(120, 200, 140, 120), 1.5f);
    for (size_t i = 0; i < e->size(); ++i) {   // breakpoint dots (styled by the recorded hover/active state)
        ImVec2 np(T2x((*e)[i].t), db2y((*e)[i].v[0]));
        bool na = ((int)i == actIdx), nh = ((int)i == hovIdx);
        dl->AddCircleFilled(np, na ? 5.5f : 4.0f, na ? IM_COL32(255, 255, 255, 255) : (nh ? IM_COL32(210, 245, 220, 255) : LINE));
        dl->AddCircle(np, na ? 5.5f : 4.0f, IM_COL32(18, 28, 22, 255));
    }
    if (delIdx >= 0) { e->erase(e->begin() + delIdx); g_undoDirty = true; }
    else if (canvasHov && !anyActive && ImGui::IsMouseClicked(0)) {   // click that missed every handle → add a breakpoint
        ImVec2 m = ImGui::GetIO().MousePos; gain_env_add(c, x2T(m.x), y2db(m.y)); g_undoDirty = true;
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y + 4));
    ImGui::TextDisabled("drag points  ·  click to add  ·  right-click a middle point to remove");
}

// Snap a dragged/trimmed clip edge to the nearest OTHER clip edge (start or end), or the playhead /
// t=0, within `tol` seconds — so a host clip can be dropped FLUSH against its neighbour and the
// pose-swap slide fires (it only triggers within ~0.35s). Returns the snapped time, or `t` unchanged
// if nothing is close. Hold Alt while dragging to bypass (place two things deliberately touching).
static double snap_edge_time(Project& p, const std::string& movingId, double t, double tol, double playhead,
                             const std::set<std::string>& rows) {
    double best = t, bestd = tol;
    auto consider = [&](double e) { double dd = std::fabs(e - t); if (dd < bestd) { bestd = dd; best = e; } };
    for (auto& kv : p.clips) {
        if (kv.first == movingId || !rows.count(kv.second.row)) continue;   // own + adjacent lanes only (not the whole timeline)
        consider(kv.second.start); consider(kv.second.start + kv.second.dur);
    }
    consider(playhead); consider(0.0);
    return best;
}

static std::string choose_place_row(Project& p, const std::string& kind, const std::string& clickedRow, double t, double dur);
static std::string choose_row_of_type(Project& p, const std::string& want, const std::string& clickedRow, double t, double dur);
static double place_default_dur(const std::string& kind);   // placement-tool helpers (defined with add_quick_clip)

static void DrawTimeline(Project& p, UIState& st, const std::map<std::string, GenLite>& gen) {
    const float GUT = 156.0f, RULER = 22.0f, ROWH = 30.0f;
    if (g_tlVScroll >= 0.f) ImGui::SetScrollY(g_tlVScroll);   // headless: pin the lane scroll for shots
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float canvasW = avail.x, canvasH = avail.y;
    float trackX = origin.x + GUT;
    double dur = p.duration();
    float fitpps = std::max(1.0f, (canvasW - GUT - 12.0f) / (float)(dur + 0.5));   // auto-fit the WHOLE project (low floor so long cuts fit, not just ~70s)
    float pps = st.tlZoom > 0.0 ? (float)st.tlZoom : fitpps;   // 0 = auto-fit
    if (st.tlScroll < 0) st.tlScroll = 0;
    auto T2X = [&](double t) { return trackX + (float)((t - st.tlScroll) * pps); };

    // Scrub hit-area FIRST (before any clip item): overlapping ImGui items give the click to the
    // first one submitted, so with the lanes scrolled down a clip lying under the sticky ruler
    // strip used to eat the scrub click. The strip's VISUALS still draw at the end (on top).
    {
        float stickyY = origin.y + ImGui::GetScrollY();
        ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(trackX, stickyY));
        ImGui::InvisibleButton("##ruler", ImVec2(std::max(canvasW - GUT, 10.0f), RULER));
        if (ImGui::IsItemActive()) {
            st.playhead = std::max(0.0, std::min(dur, st.tlScroll + (double)((ImGui::GetIO().MousePos.x - trackX) / pps)));
            st.scrubbed = true;   // so the transport reseeks the audio mixer if it's playing
        }
        ImGui::SetCursorScreenPos(keep);
    }

    // Full content height (ALL lanes) — vertical extents + the scroll region must cover lanes
    // you scroll to, not just the visible viewport. (The child scrolls vertically; sizing the
    // clip clip-rect/gridlines/playhead to the viewport height anchored at the scrolled origin
    // cut off the bottom lanes' clips by the scroll amount → they rendered empty.)
    int nLanes = 0;
    for (auto& tk : p.tracks) for (auto& rid : tk.rows) if (p.rows.count(rid)) nLanes++;
    float contentH = RULER + nLanes * ROWH;
    float vextH = std::max(canvasH, contentH);   // draw + scroll extent (≥ viewport)
    float yBot = origin.y + vextH;               // bottom of all vertical spans

    // ── ripple helpers (resize ripples by default; Shift+move ripples) ──
    auto shiftClip = [](Clip& c, double d) {            // move a clip + its (absolute) keyframe times
        c.start += d;
        for (auto& kp : c.keyframes) for (auto& k : kp.second) k.t += d;
    };
    auto rippleFrom = [&](double pivot, double d, const std::string& except, bool after) {
        for (auto& kv : p.clips) {                       // after: clips at/after pivot · else: clips at/before pivot
            if (kv.first == except) continue;
            double s = kv.second.start;
            if (after ? (s >= pivot - 1e-6) : (s <= pivot + 1e-6)) shiftClip(kv.second, d);
        }
    };

    dl->AddRectFilled(origin, ImVec2(origin.x + canvasW, yBot), IM_COL32(24, 24, 28, 255));

    // Zoom-adaptive tick ladder: pick the smallest "nice" step whose labels sit ≥ ~72px apart,
    // subdivide with minor ticks when there's room. (A fixed 1s grid drew a line every ~2px at
    // fit-zoom on a long cut and the labels overlapped into an illegible smear.)
    static const double LADDER[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200};
    double tickMajor = LADDER[(sizeof LADDER / sizeof *LADDER) - 1];
    for (double s : LADDER) if (s * pps >= 72.0) { tickMajor = s; break; }
    int tickSub = (tickMajor == 15 || tickMajor == 30 || tickMajor == 60) ? 3 : 5;  // minors per major
    double tickMinor = tickMajor / tickSub;
    bool showMinor = tickMinor * pps >= 9.0;
    auto fmt_tick = [](double t, double step, char* b, size_t n) {   // 90 → "1:30"; sub-second steps keep a decimal
        if (t >= 60.0) { int m = (int)(t / 60.0); double sec = t - m * 60;
                         if (step >= 1.0) snprintf(b, n, "%d:%02d", m, (int)(sec + 0.5));
                         else snprintf(b, n, "%d:%04.1f", m, sec); }
        else if (step >= 1.0) snprintf(b, n, "%ds", (int)(t + 0.5));
        else snprintf(b, n, "%.4gs", t);
    };
    for (long i = (long)std::floor(st.tlScroll / tickMinor); i * tickMinor <= dur + tickMajor; ++i) {
        double s = i * tickMinor;
        float x = T2X(s);
        if (x < trackX) continue;                       // scrolled off the left
        if (x > origin.x + canvasW) break;
        bool maj = (i % tickSub) == 0;
        if (!maj && !showMinor) continue;
        dl->AddLine(ImVec2(x, origin.y + RULER), ImVec2(x, yBot),   // lane gridlines; ruler ticks/labels live in the sticky header
                    maj ? IM_COL32(48, 48, 56, 255) : IM_COL32(36, 36, 43, 255));
    }

    float laneTop = origin.y + RULER;
    double snapGuideT = -1e30; const double SNAP_PX = 8.0;   // edge-snap while dragging (Alt bypasses); guide line
    int lane = 0;
    std::vector<std::pair<std::string, float>> laneY;
    std::map<std::string, bool> rowAudio;
    std::string moveTrack; int moveDir = 0;   // deferred track reorder (changes draw/z-order)
    std::string deleteTrack;                   // deferred track delete (mutates membership → after the loop)
    std::string dragTrackId; float dragMouseY = 0;                 // a track dragged vertically → live-reorder p.tracks
    struct TrackRange { std::string id; float yTop, yBot; };       // for the drop target
    std::vector<TrackRange> trackRanges;
    for (auto& tk : p.tracks) {
        bool firstRow = true;
        float trackTop = laneTop + lane * ROWH;
        int trackRowN = 0; for (auto& rid : tk.rows) if (p.rows.count(rid)) trackRowN++;
        float trackH = std::max(1, trackRowN) * ROWH;
        bool handleHot = false;
        {   // DRAG the track's gutter (left of the reorder buttons) up/down to reorder it live
            ImGui::PushID(("thandle" + tk.id).c_str());
            ImGui::SetCursorScreenPos(ImVec2(origin.x, trackTop));
            ImGui::InvisibleButton("##th", ImVec2(std::max(24.f, (trackX - 58.f) - origin.x), std::max(ROWH - 2.f, trackH - 2.f)));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { dragTrackId = tk.id; dragMouseY = ImGui::GetIO().MousePos.y; }
            handleHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (handleHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            ImGui::PopID();
        }
        for (auto& rid : tk.rows) {
            auto it = p.rows.find(rid);
            if (it == p.rows.end()) continue;
            float ly = laneTop + lane * ROWH;
            dl->AddRectFilled(ImVec2(origin.x, ly), ImVec2(origin.x + canvasW, ly + ROWH - 2),
                              (lane % 2) ? IM_COL32(30, 30, 36, 255) : IM_COL32(34, 34, 40, 255));
            dl->AddRectFilled(ImVec2(origin.x, ly), ImVec2(trackX - 2, ly + ROWH - 2), IM_COL32(42, 42, 50, 255));
            std::string lab = it->second.name + "  [" + it->second.type + "]";
            dl->AddText(ImVec2(origin.x + 6, ly + 7), IM_COL32(212, 212, 222, 255), lab.c_str());
            if (firstRow) {   // ▲▼ reorder (higher = drawn on top) · ✕ delete the whole track
                ImGui::PushID(("trk" + tk.id).c_str());
                // compact padding + right-anchored inside the gutter so the buttons never spill into the
                // clip area (the roomier global FramePadding would push 3 buttons past trackX).
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
                bool canDel = p.tracks.size() > 1;
                ImGui::SetCursorScreenPos(ImVec2(trackX - (canDel ? 56.f : 38.f), ly + 4));
                if (ImGui::SmallButton("^")) { moveTrack = tk.id; moveDir = -1; }
                ImGui::SameLine(0, 2);
                if (ImGui::SmallButton("v")) { moveTrack = tk.id; moveDir = 1; }
                if (canDel) {   // never delete the last track
                    ImGui::SameLine(0, 2);
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(122, 42, 46, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(184, 58, 62, 255));
                    if (ImGui::SmallButton("x")) deleteTrack = tk.id;
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("delete this track — its rows + all their clips");
                }
                ImGui::PopStyleVar(2);
                ImGui::PopID();
                firstRow = false;
            }
            laneY.push_back({rid, ly});
            rowAudio[rid] = (tk.kind == "audio");
            lane++;
        }
        if (handleHot)   // drag tint OVER the gutter (after the row bg/labels so it reads)
            dl->AddRectFilled(ImVec2(origin.x, trackTop), ImVec2(trackX - 2, trackTop + trackH - 2), IM_COL32(0x88, 0x78, 0xd0, 45));
        trackRanges.push_back({tk.id, trackTop, trackTop + trackH});
    }
    if (!dragTrackId.empty() && trackRanges.size() > 1) {   // live vertical reorder to the cursor's slot
        int from = -1, ins = (int)trackRanges.size();
        for (int k = 0; k < (int)trackRanges.size(); k++) if (trackRanges[k].id == dragTrackId) { from = k; break; }
        for (int k = 0; k < (int)trackRanges.size(); k++) { float mid = (trackRanges[k].yTop + trackRanges[k].yBot) * 0.5f; if (dragMouseY < mid) { ins = k; break; } }
        if (from >= 0 && ins != from && ins != from + 1) {
            Track moved = p.tracks[from];
            p.tracks.erase(p.tracks.begin() + from);
            if (ins > from) ins--;
            p.tracks.insert(p.tracks.begin() + ins, moved);
        }
    }
    if (!moveTrack.empty()) {
        int i = -1; for (int k = 0; k < (int)p.tracks.size(); k++) if (p.tracks[k].id == moveTrack) i = k;
        int j = i + moveDir;
        if (i >= 0 && j >= 0 && j < (int)p.tracks.size()) std::swap(p.tracks[i], p.tracks[j]);
    }

    // Placement catcher — submitted BEFORE the clips so ADD MODE always adds (first-submitted wins, so a
    // click on an existing clip adds a new one instead of selecting/moving it). Preview + commit happen
    // after the clip loop (below) using this captured state, so the guide draws on top.
    bool placeActivated = false, placeActive = false, placeDeact = false, placeHover = false;
    if (!g_placeType.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(trackX, laneTop));
        ImGui::InvisibleButton("##place", ImVec2(std::max(1.f, origin.x + canvasW - trackX), std::max(1.f, yBot - laneTop)));
        placeActivated = ImGui::IsItemActivated(); placeActive = ImGui::IsItemActive();
        placeDeact = ImGui::IsItemDeactivated(); placeHover = ImGui::IsItemHovered();
    }

    dl->PushClipRect(ImVec2(trackX, laneTop), ImVec2(origin.x + canvasW, yBot), true);  // clips never overdraw the label gutter; yBot (full content) so scrolled-to lanes aren't clipped away
    std::string rowMoveClip, rowMoveTo; float rowMoveY = 0;   // a clip dragged onto another same-type lane (committed after the loop)
    // edge-snap targets = the moving clip's OWN lane + the two ADJACENT lanes (owner: not the whole timeline)
    auto snapRowsFor = [&](const std::string& row) {
        std::set<std::string> s; s.insert(row);
        for (size_t li = 0; li < laneY.size(); li++) if (laneY[li].first == row) {
            if (li > 0) s.insert(laneY[li - 1].first);
            if (li + 1 < laneY.size()) s.insert(laneY[li + 1].first);
            break;
        }
        return s;
    };
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        float ly = -1;
        for (auto& pr : laneY) if (pr.first == c.row) { ly = pr.second; break; }
        if (ly < 0) continue;
        float x0 = T2X(c.start), x1 = T2X(c.start + c.dur);
        if (x1 < trackX - 2 || x0 > origin.x + canvasW + 2) continue;   // fully off-screen (zoom/scroll) → cull
        ImVec2 a(x0 + 1, ly + 2), b(std::max(x1 - 1, x0 + 4), ly + ROWH - 4);
        const float HANDLE = 6.0f;
        const double dtPerPx = 1.0 / pps;
        const double MINDUR = 0.05;
        bool wide = (b.x - a.x) > HANDLE * 3;   // room for edge trim handles?
        bool hovered = false;

        // volume-envelope breakpoint handles — submitted BEFORE the edge/body buttons so a press GRABS the
        // point, not the clip/trim (ImGui 1.91 gives an overlapping press to the first-submitted item).
        // Only the SELECTED audio clip with a ramp; the envelope LINE draws later, over the clip fill. The
        // handles are SMALL (10px on each dot), so at an endpoint the full-height trim handle stays grabbable
        // above/below the dot — you get BOTH the point (at its level) and the trim.
        {
            auto raH = rowAudio.find(c.row);
            std::vector<KF>* env = (raH != rowAudio.end() && raH->second && st.selected == c.id) ? gain_env(c) : nullptr;
            if (env && env->size() >= 2) {
                auto edb2y = [&](double db) { return a.y + gainenv_db2f(db) * (b.y - a.y); };
                for (size_t i = 0; i < env->size(); ++i) {
                    ImVec2 np(T2X((*env)[i].t), edb2y((*env)[i].v[0]));
                    if (np.x < a.x - 8 || np.x > b.x + 8) continue;
                    ImGui::SetCursorScreenPos(ImVec2(np.x - 5, np.y - 5));
                    ImGui::PushID(("ge_" + c.id).c_str()); ImGui::PushID((int)i);
                    ImGui::InvisibleButton("##ge", ImVec2(10, 10));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    if (ImGui::IsItemActive()) {
                        st.selected = c.id;
                        ImVec2 m = ImGui::GetIO().MousePos;
                        double ndb = std::max((double)GAINENV_BOT, std::min((double)GAINENV_TOP, gainenv_f2db((m.y - a.y) / std::max(1.0f, b.y - a.y))));
                        double nt = (*env)[i].t;
                        if (i != 0 && i + 1 != env->size()) {   // endpoints keep t=start/end; interior slides between neighbors
                            nt = st.tlScroll + (double)((m.x - trackX) / pps);
                            nt = std::max((*env)[i - 1].t + 1e-3, std::min((*env)[i + 1].t - 1e-3, std::max(c.start, std::min(c.start + c.dur, nt))));
                        }
                        (*env)[i].t = nt; (*env)[i].v[0] = ndb;
                        ImGui::SetTooltip("%+.1f dB", ndb);
                    }
                    ImGui::PopID(); ImGui::PopID();
                }
            }
        }
        // left/right edge handles — drag to TRIM (left moves start+dur, right moves dur).
        if (wide) {
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton((c.id + "##L").c_str(), ImVec2(HANDLE, b.y - a.y));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive()) {                  // LEFT edge: TRIM (right edge fixed; no ripple)
                st.selected = c.id;
                double d = ImGui::GetIO().MouseDelta.x * dtPerPx;
                if (c.start + d < 0) d = -c.start;        // clamp start at 0
                if (!ImGui::GetIO().KeyAlt) {             // snap the moved START edge to a nearby clip edge (Alt = free)
                    double ns = snap_edge_time(p, c.id, c.start + d, SNAP_PX * dtPerPx, st.playhead, snapRowsFor(c.row));
                    if (ns != c.start + d) { d = ns - c.start; snapGuideT = ns; }
                }
                // Timed media trims its SOURCE in-point too, so the content at any surviving
                // timeline moment doesn't move (rate/speed-aware). The drag stops where the
                // source starts. Ctrl = SLIP instead (move the window, keep the in-point).
                double k = ImGui::GetIO().KeyCtrl ? 0.0 : src_time_factor(p, c.row, c.type, c.params);
                double in0 = c.params.is_object() ? c.params.value("in", 0.0) : 0.0;
                if (k > 1e-6 && in0 + d * k < 0) d = -in0 / k;   // no media before the source start
                double nd = c.dur - d;
                if (nd >= MINDUR) {
                    if (k > 1e-6) {
                        if (!c.params.is_object()) c.params = json::object();
                        c.params["in"] = advance_in_folded(p, c.asset, c.params, in0, d * k);
                    }
                    c.start += d; c.dur = nd; reanchor_leading_fade(c, d);   // fade-in follows the moved start
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(b.x - HANDLE, a.y));
            ImGui::InvisibleButton((c.id + "##R").c_str(), ImVec2(HANDLE, b.y - a.y));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive()) {                  // RIGHT edge: trim dur + ripple the after-block (Shift = no ripple)
                st.selected = c.id;
                double d = ImGui::GetIO().MouseDelta.x * dtPerPx;
                if (!ImGui::GetIO().KeyAlt) {             // snap the moved END edge to a nearby clip edge (Alt = free)
                    double ne = snap_edge_time(p, c.id, c.start + c.dur + d, SNAP_PX * dtPerPx, st.playhead, snapRowsFor(c.row));
                    if (ne != c.start + c.dur + d) { d = ne - (c.start + c.dur); snapGuideT = ne; }
                }
                double nd = c.dur + d;
                if (nd >= MINDUR) {
                    double endBefore = c.start + c.dur;
                    c.dur = nd;
                    reanchor_trailing_fade(c, d);   // fade-out follows the moved end (extend → no stranded stale fade)
                    if (!ImGui::GetIO().KeyShift) rippleFrom(endBefore, d, c.id, true);  // hold Shift to resize WITHOUT rippling
                }
            }
        }
        // SFX-cue handle: a draggable flag at the cue's effective time (clip.start + sfx_at). Submitted
        // BEFORE the body button so dragging RETIMES the cue instead of moving the clip. Any clip type.
        if (c.params.is_object() && c.params.contains("sfx_cue") && c.params["sfx_cue"].is_string()
            && !c.params["sfx_cue"].get<std::string>().empty()) {
            float sx = T2X(c.start + c.params.value("sfx_at", 0.0));
            if (sx >= a.x - 6 && sx <= b.x + 6) {
                ImGui::SetCursorScreenPos(ImVec2(sx - 5, a.y));
                ImGui::PushID(("sfx_" + c.id).c_str());
                ImGui::InvisibleButton("##sfxh", ImVec2(10, b.y - a.y));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsItemActive()) {
                    st.selected = c.id;
                    double nsat = std::max(0.0, std::min(c.dur, c.params.value("sfx_at", 0.0) + ImGui::GetIO().MouseDelta.x * dtPerPx));
                    c.params["sfx_at"] = nsat;
                    ImGui::SetTooltip("%s cue  @ %.2fs  (%.2fs abs)", c.params["sfx_cue"].get<std::string>().c_str(), nsat, c.start + nsat);
                }
                ImGui::PopID();
            }
        }
        // body — click to select, drag to MOVE in time.
        ImVec2 bodyA = wide ? ImVec2(a.x + HANDLE, a.y) : a;
        ImVec2 bodyB = wide ? ImVec2(b.x - HANDLE, b.y) : b;
        bodyA.x = std::max(bodyA.x, trackX);   // keep the hit-box out of the label gutter (scrolled clips)
        ImGui::SetCursorScreenPos(bodyA);
        ImGui::InvisibleButton((c.id + "##clip").c_str(), ImVec2(std::max(bodyB.x - bodyA.x, 4.0f), bodyB.y - bodyA.y));
        if (ImGui::IsItemHovered()) hovered = true;
        if (ImGui::IsItemClicked()) st.selected = c.id;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            st.selected = c.id;
            ImGuiIO& io = ImGui::GetIO();
            // Ctrl+drag = duplicate: spawn a copy at the clip's current spot ONCE, then drag the
            // original away (deferred so p isn't rebuilt mid-loop; the drag survives via the stable id).
            if (io.KeyCtrl && !io.KeyShift && g_ctrlDupDragId != c.id && g_dragDupReq.empty()) {
                g_dragDupReq = c.id; g_dupAtStart = c.start; g_ctrlDupDragId = c.id;
            }
            double d = io.MouseDelta.x * dtPerPx;
            if (io.KeyShift) {                            // Shift = ripple a BLOCK with the clip: clips AFTER (default), Alt = clips BEFORE
                bool before = io.KeyAlt;
                double pivot = c.start;                    // this clip's start before the move
                double mn = c.start;                       // clamp the whole moving block at t=0 (no rebase jump)
                for (auto& kv2 : p.clips) { if (kv2.first == c.id) continue; double s = kv2.second.start;
                    if (before ? (s <= pivot + 1e-6) : (s >= pivot - 1e-6)) mn = std::min(mn, s); }
                if (mn + d < 0) d = -mn;
                shiftClip(c, d);
                for (auto& kv2 : p.clips) { if (kv2.first == c.id) continue; double s = kv2.second.start;
                    if (before ? (s <= pivot + 1e-6) : (s >= pivot - 1e-6)) shiftClip(kv2.second, d); }
            } else {                                      // plain move (no ripple); keyframes follow the clip
                double eff = d; if (c.start + eff < 0) eff = -c.start;  // clamp at t=0
                if (!io.KeyAlt) {                         // snap the START or END edge (whichever is closer) to a nearby clip edge
                    double tol = SNAP_PX * dtPerPx;
                    std::set<std::string> sr = snapRowsFor(c.row);
                    double ns = snap_edge_time(p, c.id, c.start + eff, tol, st.playhead, sr);
                    double ne = snap_edge_time(p, c.id, c.start + c.dur + eff, tol, st.playhead, sr);
                    double cs = ns - (c.start + eff), ce = ne - (c.start + c.dur + eff);
                    if (cs != 0.0 && (ce == 0.0 || std::fabs(cs) <= std::fabs(ce))) { eff += cs; snapGuideT = ns; }
                    else if (ce != 0.0) { eff += ce; snapGuideT = ne; }
                }
                shiftClip(c, eff);
                // vertical drag onto another lane of the SAME type → move the clip to that row
                // (committed after the loop so we don't reshuffle membership mid-iteration).
                float my = io.MousePos.y;
                for (auto& pr : laneY)
                    if (my >= pr.second && my < pr.second + ROWH) {
                        auto tr = p.rows.find(pr.first);
                        if (tr != p.rows.end() && pr.first != c.row && tr->second.type == c.type) {
                            rowMoveClip = c.id; rowMoveTo = pr.first; rowMoveY = pr.second;
                        }
                        break;
                    }
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        ImU32 col = (st.selected == c.id) ? IM_COL32(255, 255, 255, 255) : type_color(c.type);
        dl->AddRectFilled(a, b, col, 4.0f);
        if (hovered) dl->AddRect(a, b, IM_COL32(255, 255, 255, 200), 4.0f, 0, 2.0f);
        if (wide && st.selected == c.id) {  // subtle grips on the selected clip's trim edges
            dl->AddRectFilled(a, ImVec2(a.x + HANDLE, b.y), IM_COL32(20, 20, 28, 150), 4.0f);
            dl->AddRectFilled(ImVec2(b.x - HANDLE, a.y), b, IM_COL32(20, 20, 28, 150), 4.0f);
        }
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 4, a.y + 4), IM_COL32(12, 12, 16, 255), c.label.c_str());
        dl->PopClipRect();

        // generation badge: a dot while a job is running for this clip, red on error
        auto gi = gen.find(c.id);
        if (gi != gen.end() && (gi->second.state == 1 || gi->second.state == 3)) {
            ImU32 mc = gi->second.state == 3 ? IM_COL32(240, 80, 80, 255) : IM_COL32(255, 210, 90, 255);
            dl->AddCircleFilled(ImVec2(b.x - 6, a.y + 6), 3.5f, mc);
        }

        // SFX-cue marker: an orange flag on the clip at the cue's effective time (drag it to retime;
        // was invisible before — a boom/awkward cue lives only in params). Label shows the cue name.
        if (c.params.is_object() && c.params.contains("sfx_cue") && c.params["sfx_cue"].is_string()
            && !c.params["sfx_cue"].get<std::string>().empty()) {
            float sx = T2X(c.start + c.params.value("sfx_at", 0.0));
            if (sx >= a.x - 6 && sx <= b.x + 6) {
                ImU32 sc = IM_COL32(255, 146, 54, 255);
                dl->AddLine(ImVec2(sx, a.y + 1), ImVec2(sx, b.y - 1), sc, 1.6f);
                dl->AddTriangleFilled(ImVec2(sx - 4, a.y + 1), ImVec2(sx + 4, a.y + 1), ImVec2(sx, a.y + 7), sc);
                dl->AddText(ImVec2(sx + 5, a.y + 1), sc, c.params["sfx_cue"].get<std::string>().c_str());
            }
        }

        // audio clips: draw the waveform envelope of their asset inside the clip
        auto ra = rowAudio.find(c.row);
        if (ra != rowAudio.end() && ra->second && !c.asset.empty()) {
            std::string uri;
            auto au = p.asset_uri.find(c.asset);
            if (au != p.asset_uri.end()) uri = au->second;
            Wave* wv = uri.empty() ? nullptr : get_wave(uri);
            if (wv && !wv->env.empty()) {
                float midY = (a.y + b.y) * 0.5f, halfH = (b.y - a.y) * 0.42f;
                int N = (int)wv->env.size();
                // Map each pixel to the AUDIO's real time (at `pps` px/s) starting from the clip's
                // in-point, NOT the clip width — so resizing dur doesn't stretch the waveform and a
                // split clip shows its slice of the asset (rect-clipped to the clip body). RATE-aware:
                // a sped-up clip (shorts tts ~1.3x) plays a stretched buffer, so px→source scales by rate
                // (else the drawn waveform is time-warped vs the sound — matches the mixer + bob now).
                double inOff = c.params.value("in", 0.0);
                double wrate = c.params.value("rate", (c.type == "tts") ? p.speechRate : 1.0);
                if (wrate < 0.5) wrate = 0.5; if (wrate > 2.0) wrate = 2.0;
                dl->PushClipRect(a, b, true);
                for (float xx = a.x; xx < b.x; xx += 1.0f) {
                    float at = (float)inOff + (xx - a.x) / pps * (float)wrate;   // played px → source asset time (s)
                    float frac = at / (float)wv->dur;
                    if (frac < 0.0f || frac >= 1.0f) continue;   // before in-point / past audio end
                    int bk = (int)(frac * N);
                    if (bk < 0 || bk >= N) continue;
                    float e = wv->env[bk] * halfH;
                    dl->AddLine(ImVec2(xx, midY - e), ImVec2(xx, midY + e), IM_COL32(20, 34, 46, 235));  // dark ink over the clip colour (readable on the pale tts blue)
                }
                dl->PopClipRect();
            }
            // volume ENVELOPE (the "slope" you tune) drawn over the waveform — only for clips that
            // carry a ramp (music beds via `slop.py bed --ramp` or the Inspector graph). Selecting the
            // clip turns the breakpoints into draggable handles (fine editing lives in the Inspector).
            auto* env = gain_env(c);
            if (env && env->size() >= 2) {
                const ImU32 EC = IM_COL32(250, 214, 90, 235);
                auto edb2y = [&](double db) { return a.y + gainenv_db2f(db) * (b.y - a.y); };
                dl->PushClipRect(a, b, true);
                dl->AddLine(ImVec2(a.x, edb2y(env->front().v[0])), ImVec2(T2X(env->front().t), edb2y(env->front().v[0])), IM_COL32(250, 214, 90, 140), 1.2f);
                for (size_t i = 0; i + 1 < env->size(); ++i)
                    dl->AddLine(ImVec2(T2X((*env)[i].t), edb2y((*env)[i].v[0])), ImVec2(T2X((*env)[i + 1].t), edb2y((*env)[i + 1].v[0])), EC, 1.6f);
                dl->AddLine(ImVec2(T2X(env->back().t), edb2y(env->back().v[0])), ImVec2(b.x, edb2y(env->back().v[0])), IM_COL32(250, 214, 90, 140), 1.2f);
                for (auto& k : *env) dl->AddCircleFilled(ImVec2(T2X(k.t), edb2y(k.v[0])), st.selected == c.id ? 3.0f : 2.0f, EC);
                dl->PopClipRect();   // (the draggable handles are submitted earlier, before the body button)
            }
        }
    }
    if (!rowMoveClip.empty()) {                  // dragging onto a new same-type lane: highlight it + commit
        dl->AddRect(ImVec2(trackX, rowMoveY + 1), ImVec2(origin.x + canvasW, rowMoveY + ROWH - 3),
                    IM_COL32(120, 220, 160, 230), 4.0f, 0, 2.5f);
        if (move_clip_to_row(p, rowMoveClip, rowMoveTo)) g_undoDirty = true;
    }

    dl->PopClipRect();

    // ── STICKY ruler/header: pinned to the viewport top so the ruler stays visible and the playhead
    // stays scrubbable even when the lanes are scrolled down (it used to scroll away with the content).
    // Drawn AFTER the clips + an opaque bg so lanes scrolling underneath don't bleed through. ──
    float stickyY = origin.y + ImGui::GetScrollY();   // origin.y is content-top (scrolls up); + scrollY → current viewport top (inside padding)
    ImVec2 wp = ImGui::GetWindowPos();                // true child top/left — the strip must also cover the window PADDING band,
    float px = T2X(st.playhead);                      // or scrolled lanes peek out in the sliver above the ruler
    if (snapGuideT > -1e29) {   // edge-snap guide: a cyan line at the clip edge we snapped to this frame
        float gx = T2X(snapGuideT);
        dl->AddLine(ImVec2(gx, stickyY + RULER), ImVec2(gx, yBot), IM_COL32(120, 220, 255, 230), 1.5f);
    }
    dl->AddLine(ImVec2(px, stickyY + RULER), ImVec2(px, yBot), IM_COL32(240, 70, 70, 255), 2.0f);  // playhead line spans the lanes
    dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + ImGui::GetWindowSize().x, stickyY + RULER), IM_COL32(24, 24, 28, 255));  // opaque ruler strip (from the window edge)
    dl->AddLine(ImVec2(origin.x, stickyY + RULER), ImVec2(origin.x + canvasW, stickyY + RULER), IM_COL32(48, 48, 56, 255));  // baseline
    if (ImGui::GetScrollY() > 0.5f)                   // scrolled: soft shadow under the header so cut-off lanes read as "under" it
        dl->AddRectFilledMultiColor(ImVec2(origin.x, stickyY + RULER + 1), ImVec2(origin.x + canvasW, stickyY + RULER + 9),
                                    IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    for (long i = (long)std::floor(st.tlScroll / tickMinor); i * tickMinor <= dur + tickMajor; ++i) {   // ruler ticks; labels on majors only
        double s = i * tickMinor;
        float x = T2X(s);
        if (x < trackX) continue;
        if (x > origin.x + canvasW) break;
        bool maj = (i % tickSub) == 0;
        if (maj) {
            char b[24]; fmt_tick(s, tickMajor, b, sizeof b);
            dl->AddLine(ImVec2(x, stickyY + RULER - 7), ImVec2(x, stickyY + RULER), IM_COL32(96, 96, 108, 255));
            dl->AddText(ImVec2(x + 4, stickyY + 3), IM_COL32(150, 150, 160, 255), b);
        } else if (showMinor) {
            dl->AddLine(ImVec2(x, stickyY + RULER - 4), ImVec2(x, stickyY + RULER), IM_COL32(64, 64, 74, 255));
        }
    }
    dl->AddTriangleFilled(ImVec2(px - 5, stickyY), ImVec2(px + 5, stickyY),   // playhead marker sits on the ruler
                          ImVec2(px, stickyY + 9), IM_COL32(240, 70, 70, 255));
    // (scrub hit-area is submitted at the TOP of DrawTimeline so clips under the sticky strip
    // can't steal its clicks; visuals-only here.)

    // wheel/middle-drag over the timeline: wheel = zoom (anchored on the PLAYHEAD) · Shift+wheel OR
    // middle-mouse-drag = horizontal pan · Ctrl+wheel = vertical lane scroll. The timeline child sets
    // NoScrollWithMouse (below), so a plain wheel drives ONLY the zoom — it used to zoom AND scroll the
    // lanes at the same time (the child ate the wheel too). Lanes now scroll via the scrollbar / Ctrl+wheel.
    // (AllowWhenBlockedByActiveItem so it works while hovering a clip too.)
    ImGuiIO& io = ImGui::GetIO();
    bool tlHov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (tlHov && io.MouseWheel != 0.0f && !ImGui::IsAnyItemActive()) {
        if (io.KeyCtrl) {                                  // Ctrl = scroll the lanes vertically (wheel is otherwise zoom)
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * ROWH * 1.5f);
        } else if (io.KeyShift) {                          // Shift = horizontal pan
            st.tlScroll -= (double)io.MouseWheel * (240.0 / pps);
        } else {                                           // zoom, keeping the playhead pinned to its screen-x
            float sx = trackX + (float)((st.playhead - st.tlScroll) * pps);
            double base = st.tlZoom > 0 ? st.tlZoom : fitpps;
            double nz = base * (io.MouseWheel > 0 ? 1.18 : 1.0 / 1.18);
            st.tlZoom = (nz <= fitpps + 0.001) ? 0.0 : std::min(nz, 4000.0);  // floor = auto-fit; cap zoom-in
            double newpps = st.tlZoom > 0 ? st.tlZoom : fitpps;
            st.tlScroll = st.playhead - (sx - trackX) / newpps;
        }
    }
    if (tlHov && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {   // middle-drag = pan: horizontal time (when zoomed) + vertical lanes
        if (st.tlZoom > 0) st.tlScroll -= io.MouseDelta.x / pps;
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }
    // clamp: auto-fit always starts at 0; when zoomed, keep t=0 reachable and don't over-scroll past the end.
    if (st.tlZoom <= 0) st.tlScroll = 0.0;
    else { double cpps = st.tlZoom, vis = (canvasW - GUT) / cpps;
           st.tlScroll = std::max(0.0, std::min(st.tlScroll, std::max(0.0, dur - vis * 0.5))); }

    // overview readout (zoom % + window) — handy when zoomed in past the panel
    if (st.tlZoom > 0) {
        char ov[64]; snprintf(ov, sizeof ov, "%.0f%%  %.1f-%.1fs", pps / fitpps * 100.0,
                              st.tlScroll, st.tlScroll + (canvasW - GUT) / pps);
        dl->AddText(ImVec2(origin.x + GUT + 6, origin.y + canvasH - 16), IM_COL32(150, 150, 160, 255), ov);
    }

    // consume a dropped image file: drop it onto an image lane → add an image clip at that time
    if (g_hasDrop && !g_dropPath.empty()) {
        float dx = (float)g_dropPt.x, dy = (float)g_dropPt.y;
        if (dx >= trackX && dx <= origin.x + canvasW && dy >= laneTop && dy <= yBot) {
            double t = std::max(0.0, st.tlScroll + (dx - trackX) / pps);
            std::string row;
            for (auto& pr : laneY) if (dy >= pr.second && dy < pr.second + ROWH) { row = pr.first; break; }
            // A timeline drop IMPORTS into the project library first (same as the Media pane's
            // "Add files...") and the clip references the managed copy — an external original at
            // F:\wherever never enters the project file, and the asset shows up in the library
            // (it used to bypass the library entirely: "dragged it in, it's not in the Media pane").
            if (std::string imp = library_import(g_dropPath); !imp.empty()) g_dropPath = imp;
            LibType dt = lib_type_of(ext_lower(g_dropPath));          // route by the dropped file's kind
            if (dt == LIB_VIDEO) {                                    // mp4/mov/… → in-process decode (no proxy)
                if (row.empty() || p.rows[row].type != "video") row = find_or_create_video_row(p);
                if (!row.empty() && p.rows[row].type == "video") st.selected = add_video_clip_at(p, row, t, g_dropPath);
            } else if (dt == LIB_AUDIO) {
                if (row.empty() || p.rows[row].type != "music")
                    for (auto& kv : p.rows) if (kv.second.type == "music") { row = kv.first; break; }
                if (!row.empty() && p.rows[row].type == "music") st.selected = add_audio_clip_at(p, row, t, g_dropPath);
            } else {
                if (row.empty() || p.rows[row].type != "image")      // not an image lane → use the first image row
                    for (auto& kv : p.rows) if (kv.second.type == "image") { row = kv.first; break; }
                if (!row.empty() && p.rows[row].type == "image") st.selected = add_image_clip_at(p, row, t, g_dropPath);
            }
        } else
            library_import(g_dropPath);   // dropped outside the timeline (Media pane etc.) → import-only
        g_hasDrop = false;   // consume the drop
        g_undoDirty = true;  // OS drop is outside ImGui → flag it for the undo checkpoint
    }

    // ── placement tool: with a type armed (palette), click empty lane space to add that clip at the
    //    click time; DRAG to draw its length. Submitted after the clips so a clip still wins its own
    //    press (first-submitted wins overlaps); catches only the empty gaps. Commit is deferred. ──
    if (!g_placeType.empty()) {
        ImGuiIO& pio = ImGui::GetIO();
        auto tAt   = [&](float x) { double t = st.tlScroll + (x - trackX) / pps; return t < 0 ? 0.0 : t; };
        auto rowAt = [&](float y) -> std::string { for (auto& pr : laneY) if (y >= pr.second && y < pr.second + ROWH) return pr.first; return ""; };
        auto rowY  = [&](const std::string& r) -> float { for (auto& pr : laneY) if (pr.first == r) return pr.second; return -1.f; };
        static double placeT0 = -1e30; static std::string placeRow;
        bool active = placeActive;               // captured from the pre-clip catcher above
        if (placeActivated) { placeT0 = tAt(pio.MousePos.x); placeRow = rowAt(pio.MousePos.y); }
        bool generic = (g_placeType == "__generic__");
        if (active || placeHover) {   // live preview: WHICH lane will it land on (re-evaluated as it grows)?
            double a  = active ? std::min(placeT0, tAt(pio.MousePos.x)) : tAt(pio.MousePos.x);
            double bb = active ? std::max(placeT0, tAt(pio.MousePos.x)) : a;
            std::string clk = active ? placeRow : rowAt(pio.MousePos.y);
            std::string want = generic ? (p.rows.count(clk) ? p.rows[clk].type : std::string()) : std::string();
            if (!generic || !want.empty()) {   // generic needs a real lane under the cursor
                double durP = (bb - a > 0.15) ? (bb - a) : place_default_dur(generic ? want : g_placeType);
                std::string tgt = generic ? choose_row_of_type(p, want, clk, a, durP) : choose_place_row(p, g_placeType, clk, a, durP);
                bool newLane = tgt.empty();
                float py = rowY(tgt);
                if (py < 0) { py = rowY(clk); if (py < 0 && !laneY.empty()) py = laneY.back().second; }   // new lane rides the cursor lane
                float x0 = T2X(a), x1 = T2X(std::max(bb, a + 0.02));
                ImU32 fill = newLane ? IM_COL32(0x8a, 0x84, 0xb0, 70) : IM_COL32(0x7f, 0xd8, 0xa8, 70);
                ImU32 line = newLane ? IM_COL32(0x8a, 0x84, 0xb0, 235) : IM_COL32(0x7f, 0xd8, 0xa8, 235);
                if (py >= 0) {
                    dl->AddRectFilled(ImVec2(x0, py + 2), ImVec2(x1, py + ROWH - 3), fill, 4.f);
                    dl->AddRect(ImVec2(x0, py + 2), ImVec2(x1, py + ROWH - 3), line, 4.f, 0, 1.5f);
                    if (newLane) dl->AddText(ImVec2(x0 + 5, py + 6), IM_COL32(0xe8, 0xe4, 0xff, 255), "+ new track");
                }
            }
        }
        if (placeDeact && placeT0 > -1e29) {   // release → commit (deferred: p may add a track)
            double a = std::min(placeT0, tAt(pio.MousePos.x)), b = std::max(placeT0, tAt(pio.MousePos.x));
            if (!generic || p.rows.count(placeRow)) {   // generic needs a real clicked lane
                g_placeReq = g_placeType; g_placeReqT = a; g_placeReqDur = (b - a > 0.15) ? (b - a) : -1.0; g_placeReqRow = placeRow;
            }
            placeT0 = -1e30;
        }
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(canvasW, vextH));   // reserve the FULL lane height so the child scrolls to every lane

    // drop a Library item onto a lane → add a clip at that time (image now; audio/video in L3)
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("LIB_ITEM")) {
            std::string path((const char*)pl->Data);
            ImVec2 m = ImGui::GetMousePos();
            double t = std::max(0.0, st.tlScroll + (m.x - trackX) / pps);
            std::string row;
            for (auto& pr : laneY) if (m.y >= pr.second && m.y < pr.second + ROWH) { row = pr.first; break; }
            size_t ael = path.size(), rel = strlen(AVATAR_RIG_EXT);
            std::string rigNm;                                              // a rig def → place an avatar clip
            if (ael > rel && path.compare(ael - rel, rel, AVATAR_RIG_EXT) == 0) {            // …/<name>.avatar.json
                rigNm = path.substr(0, ael - rel);
            } else if (ael > 14 && path.compare(ael - 14, 14, "/manifest.json") == 0) {      // presets/avatars/<name>/manifest.json
                rigNm = path.substr(0, ael - 14);
            }
            if (!rigNm.empty()) { size_t sl = rigNm.find_last_of("/\\"); if (sl != std::string::npos) rigNm = rigNm.substr(sl + 1); }
            LibType lt = lib_type_of(ext_lower(path));
            if (!rigNm.empty()) {
                st.selected = add_avatar_clip_at(p, rigNm, t);
            } else if (lt == LIB_IMAGE) {
                if (row.empty() || p.rows[row].type != "image")
                    for (auto& kv : p.rows) if (kv.second.type == "image") { row = kv.first; break; }
                if (!row.empty() && p.rows[row].type == "image") st.selected = add_image_clip_at(p, row, t, path);
            } else if (lt == LIB_AUDIO) {
                if (row.empty() || p.rows[row].type != "music")
                    for (auto& kv : p.rows) if (kv.second.type == "music") { row = kv.first; break; }
                if (!row.empty() && p.rows[row].type == "music") st.selected = add_audio_clip_at(p, row, t, path);
            } else if (lt == LIB_VIDEO) {  // in-process decode → drop just works (no proxy)
                if (row.empty() || p.rows[row].type != "video") row = find_or_create_video_row(p);
                if (!row.empty() && p.rows[row].type == "video") st.selected = add_video_clip_at(p, row, t, path);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // deferred TRACK DELETE — applied LAST: delete_track rebuilds p, but this frame already drew with the
    // old p (and every ImGui item is submitted), so there's no mid-frame glitch; next frame redraws clean.
    if (!deleteTrack.empty()) { delete_track(p, deleteTrack); st.selected.clear(); g_undoDirty = true; }
}

// Editable-param buffers for the selected clip (refreshed when the selection or the
// underlying asset changes; g_bufFor is also cleared by apply_generations after reload).
static std::string g_bufFor;

// Transform clipboard (Clip menu / Ctrl+Shift+C/V): carries one clip's static transform to
// another. In-memory only — placement, not content; keyframes stay with their clip.
struct TxClipboard { bool has = false; double pos[2], scale[2], rot, opacity, anchor[2]; };
static TxClipboard g_txClip;
static void copy_transform(const Clip& c) {
    g_txClip.has = true;
    g_txClip.pos[0] = c.tx_pos[0];       g_txClip.pos[1] = c.tx_pos[1];
    g_txClip.scale[0] = c.tx_scale[0];   g_txClip.scale[1] = c.tx_scale[1];
    g_txClip.rot = c.tx_rot;             g_txClip.opacity = c.tx_opacity;
    g_txClip.anchor[0] = c.tx_anchor[0]; g_txClip.anchor[1] = c.tx_anchor[1];
}
static void paste_transform(Clip& c) {
    if (!g_txClip.has) return;
    c.tx_pos[0] = g_txClip.pos[0];       c.tx_pos[1] = g_txClip.pos[1];
    c.tx_scale[0] = g_txClip.scale[0];   c.tx_scale[1] = g_txClip.scale[1];
    c.tx_rot = g_txClip.rot;             c.tx_opacity = g_txClip.opacity;
    c.tx_anchor[0] = g_txClip.anchor[0]; c.tx_anchor[1] = g_txClip.anchor[1];
}

// ───────────────────────────── undo / redo ────────────────────────────────
// Whole-project undo: the doc fully captures project state, so a snapshot is just a compact JSON
// dump of the synced doc, and undo = "restore a past dump." Checkpoints are taken AUTOMATICALLY at
// gesture boundaries — every frame undo_checkpoint() runs, but it only commits when the UI has
// SETTLED (no ImGui item active: no clip-drag, no slider, no held button) AND the doc actually
// changed since the last commit. That coalesces a 60-frame drag into ONE step. The discipline the
// user asked for is thus structural: a NEW feature needs no push_undo() — as long as it mutates
// p.clips/p.doc, the next settle captures it. The ONLY bookkeeping is g_undoDirty, a hint for edits
// that finish OUTSIDE the ImGui active-item model (async job landings, the S-key split, OS file
// drops) so Ctrl+Z right after them still has the step; even if one is missed, the change isn't
// lost — the next gesture's settle picks it up (possibly grouped). Snapshots are compact strings,
// so the big buffer the user wanted (UNDO_CAP) costs only ~tens of MB, not GBs of live json.
// (The globals g_undo/g_redo/g_undoBase/g_undoDirty/g_undoActivePrev/UNDO_CAP are declared up top.)
static void undo_init(Project& p) {               // call once the project is (re)loaded → fresh baseline
    sync_to_doc(p); g_undoBase = p.doc.dump();
    g_undo.clear(); g_redo.clear(); g_undoDirty = false; g_undoActivePrev = false;
}
// Commit a checkpoint iff the project changed since the last commit. Self-gates to gesture
// boundaries so a continuous drag is one step. `force` skips the gate (unused by undo/redo —
// undo_apply snapshots the live state itself). Cheap: the dump+compare runs only on a settle edge
// or when g_undoDirty is set, never mid-drag and never every idle frame.
static void undo_checkpoint(Project& p, bool force = false) {
    bool active = ImGui::IsAnyItemActive();
    bool settledEdge = (!active && g_undoActivePrev);   // a drag / slider / button just released
    g_undoActivePrev = active;
    if (!force) {
        if (active) return;                              // mid-gesture → never checkpoint (coalesce)
        if (!settledEdge && !g_undoDirty) return;        // idle, nothing ended/flagged → skip the dump
    }
    g_undoDirty = false;
    sync_to_doc(p);
    std::string cur = p.doc.dump();
    if (cur == g_undoBase) return;                       // no real change → no step
    g_undo.push_back(std::move(g_undoBase));
    if (g_undo.size() > UNDO_CAP) g_undo.pop_front();    // drop the oldest past state
    g_undoBase = std::move(cur);
    g_redo.clear();                                      // a new edit invalidates the redo branch
}
static void undo_apply(Project& p, std::deque<std::string>& from, std::deque<std::string>& to) {
    if (from.empty()) return;
    sync_to_doc(p);
    to.push_back(p.doc.dump());                          // live state → the opposite stack
    if (to.size() > UNDO_CAP) to.pop_front();
    std::string snap = std::move(from.back()); from.pop_back();
    g_undoBase = snap;                                   // the restored state is now the committed base
    std::string path = p.path;
    p = parse_project_json(json::parse(snap), path);     // doc → in-memory model (no disk I/O)
    g_undoActivePrev = false; g_undoDirty = false;
    g_bufFor.clear();                                    // refresh the inspector's edit buffers
}
static void do_undo(Project& p) { undo_apply(p, g_undo, g_redo); }
static void do_redo(Project& p) { undo_apply(p, g_redo, g_undo); }

static std::map<std::string, std::vector<std::string>> g_genHist;  // clip id → past gen asset hashes (this session)
static char g_textBuf[4096];  // text (tts) / prompt (image) / mood (music)
static char g_trBuf[4096];    // tts caption-display override (params.transcript)

// ImGui's InputTextMultiline doesn't word-wrap, so the TTS text box soft-wraps the edit buffer
// to the box width on (re)load, then strips those soft newlines back out before the text reaches
// the param/provider (a synthesized sentence shouldn't gain mid-line breaks).
static std::string wrap_text(const std::string& s, float wrapPx) {
    if (wrapPx < 40.f) wrapPx = 40.f;
    float spaceW = ImGui::CalcTextSize(" ").x;
    std::string out; float lineW = 0; bool first = true;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (s[i] == '\n') { out += '\n'; lineW = 0; first = true; i++; continue; }
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') { i++; continue; }
        size_t j = i; while (j < n && s[j] != ' ' && s[j] != '\n' && s[j] != '\t' && s[j] != '\r') j++;
        std::string word = s.substr(i, j - i);
        float ww = ImGui::CalcTextSize(word.c_str()).x;
        if (!first && lineW + spaceW + ww > wrapPx) { out += '\n'; lineW = 0; first = true; }
        if (!first) { out += ' '; lineW += spaceW; }
        out += word; lineW += ww; first = false; i = j;
    }
    return out;
}
static std::string unwrap_text(const std::string& s) {
    std::string r; bool sp = false;
    for (char ch : s) {
        char c2 = (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
        if (c2 == ' ') { if (!sp && !r.empty()) r += ' '; sp = true; }
        else { r += c2; sp = false; }
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}
static char g_emoBuf[512];    // emotion (tts)
static std::vector<std::string> g_voicePresets;  // presets/voices/*.json, refreshed on selection change

// Voice preset editor (a floating window): design/tweak a `voice` instruct, Preview it (one-off
// TTS synth, played), and Save it as a new presets/voices/<name>.json.
static bool g_showVoiceEditor = false;
static bool g_openAddTrack = false;   // request to open the "Add Track" modal (set from the menu)
static bool g_openRender   = false;   // request to open the "Render video" modal

// Spawn an arbitrary bash command in the repo (WSL side), in a console window. Shared by the
// render button and the first-run stock-assets fetch.
// The editor runs as a Windows PE under WSLInterop; its CWD is the WSL repo seen as a UNC path
// (\\wsl.localhost\<distro>\opt\src\slopstudio). wsl.exe --cd can't translate that UNC form, so map
// it back to the Linux path (/opt/src/slopstudio); a real drive path (C:\…) or a relative path is
// passed through unchanged.
static std::string win_path_to_wsl(const std::string& win) {
    for (const char* pfx : { "\\\\wsl.localhost\\", "\\\\wsl$\\" }) {
        std::string p(pfx);
        if (win.rfind(p, 0) == 0) {
            std::string rest = win.substr(p.size());                     // "<distro>\opt\src\…"
            size_t sl = rest.find('\\');
            std::string path = (sl == std::string::npos) ? std::string("/") : rest.substr(sl);
            for (char& c : path) if (c == '\\') c = '/';
            return path.empty() ? "/" : path;                            // "/opt/src/slopstudio"
        }
    }
    return win;
}

static bool spawn_wsl_bash(const std::string& inner) {
    char cwd[MAX_PATH] = {0}; GetCurrentDirectoryA(sizeof cwd, cwd);
    std::string wslcwd = win_path_to_wsl(cwd);
    // Guarantee `nix`/`bash` are on PATH even when the login profile didn't fully run (a WSL systemd
    // user-session hiccup leaves a bare PATH → "nix: command not found"). NixOS keeps them in
    // /run/current-system/sw/bin. `$PATH` is expanded by the launched bash, not by Windows.
    std::string full = "export PATH=\"/run/current-system/sw/bin:/usr/bin:/bin:$PATH\"; " + inner;
    std::string esc; for (char c : full) { if (c == '"') esc += "\\\""; else esc += c; }
    std::string cmd = "wsl.exe --cd \"" + wslcwd + "\" -e bash -lc \"" + esc + "\"";
    STARTUPINFOA si = {}; si.cb = sizeof si; PROCESS_INFORMATION pi = {};
    std::vector<char> mut(cmd.begin(), cmd.end()); mut.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, mut.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok;
}

// First run: the code checkout ships no art (the stock rig + backgrounds are a release asset).
// True once the pack is unpacked. Drives a one-time "fetch stock assets" prompt.
static bool stock_assets_ready() {
    FILE* f = fopen("presets/avatars/gemma-big/manifest.json", "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// Spawn the export pipeline (tools/export.sh) via WSL. The editor is a Windows PE running under
// WSLInterop; the render is a nix + bash + ffmpeg pipeline on the Linux side, so we launch it with
// wsl.exe --cd <windows-cwd> in its own console window (the user watches ffmpeg's progress there).
// Returns false if CreateProcess failed. `out` is written under exports/.
static bool spawn_render(const std::string& projPath, int targetMb, int fps, bool p1080, const std::string& out) {
    std::string scaleArg = p1080 ? " --scale 1080" : "";
    std::string proj = win_path_to_wsl(projPath);   // a UNC project path → its Linux path (relative paths pass through)
    // single-quote the project + out for the inner bash; export.sh lives in the repo (cwd).
    std::string inner = "nix develop --command bash tools/export.sh '" + proj + "'"
                        " --target-mb " + std::to_string(targetMb) + " --fps " + std::to_string(fps) + scaleArg +
                        " --out '" + out + "'; echo; echo '[render finished — press enter to close]'; read";
    return spawn_wsl_bash(inner);
}

// The paste-ready render command for the user's OWN WSL shell — the reliable path. Launching the
// pipeline via wsl.exe from this Windows-PE-under-WSLInterop is flaky (nested WSL breaks the
// systemd user session → chdir/PATH fail), so the modal copies this to the clipboard instead.
static std::string render_command(const std::string& projPath, int targetMb, int fps, bool p1080, const std::string& out) {
    char cwd[MAX_PATH] = {0}; GetCurrentDirectoryA(sizeof cwd, cwd);
    std::string scaleArg = p1080 ? " --scale 1080" : "";
    return "cd '" + win_path_to_wsl(cwd) + "' && nix develop --command bash tools/export.sh '" + win_path_to_wsl(projPath) + "'" +
           " --target-mb " + std::to_string(targetMb) + " --fps " + std::to_string(fps) + scaleArg + " --out '" + out + "'";
}

// A paste-ready `cd '<repo>' && <inner>` for any of the WSL-spawn helpers, so the user can run it in
// their own shell when the nested wsl.exe launch won't (same nested-WSL limitation as the render).
static std::string wsl_command(const std::string& inner) {
    char cwd[MAX_PATH] = {0}; GetCurrentDirectoryA(sizeof cwd, cwd);
    return "cd '" + win_path_to_wsl(cwd) + "' && " + inner;
}
static char g_veName[128]  = "gemma-san-new";
static char g_veVoice[4096] = "A ... young woman's voice - describe timbre, pace, mood here.";
static int  g_veSeed = 7;
static char g_veLang[64]   = "English";
static char g_veText[512]  = "Fufu~ welcome back. Today we break a twenty-year-old shop game.";
static std::string g_veStatus;
static std::string g_saveStatus;  // shown in the menu bar after a File ▸ Save / Ctrl+S

static void DrawVoiceEditor(Project& p) {
    if (!g_showVoiceEditor) return;
    ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Voice presets", &g_showVoiceEditor)) {
        if (g_voicePresets.empty()) g_voicePresets = list_voice_presets();
        if (ImGui::BeginCombo("load", "(pick to edit)")) {
            for (auto& vp : g_voicePresets)
                if (ImGui::Selectable(vp.c_str())) {
                    json pr = load_voice_preset(vp);
                    snprintf(g_veName, sizeof g_veName, "%s", vp.c_str());
                    snprintf(g_veVoice, sizeof g_veVoice, "%s", jstr(pr, "voice").c_str());
                    g_veSeed = pr.value("seed", 7);
                    snprintf(g_veLang, sizeof g_veLang, "%s", pr.contains("language") ? jstr(pr, "language").c_str() : "English");
                    g_veStatus = "loaded " + vp;
                }
            ImGui::EndCombo();
        }
        ImGui::InputText("name", g_veName, sizeof g_veName);
        ImGui::TextDisabled("voice instruct (defines the timbre; per-line emotion stays on the clip):");
        ImGui::InputTextMultiline("##voice", g_veVoice, sizeof g_veVoice, ImVec2(-1, 130));
        ImGui::InputInt("seed", &g_veSeed);
        ImGui::InputText("language", g_veLang, sizeof g_veLang);
        ImGui::Separator();
        ImGui::InputText("preview text", g_veText, sizeof g_veText);
        bool busy = g_previewBusy;
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Preview")) voice_preview(g_veVoice, g_veSeed, g_veLang, g_veText);
        ImGui::SameLine();
        if (ImGui::Button("Save preset")) {
            if (save_voice_preset(g_veName, g_veVoice, g_veSeed, g_veLang)) {
                g_voicePresets = list_voice_presets();
                g_bufFor.clear();  // so the clip inspector's voice combo re-lists
                g_veStatus = std::string("saved presets/voices/") + g_veName + ".json";
            } else {
                g_veStatus = "save failed - name must be non-empty alnum/-/_";
            }
        }
        ImGui::SameLine();
        // Bake the preview text as the GOLDEN REFERENCE → preset clones it (stable timbre).
        if (ImGui::Button("Bake golden ref")) voice_bake(g_veName, g_veVoice, g_veSeed, g_veLang, g_veText);
        if (busy) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Stop")) PlaySoundA(nullptr, nullptr, 0);
        if (g_voicesDirty) {  // a bake finished on the worker thread → refresh the lists (UI thread)
            g_voicePresets = list_voice_presets();
            g_bufFor.clear();
            g_voicesDirty = false;
        }
        EnterCriticalSection(&g_genCS); std::string pm = g_previewMsg; LeaveCriticalSection(&g_genCS);
        if (!pm.empty()) ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1), "%s", pm.c_str());
        if (!g_veStatus.empty()) ImGui::TextDisabled("%s", g_veStatus.c_str());
        ImGui::TextDisabled("Design -> Preview to audition -> Bake golden ref (clones it, stable) -> pick in a VO clip.");
    }
    ImGui::End();
    (void)p;
}

// ── inspector helpers ─────────────────────────────────────────────────────
// Edit a [r,g,b,a] (0..255) color param via an ImGui color picker.
static void color_param(const char* label, json& P, const char* key, ImU32 def) {
    float col[4];
    if (P.contains(key) && P[key].is_array() && P[key].size() >= 3) {
        auto& a = P[key];
        col[0] = a[0].get<float>() / 255.f; col[1] = a[1].get<float>() / 255.f;
        col[2] = a[2].get<float>() / 255.f; col[3] = (a.size() >= 4 ? a[3].get<float>() : 255.f) / 255.f;
    } else {
        col[0] = ((def >> IM_COL32_R_SHIFT) & 0xFF) / 255.f; col[1] = ((def >> IM_COL32_G_SHIFT) & 0xFF) / 255.f;
        col[2] = ((def >> IM_COL32_B_SHIFT) & 0xFF) / 255.f; col[3] = ((def >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
    }
    if (ImGui::ColorEdit4(label, col, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
        auto q = [](float f){ return (int)(f * 255.f + 0.5f); };
        P[key] = json::array({q(col[0]), q(col[1]), q(col[2]), q(col[3])});
    }
}

// Inspector widgets for the native compositing clips (code / caption / shape) — these are NOT
// generated, so they had no inspector at all (JSON-only). Each field reads c.params live and
// writes back on edit (instant, like the transform widgets).
static void draw_native_params(Project& p, Clip& c) {
    json& P = c.params;
    if (c.type == "code") {
        ImGui::SeparatorText("code card");
        std::string code = P.value("code", std::string());
        ImGui::TextDisabled("code (the typewriter source)");
        if (ImGui::InputTextMultiline("##code", &code, ImVec2(-1, 220),
                ImGuiInputTextFlags_AllowTabInput)) { P["code"] = code; c.label = snippet(code); }
        std::string title = P.value("title", std::string());
        if (ImGui::InputText("title", &title)) P["title"] = title;
        const char* LANGS[] = {"c", "cpp", "lua", "toml", "text"};
        std::string lang = P.value("lang", std::string("c"));
        if (ImGui::BeginCombo("lang", lang.c_str())) {
            for (auto l : LANGS) { bool s = (lang == l); if (ImGui::Selectable(l, s)) P["lang"] = std::string(l); }
            ImGui::EndCombo();
        }
        bool ln = P.value("line_numbers", false);
        if (ImGui::Checkbox("line numbers", &ln)) P["line_numbers"] = ln;
        if (ln) { ImGui::SameLine(); int fl = P.value("first_line", 1);
            ImGui::SetNextItemWidth(90); if (ImGui::InputInt("first line", &fl)) P["first_line"] = fl; }
        int fpx = P.value("font_px", 30); if (ImGui::DragInt("font px", &fpx, 0.5f, 8, 120)) P["font_px"] = fpx;
        float tw = (float)P.value("typewrite", 1.0); if (ImGui::SliderFloat("typewrite", &tw, 0.f, 1.f)) P["typewrite"] = tw;
        float sc = (float)P.value("scroll", 0.0);    if (ImGui::DragFloat("scroll (lines)", &sc, 0.1f)) P["scroll"] = sc;
        std::string hl;
        if (P.contains("highlight") && P["highlight"].is_array())
            for (auto& v : P["highlight"]) { if (!hl.empty()) hl += ","; hl += std::to_string(v.get<int>()); }
        if (ImGui::InputText("highlight lines (csv)", &hl)) {
            json arr = json::array(); std::stringstream ss(hl); std::string tok;
            while (std::getline(ss, tok, ',')) { try { arr.push_back(std::stoi(tok)); } catch (...) {} }
            P["highlight"] = arr;
        }
    } else if (c.type == "caption" || c.type == "text") {
        ImGui::SeparatorText("caption");
        const char* STYLES[] = {"plain", "lower_third", "term", "jp_lesson"};
        std::string style = P.value("style", std::string("plain"));
        if (ImGui::BeginCombo("style", style.c_str())) {
            for (auto s2 : STYLES) { bool s = (style == s2); if (ImGui::Selectable(s2, s)) P["style"] = std::string(s2); }
            ImGui::EndCombo();
        }
        // place: one-click pins. A plate often needs to hop out of the way of content — the four
        // corner pins do that instantly (pos then nudges FROM the pinned corner, no jump);
        // auto = least-busy corner over the clip's span · strap = the bottom lower-third band ·
        // (manual) = the authored transform only.
        {
            static const char* PL[] = {"(manual)", "auto", "tl", "tr", "bl", "br", "strap"};
            std::string pv = jstr(P, "place");
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("place"); ImGui::SameLine();
            for (int k = 0; k < 7; k++) {
                bool sel = (k == 0) ? pv.empty() : (pv == PL[k]);
                if (k) ImGui::SameLine(0, 4);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::SmallButton(PL[k])) { if (k == 0) P.erase("place"); else P["place"] = std::string(PL[k]); }
                if (sel) ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("pin the plate: tl/tr/bl/br = a frame corner (pos nudges from it)\nauto = least-busy top corner · strap = bottom band · (manual) = transform only");
        }
        std::string text = P.value("text", std::string());
        if (ImGui::InputTextMultiline("text", &text, ImVec2(-1, 56))) { P["text"] = text; c.label = snippet(text); }
        std::string sub = P.value("sub", std::string());     if (ImGui::InputText("sub", &sub)) P["sub"] = sub;
        std::string gloss = P.value("gloss", std::string()); if (ImGui::InputText("gloss", &gloss)) P["gloss"] = gloss;
        int fpx = P.value("font_px", 54); if (ImGui::DragInt("font px", &fpx, 0.5f, 8, 220)) P["font_px"] = fpx;
        const char* ALIGN[] = {"left", "center", "right"};
        std::string al = P.value("align", std::string("center"));
        if (ImGui::BeginCombo("align", al.c_str())) {
            for (auto a : ALIGN) { bool s = (al == a); if (ImGui::Selectable(a, s)) P["align"] = std::string(a); }
            ImGui::EndCombo();
        }
        bool box = P.value("box", style != "plain"); if (ImGui::Checkbox("panel box", &box)) P["box"] = box;
        color_param("text color", P, "color", IM_COL32(245, 247, 252, 255));
        color_param("accent",     P, "accent", IM_COL32(220, 120, 200, 255));
        color_param("sub color",  P, "sub_color", IM_COL32(202, 208, 222, 255));
    } else if (c.type == "shape") {
        ImGui::SeparatorText("shape");
        const char* SHAPES[] = {"box", "ellipse", "line", "arrow", "bracket"};
        std::string sh = P.value("shape", std::string("box"));
        if (ImGui::BeginCombo("shape", sh.c_str())) {
            for (auto s2 : SHAPES) { bool s = (sh == s2); if (ImGui::Selectable(s2, s)) P["shape"] = std::string(s2); }
            ImGui::EndCombo();
        }
        float th = (float)P.value("thickness", 4.0); if (ImGui::DragFloat("thickness", &th, 0.1f, 0.f, 40.f)) P["thickness"] = th;
        if (sh == "box" || sh == "ellipse" || sh == "bracket") {
            float w = (float)P.value("w", 200.0), h = (float)P.value("h", 120.0);
            if (ImGui::DragFloat("w", &w, 1.f)) P["w"] = w;
            if (ImGui::DragFloat("h", &h, 1.f)) P["h"] = h;
            if (sh == "box") { float r = (float)P.value("round", 10.0); if (ImGui::DragFloat("round", &r, 0.5f, 0.f, 120.f)) P["round"] = r; }
        }
        if (sh == "line" || sh == "arrow") {
            float fr[2] = {0, 0}, to[2] = {120, 0};
            if (P.contains("from") && P["from"].is_array() && P["from"].size() == 2) { fr[0] = P["from"][0].get<float>(); fr[1] = P["from"][1].get<float>(); }
            if (P.contains("to") && P["to"].is_array() && P["to"].size() == 2) { to[0] = P["to"][0].get<float>(); to[1] = P["to"][1].get<float>(); }
            if (ImGui::DragFloat2("from", fr, 1.f)) P["from"] = json::array({fr[0], fr[1]});
            if (ImGui::DragFloat2("to", to, 1.f)) P["to"] = json::array({to[0], to[1]});
            float grow = (float)P.value("grow", 1.0); if (ImGui::SliderFloat("grow", &grow, 0.f, 1.f)) P["grow"] = grow;
        }
        color_param("stroke", P, "color", IM_COL32(255, 214, 90, 255));
        color_param("fill",   P, "fill",  IM_COL32(0, 0, 0, 0));
    } else if (c.type == "gradient") {
        ImGui::SeparatorText("gradient / vignette");
        const char* KINDS[] = {"vignette", "linear", "radial"};
        std::string k = P.value("gradient", std::string("vignette"));
        if (ImGui::BeginCombo("kind", k.c_str())) {
            for (auto x : KINDS) { bool s = (k == x); if (ImGui::Selectable(x, s)) P["gradient"] = std::string(x); }
            ImGui::EndCombo();
        }
        float strength = (float)P.value("strength", 0.6); if (ImGui::SliderFloat("strength", &strength, 0.f, 1.f)) P["strength"] = strength;
        float feather  = (float)P.value("feather", 0.45); if (ImGui::SliderFloat("feather", &feather, 0.05f, 1.f)) P["feather"] = feather;
        float anch[2] = {0.5f, 0.5f};
        if (P.contains("anchor") && P["anchor"].is_array() && P["anchor"].size() == 2) { anch[0] = P["anchor"][0].get<float>(); anch[1] = P["anchor"][1].get<float>(); }
        if (ImGui::DragFloat2("anchor (0..1)", anch, 0.005f, 0.f, 1.f)) P["anchor"] = json::array({anch[0], anch[1]});
        if (k == "linear") {
            const char* DIRS[] = {"down", "up", "left", "right"};
            std::string d = P.value("dir", std::string("down"));
            if (ImGui::BeginCombo("direction", d.c_str())) {
                for (auto x : DIRS) { bool s = (d == x); if (ImGui::Selectable(x, s)) P["dir"] = std::string(x); }
                ImGui::EndCombo();
            }
        }
        color_param("color", P, "color", IM_COL32(0, 0, 0, 255));
    } else if (c.type == "blur") {
        ImGui::SeparatorText("blur transition");
        ImGui::TextDisabled("WHOLE-FRAME defocus over the ENTIRE composite — host, captions,\ncode, insets, everything (not one plate). Default strength is an\nease-in -> ease-out bell (peak at the midpoint); span one over a\nscene cut to blur A out, hide the cut, and sharpen back in on B.");
        float mb = (float)P.value("blur", 30.0); if (ImGui::SliderFloat("max blur (px)", &mb, 2.f, 120.f, "%.0f")) P["blur"] = (double)mb;
        if (P.contains("strength")) {
            float stv = (float)P.value("strength", 1.0);
            if (ImGui::SliderFloat("strength (manual)", &stv, 0.f, 1.f)) P["strength"] = (double)stv;
            ImGui::SameLine(); if (ImGui::SmallButton("→ auto bell")) P.erase("strength");
        } else if (ImGui::SmallButton("override strength (manual/keyframe)")) P["strength"] = 1.0;
    } else if (c.type == "diagram") {
        ImGui::SeparatorText("diagram (boxes + arrows)");
        ImGui::TextDisabled("author in JSON: flow:[\"A\",\"B\",...] = auto-laid-out chain,\nor nodes:[{label,x,y}] + edges:[{from,to}] = a graph.\nitems can be {label, sub}. reveal 0..1 stages it in.");
        float rv = (float)P.value("reveal", 1.0); if (ImGui::SliderFloat("reveal (0..1)", &rv, 0.f, 1.f)) P["reveal"] = (double)rv;
        float fp = (float)P.value("font_px", 30.0); if (ImGui::DragFloat("font px", &fp, 0.5f, 6.f, 200.f)) P["font_px"] = (double)fp;
        float gp = (float)P.value("gap", 56.0); if (ImGui::DragFloat("gap (px)", &gp, 1.f, 0.f, 400.f)) P["gap"] = (double)gp;
        if (!P.contains("nodes")) {
            const char* DIRS[] = {"h", "v"};
            std::string d = P.value("dir", std::string("h"));
            if (ImGui::BeginCombo("flow direction", d.c_str())) {
                for (auto x : DIRS) { bool sel = (d == x); if (ImGui::Selectable(x, sel)) P["dir"] = std::string(x); }
                ImGui::EndCombo();
            }
        }
        std::string ti = jstr(P, "title");
        char tb[160]; strncpy(tb, ti.c_str(), sizeof tb - 1); tb[sizeof tb - 1] = 0;
        if (ImGui::InputText("title", tb, sizeof tb)) P["title"] = std::string(tb);
        color_param("accent",   P, "accent", IM_COL32(120, 205, 235, 255));
        color_param("box fill", P, "fill",   IM_COL32(26, 30, 40, 235));
    } else if (c.type == "anchor") {
        ImGui::SeparatorText("caption anchor");
        ImGui::TextDisabled("A move handle: every caption whose START falls inside this clip's span\n"
                            "renders shifted by `pos (px)` above — one drag moves a whole time range\n"
                            "of captions, and the offset survives a transcript regen (slop.py rewrites\n"
                            "only the caption clips, not this one). Overlapping anchors sum.");
        // target rows: default = every caption/text row; params.rows narrows it
        std::vector<std::string> capRows;
        for (auto& kv : p.rows) if (kv.second.type == "caption" || kv.second.type == "text") capRows.push_back(kv.first);
        bool all = !(P.contains("rows") && P["rows"].is_array());
        auto rowOn = [&](const std::string& rid) {
            if (all) return true;
            for (auto& r : P["rows"]) if (r.is_string() && r.get<std::string>() == rid) return true;
            return false;
        };
        ImGui::TextDisabled("moves captions on:");
        bool changed = false; std::vector<std::string> onRows;
        for (auto& rid : capRows) {
            bool on = rowOn(rid);
            if (ImGui::Checkbox(rid.c_str(), &on)) changed = true;
            if (on) onRows.push_back(rid);
        }
        if (changed) {
            if (onRows.size() == capRows.size()) P.erase("rows");   // all checked = the default (no param)
            else { json arr = json::array(); for (auto& r : onRows) arr.push_back(r); P["rows"] = arr; }
        }
        // live readout: how many captions this anchor is moving right now
        int n = 0;
        for (auto& kv : p.clips) {
            const Clip& oc = kv.second;
            if (oc.type != "caption" && oc.type != "text") continue;
            if (oc.start < c.start - 1e-4 || oc.start >= c.start + c.dur - 1e-4) continue;
            if (!rowOn(oc.row)) continue;
            n++;
        }
        ImGui::Text("%d caption clip(s) in range", n);
    }
}

// The keyframe editor — moved out of the (generable-only) block so the native clips
// (code/caption/shape) get it too. t is TIMELINE seconds; v is 1 number or [x,y].
static void draw_keyframes_panel(Clip& c, double playhead) {
    if (!ImGui::CollapsingHeader("Keyframes (animation)")) return;
    std::vector<std::string> killPath;
    for (auto& kp : c.keyframes) {
        ImGui::PushID(kp.first.c_str());
        ImGui::TextDisabled("%s", kp.first.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("del path")) killPath.push_back(kp.first);
        auto& ks = kp.second;
        int killIdx = -1;
        for (size_t i = 0; i < ks.size(); i++) {
            KF& k = ks[i];
            ImGui::PushID((int)i);
            float tt = (float)k.t; ImGui::SetNextItemWidth(64);
            if (ImGui::DragFloat("t", &tt, 0.01f, 0.0f, 1e5f)) k.t = tt;
            ImGui::SameLine();
            if (k.v.size() >= 2) {
                float v2[2] = {(float)k.v[0], (float)k.v[1]}; ImGui::SetNextItemWidth(120);
                if (ImGui::DragFloat2("v", v2, 0.01f)) { k.v[0] = v2[0]; k.v[1] = v2[1]; }
            } else {
                float v1 = k.v.empty() ? 0.f : (float)k.v[0]; ImGui::SetNextItemWidth(78);
                if (ImGui::DragFloat("v", &v1, 0.01f)) { if (k.v.empty()) k.v.push_back(v1); else k.v[0] = v1; }
            }
            ImGui::SameLine();
            const char* INTERPS[] = {"linear", "constant", "bezier", "spring"};
            int ci = 0; for (int j = 0; j < 4; j++) if (k.interp == INTERPS[j]) ci = j;
            ImGui::SetNextItemWidth(82);
            if (ImGui::Combo("##in", &ci, INTERPS, 4)) k.interp = INTERPS[ci];
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) killIdx = (int)i;
            if (k.interp == "spring") {
                float sf = (float)k.stiffness, dm = (float)k.damping;
                ImGui::SetNextItemWidth(78); if (ImGui::DragFloat("stiff", &sf, 1.0f, 1.0f, 1000.0f)) k.stiffness = sf;
                ImGui::SameLine(); ImGui::SetNextItemWidth(78); if (ImGui::DragFloat("damp", &dm, 0.5f, 1.0f, 200.0f)) k.damping = dm;
            }
            ImGui::PopID();
        }
        if (killIdx >= 0) ks.erase(ks.begin() + killIdx);
        if (ImGui::SmallButton("+ keyframe @ playhead")) {
            KF nk; nk.t = playhead; nk.v = ks.empty() ? std::vector<double>{1.0} : ks.back().v;
            ks.push_back(nk);
            std::sort(ks.begin(), ks.end(), [](const KF& a, const KF& b) { return a.t < b.t; });
        }
        ImGui::PopID();
    }
    for (auto& kpath : killPath) c.keyframes.erase(kpath);
    static char kfPath[64] = "transform.scale";
    ImGui::SetNextItemWidth(150); ImGui::InputText("##kfp", kfPath, sizeof kfPath);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ param") && kfPath[0] && c.keyframes.find(kfPath) == c.keyframes.end()) {
        std::string pp = kfPath;
        bool vec2 = (pp == "transform.scale" || pp == "transform.pos" || pp == "transform.anchor");
        KF a0; a0.t = playhead; a0.v = vec2 ? std::vector<double>{1.0, 1.0} : std::vector<double>{1.0};
        c.keyframes[pp] = {a0};
    }
}

// ── "Tune face box" gizmo (avatar inspector) ────────────────────────────────────────────────────
// Drag a box over the sprite to pin the face CENTER / WIDTH / EYE-LINE that the face-anchored framing
// (bust/closeup/full) uses. The auto-detector measures skin spread over the upper body, so poses that
// bare more shoulder/chest skin report a wider "face" ⇒ render smaller at the same scale; hand-tuning
// each sprite to a consistent face width makes every pose match. Persists to the sprite's sidecar
// "face":{cx,eyeY,w} (SOURCE px) — keyed by FILE, so it applies to every emotion/pose that resolves
// to that sprite. Non-overlapping hit regions (left edge · body · right edge) so drags never fight.
static bool g_faceTune = false;               // gizmo visible (per session)
static std::string g_faceDrag;                // "", "body", "left", "right" — the handle grabbed this drag
static void draw_face_box_gizmo(const std::string& spritePath) {
    if (spritePath.empty()) { ImGui::TextDisabled("(no sprite for this emotion)"); return; }
    Tex* t = get_texture(spritePath);
    if (!t || !t->srv || t->w <= 0 || t->h <= 0) { ImGui::TextDisabled("(sprite loading…)"); return; }
    float iw = (float)t->w, ih = (float)t->h;

    AvatarFace fa = avatar_face(spritePath);                       // override if present, else auto-detected
    if (!fa.ok) { fa.cx = iw * 0.5f; fa.eyeY = ih * 0.3f; fa.w = iw * 0.3f; fa.ok = true; }
    bool hasOverride; { AvatarFace tmp; hasOverride = sidecar_face_override(spritePath, tmp); }

    ImGui::PushID(spritePath.c_str());
    float boxW = std::min(ImGui::GetContentRegionAvail().x, 340.f), boxH = 260.f;
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    float ds = std::min(boxW / iw, boxH / ih);                     // display scale (fit)
    float dw = iw * ds, dh = ih * ds;
    ImVec2 dorg(c0.x + (boxW - dw) * 0.5f, c0.y + (boxH - dh) * 0.5f);
    auto s2x = [&](float sx) { return dorg.x + sx * ds; };
    auto s2y = [&](float sy) { return dorg.y + sy * ds; };
    auto x2s = [&](float px) { return (px - dorg.x) / ds; };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(c0, ImVec2(c0.x + boxW, c0.y + boxH), IM_COL32(26, 26, 32, 255));
    for (float yy = dorg.y; yy < dorg.y + dh; yy += 10.f)          // checker so alpha reads
        for (float xx = dorg.x; xx < dorg.x + dw; xx += 10.f)
            if (((int)((xx - dorg.x) / 10.f) + (int)((yy - dorg.y) / 10.f)) & 1)
                dl->AddRectFilled(ImVec2(xx, yy), ImVec2(std::min(xx + 10.f, dorg.x + dw), std::min(yy + 10.f, dorg.y + dh)), IM_COL32(42, 42, 50, 255));
    dl->AddImage((ImTextureID)(intptr_t)t->srv, dorg, ImVec2(dorg.x + dw, dorg.y + dh));

    float boxHsrc = fa.w * 1.15f;                                  // cosmetic vertical extent (only cx/eyeY/w matter)
    float Lx = s2x(fa.cx - fa.w * 0.5f), Rx = s2x(fa.cx + fa.w * 0.5f);
    float Ty = s2y(fa.eyeY - boxHsrc * 0.5f), By = s2y(fa.eyeY + boxHsrc * 0.5f);
    ImGuiIO& io = ImGui::GetIO();
    bool changed = false;
    const float HW = 9.f;                                          // edge hit half-width (screen px)
    // LEFT edge
    ImGui::SetCursorScreenPos(ImVec2(Lx - HW, Ty));
    ImGui::InvisibleButton("##fL", ImVec2(HW * 2, std::max(10.f, By - Ty)));
    if (ImGui::IsItemHovered() || g_faceDrag == "left") ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) g_faceDrag = "left";
    if (g_faceDrag == "left" && ImGui::IsItemActive()) { float rr = fa.cx + fa.w * 0.5f, nl = std::min(x2s(io.MousePos.x), rr - 8.f); fa.cx = (nl + rr) * 0.5f; fa.w = rr - nl; changed = true; }
    // RIGHT edge
    ImGui::SetCursorScreenPos(ImVec2(Rx - HW, Ty));
    ImGui::InvisibleButton("##fR", ImVec2(HW * 2, std::max(10.f, By - Ty)));
    if (ImGui::IsItemHovered() || g_faceDrag == "right") ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) g_faceDrag = "right";
    if (g_faceDrag == "right" && ImGui::IsItemActive()) { float ll = fa.cx - fa.w * 0.5f, nr = std::max(x2s(io.MousePos.x), ll + 8.f); fa.cx = (ll + nr) * 0.5f; fa.w = nr - ll; changed = true; }
    // BODY (interior only — no overlap with the edge strips → submission order is irrelevant)
    if (Rx - HW > Lx + HW) {
        ImGui::SetCursorScreenPos(ImVec2(Lx + HW, Ty));
        ImGui::InvisibleButton("##fB", ImVec2(Rx - HW - (Lx + HW), std::max(10.f, By - Ty)));
        if (ImGui::IsItemHovered() || g_faceDrag == "body") ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsItemActivated()) g_faceDrag = "body";
        if (g_faceDrag == "body" && ImGui::IsItemActive()) { fa.cx += io.MouseDelta.x / ds; fa.eyeY += io.MouseDelta.y / ds; changed = true; }
    }
    if (!ImGui::IsMouseDown(0)) g_faceDrag.clear();

    fa.cx = std::max(0.f, std::min(iw, fa.cx));
    fa.eyeY = std::max(0.f, std::min(ih, fa.eyeY));
    fa.w = std::max(8.f, std::min(iw, fa.w));
    // redraw geometry from the (possibly updated) values so the box tracks the drag this frame
    Lx = s2x(fa.cx - fa.w * 0.5f); Rx = s2x(fa.cx + fa.w * 0.5f);
    boxHsrc = fa.w * 1.15f; Ty = s2y(fa.eyeY - boxHsrc * 0.5f); By = s2y(fa.eyeY + boxHsrc * 0.5f);
    float eyeScr = s2y(fa.eyeY);
    ImU32 col = IM_COL32(120, 220, 140, 235), colE = IM_COL32(255, 210, 90, 235);
    dl->AddRect(ImVec2(Lx, Ty), ImVec2(Rx, By), col, 0, 0, 2.f);
    dl->AddLine(ImVec2(Lx, eyeScr), ImVec2(Rx, eyeScr), colE, 1.6f);
    dl->AddCircleFilled(ImVec2(s2x(fa.cx), eyeScr), 3.f, colE);
    float midY = (Ty + By) * 0.5f;
    dl->AddRectFilled(ImVec2(Lx - 3, midY - 11), ImVec2(Lx + 3, midY + 11), col);
    dl->AddRectFilled(ImVec2(Rx - 3, midY - 11), ImVec2(Rx + 3, midY + 11), col);

    ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + boxH + 4));      // restore layout flow below the preview
    ImGui::Dummy(ImVec2(boxW, 0));

    if (changed) {                                                // persist (throttled to integer-px changes)
        std::string rp = resolve_asset(spritePath);
        json side = lib_load_sidecar(rp);
        json fj; fj["cx"] = (int)(fa.cx + 0.5f); fj["eyeY"] = (int)(fa.eyeY + 0.5f); fj["w"] = (int)(fa.w + 0.5f);
        if (!side.contains("face") || side["face"] != fj) { side["face"] = fj; lib_save_sidecar(rp, side); invalidate_face_cache(); }
    }
    std::string fn = spritePath.substr(spritePath.find_last_of("/\\") + 1);
    ImGui::TextDisabled("%s · cx %.0f eye %.0f w %.0f · %s", fn.c_str(), fa.cx, fa.eyeY, fa.w, hasOverride ? "tuned" : "auto");
    if (hasOverride && ImGui::SmallButton("Reset to auto-detect")) {
        std::string rp = resolve_asset(spritePath);
        json side = lib_load_sidecar(rp); side.erase("face"); lib_save_sidecar(rp, side); invalidate_face_cache();
    }
    ImGui::TextDisabled("drag box = move (cx/eye) · edges = width · yellow = eye-line");
    ImGui::PopID();
}

static void draw_avatar_emotion_panel(Clip& c, const std::string& rigName) {
    if (!ImGui::CollapsingHeader("Emotion poses", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const AvatarRig* rig = get_rig(rigName.empty() ? "gemma-big" : rigName);
    std::string cur = jstr(c.params, "emotion");
    if (cur.empty()) cur = "auto";
    const char* framings[] = {"full", "bust", "closeup", "floating"};
    std::string fr = avatar_framing(c.params, cur == "auto" ? "neutral" : cur);
    int fi = 0; for (int i = 0; i < 4; i++) if (fr == framings[i]) fi = i;
    ImGui::SetNextItemWidth(128);
    if (ImGui::Combo("framing", &fi, framings, 4)) c.params["framing"] = std::string(framings[fi]);
    ImGui::SameLine();
    if (ImGui::SmallButton("raw")) c.params.erase("framing");   // unset → authored transform verbatim
    ImGui::TextDisabled("face-anchored: bust/closeup = face & half-bust; full = whole body (floating sprite).\n"
                        "raw = authored transform. All place the WHOLE sprite — feet just run off-frame.");
    // POSE variant override — auto picks front / ¾-toward-content / floating; override to taste.
    const char* poses[] = {"auto", "front", "front34", "viewer34", "float_front", "float34", "float_viewer34"};
    std::string pcur = jstr(c.params, "pose"); if (pcur.empty()) pcur = "auto";
    int pi = 0; for (int i = 0; i < 7; i++) if (pcur == poses[i]) pi = i;
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("pose", &pi, poses, 7)) { if (pi == 0) c.params.erase("pose"); else c.params["pose"] = std::string(poses[pi]); }
    ImGui::TextDisabled("pose: auto = front / ¾-facing-content / floating(full). Override per clip.");

    // Face-box gizmo: tune the face anchor for the CURRENT emotion's sprite so bust/closeup/full framings
    // match apparent size across poses (fixes room-shot size drift). Tunes the sprite FILE (all emotions
    // sharing it). Auto-detection seeds it; drag to correct where the skin heuristic over/under-reaches.
    ImGui::Checkbox("Tune face box", &g_faceTune);
    if (g_faceTune) {
        std::string tuneEmo = cur == "auto" ? "neutral" : cur;
        draw_face_box_gizmo(avatar_sprite_path(rig, tuneEmo));
    }

    std::vector<std::string> emos;
    emos.push_back("auto");
    for (int i = 0; i < AVATAR_EMO_N; i++) emos.push_back(AVATAR_EMOS[i]);
    if (rig) {
        for (auto& kv : rig->mouths)
            if (std::find(emos.begin(), emos.end(), kv.first) == emos.end()) emos.push_back(kv.first);
    }
    const float tile = 74.f, pad = 8.f;
    int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (tile + pad)));
    int shown = 0;
    for (auto& emo : emos) {
        ImGui::PushID(emo.c_str());
        ImGui::BeginGroup();
        bool selected = (emo == "auto") ? (cur == "auto") : (canon_emotion(cur) == canon_emotion(emo));
        std::string useEmo = emo == "auto" ? "neutral" : emo;
        std::string path = avatar_sprite_path(rig, useEmo);
        EmoThumb th;
        bool clicked = false;
        if (!path.empty() && get_emo_thumb(path, th) && th.srv) {   // async — the face-zoomed thumb pops in when ready
            ImVec4 bg = selected ? ImVec4(0.30f, 0.52f, 0.85f, 1) : ImVec4(0.09f, 0.10f, 0.13f, 1);
            clicked = ImGui::ImageButton("pose", (ImTextureID)(intptr_t)th.srv, ImVec2(tile, tile), th.uv0, th.uv1, bg);
        } else {
            clicked = ImGui::Button(path.empty() ? emo.c_str() : "...", ImVec2(tile, tile));   // loading / no sprite
        }
        if (clicked) {
            c.params["emotion"] = emo;
            strncpy(g_emoBuf, emo.c_str(), sizeof g_emoBuf - 1);
            g_emoBuf[sizeof g_emoBuf - 1] = 0;
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tile);
        if (selected) ImGui::TextColored(ImVec4(0.55f, 0.78f, 1, 1), "%s", emo.c_str());
        else ImGui::TextWrapped("%s", emo.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();
        if (++shown % cols) ImGui::SameLine(); else ImGui::Dummy(ImVec2(0, 2));
    }
}

// Native Windows "open file" dialog → a UTF-8 path (empty if cancelled).
static std::string pick_image_file() {
    wchar_t buf[2048] = {0};
    OPENFILENAMEW ofn = {}; ofn.lStructSize = sizeof ofn;
    ofn.lpstrFile = buf; ofn.nMaxFile = 2048;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return "";
    char out[2048] = {0};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    return std::string(out);
}
// Multi-select open dialog → UTF-8 paths (empty on cancel). On multi-select the buffer is
// "dir\0name1\0name2\0\0"; a single pick is just one full path.
static std::vector<std::string> pick_files_multi() {
    static wchar_t buf[16384]; buf[0] = 0;
    OPENFILENAMEW ofn = {}; ofn.lStructSize = sizeof ofn;
    ofn.lpstrFile = buf; ofn.nMaxFile = 16384;
    ofn.lpstrFilter = L"Media\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.wav;*.mp3;*.ogg;*.flac;*.m4a;*.mp4;*.mov;*.webm;*.mkv\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) return out;
    std::wstring dir = buf;
    wchar_t* q = buf + dir.size() + 1;
    if (!*q) { out.push_back(from_w(buf)); return out; }   // single selection → buf IS the full path
    for (; *q; q += wcslen(q) + 1) out.push_back(from_w((dir + L"\\" + q).c_str()));
    return out;
}
// Point an image clip at an external file: register an asset (model + doc) and select it.
static void set_clip_image_file(Project& p, Clip& c, const std::string& path) {
    std::string key; int n = 1;
    while (p.asset_uri.count(key = "ext_img" + std::to_string(n))) n++;
    p.asset_uri[key] = path; c.asset = key;
    if (p.doc.contains("assets") && p.doc["assets"].is_object())
        p.doc["assets"][key] = json{{"provider", "external"}, {"type", "image"}, {"status", "ready"}, {"uri", path}, {"meta", json::object()}};
}

// Add a new track+row of `type` (in-memory model + the doc so Save persists it). New track
// goes on TOP (drawn first); reorder with the gutter ▲▼.
static std::string add_track(Project& p, const std::string& type, const std::string& name) {
    std::string rid, tid; int n = 1;
    do { rid = "r_" + type + std::to_string(n); tid = "tk_" + type + std::to_string(n); n++; } while (p.rows.count(rid));
    std::string nm = name.empty() ? rid : name;
    Row r; r.id = rid; r.type = type; r.name = nm; r.params = json::object();
    p.rows[rid] = r;
    Track t; t.id = tid; t.name = nm; t.kind = (type == "tts" || type == "music") ? "audio" : "video"; t.rows = {rid};
    p.tracks.insert(p.tracks.begin(), t);
    if (p.doc.contains("rows") && p.doc["rows"].is_object())
        p.doc["rows"][rid] = json{{"type", type}, {"name", nm}, {"params", json::object()}, {"clips", json::array()}};
    if (p.doc.contains("tracks") && p.doc["tracks"].is_array())
        p.doc["tracks"].insert(p.doc["tracks"].begin(), json{{"id", tid}, {"name", nm}, {"kind", t.kind}, {"rows", json::array({rid})}});
    return rid;
}

// Add a NATIVE "special" compositing clip from scratch at time t — a caption, code card, shape callout,
// vignette/gradient wash, blur transition, background fill, or diagram — no generation, no asset, just a
// sensible default you then tweak in the inspector. Finds (or creates) a row of that type. This is the
// "simple way to add special clips like the vignette from scratch" — they were previously only reachable
// by hand-editing the project JSON or by splitting an existing clip. Returns the new clip id (or "").
static std::string add_native_clip_at(Project& p, const std::string& type, double t, const std::string& forceRow = "") {
    double dur = 3.0; json params = json::object(); std::string label = type;
    if      (type == "caption")  { dur = 3.0; params = json{{"text","New caption"},{"style","lower_third"},{"align","center"}}; label = "caption"; }
    else if (type == "code")     { dur = 4.0; params = json{{"lang","python"},{"code","# your code here\nprint(\"hi\")"},{"typewrite",true},{"line_numbers",true}}; label = "code"; }
    else if (type == "shape")    { dur = 3.0; params = json{{"kind","box"},{"thickness",3.0},{"grow",true}}; label = "shape"; }
    else if (type == "gradient") { dur = 4.0; params = json{{"gradient","vignette"},{"strength",0.6}}; label = "vignette"; }
    else if (type == "diagram")  { dur = 5.0; params = json::object(); label = "diagram"; }
    else if (type == "blur")     { dur = 1.5; params = json{{"blur",30.0}}; label = "blur"; t = std::max(0.0, t - dur * 0.5); }  // center the bell on the playhead
    else if (type == "filler")   { dur = 5.0; params = json{{"source","auto"},{"blur",30.0},{"dim",0.55}}; label = "bg fill"; }
    else if (type == "anchor")   { dur = 8.0; params = json::object(); label = "cap anchor"; }   // caption move-handle: pos shifts every caption starting in its span

    std::string row = forceRow;   // placement tool passes a pre-chosen (overlap-free) row; else resolve
    if (row.empty()) for (auto& kv : p.rows) if (kv.second.type == type) { row = kv.first; break; }
    if (row.empty()) {
        row = add_track(p, type, type);
        // a filler is a BACKDROP → drop its fresh track to the BOTTOM of the stack (add_track inserts on
        // top, which would cover the scene). Overlays (vignette/blur/caption/…) correctly stay on top.
        if (type == "filler" && p.tracks.size() > 1) {
            Track tk0 = p.tracks.front(); p.tracks.erase(p.tracks.begin()); p.tracks.push_back(tk0);
            if (p.doc.contains("tracks") && p.doc["tracks"].is_array() && p.doc["tracks"].size() > 1) {
                json d0 = p.doc["tracks"].front(); p.doc["tracks"].erase(p.doc["tracks"].begin()); p.doc["tracks"].push_back(d0);
            }
        }
    }
    if (row.empty()) return "";

    std::string id; int m = 1; while (p.clips.count(id = "c_" + type + std::to_string(m))) m++;
    Clip c; c.id = id; c.row = row; c.type = type; c.start = t; c.dur = dur; c.label = label; c.params = params;
    p.clips[id] = c;
    if (p.rows.count(row)) p.rows[row].clips.push_back(id);
    if (p.doc.contains("clips"))
        p.doc["clips"][id] = json{{"row", row}, {"start", t}, {"dur", dur}, {"params", params},
            {"transform", json{{"pos", {0, 0}}, {"scale", {1.0, 1.0}}, {"rot", 0}, {"opacity", 1}, {"anchor", {0.5, 0.5}}}}};
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips"))
        p.doc["rows"][row]["clips"].push_back(id);
    return id;
}

// ── Placement helpers: which ROW does a quick-add kind live on, and does a span fit without overlap? ──
static std::string place_row_type(const std::string& kind) {
    if (kind == "host")  return "avatar";
    if (kind == "voice") return "tts";
    if (kind == "music" || kind == "sound") return "music";
    if (kind == "backdrop") return "image";
    return kind;   // caption/code/shape/diagram/gradient/blur/filler/anchor
}
static double place_default_dur(const std::string& kind) {
    if (kind == "diagram") return 5.0;
    if (kind == "code" || kind == "gradient" || kind == "backdrop" || kind == "filler") return 4.0;
    if (kind == "blur")  return 1.5;
    if (kind == "sound") return 1.0;
    if (kind == "music") return 20.0;
    return 3.0;   // host/voice/caption/shape/anchor
}
// true if [t, t+dur] overlaps no clip already on the row
static bool row_span_free(Project& p, const std::string& rowId, double t, double dur) {
    auto rit = p.rows.find(rowId);
    if (rit == p.rows.end()) return true;
    for (auto& cid : rit->second.clips) {
        auto c = p.clips.find(cid);
        if (c == p.clips.end()) continue;
        double s = c->second.start, e = s + c->second.dur;
        if (s < t + dur - 1e-3 && e > t + 1e-3) return false;
    }
    return true;
}
// Pick the placement row: the CLICKED lane (if it's the right type and the span fits) > any existing
// row of the type where it fits (track/lane order) > "" (caller makes a new lane). This is what lets a
// click land in the clicked lane's free space, spill to another lane, or make a new track on demand.
static std::string choose_row_of_type(Project& p, const std::string& want, const std::string& clickedRow, double t, double dur) {
    if (!clickedRow.empty()) {
        auto rit = p.rows.find(clickedRow);
        if (rit != p.rows.end() && rit->second.type == want && row_span_free(p, clickedRow, t, dur)) return clickedRow;
    }
    for (auto& tk : p.tracks) for (auto& rid : tk.rows) {
        if (rid == clickedRow) continue;
        auto rit = p.rows.find(rid);
        if (rit != p.rows.end() && rit->second.type == want && row_span_free(p, rid, t, dur)) return rid;
    }
    return "";   // no room anywhere → new lane
}
static std::string choose_place_row(Project& p, const std::string& kind, const std::string& clickedRow, double t, double dur) {
    return choose_row_of_type(p, place_row_type(kind), clickedRow, t, dur);
}
// Create + set up a fresh row for a kind (the "new lane" case): rig + driven_by for a host, the golden
// clone voice for a VO row, and a cover backdrop sinks to the bottom of the stack.
static std::string create_place_row(Project& p, const std::string& kind) {
    std::string want = place_row_type(kind);
    const char* nm = want == "avatar" ? "Avatar" : want == "tts" ? "VO" : want == "music" ? "Music"
                   : (kind == "backdrop" ? "Backdrop" : want == "image" ? "Footage" : want.c_str());
    std::string row = add_track(p, want, nm);
    if (row.empty()) return "";
    if (kind == "host") {
        p.rows[row].params["rig"] = "gemma-gpt-static";
        for (auto& kv : p.rows) if (kv.second.type == "tts") { p.rows[row].params["driven_by"] = kv.first; break; }
        if (p.doc.contains("rows") && p.doc["rows"].contains(row)) p.doc["rows"][row]["params"] = p.rows[row].params;
    } else if (kind == "voice") {
        p.rows[row].params["voice_preset"] = "gemma-san-deep-clone";
        if (p.doc.contains("rows") && p.doc["rows"].contains(row)) p.doc["rows"][row]["params"] = p.rows[row].params;
    } else if (kind == "backdrop" && p.tracks.size() > 1) {   // a cover backdrop belongs at the BOTTOM
        Track t0 = p.tracks.front(); p.tracks.erase(p.tracks.begin()); p.tracks.push_back(t0);
        if (p.doc.contains("tracks") && p.doc["tracks"].is_array() && p.doc["tracks"].size() > 1) {
            json d0 = p.doc["tracks"].front(); p.doc["tracks"].erase(p.doc["tracks"].begin()); p.doc["tracks"].push_back(d0);
        }
    }
    return row;
}
// Create the kind's clip on a GIVEN row (assumed the right type; a host uses the row's own rig). dur<=0
// → the kind's default length.
static std::string place_clip_on_row(Project& p, const std::string& kind, const std::string& row, double t, double dur) {
    if (row.empty()) return "";
    auto with_dur = [&](const std::string& id) {
        if (dur > 0.02 && !id.empty() && p.clips.count(id)) { p.clips[id].dur = dur;
            if (p.doc.contains("clips") && p.doc["clips"].contains(id)) p.doc["clips"][id]["dur"] = dur; }
        return id;
    };
    if (kind == "music" || kind == "sound")
        return with_dur(add_audio_clip_at(p, row, t, kind == "music" ? "library/music/deadly-roulette.mp3"
                                                                      : "presets/voice-snips/gemma-heh.wav"));
    if (kind == "backdrop") {
        std::string id = add_image_clip_at(p, row, t, "presets/backgrounds/room-day.png");
        if (!id.empty() && p.clips.count(id)) { p.clips[id].params["layout"] = "cover"; p.clips[id].label = "backdrop";
            if (p.doc.contains("clips") && p.doc["clips"].contains(id)) p.doc["clips"][id]["params"]["layout"] = "cover"; }
        return with_dur(id);
    }
    if (kind != "host" && kind != "voice")   // native compositing types → place on the given row
        return with_dur(add_native_clip_at(p, kind, t, row));
    // host / voice → build the clip inline on `row`
    double d = dur > 0.02 ? dur : place_default_dur(kind);
    std::string ctype, idpfx, label; json params;
    if (kind == "host") {
        std::string rig = jstr(p.rows[row].params, "rig"); if (rig.empty()) rig = "gemma-gpt-static";
        bool driven = false; for (auto& kv : p.rows) if (kv.second.type == "tts") { driven = true; break; }
        ctype = "avatar"; idpfx = "c_av"; label = "avatar:" + rig;
        params = json{{"emotion", driven ? "auto" : "neutral"}, {"framing", "bust"}};
    } else {   // voice — an empty line on the clone-voice row; type it + Generate VO
        ctype = "tts"; idpfx = "c_vo"; label = "vo"; params = json{{"text", "New line — type here, then Generate VO"}};
    }
    std::string id; int m = 1; while (p.clips.count(id = idpfx + std::to_string(m))) m++;
    Clip c; c.id = id; c.row = row; c.type = ctype; c.start = t; c.dur = d; c.label = label; c.params = params;
    p.clips[id] = c; if (p.rows.count(row)) p.rows[row].clips.push_back(id);
    if (p.doc.contains("clips"))
        p.doc["clips"][id] = json{{"row", row}, {"start", t}, {"dur", d}, {"params", params},
            {"transform", json{{"pos", {0, 0}}, {"scale", {1.0, 1.0}}, {"rot", 0}, {"opacity", 1}, {"anchor", {0.5, 0.5}}}}};
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips"))
        p.doc["rows"][row]["clips"].push_back(id);
    return id;
}
// Quick-add a common clip (house defaults), OVERLAP-AWARE: land in the clicked lane's free space, spill
// to another compatible lane, or make a new track — never overlapping. `clickedRow` = the lane under the
// cursor ("" for the menu at-playhead path). Returns the new clip id.
static std::string add_quick_clip(Project& p, const std::string& kind, double t, double dur = -1.0, const std::string& clickedRow = "") {
    if (t < 0) t = 0;
    double dCheck = dur > 0.02 ? dur : place_default_dur(kind);
    std::string row = choose_place_row(p, kind, clickedRow, t, dCheck);
    if (row.empty()) row = create_place_row(p, kind);
    return place_clip_on_row(p, kind, row, t, dur);
}

// Generic add (hotkey A): a clip in the CLICKED lane's type, inheriting the nearest reference clip's
// settings — the whole transform + non-content params (playback rate, loop, framing, style, glow, …),
// preferring an occupying/BEFORE clip over one AFTER. Content (asset/text/code) stays empty to fill.
// Overlap-aware: lands in the clicked lane's free space, spills to another lane of that type, or makes a
// new lane of the same type (copying the clicked lane's rig/voice/etc.). No reference → typed defaults.
static std::string add_generic_clip(Project& p, const std::string& clickedRow, double t, double dur) {
    auto crit = p.rows.find(clickedRow);
    if (crit == p.rows.end()) return "";
    if (t < 0) t = 0;
    std::string rtype = crit->second.type;
    std::string ref; double refScore = 1e30;   // occupying t > nearest BEFORE > nearest AFTER (on the clicked lane)
    for (auto& cid : crit->second.clips) {
        auto c = p.clips.find(cid); if (c == p.clips.end()) continue;
        double s = c->second.start, e = s + c->second.dur, score;
        if (s <= t + 1e-3 && e >= t - 1e-3) score = -1.0;      // occupies the click → best reference
        else if (e <= t) score = (t - e);                       // before
        else score = (s - t) + 1e6;                             // after (always worse than any before)
        if (score < refScore) { refScore = score; ref = cid; }
    }
    double dCheck = dur > 0.02 ? dur : (ref.empty() ? place_default_dur(rtype) : p.clips[ref].dur);
    std::string row = choose_row_of_type(p, rtype, clickedRow, t, dCheck);
    if (row.empty()) {   // no room → a new lane of the same type, matching the clicked lane's row params
        row = add_track(p, rtype, crit->second.name.empty() ? rtype : crit->second.name);
        if (!row.empty() && crit->second.params.is_object() && !crit->second.params.empty()) {
            p.rows[row].params = crit->second.params;   // rig / voice_preset / driven_by / model / lora
            if (p.doc.contains("rows") && p.doc["rows"].contains(row)) p.doc["rows"][row]["params"] = p.rows[row].params;
        }
    }
    if (row.empty()) return "";
    std::string id; int m = 1; while (p.clips.count(id = "c_" + rtype + std::to_string(m))) m++;
    double d = dur > 0.02 ? dur : (ref.empty() ? place_default_dur(rtype) : p.clips[ref].dur);
    Clip c; c.id = id; c.row = row; c.type = rtype; c.start = t; c.dur = d; c.label = rtype;
    if (!ref.empty()) {   // inherit the reference's transform + non-content params
        Clip& s = p.clips[ref];
        for (int i = 0; i < 2; i++) { c.tx_pos[i] = s.tx_pos[i]; c.tx_scale[i] = s.tx_scale[i]; c.tx_anchor[i] = s.tx_anchor[i]; }
        c.tx_rot = s.tx_rot; c.tx_opacity = s.tx_opacity;
        static const char* CONTENT[] = {"text", "code", "transcript", "dialog", "sfx_cue", "sfx_at", "credit", "in", "title", "artist"};
        if (s.params.is_object()) for (auto& pr : s.params.items()) {
            bool skip = false; for (auto* k : CONTENT) if (pr.key() == k) { skip = true; break; }
            if (!skip) c.params[pr.key()] = pr.value();
        }
        if (!s.label.empty()) c.label = s.label;
    } else {   // typed defaults for an empty lane
        if      (rtype == "caption") c.params = json{{"text", "New caption"}, {"style", "lower_third"}};
        else if (rtype == "code")    c.params = json{{"lang", "python"}, {"code", "# ..."}, {"typewrite", true}, {"line_numbers", true}};
        else if (rtype == "tts")     c.params = json{{"text", "New line — type here, then Generate VO"}};
        else if (rtype == "avatar")  c.params = json{{"emotion", "auto"}, {"framing", "bust"}};
        else if (rtype == "shape")   c.params = json{{"kind", "box"}, {"thickness", 3.0}};
    }
    p.clips[id] = c; p.rows[row].clips.push_back(id);
    if (p.doc.contains("clips"))
        p.doc["clips"][id] = json{{"row", row}, {"start", t}, {"dur", d}, {"params", c.params},
            {"transform", json{{"pos", {c.tx_pos[0], c.tx_pos[1]}}, {"scale", {c.tx_scale[0], c.tx_scale[1]}}, {"rot", c.tx_rot}, {"opacity", c.tx_opacity}, {"anchor", {c.tx_anchor[0], c.tx_anchor[1]}}}}};
    if (p.doc.contains("rows") && p.doc["rows"].contains(row) && p.doc["rows"][row].contains("clips")) p.doc["rows"][row]["clips"].push_back(id);
    return id;
}

// ── Library panel: browse/search/preview the global media library; double-click drops at the
// playhead, drag onto the timeline places at the cursor. Add (copies in), rename, delete in place.
static bool g_showLibrary = true;
static float g_leftW = 318.f;    // media-pane width (drag the splitter to resize)
static float g_inspW = 380.f;    // right inspector width (drag to resize; the preview flexes)
static float g_topH = -1.f;      // preview/inspector row height (drag the divider; -1 = init to 50%)
static char g_libSearch[128] = "";
static int  g_libTypeFilter = 0;             // 0=all 1=image 2=audio 3=video
static std::string g_libSelected;            // selected item path (drives the viewer, L5)
static std::string g_libRenamePath;          // item being renamed (modal)
static bool g_openLibRename = false;
static char g_libRenameBuf[128] = "";

static void library_add_to_timeline(Project& p, UIState& st, const LibItem& it) {
    if (it.type == LIB_IMAGE) {
        std::string row;
        for (auto& kv : p.rows) if (kv.second.type == "image") { row = kv.first; break; }
        if (row.empty()) { add_track(p, "image", "Footage"); for (auto& kv : p.rows) if (kv.second.type == "image") { row = kv.first; break; } }
        if (!row.empty()) st.selected = add_image_clip_at(p, row, st.playhead, it.path);
    } else if (it.type == LIB_AUDIO) {
        std::string row;
        for (auto& kv : p.rows) if (kv.second.type == "music") { row = kv.first; break; }
        if (row.empty()) { add_track(p, "music", "Music"); for (auto& kv : p.rows) if (kv.second.type == "music") { row = kv.first; break; } }
        if (!row.empty()) st.selected = add_audio_clip_at(p, row, st.playhead, it.path);
    } else if (it.type == LIB_VIDEO) {  // in-process libav decode → no proxy step needed
        std::string row = find_or_create_video_row(p);
        if (!row.empty()) st.selected = add_video_clip_at(p, row, st.playhead, it.path);
    } else if (it.type == LIB_AVATAR) {  // a rig def → an avatar clip from scratch (it.name = the rig name)
        st.selected = add_avatar_clip_at(p, it.name, st.playhead);
    }
}

// ── "Add gen item" (L4 entry point): modals to create a NEW generatable library entry via a
// provider (not just import a pre-baked file). Image = Anima host engine; voice = a TTS preset.
// Each lands as a library file + a `.meta.json` gen sidecar (recipe + history → regen/restore). ──
static bool g_openGenImg = false, g_openGenVoice = false;
static char g_giName[96] = "gemma-host";
static std::string g_giPrompt = "gemma-san, chibi, super deformed, 1girl, solo, smug, smirk, "
                                "upper body, simple background, masterpiece, best quality, very aesthetic";
static int  g_giSeed = 1234;
static bool g_giHost = true;          // Gemma host LoRA (Anima) on
static int  g_giW = 1024, g_giH = 1024;
static char g_gvName[96] = "voice-line";
static std::string g_gvText = "Fufu~ welcome back, mortals.";
static char g_gvEmotion[128] = "";
static int  g_gvSeed = 7;
static std::string g_gvVoice;         // selected voice preset (defaults to the first golden)
static bool g_openNewAvatar = false;  // "+ avatar" → author a library rig def
static char g_naName[96] = "gemma-host";
static char g_naPrefix[96] = "gemma-";

static json lib_img_recipe() {
    json r = json::object();
    r["prompt"] = g_giPrompt; r["seed"] = g_giSeed; r["width"] = g_giW; r["height"] = g_giH;
    r["arch"] = "anima";              // Anima is the default/main image engine (backgrounds + host)
    if (g_giHost) { r["lora"] = "gemma-san-anima"; r["lora_strength"] = 0.9; }
    return r;
}
static json lib_voice_recipe() {
    json r = json::object();
    r["text"] = g_gvText;
    if (!g_gvVoice.empty()) r["voice_preset"] = g_gvVoice;
    if (g_gvEmotion[0]) r["emotion"] = std::string(g_gvEmotion);
    r["seed"] = g_gvSeed;
    return r;
}
static void draw_lib_gen_modals() {
    if (g_openGenImg)   { ImGui::OpenPopup("New gen image"); g_openGenImg = false; }
    if (g_openGenVoice) { ImGui::OpenPopup("New gen voice"); g_openGenVoice = false; }
    if (g_openNewAvatar) { ImGui::OpenPopup("New avatar rig"); g_openNewAvatar = false; }
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("New avatar rig", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("name", g_naName, sizeof g_naName);
        ImGui::InputText("image prefix", g_naPrefix, sizeof g_naPrefix);
        ImGui::TextDisabled("emotion E → library/images/<prefix>E.png");
        // live preview: which canonical emotions the prefix resolves right now
        std::string have, miss;
        for (int i = 0; i < AVATAR_EMO_N; i++) {
            std::string f = g_libraryDir + "/images/" + std::string(g_naPrefix) + AVATAR_EMOS[i] + ".png";
            bool ok = GetFileAttributesW(to_w(f).c_str()) != INVALID_FILE_ATTRIBUTES;
            (ok ? have : miss) += (ok ? (have.empty() ? "" : ", ") : (miss.empty() ? "" : ", ")) + std::string(AVATAR_EMOS[i]);
        }
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1), "resolves: %s", have.empty() ? "(none — check the prefix)" : have.c_str());
        if (!miss.empty()) ImGui::TextDisabled("missing: %s  (add overrides in the Viewer)", miss.c_str());
        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(120, 0)) && g_naName[0]) {
            json j; j["schema"] = "slopstudio.avatar_rig/1"; j["prefix"] = std::string(g_naPrefix);
            j["fallback"] = "neutral"; j["emotions"] = json::object();
            save_avatar_rig(g_naName, j);
            g_libSelected = avatar_rig_path(g_naName);   // focus it in the Viewer to add overrides
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(); if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::TextDisabled("Then: refine the prefix + add per-emotion image overrides in the Viewer.\nDrop the rig onto the timeline → an avatar clip (all clips on its row share the frames).");
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("New gen image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("name", g_giName, sizeof g_giName);
        ImGui::TextDisabled("prompt (Anima tags)");
        ImGui::InputTextMultiline("##giprompt", &g_giPrompt, ImVec2(516, 96));
        ImGui::SetNextItemWidth(150); ImGui::InputInt("seed", &g_giSeed); ImGui::SameLine();
        if (ImGui::Button("random")) g_giSeed = (int)(GetTickCount() & 0x7fffffff);
        ImGui::SameLine(); ImGui::Checkbox("Gemma host LoRA", &g_giHost);
        ImGui::SetNextItemWidth(110); ImGui::InputInt("w", &g_giW); ImGui::SameLine();
        ImGui::SetNextItemWidth(110); ImGui::InputInt("h", &g_giH);
        ImGui::Separator();
        if (ImGui::Button("Generate", ImVec2(120, 0)) && g_giName[0]) {
            std::string base = lib_unique_base(LIB_IMAGE, g_giName);
            json recipe = lib_img_recipe();
            start_lib_gen(LIB_IMAGE, base, "image", "text2image", lib_job_body("text2image", recipe), recipe, false);
            g_libSelected = g_libraryDir + "/images/" + base + ".png";   // focus it once it lands
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::TextDisabled("Anima · runs on lame · lands in library/images + a gen sidecar (regen/restore in the viewer)");
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("New gen voice", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_voicePresets.empty()) g_voicePresets = list_voice_presets();
        if (g_gvVoice.empty() && !g_voicePresets.empty()) g_gvVoice = g_voicePresets.front();
        ImGui::InputText("name", g_gvName, sizeof g_gvName);
        ImGui::TextDisabled("text");
        ImGui::InputTextMultiline("##gvtext", &g_gvText, ImVec2(516, 72));
        if (ImGui::BeginCombo("voice", g_gvVoice.empty() ? "(none)" : g_gvVoice.c_str())) {
            for (auto& vp : g_voicePresets) if (ImGui::Selectable(vp.c_str(), vp == g_gvVoice)) g_gvVoice = vp;
            ImGui::EndCombo();
        }
        ImGui::InputText("emotion (optional)", g_gvEmotion, sizeof g_gvEmotion);
        ImGui::SetNextItemWidth(150); ImGui::InputInt("seed", &g_gvSeed);
        ImGui::Separator();
        if (ImGui::Button("Generate", ImVec2(120, 0)) && g_gvName[0]) {
            std::string base = lib_unique_base(LIB_AUDIO, g_gvName);
            json recipe = lib_voice_recipe();
            start_lib_gen(LIB_AUDIO, base, "tts", "speech", lib_job_body("speech", recipe), recipe, false);
            g_libSelected = g_libraryDir + "/audio/" + base + ".wav";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void draw_library_contents(Project& p, UIState& st) {
    {   // periodic rescan: files an agent/tool drops on disk (rigs, screenshots, gens) JUST APPEAR —
        // no Refresh hunt. The scan is a few dirs of FindFirstFile + tiny sidecars (~ms); changed
        // files also drop their stale textures (lib_track_mtime), so overwrites refresh too.
        static double s_lastScan = -10.0;
        double now = ImGui::GetTime();
        if (now - s_lastScan > 2.5) { s_lastScan = now; g_libraryDirty = true; }
    }
    if (g_libraryDirty) scan_library();
    if (ImGui::Button("Add files...")) for (auto& f : pick_files_multi()) library_import(f);
    ImGui::SameLine(); if (ImGui::Button("+ image")) g_openGenImg = true;
    ImGui::SameLine(); if (ImGui::Button("+ voice")) g_openGenVoice = true;
    ImGui::SameLine(); if (ImGui::Button("+ avatar")) g_openNewAvatar = true;   // author a rig (prefix + overrides)
    if (ImGui::SmallButton("Refresh")) g_libraryDirty = true;
    ImGui::SameLine(); ImGui::TextDisabled("%d items", (int)g_library.size());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##libsearch", "search by name...", g_libSearch, sizeof g_libSearch);
    ImGui::RadioButton("all", &g_libTypeFilter, 0); ImGui::SameLine();
    ImGui::RadioButton("img", &g_libTypeFilter, 1); ImGui::SameLine();
    ImGui::RadioButton("aud", &g_libTypeFilter, 2); ImGui::SameLine();
    ImGui::RadioButton("vid", &g_libTypeFilter, 3); ImGui::SameLine();
    ImGui::RadioButton("rig", &g_libTypeFilter, 4);
    if (!g_projLibDir.empty()) {   // scope: this video's own assets vs the cross-video library
        ImGui::TextDisabled("in:"); ImGui::SameLine();
        ImGui::RadioButton("both", &g_libScope, 0); ImGui::SameLine();
        ImGui::RadioButton("project", &g_libScope, 1); ImGui::SameLine();
        ImGui::RadioButton("common", &g_libScope, 2); ImGui::SameLine();
        ImGui::TextDisabled(g_libScope == 2 ? "(new items -> library/)" : "(new items -> project)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("imports + generations land in %s",
                                                      g_libScope == 2 ? g_libraryDir.c_str() : g_projLibDir.c_str());
    }
    // active library-gen status (a NEW gen has no grid cell until it lands)
    { std::string busy;
      EnterCriticalSection(&g_genCS);
      for (auto& kv : g_libGen) if (kv.second.state == 1) {
          size_t s = kv.first.find_last_of('/');
          busy = (s == std::string::npos ? kv.first : kv.first.substr(s + 1)) + " - " + kv.second.message; break; }
      LeaveCriticalSection(&g_genCS);
      if (!busy.empty()) ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1), "generating: %s", busy.c_str()); }
    ImGui::Separator();
    std::string needle = lower_str(g_libSearch);
    ImGui::BeginChild("libgrid");
        const float cell = 92.f, padx = 8.f;
        int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + padx)));
        int shown = 0;
        for (auto& it : g_library) {
            if (g_libTypeFilter == 1 && it.type != LIB_IMAGE) continue;
            if (g_libTypeFilter == 2 && it.type != LIB_AUDIO) continue;
            if (g_libTypeFilter == 3 && it.type != LIB_VIDEO) continue;
            if (g_libTypeFilter == 4 && it.type != LIB_AVATAR) continue;
            if (g_libScope == 2 && it.proj) continue;                             // "common" = shared library only
            if (g_libScope != 2) {   // "both"/"project": hide cross-project JUNK but keep the wanted shared resources
                // superseded pre-GPT rigs (the "old sprites" the owner sees) — only reachable via "common".
                static const char* LEGACY_RIGS[] = {"gemma-chibi", "gemma-pngtuber", "gemma-host", "gemma-host-teacher-test"};
                if (it.type == LIB_AVATAR) { bool leg = false; for (auto* n : LEGACY_RIGS) if (it.name == n) { leg = true; break; }
                                             if (leg) continue; }
                if (!it.proj) {
                    // shared library/ items: keep music/audio beds + current rigs (+ shared video); hide the loose
                    // library/images legacy sprites and any stray Pictures imports. "project" scope shows rigs only.
                    bool keep = (it.type == LIB_AUDIO) || (it.type == LIB_AVATAR) || (it.type == LIB_VIDEO);
                    if (g_libScope == 1) keep = (it.type == LIB_AVATAR);
                    if (!keep) continue;
                }
            }
            if (!needle.empty() && lower_str(it.name).find(needle) == std::string::npos) continue;
            ImGui::PushID(it.path.c_str());
            ImGui::BeginGroup();
            bool clicked = false;
            if (it.type == LIB_IMAGE) {
                Tex* t = get_texture(it.path);
                if (t && t->srv) {
                    ImVec4 bg = (g_libSelected == it.path) ? ImVec4(0.28f, 0.5f, 0.85f, 1) : ImVec4(0.10f, 0.10f, 0.12f, 1);
                    // square center-crop ("cover"): fit the SHORTER dim, crop the longer — never squash aspect.
                    ImVec2 uv0(0, 0), uv1(1, 1);
                    float tw = (float)t->w, th = (float)t->h;
                    if (tw > th) { float d = (1.f - th / tw) * 0.5f; uv0.x = d; uv1.x = 1.f - d; }
                    else if (th > tw) { float d = (1.f - tw / th) * 0.5f; uv0.y = d; uv1.y = 1.f - d; }
                    clicked = ImGui::ImageButton("t", (ImTextureID)(intptr_t)t->srv, ImVec2(cell, cell), uv0, uv1, bg);
                } else clicked = ImGui::Button("img?", ImVec2(cell, cell));
            } else if (it.type == LIB_AVATAR) {            // rig def → thumbnail = its fallback pose (teal cell)
                const AvatarRig* rg = get_rig(it.name);   // cached; resolves library/avatars/<name>.avatar.json
                Tex* t = nullptr;
                if (rg) {
                    auto mi = rg->mouths.find(rg->fallback);
                    if (mi == rg->mouths.end() && !rg->mouths.empty()) mi = rg->mouths.begin();
                    if (mi != rg->mouths.end() && !mi->second.empty()) t = get_texture(mi->second[0]);
                }
                ImVec4 bg = (g_libSelected == it.path) ? ImVec4(0.20f, 0.66f, 0.60f, 1) : ImVec4(0.07f, 0.15f, 0.14f, 1);
                if (t && t->srv) {
                    ImVec2 uv0(0, 0), uv1(1, 1); float tw = (float)t->w, th = (float)t->h;
                    if (tw > th) { float d = (1.f - th / tw) * 0.5f; uv0.x = d; uv1.x = 1.f - d; }
                    else if (th > tw) { float d = (1.f - tw / th) * 0.5f; uv0.y = d; uv1.y = 1.f - d; }
                    clicked = ImGui::ImageButton("t", (ImTextureID)(intptr_t)t->srv, ImVec2(cell, cell), uv0, uv1, bg);
                } else clicked = ImGui::Button("rig", ImVec2(cell, cell));
            } else {
                clicked = ImGui::Button(it.type == LIB_AUDIO ? "audio" : "video", ImVec2(cell, cell));
            }
            if (clicked) g_libSelected = it.path;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) library_add_to_timeline(p, st, it);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("LIB_ITEM", it.path.c_str(), it.path.size() + 1);
                ImGui::TextUnformatted(it.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("ctx")) {
                if (ImGui::MenuItem("Add to timeline")) library_add_to_timeline(p, st, it);
                if (it.type != LIB_AVATAR && ImGui::MenuItem("Rename…")) {   // (rigs: delete + recreate — rename would orphan `rig` refs)
                    g_libRenamePath = it.path; g_openLibRename = true;
                    std::string b = it.name; size_t d = b.find_last_of('.'); if (d != std::string::npos) b = b.substr(0, d);
                    strncpy(g_libRenameBuf, b.c_str(), sizeof g_libRenameBuf - 1); g_libRenameBuf[sizeof g_libRenameBuf - 1] = 0;
                }
                if (!it.preset && ImGui::MenuItem("Delete")) library_delete(it);   // can't delete a bundled preset rig
                ImGui::EndPopup();
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
            if (it.type == LIB_AVATAR) ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.78f, 1), "%s", it.name.c_str());  // rig (teal)
            else if (it.gen) ImGui::TextColored(ImVec4(0.80f, 0.64f, 0.96f, 1), "%s", it.name.c_str());  // gen item (purple)
            else if (it.proj) ImGui::TextColored(ImVec4(0.94f, 0.80f, 0.55f, 1), "%s", it.name.c_str());  // project item (amber)
            else        ImGui::TextWrapped("%s", it.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();
            if (++shown % cols) ImGui::SameLine(); else ImGui::Dummy(ImVec2(0, 2));
        }
        // placeholder cells for NEW gens still in flight (no file on disk yet → not in g_library)
        for (auto& pg : libgen_pending()) {
            if (g_libTypeFilter == 1 && pg.type != LIB_IMAGE) continue;
            if (g_libTypeFilter == 2 && pg.type != LIB_AUDIO) continue;
            if (g_libTypeFilter == 3 && pg.type != LIB_VIDEO) continue;
            if (!needle.empty() && lower_str(pg.name).find(needle) == std::string::npos) continue;
            bool landed = false;
            for (auto& it : g_library) if (strip_ext(it.path) == pg.key) { landed = true; break; }
            if (landed) continue;                       // already on disk → its real cell shows it
            ImGui::PushID(pg.key.c_str());
            ImGui::BeginGroup();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(cell, cell));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell), IM_COL32(40, 38, 52, 255), 4.f);
            dl->AddRect(p0, ImVec2(p0.x + cell, p0.y + cell), IM_COL32(120, 100, 170, 255), 4.f);
            char pct[16]; snprintf(pct, sizeof pct, "%d%%", (int)(pg.progress * 100.f));
            ImVec2 ts = ImGui::CalcTextSize(pct);
            dl->AddText(ImVec2(p0.x + (cell - ts.x) * 0.5f, p0.y + cell * 0.5f - 14), IM_COL32(200, 190, 230, 255), pct);
            const char* g = "gen…"; ts = ImGui::CalcTextSize(g);
            dl->AddText(ImVec2(p0.x + (cell - ts.x) * 0.5f, p0.y + cell * 0.5f + 2), IM_COL32(150, 140, 180, 255), g);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1), "%s", pg.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();
            if (++shown % cols) ImGui::SameLine(); else ImGui::Dummy(ImVec2(0, 2));
        }
        if (!shown) ImGui::TextDisabled(g_library.empty() ? "empty — drop files in library/ or click Add files…" : "no matches");
    ImGui::EndChild();

    if (g_openLibRename) { ImGui::OpenPopup("Rename library item"); g_openLibRename = false; }
    if (ImGui::BeginPopupModal("Rename library item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("new name", g_libRenameBuf, sizeof g_libRenameBuf);
        if (ImGui::Button("Rename", ImVec2(120, 0)) && g_libRenameBuf[0]) {
            for (auto& it : g_library) if (it.path == g_libRenamePath) { library_rename(it, g_libRenameBuf); break; }
            g_libRenamePath.clear(); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_libRenamePath.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    draw_lib_gen_modals();
}

// ── Library viewer (L5 + L4): a detail panel for the selected library item. Image = pan (drag) +
// zoom (wheel, anchored at the cursor; double-click resets to fit). Audio = waveform + a draggable
// scrub marker + Play/Stop (WAV, whole-file). When the item is a GEN item (has a recipe sidecar),
// also shows its editable recipe + Regenerate (fresh) + an expandable history of past gens you can
// restore (L4). The detail view the global/per-project library needs for human fine-tuning. ──
static bool g_showViewer = true;
static float g_vZoom = 0.f;          // <=0 → fit-to-window next draw
static ImVec2 g_vPan(0, 0);
static float g_vAudioPos = 0.f;      // 0..1 scrub marker (position in the clip)
static float g_vAudioZoom = 1.f;     // waveform horizontal zoom (1 = whole clip fits)
static float g_vAudioView0 = 0.f;    // left edge of the visible window (0..1 of the clip)
static std::string g_vFor;           // selection the pan/zoom/scrub state is for (reset on change)
// g_vSideFor is defined earlier (with the rembg state) so rembg_poll_done can force a reload.
static json g_vSide, g_vRecipe;      // the loaded sidecar + its editable gen-params copy
static bool g_vPickBg = false;       // eyedropper armed → next canvas click picks the bg key colour
// ── crude crop/mask paint tool (the Viewer): paint-to-erase what removebg misses + crop. The mask
//    is a grayscale buffer at source dims (255 keep / 0 erased); the live preview is the working
//    image × mask; commit writes <file>.mask.png + the sidecar crop/mask blocks (non-destructive). ──
static bool g_vMaskMode = false;     // tool panel active for the selected image
static int  g_vMaskTool = 0;         // 0 erase brush · 1 restore brush · 2 box erase · 3 crop
static float g_vBrush = 36.f, g_vFeather = 0.4f;       // brush radius (screen px) + feather 0..1
static std::vector<unsigned char> g_vMaskBuf, g_vMaskImg;  // grayscale mask + working RGBA (post-removebg)
static int g_vMaskW = 0, g_vMaskH = 0;
static std::string g_vMaskFor;       // which item the buffers belong to
static bool g_vMaskDirty = false;    // preview SRV needs rebuild
static ID3D11ShaderResourceView* g_vMaskSrv = nullptr;    // live preview (working img × mask)
static ImVec2 g_vDragA(0, 0), g_vLastPaint(-1, -1); static bool g_vRubber = false;  // box/crop rubber-band + brush interp
static bool g_vMaskArm = false;      // --mask-mode: re-enable the tool after the selection-change reset (shots)

// Restore a past gen (history[idx]) as the current one: copy its cached bytes over the library file
// and swap the sidecar (the current gen demotes into history). Needs the cached bytes (gitignored —
// best-effort; if the cache was cleared, no-op). UI thread.
static void lib_restore_gen(const std::string& itemPath, int idx) {
    json hist = g_vSide.value("history", json::array());
    if (idx < 0 || idx >= (int)hist.size()) return;
    json pick = hist[idx];
    std::string prov = pick.value("provider", std::string()), hash = pick.value("hash", std::string()),
                ext = pick.value("ext", std::string("png"));
    std::string src = g_cacheDir + "/" + prov + "/" + hash + "." + ext;
    if (GetFileAttributesW(to_w(src).c_str()) == INVALID_FILE_ATTRIBUTES) return;  // bytes gone
    json cur = json{{"hash", g_vSide.value("hash", std::string())}, {"ext", g_vSide.value("ext", std::string("png"))},
                    {"provider", g_vSide.value("provider", std::string())}, {"params", g_vSide.value("params", json::object())}};
    hist.erase(hist.begin() + idx);
    hist.insert(hist.begin(), cur);
    g_vSide["hash"] = hash; g_vSide["ext"] = ext; g_vSide["provider"] = prov;
    g_vSide["params"] = pick.value("params", json::object());
    g_vSide["history"] = hist;
    g_vRecipe = g_vSide["params"];
    CopyFileW(to_w(src).c_str(), to_w(itemPath).c_str(), FALSE);
    lib_save_sidecar(itemPath, g_vSide);
    invalidate_texture(itemPath); g_pcmCache.erase(itemPath);
    g_libraryDirty = true;
}

// ── Viewer panel: avatar-rig editor (library/avatars/<name>.avatar.json) ──
// Edit the prefix + per-emotion image overrides; thumbnails resolve LIVE from the in-memory state
// (override > prefix), and every change saves the def + drops the rig cache so the compositor re-
// resolves. The canonical emotion set is always shown (missing ones flagged); add a custom emotion
// by name. (This satisfies "set a prefix plus optionally select specific images that override a
// manually entered emotion name.")
static std::string g_arFor;                 // which rig path is loaded into the editing buffers
static char g_arPrefix[96] = "";
static json g_arEmotions = json::object();  // emotion → library-image filename (overrides)
static char g_arNewEmo[64] = "";

static void avatar_rig_save(const std::string& name) {
    json j; j["schema"] = "slopstudio.avatar_rig/1"; j["prefix"] = std::string(g_arPrefix);
    j["fallback"] = "neutral"; j["emotions"] = g_arEmotions;
    save_avatar_rig(name, j);                // writes + invalidate_rig + g_libraryDirty
}
static void draw_avatar_rig_editor(const std::string& path) {
    std::string fn = path.substr(path.find_last_of("/\\") + 1);
    std::string name = fn.substr(0, fn.size() - strlen(AVATAR_RIG_EXT));
    if (g_arFor != path) {                   // (re)load the editing buffers on selection change
        g_arFor = path;
        json j = load_avatar_rig(name);
        std::string pf = j.value("prefix", std::string());
        strncpy(g_arPrefix, pf.c_str(), sizeof g_arPrefix - 1); g_arPrefix[sizeof g_arPrefix - 1] = 0;
        g_arEmotions = j.value("emotions", json::object());
        if (!g_arEmotions.is_object()) g_arEmotions = json::object();
        g_arNewEmo[0] = 0;
    }
    ImGui::Text("%s", name.c_str()); ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.78f, 1), "· avatar rig");
    ImGui::Separator();
    bool changed = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##prefix", g_arPrefix, sizeof g_arPrefix)) changed = true;
    ImGui::TextDisabled("prefix: emotion E → library/images/<prefix>E.png  ·  overrides win");

    auto resolve = [&](const std::string& emo) -> std::string {   // live: override > prefix
        std::string file = (g_arEmotions.contains(emo) && g_arEmotions[emo].is_string()) ? g_arEmotions[emo].get<std::string>() : std::string();
        if (file.empty() && g_arPrefix[0]) file = std::string(g_arPrefix) + emo + ".png";
        if (file.empty()) return "";
        std::string full = g_libraryDir + "/images/" + file;
        return GetFileAttributesW(to_w(full).c_str()) != INVALID_FILE_ATTRIBUTES ? full : "";
    };
    std::vector<std::string> emos(AVATAR_EMOS, AVATAR_EMOS + AVATAR_EMO_N);
    for (auto& kv : g_arEmotions.items()) if (std::find(emos.begin(), emos.end(), kv.key()) == emos.end()) emos.push_back(kv.key());

    ImGui::BeginChild("rigframes", ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y - 70)), true);
    const float th = 56.f;
    for (auto& emo : emos) {
        ImGui::PushID(emo.c_str());
        bool over = g_arEmotions.contains(emo);
        std::string full = resolve(emo);
        Tex* t = full.empty() ? nullptr : get_texture(full);
        if (t && t->srv) {
            ImVec2 uv0(0, 0), uv1(1, 1); float tw = (float)t->w, ht = (float)t->h;
            if (tw > ht) { float d = (1.f - ht / tw) * 0.5f; uv0.x = d; uv1.x = 1.f - d; }
            else if (ht > tw) { float d = (1.f - tw / ht) * 0.5f; uv0.y = d; uv1.y = 1.f - d; }
            ImGui::Image((ImTextureID)(intptr_t)t->srv, ImVec2(th, th), uv0, uv1);
        } else { ImVec2 pp = ImGui::GetCursorScreenPos(); ImGui::Dummy(ImVec2(th, th));
                 ImGui::GetWindowDrawList()->AddRect(pp, ImVec2(pp.x + th, pp.y + th), IM_COL32(90, 70, 70, 255)); }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s%s", emo.c_str(), over ? "  (override)" : "");
        if (full.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "missing");
        std::string cur = over && g_arEmotions[emo].is_string() ? g_arEmotions[emo].get<std::string>() : std::string("(prefix)");
        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("##img", cur.c_str())) {
            if (ImGui::Selectable("(use prefix)", !over) && over) { g_arEmotions.erase(emo); changed = true; }
            for (auto& li : g_library) if (li.type == LIB_IMAGE)
                if (ImGui::Selectable(li.name.c_str(), over && cur == li.name)) { g_arEmotions[emo] = li.name; changed = true; }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(120); ImGui::InputTextWithHint("##newemo", "emotion name", g_arNewEmo, sizeof g_arNewEmo);
    ImGui::SameLine();
    if (ImGui::Button("Add override") && g_arNewEmo[0]) {
        std::string img; for (auto& li : g_library) if (li.type == LIB_IMAGE) { img = li.name; break; }
        if (!img.empty()) { g_arEmotions[std::string(g_arNewEmo)] = img; changed = true; g_arNewEmo[0] = 0; }
    }
    ImGui::TextDisabled("Drop the rig onto the timeline (or double-click) → an avatar clip; clips on its row share these frames.");
    if (changed) avatar_rig_save(name);      // live-save → cache invalidated → preview updates next frame
}

// ── crop/mask paint-tool helpers (the Viewer) ──
// Load the working buffers for the selected image: source RGBA after removebg (the cutout base the
// mask refines) + the saved mask (or blank). Marks the preview dirty.
static void vmask_load(const std::string& path) {
    g_vMaskImg.clear(); g_vMaskBuf.clear(); g_vMaskW = g_vMaskH = 0; g_vMaskFor = path;
    int w, h, n; unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!d) return;
    std::vector<unsigned char> keyed;
    if (item_removebg(path, d, w, h, keyed)) g_vMaskImg.swap(keyed);
    else g_vMaskImg.assign(d, d + (size_t)w * h * 4);
    stbi_image_free(d);
    g_vMaskW = w; g_vMaskH = h;
    g_vMaskBuf = mask_load_or_blank(path, w, h);
    g_vMaskDirty = true;
}
// Rebuild the live preview SRV = working image with alpha × mask (releases the old).
static void vmask_rebuild() {
    if (g_vMaskSrv) { g_vMaskSrv->Release(); g_vMaskSrv = nullptr; }
    if (g_vMaskImg.empty() || (int)g_vMaskBuf.size() != g_vMaskW * g_vMaskH) { g_vMaskDirty = false; return; }
    std::vector<unsigned char> px = g_vMaskImg;
    for (size_t i = 0; i < (size_t)g_vMaskW * g_vMaskH; i++) px[i * 4 + 3] = (unsigned char)((int)px[i * 4 + 3] * g_vMaskBuf[i] / 255);
    g_vMaskSrv = make_rgba_srv(px.data(), g_vMaskW, g_vMaskH);
    g_vMaskDirty = false;
}
// Commit the painted mask: write <file>.mask.png + the sidecar mask block, invalidate the cached
// texture so the cutout flows to grid/timeline/export. Reloads g_vSide so the panel agrees.
static void vmask_commit(const std::string& path) {
    mask_save(path, g_vMaskBuf, g_vMaskW, g_vMaskH);
    invalidate_texture(path);
    g_vSide = lib_load_sidecar(path);
}
// Leave the tool / free the preview texture (item switch, toggle off).
static void vmask_reset() {
    if (g_vMaskSrv) { g_vMaskSrv->Release(); g_vMaskSrv = nullptr; }
    g_vMaskImg.clear(); g_vMaskBuf.clear(); g_vMaskFor.clear(); g_vMaskW = g_vMaskH = 0;
    g_vRubber = false; g_vLastPaint = ImVec2(-1, -1);
}

// Read-only Viewer for a bundled preset rig (presets/avatars/<name>/manifest.json): the rig name, a
// "drag onto the timeline to place" hint, and a face-cropped grid of its emotion poses.
static void draw_preset_rig_view(const std::string& path) {
    std::string name = path;
    if (name.size() > 14) name = name.substr(0, name.size() - 14);            // strip /manifest.json
    size_t sl = name.find_last_of("/\\"); if (sl != std::string::npos) name = name.substr(sl + 1);
    const AvatarRig* rig = get_rig(name);
    ImGui::Text("preset rig: %s", name.c_str());
    ImGui::TextDisabled("read-only (bundled). Double-click or drag onto the timeline to place.");
    ImGui::Separator();
    if (!rig) { ImGui::TextDisabled("rig failed to load"); return; }
    const float tile = 84.f, pad = 8.f;
    int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (tile + pad)));
    int shown = 0;
    for (int i = 0; i < AVATAR_EMO_N; i++) {
        std::string ipath = avatar_sprite_path(rig, AVATAR_EMOS[i]);
        if (ipath.empty()) continue;
        EmoThumb th;
        ImGui::PushID(i);
        ImGui::BeginGroup();
        if (get_emo_thumb(ipath, th) && th.srv)   // async — placeholder until the thumbnail loads
            ImGui::Image((ImTextureID)(intptr_t)th.srv, ImVec2(tile, tile), th.uv0, th.uv1);
        else
            ImGui::Dummy(ImVec2(tile, tile));
        ImGui::TextDisabled("%s", AVATAR_EMOS[i]);
        ImGui::EndGroup();
        ImGui::PopID();
        if (++shown % cols) ImGui::SameLine(); else ImGui::Dummy(ImVec2(0, 2));
    }
}

static void draw_viewer_contents() {
    const std::string path = g_libSelected;
    if (path.empty()) { ImGui::TextDisabled("select a library item to view / regenerate."); return; }
    { size_t rel = strlen(AVATAR_RIG_EXT);   // avatar rig def → its own editor (prefix + overrides)
      if (path.size() > rel && path.compare(path.size() - rel, rel, AVATAR_RIG_EXT) == 0) {
          draw_avatar_rig_editor(path); return; } }
    if (path.size() > 14 && path.compare(path.size() - 14, 14, "/manifest.json") == 0) {   // bundled preset rig
        draw_preset_rig_view(path); return; }
    if (g_vFor != path) { g_vFor = path; g_vZoom = 0; g_vPan = ImVec2(0, 0);
                          g_vAudioPos = 0; g_vAudioZoom = 1; g_vAudioView0 = 0; g_vPickBg = false; audition_stop();
                          g_vMaskMode = false; vmask_reset();
                          if (g_vMaskArm) { g_vMaskMode = true; g_vMaskArm = false; } }

    // type by ext (the item may be a NEW gen not yet in g_library)
    std::string e = ext_lower(path.substr(path.find_last_of("/\\") + 1));
    LibType type = lib_type_of(e);
    std::string base = strip_ext(path.substr(path.find_last_of("/\\") + 1));
    std::string key = strip_ext(path);

    // editable sidecar (recipe + removebg), loaded once per selection
    if (g_vSideFor != path) { g_vSideFor = path; g_vSide = lib_load_sidecar(path); g_vRecipe = g_vSide.value("params", json::object()); }
    // a gen in flight / just-finished for THIS item → reload the sidecar when it lands
    LibGenState gs = libgen_get(key);
    if (gs.state == 2) { g_vSide = lib_load_sidecar(path); g_vRecipe = g_vSide.value("params", json::object());
                         invalidate_texture(path); g_pcmCache.erase(path); libgen_set(key, 0, 0, ""); }

    bool gen = lib_is_gen(g_vSide);
    ImGui::Text("%s", base.c_str());
    ImGui::SameLine();
    if (gen) ImGui::TextColored(ImVec4(0.80f, 0.64f, 0.96f, 1), "· gen [%s]", g_vSide.value("provider", std::string("?")).c_str());
    else     ImGui::TextDisabled("· %s", type == LIB_IMAGE ? "image" : type == LIB_AUDIO ? "audio" : "video");
    if (gs.state == 1) ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1), "generating — %s %.0f%%", gs.message.c_str(), gs.progress * 100.f);
    else if (gs.state == 3) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "ERR: %s", gs.message.c_str());
    ImGui::Separator();

    float footH = gen ? ImGui::GetContentRegionAvail().y * 0.46f : 0.f;   // reserve for the recipe panel
    // ── media region ──
    if (type == LIB_AUDIO) {
        Pcm* pc = (gs.state == 1) ? nullptr : get_pcm(path);
        ImVec2 wp = ImGui::GetCursorScreenPos();
        float W = ImGui::GetContentRegionAvail().x, H = 120;
        ImGui::InvisibleButton("wave", ImVec2(W, H));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + H), IM_COL32(18, 20, 26, 255));
        if (pc && !pc->mono.empty()) {
            ImGuiIO& io = ImGui::GetIO();
            bool dragging = ImGui::IsItemActive();
            // advance the marker with the play cursor while auditioning (auto-follow when zoomed);
            // the drag owns the marker while held, so don't fight it.
            double apos = audition_pos();
            if (apos >= 0 && !dragging) {
                g_vAudioPos = pc->dur > 0 ? (float)(apos / pc->dur) : 0.f;
                if (g_vAudioPos > 1) g_vAudioPos = 1;
                float span = 1.f / g_vAudioZoom;
                if (g_vAudioPos < g_vAudioView0 || g_vAudioPos > g_vAudioView0 + span)
                    g_vAudioView0 = g_vAudioPos - span * 0.5f;
            }
            // wheel = zoom (anchored at cursor); Shift+wheel = scroll the window
            if (ImGui::IsItemHovered() && io.MouseWheel != 0.f) {
                float span = 1.f / g_vAudioZoom;
                if (io.KeyShift) {
                    g_vAudioView0 -= io.MouseWheel * span * 0.15f;
                } else {
                    float curN = g_vAudioView0 + ((io.MousePos.x - wp.x) / W) * span;  // clip-norm under cursor
                    g_vAudioZoom *= powf(1.2f, io.MouseWheel);
                    if (g_vAudioZoom < 1.f) g_vAudioZoom = 1.f; if (g_vAudioZoom > 2000.f) g_vAudioZoom = 2000.f;
                    span = 1.f / g_vAudioZoom;
                    g_vAudioView0 = curN - ((io.MousePos.x - wp.x) / W) * span;
                }
            }
            float span = 1.f / g_vAudioZoom;
            if (g_vAudioView0 < 0) g_vAudioView0 = 0;
            if (g_vAudioView0 > 1 - span) g_vAudioView0 = 1 - span;
            int N = (int)pc->mono.size(); float mid = wp.y + H * 0.5f;
            for (int x = 0; x < (int)W; x++) {                        // window [view0, view0+span] of the clip
                double n0 = g_vAudioView0 + ((double)x / W) * span, n1 = g_vAudioView0 + ((double)(x + 1) / W) * span;
                int s0 = (int)(n0 * N), s1 = (int)(n1 * N);
                if (s1 <= s0) s1 = s0 + 1; if (s1 > N) s1 = N; if (s0 < 0) s0 = 0;
                float mn = 0, mx = 0; for (int s = s0; s < s1; s++) { float v = pc->mono[s]; if (v < mn) mn = v; if (v > mx) mx = v; }
                dl->AddLine(ImVec2(wp.x + x, mid - mx * H * 0.46f), ImVec2(wp.x + x, mid - mn * H * 0.46f), IM_COL32(110, 165, 235, 255));
            }
            // click/drag in the waveform → scrub to that position (within the visible window);
            // re-seek the audition to the marker on release if it was playing.
            if (dragging) {
                float rx = (io.MousePos.x - wp.x) / W;
                g_vAudioPos = g_vAudioView0 + (rx < 0 ? 0 : rx > 1 ? 1 : rx) * span;
            }
            if (ImGui::IsItemDeactivated() && g_audPlaying) audition_play(pc, g_vAudioPos * pc->dur);
            float mpx = (g_vAudioPos - g_vAudioView0) / span;          // marker in window space
            if (mpx >= 0 && mpx <= 1)
                dl->AddLine(ImVec2(wp.x + mpx * W, wp.y), ImVec2(wp.x + mpx * W, wp.y + H), IM_COL32(255, 210, 90, 255), 2.f);
            ImGui::Text("%.2f / %.2f s", g_vAudioPos * pc->dur, pc->dur);
            ImGui::SameLine(); ImGui::TextDisabled("· zoom %.0f×", g_vAudioZoom);
            if (ImGui::Button(g_audPlaying ? "Pause" : "Play from marker")) {
                if (g_audPlaying) audition_stop(); else audition_play(pc, g_vAudioPos * pc->dur);
            }
            ImGui::SameLine(); if (ImGui::Button("Stop")) { audition_stop(); g_vAudioPos = 0; }
            ImGui::SameLine(); ImGui::TextDisabled("(WAV · wheel=zoom, Shift+wheel=scroll, drag=scrub)");
        } else ImGui::TextDisabled(gs.state == 1 ? "(generating…)" : e == "wav" ? "(decoding…)" : "preview is WAV-only (mp3/ogg play on export)");
    } else if (type == LIB_IMAGE) {
        float canvasReserve = footH + (g_vMaskMode ? 150.f : 0.f);   // keep the paint-tool controls on-screen
        ImGui::BeginChild("canvas", ImVec2(0, ImGui::GetContentRegionAvail().y - canvasReserve), true, ImGuiWindowFlags_NoScrollWithMouse);
        ImVec2 c0 = ImGui::GetCursorScreenPos(), cs = ImGui::GetContentRegionAvail();
        Tex* t = (gs.state == 1) ? nullptr : get_texture(path);  // don't fopen a not-yet-landed gen
        if (t && t->srv && cs.x > 4 && cs.y > 4) {
            // mask/crop tool works in FULL source space (the preview is the uncropped image so you
            // can paint + draw a crop rect); lazy-load the working buffers on enter.
            if (g_vMaskMode && (g_vMaskFor != path || g_vMaskW == 0)) vmask_load(path);
            bool masking = g_vMaskMode && g_vMaskFor == path && g_vMaskW > 0;
            float iw = masking ? (float)g_vMaskW : (float)t->w, ih = masking ? (float)g_vMaskH : (float)t->h;
            if (g_vZoom <= 0.f) { float fit = std::min(cs.x / iw, cs.y / ih); g_vZoom = fit > 0 ? fit : 1.f; g_vPan = ImVec2(0, 0); }
            ImGui::InvisibleButton("cv", cs);
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 ctr(c0.x + cs.x * 0.5f + g_vPan.x, c0.y + cs.y * 0.5f + g_vPan.y);
            if (ImGui::IsItemHovered() && io.MouseWheel != 0.f) {
                float f = powf(1.12f, io.MouseWheel), nz = g_vZoom * f;
                nz = nz < 0.02f ? 0.02f : nz > 64.f ? 64.f : nz;
                ImVec2 rel((io.MousePos.x - ctr.x) / g_vZoom, (io.MousePos.y - ctr.y) / g_vZoom);  // image-space under cursor
                g_vPan.x = io.MousePos.x - rel.x * nz - (c0.x + cs.x * 0.5f);
                g_vPan.y = io.MousePos.y - rel.y * nz - (c0.y + cs.y * 0.5f);
                g_vZoom = nz;
                ctr = ImVec2(c0.x + cs.x * 0.5f + g_vPan.x, c0.y + cs.y * 0.5f + g_vPan.y);
            }
            float dw = iw * g_vZoom, dh = ih * g_vZoom;
            ImVec2 a(ctr.x - dw * 0.5f, ctr.y - dh * 0.5f), b(ctr.x + dw * 0.5f, ctr.y + dh * 0.5f);
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            // checkerboard behind a cutout so the keyed/erased transparency reads (clamped to canvas∩image)
            if (g_vSide.contains("removebg") || masking) {
                float x0 = std::max(a.x, c0.x), y0 = std::max(a.y, c0.y),
                      x1 = std::min(b.x, c0.x + cs.x), y1 = std::min(b.y, c0.y + cs.y);
                for (float yy = y0; yy < y1; yy += 8.f)
                    for (float xx = x0; xx < x1; xx += 8.f) {
                        bool dk = ((int)((xx - a.x) / 8.f) + (int)((yy - a.y) / 8.f)) & 1;
                        cdl->AddRectFilled(ImVec2(xx, yy), ImVec2(std::min(xx + 8.f, x1), std::min(yy + 8.f, y1)),
                                           dk ? IM_COL32(64, 64, 70, 255) : IM_COL32(104, 104, 112, 255));
                    }
            }
            if (masking) {                                    // ── crop/mask paint tool ──
                if (g_vMaskDirty) vmask_rebuild();
                auto toImg = [&](ImVec2 m) { return ImVec2((m.x - a.x) / g_vZoom, (m.y - a.y) / g_vZoom); };
                bool hov = ImGui::IsItemHovered();
                // pan with MIDDLE/RIGHT drag (left button is the tool; the canvas button is left-only,
                // so these aren't captured by it → drive pan off hover + the raw drag).
                if (hov && (ImGui::IsMouseDragging(2) || ImGui::IsMouseDragging(1))) { g_vPan.x += io.MouseDelta.x; g_vPan.y += io.MouseDelta.y; }
                if (g_vMaskTool <= 1) {                        // brush: 0 erase → 0, 1 restore → 255
                    int val = g_vMaskTool == 0 ? 0 : 255;
                    if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
                        ImVec2 ip = toImg(io.MousePos); float r = g_vBrush / g_vZoom;
                        if (g_vLastPaint.x >= 0) {            // interpolate along the stroke so fast drags don't gap
                            float dx = ip.x - g_vLastPaint.x, dy = ip.y - g_vLastPaint.y, dist = sqrtf(dx * dx + dy * dy);
                            int steps = (int)(dist / std::max(1.f, r * 0.4f));
                            for (int s = 1; s <= steps; s++) mask_dab(g_vMaskBuf, g_vMaskW, g_vMaskH, g_vLastPaint.x + dx * s / steps, g_vLastPaint.y + dy * s / steps, r, g_vFeather, val);
                        }
                        mask_dab(g_vMaskBuf, g_vMaskW, g_vMaskH, ip.x, ip.y, r, g_vFeather, val);
                        g_vLastPaint = ip; g_vMaskDirty = true;
                    }
                    if (ImGui::IsItemDeactivated()) { g_vLastPaint = ImVec2(-1, -1); vmask_commit(path); }  // commit on stroke release
                } else {                                       // box erase (2) / crop (3): rubber-band rect
                    if (ImGui::IsItemActivated()) { g_vDragA = io.MousePos; g_vRubber = true; }
                    if (g_vRubber && ImGui::IsItemDeactivated()) {
                        ImVec2 p0 = toImg(g_vDragA), p1 = toImg(io.MousePos);
                        int rx = (int)std::min(p0.x, p1.x), ry = (int)std::min(p0.y, p1.y), rw = (int)fabsf(p1.x - p0.x), rh = (int)fabsf(p1.y - p0.y);
                        if (rw > 2 && rh > 2) {
                            if (g_vMaskTool == 2) { mask_box(g_vMaskBuf, g_vMaskW, g_vMaskH, rx, ry, rw, rh, 0); g_vMaskDirty = true; vmask_commit(path); }
                            else { json side = lib_load_sidecar(path); side["crop"] = json::array({rx, ry, rw, rh}); lib_save_sidecar(path, side); g_vSide = side; invalidate_texture(path); }
                        }
                        g_vRubber = false;
                    }
                }
                cdl->AddImage((ImTextureID)(intptr_t)(g_vMaskSrv ? g_vMaskSrv : t->srv), a, b);
                if (g_vSide.contains("crop") && g_vSide["crop"].is_array() && g_vSide["crop"].size() == 4) {   // current crop boundary
                    float cx0 = a.x + g_vSide["crop"][0].get<float>() * g_vZoom, cy0 = a.y + g_vSide["crop"][1].get<float>() * g_vZoom;
                    cdl->AddRect(ImVec2(cx0, cy0), ImVec2(cx0 + g_vSide["crop"][2].get<float>() * g_vZoom, cy0 + g_vSide["crop"][3].get<float>() * g_vZoom), IM_COL32(120, 200, 255, 230), 0, 0, 2.f);
                }
                if (g_vRubber) cdl->AddRect(g_vDragA, io.MousePos, g_vMaskTool == 3 ? IM_COL32(120, 200, 255, 255) : IM_COL32(255, 120, 120, 255), 0, 0, 2.f);
                if (hov && g_vMaskTool <= 1) { ImGui::SetMouseCursor(ImGuiMouseCursor_None); cdl->AddCircle(io.MousePos, g_vBrush, IM_COL32(255, 255, 255, 220), 28, 1.5f); }
            } else {
                if (g_vPickBg) {                              // eyedropper armed → click samples the bg key
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        if (ImGui::IsMouseClicked(0)) {
                            int px = (int)((io.MousePos.x - a.x) / g_vZoom), py = (int)((io.MousePos.y - a.y) / g_vZoom);
                            int iw2, ih2, nn; unsigned char* d = stbi_load(path.c_str(), &iw2, &ih2, &nn, 4);
                            if (d && px >= 0 && py >= 0 && px < iw2 && py < ih2) {
                                unsigned char* q = d + ((size_t)py * iw2 + px) * 4;
                                json rb = g_vSide.value("removebg", json::object());
                                rb["method"] = "colorkey"; rb["enabled"] = true;
                                rb["key"] = json::array({(int)q[0], (int)q[1], (int)q[2]});
                                if (!rb.contains("fuzz")) rb["fuzz"] = 60.0;
                                g_vSide["removebg"] = rb; lib_save_sidecar(path, g_vSide); invalidate_texture(path);
                            }
                            if (d) stbi_image_free(d);
                            g_vPickBg = false;
                        }
                    }
                } else {
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) { g_vPan.x += io.MouseDelta.x; g_vPan.y += io.MouseDelta.y; }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) g_vZoom = 0.f;  // → refit next frame
                }
                cdl->AddImage((ImTextureID)(intptr_t)t->srv, a, b);
            }
        } else if (gs.state == 1) ImGui::TextDisabled("(generating…)");
        else ImGui::TextDisabled("(no preview)");
        ImGui::EndChild();
        if (g_vMaskMode) ImGui::TextDisabled("paint=erase/restore · middle/right-drag=pan · wheel=zoom · %d%%", (int)(g_vZoom * 100.f + 0.5f));
        else ImGui::TextDisabled("scroll=zoom · drag=pan · dbl-click=fit · %d%%", (int)(g_vZoom * 100.f + 0.5f));
        // ── crude crop / mask paint tool (non-destructive, rides the sidecar like remove-bg): zoom in
        //    + paint to ERASE what removebg missed (or RESTORE), box-erase a region, or drag a crop
        //    rect. Commits to <file>.mask.png + sidecar on each stroke → flows to grid/timeline/export.
        ImGui::Separator();
        if (ImGui::Checkbox("crop / mask (paint)", &g_vMaskMode)) { if (!g_vMaskMode) vmask_reset(); g_vZoom = 0.f; }
        if (g_vMaskMode) {
            const char* tools[] = {"erase", "restore", "box-erase", "crop"};
            for (int i = 0; i < 4; i++) { if (i) ImGui::SameLine(); if (ImGui::RadioButton(tools[i], g_vMaskTool == i)) { g_vMaskTool = i; g_vRubber = false; } }
            if (g_vMaskTool <= 1) {
                ImGui::SetNextItemWidth(120); ImGui::SliderFloat("brush", &g_vBrush, 3.f, 200.f, "%.0f px");
                ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::SliderFloat("feather", &g_vFeather, 0.f, 1.f, "%.2f");
            }
            if (ImGui::SmallButton("clear mask") && g_vMaskW > 0) { std::fill(g_vMaskBuf.begin(), g_vMaskBuf.end(), (unsigned char)255); g_vMaskDirty = true; vmask_commit(path); }
            ImGui::SameLine(); if (ImGui::SmallButton("reset crop")) { json side = lib_load_sidecar(path); side.erase("crop"); lib_save_sidecar(path, side); g_vSide = side; invalidate_texture(path); g_vZoom = 0.f; }
            if (g_vSide.contains("mask") && g_vSide["mask"].is_object()) {
                bool mOn = g_vSide["mask"].value("enabled", true);
                ImGui::SameLine(); if (ImGui::Checkbox("mask on", &mOn)) { g_vSide["mask"]["enabled"] = mOn; lib_save_sidecar(path, g_vSide); invalidate_texture(path); }
            }
            ImGui::TextDisabled(g_vMaskTool == 3 ? "drag a rectangle to crop (blue = current crop)"
                              : g_vMaskTool == 2 ? "drag a rectangle to erase it"
                              : "paint on the image; commits on release. middle/right-drag to pan.");
        }
        // ── remove background (non-destructive, on the fly): params ride the item's sidecar, so
        //    it can be re-tweaked anytime + the matte flows to the grid/timeline/export via get_texture.
        //    TWO methods: an instant flat-bg colour KEY, or a provider REMBG matte (isnet-anime/…) that
        //    segments soft/gradient bgs + purple-on-purple the key can't.
        ImGui::Separator();
        std::string method = g_vSide.value("removebg", json::object()).value("method", std::string("colorkey"));
        bool rbOn = g_vSide.contains("removebg") && g_vSide["removebg"].value("enabled", true);
        ImGui::TextUnformatted("remove bg"); ImGui::SameLine();
        const char* kMethods[] = {"color key", "rembg (AI)"};
        int mi = (method == "rembg") ? 1 : 0;
        ImGui::SetNextItemWidth(100);
        if (ImGui::Combo("##rbmethod", &mi, kMethods, 2)) {
            json rb = g_vSide.value("removebg", json::object());
            rb["method"] = (mi == 1) ? "rembg" : "colorkey";
            if (!rb.contains("enabled")) rb["enabled"] = true;
            if (mi == 0) { if (!rb.contains("key")) rb["key"] = json::array({255, 0, 255}); if (!rb.contains("fuzz")) rb["fuzz"] = 60.0; }
            g_vSide["removebg"] = rb; method = rb["method"]; rbOn = rb.value("enabled", true);
            lib_save_sidecar(path, g_vSide); invalidate_texture(path);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("on", &rbOn)) {
            json rb = g_vSide.value("removebg", json::object());
            rb["enabled"] = rbOn; if (!rb.contains("method")) rb["method"] = method;
            g_vSide["removebg"] = rb; lib_save_sidecar(path, g_vSide); invalidate_texture(path);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) { g_vSide.erase("removebg"); lib_save_sidecar(path, g_vSide); invalidate_texture(path); g_vPickBg = false; }

        if (method == "rembg") {
            // provider matte: pick a model + Cut out (submits the source → cached cutout → sidecar).
            json rb = g_vSide.value("removebg", json::object());
            static const char* kModels[] = {"isnet-anime", "isnet-general-use", "u2net", "u2net_human_seg", "silueta"};
            std::string cur = rb.value("model", std::string("isnet-anime"));
            int midx = 0; for (int i = 0; i < 5; i++) if (cur == kModels[i]) midx = i;
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("model##rembg", &midx, kModels, 5)) {
                json r = g_vSide.value("removebg", json::object()); r["method"] = "rembg"; r["model"] = kModels[midx];
                g_vSide["removebg"] = r; lib_save_sidecar(path, g_vSide);
            }
            long st; std::string msg; float pr; std::string rpath;
            EnterCriticalSection(&g_genCS); st = g_rembgState; msg = g_rembgMsg; pr = g_rembgProg; rpath = g_rembgPath; LeaveCriticalSection(&g_genCS);
            ImGui::SameLine();
            if (st == 1 && rpath == path) ImGui::TextDisabled("cutting… %.0f%%", pr * 100.f);
            else if (ImGui::Button("Cut out")) start_rembg(path, kModels[midx]);
            if (st == 3 && rpath == path) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.55f, 1), "%s", msg.c_str());
            else if (rb.contains("cache")) ImGui::TextDisabled("cutout cached · %s", cur.c_str());
            else ImGui::TextDisabled("Cut out → isnet-anime matte (cuts soft/gradient bgs the key can't)");
            if (g_vPickBg) g_vPickBg = false;  // eyedropper is colour-key only
        } else if (rbOn) {
            // colour key: eyedropper + key swatch + fuzz (instant, flat backgrounds)
            json& rb = g_vSide["removebg"];
            ImGui::SameLine(); if (ImGui::Button(g_vPickBg ? "click image…" : "pick bg")) g_vPickBg = !g_vPickBg;
            json k = rb.value("key", json::array({255, 0, 255}));
            ImVec4 kc(k[0].get<int>() / 255.f, k[1].get<int>() / 255.f, k[2].get<int>() / 255.f, 1.f);
            ImGui::SameLine(); ImGui::ColorButton("##key", kc, 0, ImVec2(18, 18));
            float fz = (float)rb.value("fuzz", 60.0);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##fuzz", &fz, 0.f, 200.f, "fuzz %.0f")) { rb["fuzz"] = fz; lib_save_sidecar(path, g_vSide); invalidate_texture(path); }
        } else if (g_vPickBg) g_vPickBg = false;
    } else {
        ImGui::TextDisabled("video — drag onto a video lane (decoded in-process by libav).");
    }

    // ── L4: gen recipe + regenerate + history ──
    if (gen) {
        ImGui::Separator();
        std::string cap = g_vSide.value("cap", std::string(type == LIB_IMAGE ? "text2image" : "speech"));
        ImGui::BeginChild("recipe");
        if (cap == "text2image") {
            std::string pr = g_vRecipe.value("prompt", std::string());
            ImGui::TextDisabled("prompt"); if (ImGui::InputTextMultiline("##vpr", &pr, ImVec2(-1, 70))) g_vRecipe["prompt"] = pr;
            int sd = g_vRecipe.value("seed", 0); ImGui::SetNextItemWidth(150);
            if (ImGui::InputInt("seed", &sd)) g_vRecipe["seed"] = sd;
            bool host = g_vRecipe.contains("lora");
            if (ImGui::Checkbox("Gemma host LoRA", &host)) {
                if (host) { g_vRecipe["arch"] = "anima"; g_vRecipe["lora"] = "gemma-san-anima"; g_vRecipe["lora_strength"] = 0.9; }
                else { g_vRecipe.erase("lora"); g_vRecipe.erase("lora_strength"); }
            }
        } else {  // speech
            std::string tx = g_vRecipe.value("text", std::string());
            ImGui::TextDisabled("text"); if (ImGui::InputTextMultiline("##vtx", &tx, ImVec2(-1, 56))) g_vRecipe["text"] = tx;
            if (g_voicePresets.empty()) g_voicePresets = list_voice_presets();
            std::string cv = g_vRecipe.value("voice_preset", std::string());
            if (ImGui::BeginCombo("voice", cv.empty() ? "(none)" : cv.c_str())) {
                for (auto& v : g_voicePresets) if (ImGui::Selectable(v.c_str(), v == cv)) g_vRecipe["voice_preset"] = v;
                ImGui::EndCombo();
            }
            std::string em = g_vRecipe.value("emotion", std::string());
            if (ImGui::InputText("emotion", &em)) { if (em.empty()) g_vRecipe.erase("emotion"); else g_vRecipe["emotion"] = em; }
            int sd = g_vRecipe.value("seed", 0); ImGui::SetNextItemWidth(150);
            if (ImGui::InputInt("seed", &sd)) g_vRecipe["seed"] = sd;
        }
        bool busy = (gs.state == 1);
        if (busy) ImGui::BeginDisabled();
        std::string prov = g_vSide.value("provider", std::string(type == LIB_IMAGE ? "image" : "tts"));
        std::string ownDir = path.substr(0, path.find_last_of("/\\"));   // regen in place (never re-homes the item)
        if (ImGui::Button("Regenerate"))
            start_lib_gen(type, base, prov, cap, lib_job_body(cap, g_vRecipe), g_vRecipe, true, ownDir);
        ImGui::SameLine();
        if (ImGui::Button("Regenerate (fresh)")) {
            g_vRecipe["seed"] = g_vRecipe.value("seed", 0) + 1;
            start_lib_gen(type, base, prov, cap, lib_job_body(cap, g_vRecipe), g_vRecipe, true, ownDir);
        }
        if (busy) ImGui::EndDisabled();

        json hist = g_vSide.value("history", json::array());
        if (ImGui::TreeNodeEx("history", ImGuiTreeNodeFlags_DefaultOpen, "previous generations (%d)", (int)hist.size())) {
            if (hist.empty()) ImGui::TextDisabled("none yet — Regenerate keeps the prior gen here to restore.");
            int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / 78.f)), shown = 0;
            for (size_t i = 0; i < hist.size(); i++) {
                json& h = hist[i]; ImGui::PushID((int)i);
                std::string hp = g_cacheDir + "/" + h.value("provider", std::string()) + "/" +
                                 h.value("hash", std::string()) + "." + h.value("ext", std::string("png"));
                bool restore = false;
                if (type == LIB_IMAGE) {
                    Tex* ht = get_texture(hp);
                    if (ht && ht->srv) restore = ImGui::ImageButton("h", (ImTextureID)(intptr_t)ht->srv, ImVec2(70, 70));
                    else restore = ImGui::Button("(no\ncache)", ImVec2(70, 70));
                } else restore = ImGui::Button(("take\n" + std::to_string(i + 1)).c_str(), ImVec2(70, 40));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("seed %d — click to restore", (int)h.value("params", json::object()).value("seed", 0));
                if (restore) lib_restore_gen(path, (int)i);
                ImGui::PopID();
                if (++shown % cols) ImGui::SameLine(); else ImGui::Dummy(ImVec2(0, 2));
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();
    }
}

// ── Sprite-sheet panel: load a GPT sheet, key out the flat background (picker + fuzziness),
// mark rectangle crops (auto-grid or freehand drag), export each auto-trimmed into the library. ──
static bool g_showSprite = false;
static std::string g_spritePath, g_spriteStatus;
static int g_spriteW = 0, g_spriteH = 0;
static std::vector<unsigned char> g_spriteSrc;            // original RGBA
static float g_spriteKey[3] = {1.f, 0.f, 1.f};           // key colour (magenta default)
static float g_spriteFuzz = 60.f;                        // 0..200 (RGB distance)
static bool g_spritePick = false;                        // eyedropper armed
static int g_spriteGridR = 2, g_spriteGridC = 3;
static std::vector<SpriteRect> g_spriteRects;
static char g_spritePrefix[64] = "sprite";
static ID3D11ShaderResourceView* g_spritePrevSrv = nullptr;
static std::string g_spritePrevKey;                      // param hash of the live keyed preview
static bool g_spriteDragging = false; static ImVec2 g_spriteDragStart;

static void sprite_load(const std::string& path) {
    int w, h, n;
    unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!d) { g_spriteStatus = "load FAILED"; return; }
    g_spriteSrc.assign(d, d + (size_t)w * h * 4); stbi_image_free(d);
    g_spriteW = w; g_spriteH = h; g_spritePath = path; g_spriteRects.clear(); g_spritePrevKey.clear();
    g_spriteStatus = std::to_string(w) + "x" + std::to_string(h);
}
static void sprite_refresh_preview() {
    if (g_spriteSrc.empty()) return;
    int kr = (int)(g_spriteKey[0] * 255), kg = (int)(g_spriteKey[1] * 255), kb = (int)(g_spriteKey[2] * 255);
    char key[160]; snprintf(key, sizeof key, "%s|%d|%d|%d|%.1f", g_spritePath.c_str(), kr, kg, kb, g_spriteFuzz);
    if (g_spritePrevKey == key) return;
    g_spritePrevKey = key;
    if (g_spritePrevSrv) { g_spritePrevSrv->Release(); g_spritePrevSrv = nullptr; }
    std::vector<unsigned char> px = g_spriteSrc;
    sprite_color_key(px, kr, kg, kb, g_spriteFuzz);
    D3D11_TEXTURE2D_DESC dsc = {}; dsc.Width = g_spriteW; dsc.Height = g_spriteH; dsc.MipLevels = 1; dsc.ArraySize = 1;
    dsc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dsc.SampleDesc.Count = 1; dsc.Usage = D3D11_USAGE_IMMUTABLE; dsc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = px.data(); sd.SysMemPitch = g_spriteW * 4;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&dsc, &sd, &tex)) && tex) { g_dev->CreateShaderResourceView(tex, nullptr, &g_spritePrevSrv); tex->Release(); }
}

static std::string g_spriteAutoload;   // --sprite-load <png>: auto-open the panel on this sheet (dev/verify)
static void draw_sprite_window() {
    if (!g_spriteAutoload.empty()) {    // one-shot: load + show + auto-grid, then clear
        sprite_load(g_spriteAutoload); g_spriteAutoload.clear(); g_showSprite = true;
        int cw = g_spriteW / std::max(1, g_spriteGridC), ch = g_spriteH / std::max(1, g_spriteGridR);
        for (int yy = 0; yy < g_spriteGridR; yy++) for (int xx = 0; xx < g_spriteGridC; xx++)
            g_spriteRects.push_back({xx * cw, yy * ch, cw, ch});
    }
    if (!g_showSprite) return;
    ImGui::SetNextWindowSize(ImVec2(580, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sprite sheet", &g_showSprite)) {
        if (ImGui::Button("Load sheet…")) { std::string f = pick_image_file(); if (!f.empty()) sprite_load(f); }
        ImGui::SameLine(); ImGui::TextDisabled("%s  %s", g_spritePath.empty() ? "(no sheet)" : g_spritePath.c_str(), g_spriteStatus.c_str());
        if (g_spriteSrc.empty()) { ImGui::TextDisabled("Load a GPT sprite sheet (flat-colour background) to begin."); ImGui::End(); return; }

        ImGui::ColorEdit3("bg key", g_spriteKey, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine(); if (ImGui::Button(g_spritePick ? "picking… click image" : "pick from image")) g_spritePick = !g_spritePick;
        ImGui::SameLine(); ImGui::SetNextItemWidth(150); ImGui::SliderFloat("fuzz", &g_spriteFuzz, 0.f, 200.f, "%.0f");
        ImGui::SetNextItemWidth(64); ImGui::InputInt("rows", &g_spriteGridR, 0); ImGui::SameLine();
        ImGui::SetNextItemWidth(64); ImGui::InputInt("cols", &g_spriteGridC, 0); ImGui::SameLine();
        if (ImGui::Button("grid")) {
            g_spriteRects.clear();
            int r = std::max(1, g_spriteGridR), c = std::max(1, g_spriteGridC), cw = g_spriteW / c, ch = g_spriteH / r;
            for (int yy = 0; yy < r; yy++) for (int xx = 0; xx < c; xx++) g_spriteRects.push_back({xx * cw, yy * ch, cw, ch});
        }
        ImGui::SameLine(); if (ImGui::Button("clear")) g_spriteRects.clear();
        ImGui::SameLine(); ImGui::TextDisabled("%d crops (or drag on image)", (int)g_spriteRects.size());
        ImGui::SetNextItemWidth(170); ImGui::InputText("prefix", g_spritePrefix, sizeof g_spritePrefix);
        ImGui::SameLine();
        if (ImGui::Button("Export → library") && !g_spriteRects.empty()) {
            int n = sprite_export_to_library(g_spriteSrc, g_spriteW, g_spriteH,
                (int)(g_spriteKey[0] * 255), (int)(g_spriteKey[1] * 255), (int)(g_spriteKey[2] * 255),
                g_spriteFuzz, g_spriteRects, g_spritePrefix);
            g_spriteStatus = "exported " + std::to_string(n) + " → library/images";
        }
        ImGui::Separator();

        sprite_refresh_preview();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float scale = std::min(avail.x / g_spriteW, avail.y / g_spriteH); if (scale > 1.f) scale = 1.f;
        float dispW = g_spriteW * scale, dispH = g_spriteH * scale;
        ImVec2 ip = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ck = 12.f;                                   // checker → transparency reads clearly
        for (float yy = 0; yy < dispH; yy += ck) for (float xx = 0; xx < dispW; xx += ck) {
            bool a = ((int)(xx / ck) + (int)(yy / ck)) & 1;
            dl->AddRectFilled(ImVec2(ip.x + xx, ip.y + yy), ImVec2(ip.x + std::min(xx + ck, dispW), ip.y + std::min(yy + ck, dispH)),
                              a ? IM_COL32(70, 70, 76, 255) : IM_COL32(48, 48, 54, 255));
        }
        if (g_spritePrevSrv) dl->AddImage((ImTextureID)(intptr_t)g_spritePrevSrv, ip, ImVec2(ip.x + dispW, ip.y + dispH));
        ImGui::InvisibleButton("sheet", ImVec2(dispW, dispH));
        bool hov = ImGui::IsItemHovered();
        ImVec2 m = ImGui::GetMousePos();
        auto toImg = [&](ImVec2 q) { return ImVec2((q.x - ip.x) / scale, (q.y - ip.y) / scale); };
        if (hov && g_spritePick && ImGui::IsMouseClicked(0)) {
            ImVec2 q = toImg(m); int xi = (int)q.x, yi = (int)q.y;
            if (xi >= 0 && xi < g_spriteW && yi >= 0 && yi < g_spriteH) {
                size_t o = ((size_t)yi * g_spriteW + xi) * 4;
                g_spriteKey[0] = g_spriteSrc[o] / 255.f; g_spriteKey[1] = g_spriteSrc[o + 1] / 255.f; g_spriteKey[2] = g_spriteSrc[o + 2] / 255.f;
            }
            g_spritePick = false;
        } else if (hov && !g_spritePick && ImGui::IsMouseClicked(0)) { g_spriteDragging = true; g_spriteDragStart = m; }
        if (g_spriteDragging && ImGui::IsMouseReleased(0)) {
            ImVec2 a = toImg(g_spriteDragStart), b = toImg(m);
            int x0 = (int)std::min(a.x, b.x), y0 = (int)std::min(a.y, b.y), x1 = (int)std::max(a.x, b.x), y1 = (int)std::max(a.y, b.y);
            if (x1 - x0 > 3 && y1 - y0 > 3) g_spriteRects.push_back({x0, y0, x1 - x0, y1 - y0});
            g_spriteDragging = false;
        }
        for (size_t i = 0; i < g_spriteRects.size(); i++) {
            auto& r = g_spriteRects[i];
            ImVec2 a(ip.x + r.x * scale, ip.y + r.y * scale), b(ip.x + (r.x + r.w) * scale, ip.y + (r.y + r.h) * scale);
            dl->AddRect(a, b, IM_COL32(255, 220, 90, 255), 0, 0, 2.f);
            char idx[8]; snprintf(idx, sizeof idx, "%d", (int)i + 1);
            dl->AddText(ImVec2(a.x + 3, a.y + 1), IM_COL32(255, 220, 90, 255), idx);
        }
        if (g_spriteDragging) dl->AddRect(g_spriteDragStart, m, IM_COL32(120, 200, 255, 255), 0, 0, 2.f);
    }
    ImGui::End();
}

// ── docked Project panel: the project-GLOBAL settings, persisted in meta (Ctrl+S) ─────────
// `format` picks the DEFAULT bundle — 1080p = the locked full-length-video conventions
// (built-in SFX off, speech 1.0x); portrait = shorts (1080x1920, SFX on, ~1.3x speech).
// Switching format only re-derives fields the user hasn't pinned (explicit meta keys win).
static void draw_project_settings(Project& p) {
    if (!p.doc.is_object()) { ImGui::TextDisabled("no project loaded"); return; }
    if (!p.doc.contains("meta") || !p.doc["meta"].is_object()) p.doc["meta"] = json::object();
    json& meta = p.doc["meta"];
    ImGui::TextDisabled("project-global settings — saved to meta with Ctrl+S");
    ImGui::SeparatorText("format");
    int fmt = (p.format == "portrait") ? 1 : 0;
    bool chg = ImGui::RadioButton("1080p landscape (1920 x 1080)", &fmt, 0);
    chg     |= ImGui::RadioButton("portrait / shorts (1080 x 1920)", &fmt, 1);
    if (chg) {
        bool por = (fmt == 1);
        p.format = por ? "portrait" : "1080p";
        meta["format"] = p.format;
        p.width = por ? 1080 : 1920; p.height = por ? 1920 : 1080;
        meta["resolution"] = json::array({(int)p.width, (int)p.height});
        if (!meta.contains("sfx")) p.sfx = por;                        // re-derive unpinned defaults
        if (!meta.contains("speech_rate")) p.speechRate = por ? 1.3 : 1.0;
        g_undoDirty = true;
    }
    ImGui::TextDisabled(p.format == "portrait"
        ? "shorts defaults: built-in SFX on, ~1.3x speech, fast pace"
        : "full-video defaults: built-in SFX off, 1.0x speech");
    ImGui::SeparatorText("audio");
    bool sfx = p.sfx;
    if (ImGui::Checkbox("built-in sound effects", &sfx)) { p.sfx = sfx; meta["sfx"] = sfx; g_undoDirty = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("pop / swipe one-shots on the default transitions (library/sfx).\n"
                          "Only the BUILT-IN sounds — audio clips you add are never gated.\n"
                          "Per-clip opt-out: params.sfx = false.");
    float mg = (float)p.masterGainDb;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("final gain (dB)", &mg, -24.f, 24.f, "%+.1f")) {
        p.masterGainDb = mg; meta["gain_db"] = (double)mg; g_undoDirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("master volume on the FINAL mix, after per-clip loudness\nnormalization (preview + export identically).");
    float sg = (float)p.speechGainDb;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("speech volume (dB)", &sg, -24.f, 24.f, "%+.1f")) {
        p.speechGainDb = sg; meta["speech_gain_db"] = (double)sg; g_undoDirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("GLOBAL boost added to every speech (tts) clip, ON TOP of its loudness\n"
                          "normalization + per-clip/lane gain — the relative dynamics between\n"
                          "lines are unchanged, just louder overall (preview + export). +12 dB default.");
    float srt = (float)p.speechRate;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("default speech rate", &srt, 0.5f, 2.0f, "%.2fx")) {
        p.speechRate = srt; meta["speech_rate"] = (double)srt; g_undoDirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("applies to tts clips with NO explicit rate of their own.\n"
                          "Existing clip durations are not rescaled — use slop.py retime\nafter changing this on a composed cut.");
    // ── song credits: the auto on-screen "now playing" chip at each song's start ──
    ImGui::SeparatorText("song credits");
    bool scr = p.songCredits;
    if (ImGui::Checkbox("show \"now playing\" chip", &scr)) { p.songCredits = scr; meta["song_credits"] = scr; g_undoDirty = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("when a song STARTS, fade a ♪ title — artist chip into a corner for a few seconds\n"
                          "(from the music asset's title/artist meta — the same credit the export description\n"
                          "lists). A looped bed triggers once; a re-used song triggers again.\n"
                          "Per-song opt-out: the start clip's params.credit = false.");
    if (p.songCredits) {
        const char* CORN[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
        const char* CKEY[] = {"tl", "tr", "bl", "br"};
        int ci = 0; for (int i = 0; i < 4; i++) if (p.songCreditCorner == CKEY[i]) ci = i;
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("corner", &ci, CORN, 4)) { p.songCreditCorner = CKEY[ci]; meta["song_credit_corner"] = p.songCreditCorner; g_undoDirty = true; }
        float ss = (float)p.songCreditSecs;
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat("hold (s)", &ss, 3.f, 20.f, "%.0f")) { p.songCreditSecs = ss; meta["song_credit_secs"] = (double)ss; g_undoDirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("how long the chip stays before fading (a shorter song shows only for its length).");
    }
    if (p.format == "portrait") {   // shorts: the animated on-screen transcript captions (r_transcript)
        ImGui::SeparatorText("transcript");
        std::string trInner = "nix develop --command python tools/slop.py transcript '" + win_path_to_wsl(p.path) + "'";
        if (ImGui::Button("Regenerate transcript")) {   // saves, runs slop.py, the file-watch live-reloads the result
            if (save_project(p) && !spawn_wsl_bash(trInner))   // trInner already win_path_to_wsl's p.path (a raw UNC path never resolves inside WSL)
                fprintf(stderr, "transcript: could not launch wsl.exe\n");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("copy cmd##tr")) { save_project(p); ImGui::SetClipboardText(wsl_command(trInner).c_str()); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("saves, then copies the command — run it in your WSL terminal if the console launch fails.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("(re)build the pop-in caption chunks on the r_transcript track (its own track, floated to the\n"
                              "TOP) from every VO line, loosely word-timed to each take's speech. Runs slop.py in a console;\n"
                              "the editor live-reloads the result — then tweak text / drag / reposition each chunk like any caption.");
    }
    // ── anchors: per-project category bases (meta.anchors) — one knob nudges every clip that
    // names the key in params.anchor (their pos is an offset from it). Seeded by slop.py
    // skeleton/transcript; only ever shown when the project has some.
    if (!p.anchors.empty()) {
        ImGui::SeparatorText("anchors");
        ImGui::TextDisabled("category base positions — clips with params.anchor ride these");
        for (auto& kv : p.anchors) {
            float v[2] = {(float)kv.second[0], (float)kv.second[1]};
            ImGui::SetNextItemWidth(200);
            if (ImGui::DragFloat2(kv.first.c_str(), v, 1.0f)) {
                kv.second = {(double)v[0], (double)v[1]};
                meta["anchors"][kv.first] = json::array({(double)v[0], (double)v[1]});
                g_undoDirty = true;
            }
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s  %g x %g @ %d fps", p.title.c_str(), p.width, p.height, p.fps);
}

// A draggable vertical splitter (6px wide, height h) that resizes a panel width `*w` by the drag,
// clamped [mn,mx]. `sign` = +1 when the panel it resizes sits to the splitter's LEFT (drag right →
// wider), -1 when to its RIGHT. Draws a thin handle that brightens on hover.
static void v_splitter(const char* id, float* w, float h, float mn, float mx, float sign) {
    ImGui::InvisibleButton(id, ImVec2(6.0f, h));
    if (ImGui::IsItemActive()) *w += sign * ImGui::GetIO().MouseDelta.x;
    if (*w < mn) *w = mn;  if (*w > mx) *w = mx;
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float cx = (a.x + b.x) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, a.y + 4), ImVec2(cx, b.y - 4),
        hot ? IM_COL32(0x8a, 0x84, 0xb0, 255) : IM_COL32(0x3a, 0x35, 0x60, 255), hot ? 2.0f : 1.0f);
}

// Horizontal splitter (full width w, 6px tall): drags a panel height `*h` by the vertical mouse move.
static void h_splitter(const char* id, float* h, float w, float mn, float mx) {
    ImGui::InvisibleButton(id, ImVec2(w, 6.0f));
    if (ImGui::IsItemActive()) *h += ImGui::GetIO().MouseDelta.y;
    if (*h < mn) *h = mn;  if (*h > mx) *h = mx;
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float cy = (a.y + b.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 4, cy), ImVec2(b.x - 4, cy),
        hot ? IM_COL32(0x8a, 0x84, 0xb0, 255) : IM_COL32(0x3a, 0x35, 0x60, 255), hot ? 2.0f : 1.0f);
}

static void DrawUI(Project& p, UIState& st, bool& reload, const std::map<std::string, GenLite>& gen) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar |
                             ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("slopstudio", nullptr, flags);

    bool doSave = false, doUndo = false, doRedo = false;
    std::string splitReq, delReq, dupReq;   // deferred (split/delete/duplicate/undo/redo reassign p → run after the panels)
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save project", "Ctrl+S")) doSave = true;
            if (ImGui::MenuItem("Reload project")) reload = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Render video…")) g_openRender = true;
            if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("Voice preset editor", nullptr, &g_showVoiceEditor);
            ImGui::MenuItem("Media browser pane", nullptr, &g_showLibrary);
            ImGui::MenuItem("Asset detail tab", nullptr, &g_showViewer);
            ImGui::MenuItem("Sprite sheet", nullptr, &g_showSprite);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Track")) {
            if (ImGui::MenuItem("Add track…")) g_openAddTrack = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Clip")) {
            bool hasSel = !st.selected.empty() && p.clips.count(st.selected);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) dupReq = st.selected;
            if (ImGui::MenuItem("Split at playhead", "S", false, hasSel)) splitReq = st.selected;
            if (ImGui::MenuItem("Delete", "Del", false, hasSel)) delReq = st.selected;
            ImGui::Separator();
            if (ImGui::MenuItem("Copy transform", "Ctrl+Shift+C", false, hasSel)) copy_transform(p.clips[st.selected]);
            if (ImGui::MenuItem("Paste transform", "Ctrl+Shift+V", false, hasSel && g_txClip.has)) {
                paste_transform(p.clips[st.selected]); g_undoDirty = true;
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Quick add (at playhead)")) {   // common clips, house defaults (also: click empty timeline space)
                struct QT { const char* label; const char* kind; };
                static const QT QTS[] = {
                    {"Host — Gemma sprite",  "host"},
                    {"Voice — VO line",      "voice"},
                    {"Sound — Heh~",         "sound"},
                    {"Music — theme",        "music"},
                    {"Backdrop — the room",  "backdrop"},
                };
                for (auto& qt : QTS)
                    if (ImGui::MenuItem(qt.label)) {
                        std::string nid = add_quick_clip(p, qt.kind, st.playhead);
                        if (!nid.empty()) { st.selected = nid; g_bufFor.clear(); g_undoDirty = true; }
                    }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Add special clip (at playhead)")) {   // native compositing clips from scratch — no gen, no JSON
                struct NT { const char* label; const char* type; };
                static const NT NTS[] = {
                    {"Vignette / gradient wash", "gradient"},
                    {"Blur transition",          "blur"},
                    {"Background fill (blur)",   "filler"},
                    {"Caption / text",           "caption"},
                    {"Code card",                "code"},
                    {"Shape / callout",          "shape"},
                    {"Diagram (boxes + arrows)", "diagram"},
                    {"Caption anchor (move captions in range)", "anchor"},
                };
                for (auto& nt : NTS)
                    if (ImGui::MenuItem(nt.label)) {
                        std::string nid = add_native_clip_at(p, nt.type, st.playhead);
                        if (!nid.empty()) { st.selected = nid; g_bufFor.clear(); g_undoDirty = true; }
                    }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGuiIO& kio = ImGui::GetIO();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) doSave = true;
        // Undo/redo: Ctrl+Z · Ctrl+Shift+Z / Ctrl+Y (deferred below — they reassign p). Suppressed
        // while typing in a text field so Ctrl+Z there is the input box's own char-undo.
        if (kio.KeyCtrl && !kio.WantTextInput) {
            if (!kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) doUndo = true;
            if ((kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) || ImGui::IsKeyPressed(ImGuiKey_Y)) doRedo = true;
            if (ImGui::IsKeyPressed(ImGuiKey_D) && !st.selected.empty()) dupReq = st.selected;   // Ctrl+D = duplicate
            if (kio.KeyShift && !st.selected.empty() && p.clips.count(st.selected)) {            // Ctrl+Shift+C/V = transform
                if (ImGui::IsKeyPressed(ImGuiKey_C)) copy_transform(p.clips[st.selected]);
                if (ImGui::IsKeyPressed(ImGuiKey_V) && g_txClip.has) { paste_transform(p.clips[st.selected]); g_undoDirty = true; }
            }
        }
        if (doSave) {
            g_saveStatus = save_project(p) ? ("saved " + p.path) : "SAVE FAILED";
            g_projMtime = file_mtime(p.path.c_str());   // adopt our own write → the mtime watcher won't reload (which would wipe undo)
        }
        ImGui::Text("   %s   %g x %g @ %dfps", p.title.c_str(), p.width, p.height, p.fps);
        if (!g_saveStatus.empty()) { ImGui::SameLine(); ImGui::TextDisabled(" · %s", g_saveStatus.c_str()); }
        if (!p.ok) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "  ERROR: %s", p.error.c_str());
        }
        // provider health: green = up, red = unreachable, grey = unknown/checking
        EnterCriticalSection(&g_healthCS);
        std::map<std::string, int> health = g_health;
        LeaveCriticalSection(&g_healthCS);
        if (!g_providers.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("  providers:");
            for (auto& kv : g_providers) {
                int h = health.count(kv.first) ? health[kv.first] : 0;
                ImVec4 col = h == 1 ? ImVec4(0.4f, 0.9f, 0.4f, 1) : h == 2 ? ImVec4(0.9f, 0.4f, 0.4f, 1)
                                                                            : ImVec4(0.6f, 0.6f, 0.6f, 1);
                ImGui::SameLine();
                ImGui::TextColored(col, "%s", kv.first.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s — %s", kv.second.url.c_str(),
                                      h == 1 ? "up" : h == 2 ? "unreachable" : "checking…");
            }
        }
        ImGui::EndMenuBar();
    }

    // Add-track modal (from the Track menu) — pick a type + name; new track goes on top, reorder with ▲▼.
    if (g_openAddTrack) { ImGui::OpenPopup("Add Track"); g_openAddTrack = false; }
    if (ImGui::BeginPopupModal("Add Track", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static int tIdx = 0; static char tName[64] = "";
        const char* TYPES[] = {"image", "caption", "code", "shape", "gradient", "filler", "avatar", "tts", "music", "anchor"};
        ImGui::Combo("type", &tIdx, TYPES, IM_ARRAYSIZE(TYPES));
        ImGui::InputText("name (optional)", tName, sizeof tName);
        if (ImGui::Button("Add", ImVec2(120, 0))) { add_track(p, TYPES[tIdx], tName); tName[0] = 0; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // First-run stock-assets check (once per session): the code checkout ships no art.
    static int g_stockChecked = 0;
    if (g_stockChecked == 0) { g_stockChecked = stock_assets_ready() ? 2 : 1; if (g_stockChecked == 1) ImGui::OpenPopup("Stock assets missing"); }
    if (ImGui::BeginPopupModal("Stock assets missing", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char smsg[200] = "";
        ImGui::TextWrapped("The host rig and backgrounds aren't unpacked yet (a fresh checkout ships\n"
                           "no art). Fetch the stock pack now?");
        ImGui::TextDisabled("runs: tools/fetch-stock-assets.py  (local dist/ first, else the release)");
        if (smsg[0]) { ImGui::Separator(); ImGui::TextWrapped("%s", smsg); }
        ImGui::Separator();
        if (ImGui::Button("Fetch now", ImVec2(130, 0))) {
            bool ok = spawn_wsl_bash("nix develop --command python tools/fetch-stock-assets.py; "
                                     "echo; echo '[done — reload the project (File > Reload). press enter]'; read");
            snprintf(smsg, sizeof smsg, ok ? "Fetching in a console window — reload the project when it finishes."
                                           : "Could not launch wsl.exe — use \"Copy command\" and run it yourself.");
            g_stockChecked = 2;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy command", ImVec2(150, 0))) {
            ImGui::SetClipboardText(wsl_command("nix develop --command python tools/fetch-stock-assets.py").c_str());
            snprintf(smsg, sizeof smsg, "Copied — paste into your WSL terminal, then reload the project.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip", ImVec2(90, 0))) { g_stockChecked = 2; smsg[0] = 0; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (g_openRender) { ImGui::OpenPopup("Render video"); g_openRender = false; }
    if (ImGui::BeginPopupModal("Render video", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static int targetMb = 300, fps = 60; static bool p1080 = true; static char msg[256] = "";
        bool portrait = (p.format == "portrait");
        if (ImGui::IsWindowAppearing()) {   // format-aware defaults each time the dialog opens
            double dur = p.duration();
            // a ~60s Short doesn't need the full-video's fat cap — size it to the length (~0.7 MB/s ≈ 5.5 Mbps
            // at 1080x1920); the full-length landscape video keeps the owner's ~300 MB pick.
            targetMb = portrait ? std::max(15, (int)std::lround(dur * 0.7)) : 300;
            p1080 = !portrait;              // portrait is ALREADY 1080x1920 — scaling to 1080p would squash it
            if (fps > p.fps) fps = (int)p.fps;   // don't ask for more fps than the project has
        }
        ImGui::Text("Project: %s   (%g x %g, %.0f fps native)", p.path.c_str(), p.width, p.height, (double)p.fps);
        ImGui::Separator();
        if (!portrait) ImGui::Checkbox("Scale to 1080p", &p1080);   // landscape only — portrait renders native
        else ImGui::TextDisabled("portrait: native 1080 x 1920 (no downscale).");
        ImGui::SetNextItemWidth(120); ImGui::InputInt("output fps", &fps);
        if (fps < 1) fps = 1; if (fps > 120) fps = 120;
        ImGui::SetNextItemWidth(120); ImGui::InputInt("target size (MB)", &targetMb);
        if (targetMb < 10) targetMb = 10;
        ImGui::TextDisabled(portrait ? "1-pass x264 ABR, sized to the Short's length (adjust above)."
                                     : "space-efficient 1-pass x264 ABR (YouTube re-encodes anyway).");
        ImGui::TextDisabled("Native fps must be >= output fps for real motion — raise meta.fps to render more frames.");
        std::string stem = p.path; size_t sl = stem.find_last_of("/\\"); if (sl != std::string::npos) stem = stem.substr(sl + 1);
        size_t dot = stem.find(".slop.json"); if (dot != std::string::npos) stem = stem.substr(0, dot);
        std::string out = "exports/" + stem + "-" + std::to_string(p1080 ? 1080 : (int)p.height) + "p" + std::to_string(fps) + ".mp4";
        ImGui::Text("→ %s", out.c_str());
        ImGui::Separator();
        // The render is a nix + bash + ffmpeg pipeline on the Linux side. Launching it via wsl.exe from
        // this Windows-PE-under-WSLInterop is unreliable (nested WSL breaks the systemd user session →
        // chdir + PATH fail), so hand the exact command to the clipboard — the user runs it in their own
        // WSL terminal (where the env is correct) and watches ffmpeg there.
        std::string cmd = render_command(p.path, targetMb, fps, p1080, out);
        ImGui::TextUnformatted("Run this in your WSL terminal:");
        static char cbuf[1200]; strncpy(cbuf, cmd.c_str(), sizeof cbuf - 1); cbuf[sizeof cbuf - 1] = 0;
        ImGui::SetNextItemWidth(560);
        ImGui::InputText("##rendercmd", cbuf, sizeof cbuf, ImGuiInputTextFlags_ReadOnly);   // selectable/scrollable
        if (msg[0]) ImGui::TextColored(ImVec4(0.55f, 1.f, 0.6f, 1.f), "%s", msg);
        ImGui::Separator();
        if (ImGui::Button("Copy command", ImVec2(150, 0))) {
            ImGui::SetClipboardText(cmd.c_str());
            snprintf(msg, sizeof msg, "Copied — paste into your WSL terminal (fish/bash) and press Enter.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("try launching via WSL")) {   // fallback for setups where nested wsl.exe works
            bool ok = spawn_render(p.path, targetMb, fps, p1080, out);
            snprintf(msg, sizeof msg, ok ? "Launched a console window (if your WSL allows nested launches)."
                                         : "Could not launch wsl.exe — use Copy command instead.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(90, 0))) { msg[0] = 0; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImVec2 workspace = ImGui::GetContentRegionAvail();
    if (g_showLibrary) {
        float leftMx = std::max(240.f, workspace.x - 520.f);
        if (g_leftW < 200.f) g_leftW = 200.f;  if (g_leftW > leftMx) g_leftW = leftMx;
        ImGui::BeginChild("media_browser", ImVec2(g_leftW, 0), true);
        ImGui::TextUnformatted("Media");
        ImGui::Separator();
        draw_library_contents(p, st);
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
        v_splitter("##splitLeft", &g_leftW, workspace.y, 200.f, leftMx, +1.0f);
        ImGui::SameLine(0, 0);
    }
    ImGui::BeginGroup();

    float availY = ImGui::GetContentRegionAvail().y;
    if (g_topH < 0.f) g_topH = availY * 0.5f;                        // first frame: default 50/50
    float topMin = 140.f, topMax = std::max(topMin + 1.f, availY - 170.f);   // keep room for transport + timeline
    if (g_topH < topMin) g_topH = topMin;  if (g_topH > topMax) g_topH = topMax;
    float topH = g_topH;
    float groupW = ImGui::GetContentRegionAvail().x;
    float inspMx = std::max(320.f, groupW - 360.f);
    if (g_inspW < 260.f) g_inspW = 260.f;  if (g_inspW > inspMx) g_inspW = inspMx;
    float previewW = groupW - g_inspW - 6.f; if (previewW < 200.f) previewW = 200.f;

    ImGui::BeginChild("preview", ImVec2(previewW, topH), true);
    {
        ImGui::TextUnformatted("Preview");
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImVec2 sz = ImGui::GetContentRegionAvail();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float ar = (float)(p.width / p.height);
        float fw = sz.x, fh = fw / ar;
        if (fh > sz.y) { fh = sz.y; fw = fh * ar; }
        // Snap the frame rect to whole pixels: a fractional f0/fw put the bg fill, clip-rect scissor,
        // and the vignette edge bands on sub-pixel boundaries, so the GPU scissor rounding left a 1px
        // bright seam on the (left) edge. Integer-aligning all of it removes the seam (export path
        // already passes f0=(0,0), so it's unaffected).
        fw = floorf(fw); fh = floorf(fh);
        ImVec2 f0(floorf(a.x + (sz.x - fw) * 0.5f), floorf(a.y + (sz.y - fh) * 0.5f));
        if (g_pvBlurActive && g_pvBlurSrv)   // whole-frame blur transition: show the pre-blurred composite (covers every layer)
            dl->AddImage((ImTextureID)(intptr_t)g_pvBlurSrv, f0, ImVec2(f0.x + fw, f0.y + fh));
        else
            composite_frame(p, st, dl, f0, fw, fh);
        char b[64]; snprintf(b, sizeof b, "t = %.2fs", st.playhead);
        dl->AddText(ImVec2(f0.x + 8, f0.y + 8), IM_COL32(180, 180, 190, 255), b);
        int nActive = 0;
        for (auto& kv : gen) if (kv.second.state == 1) nActive++;
        if (nActive) {
            char g[48]; snprintf(g, sizeof g, "regenerating %d clip(s)...", nActive);
            dl->AddText(ImVec2(f0.x + 8, f0.y + 26), IM_COL32(255, 210, 90, 255), g);
        }
        ImGui::Dummy(sz);
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    v_splitter("##splitRight", &g_inspW, topH, 260.f, inspMx, -1.0f);
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("properties", ImVec2(0, topH), true);
    if (ImGui::BeginTabBar("right_panes")) {
        if (ImGui::BeginTabItem("Inspector")) {
        auto it = p.clips.find(st.selected);
        if (!st.selected.empty() && it != p.clips.end()) {
            Clip& c = it->second;
            ImGui::Text("clip: %s", c.id.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Duplicate")) dupReq = c.id;   // copy placed right after (Ctrl+D)
            ImGui::SameLine(); ImGui::TextDisabled("(Ctrl+D)");
            ImGui::Text("row:  %s  [%s]", c.row.c_str(), c.type.c_str());
            if (c.type == "avatar") {
                auto rit = p.rows.find(c.row);
                std::string rigName = (rit != p.rows.end()) ? jstr(rit->second.params, "rig") : std::string();
                if (rigName.empty()) rigName = "gemma-big";
                draw_avatar_emotion_panel(c, rigName);
            }
            float start = (float)c.start, len = (float)c.dur;
            if (ImGui::DragFloat("start (s)", &start, 0.01f, 0.0f, 1e5f)) c.start = start;  // instant
            if (ImGui::DragFloat("dur (s)", &len, 0.01f, 0.01f, 1e5f)) c.dur = len;          // instant
            float pos[2] = {(float)c.tx_pos[0], (float)c.tx_pos[1]};
            float op = (float)c.tx_opacity;
            if (ImGui::DragFloat2("pos (px)", pos, 1.0f)) { c.tx_pos[0] = pos[0]; c.tx_pos[1] = pos[1]; }
            {   // anchored clip → pos is an OFFSET from the project anchor (Project panel tunes the base)
                std::string ank = jstr(c.params, "anchor");
                if (!ank.empty() && p.anchors.count(ank))
                    ImGui::TextDisabled("offset from anchor \"%s\" [%g, %g]", ank.c_str(),
                                        p.anchors[ank][0], p.anchors[ank][1]);
            }
            // ── size: actual output px (default) ⇄ scale factor, aspect-locked + synchronized.
            //    Both drive transform.scale; when scale is KEYFRAMED the edit rescales the whole
            //    track (so resizing the CRT / a zoomed host works, not a dead static field). ──
            {
                static bool aspectLock = true;
                bool sclKf = c.keyframes.count("transform.scale") > 0;
                double T = st.playhead;
                double repX = clip_rep_scale(c, 0, T), repY = clip_rep_scale(c, 1, T);
                float nw = 0, nh = 0; bool hasNative = clip_native_size(p, c, nw, nh);
                if (hasNative) {                              // pixel size = native × scale × layout multiplier (ON-SCREEN px)
                    float Lm = layout_scale_mul(p, c);
                    float wpx = nw * std::fabs((float)repX) * Lm, hpx = nh * std::fabs((float)repY) * Lm, w0 = wpx, h0 = hpx;
                    ImGui::SetNextItemWidth(80);
                    bool cw = ImGui::DragFloat("##wpx", &wpx, 1.0f, 1.f, 30000.f, "%.0f");
                    ImGui::SameLine(0, 4); ImGui::TextUnformatted("×"); ImGui::SameLine(0, 4);
                    ImGui::SetNextItemWidth(80);
                    bool ch = ImGui::DragFloat("px##hpx", &hpx, 1.0f, 1.f, 30000.f, "%.0f");
                    if (cw && w0 > 1e-3f) { double r = std::fabs(wpx) / w0; clip_rescale(c, r, aspectLock ? r : 1.0); g_undoDirty = true; }
                    else if (ch && h0 > 1e-3f) { double r = std::fabs(hpx) / h0; clip_rescale(c, aspectLock ? r : 1.0, r); g_undoDirty = true; }
                    if (Lm != 1.0f) { ImGui::SameLine(); ImGui::TextDisabled("(layout ×%.2f)", Lm);
                                      if (ImGui::IsItemHovered()) ImGui::SetTooltip("on-screen size includes the '%s' layout auto-scale", jstr(c.params, "layout").c_str()); }
                    repX = clip_rep_scale(c, 0, T); repY = clip_rep_scale(c, 1, T);   // re-read for the synced scale row
                }
                float scl[2] = {(float)repX, (float)repY}, sx0 = scl[0], sy0 = scl[1];
                ImGui::SetNextItemWidth(168);
                if (ImGui::DragFloat2(hasNative ? "scale ×" : "scale", scl, 0.005f, -50.f, 50.f, "%.3f")) {
                    double rx = std::fabs(sx0) > 1e-6 ? scl[0] / sx0 : 1.0, ry = std::fabs(sy0) > 1e-6 ? scl[1] / sy0 : 1.0;
                    if (aspectLock) { double r = std::fabs(scl[0] - sx0) >= std::fabs(scl[1] - sy0) ? rx : ry; clip_rescale(c, r, r); }
                    else clip_rescale(c, rx, ry);
                    g_undoDirty = true;
                }
                ImGui::SameLine(); ImGui::Checkbox("lock", &aspectLock);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("lock aspect ratio (W and H scale together)");
                if (sclKf) { ImGui::SameLine(); ImGui::TextDisabled("(anim)");
                             if (ImGui::IsItemHovered()) ImGui::SetTooltip("scale is keyframed — a size edit rescales the whole animation"); }
            }
            {   // flip / mirror — a clean toggle instead of hand-typing a negative into the scale field
                //  (the aspect-lock coupled both axes, so a negative scale read as a 180° rotation, not a mirror).
                bool fH = clip_rep_scale(c, 0, st.playhead) < 0, fV = clip_rep_scale(c, 1, st.playhead) < 0;
                if (ImGui::Checkbox("flip H", &fH)) { clip_flip(c, 0, fH); g_undoDirty = true; }
                ImGui::SameLine();
                if (ImGui::Checkbox("flip V", &fV)) { clip_flip(c, 1, fV); g_undoDirty = true; }
                ImGui::SameLine(); ImGui::TextDisabled("(mirror)");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("mirror the clip horizontally / vertically (negative scale)");
            }
            if (ImGui::DragFloat("opacity", &op, 0.01f, 0.0f, 1.0f)) c.tx_opacity = op;
            float anch[2] = {(float)c.tx_anchor[0], (float)c.tx_anchor[1]};
            if (ImGui::DragFloat2("anchor (0..1)", anch, 0.005f, 0.0f, 1.0f)) { c.tx_anchor[0] = anch[0]; c.tx_anchor[1] = anch[1]; }
            // transition: default smooth fade in/out; pick a type per edge ("none" disables).
            {
                static const char* TT[] = {"fade", "slide_down", "slide_up", "slide_left", "slide_right", "rise", "pop", "swap", "none"};
                auto curType = [&](const char* side) -> int {
                    std::string t = "fade";
                    if (c.params.is_object() && c.params.contains("transition")) {
                        const json& tr = c.params["transition"];
                        if (tr.is_boolean() && !tr.get<bool>()) t = "none";
                        else if (tr.is_object() && tr.contains(side)) {
                            const json& sp = tr[side];
                            if (sp.is_string()) t = sp.get<std::string>();
                            else if (sp.is_object()) t = sp.value("type", std::string("fade"));
                        }
                    }
                    for (int i = 0; i < 9; i++) if (t == TT[i]) return i;
                    return 0;
                };
                auto setSide = [&](const char* side, int idx) {
                    if (!c.params.is_object()) c.params = json::object();
                    if (!c.params["transition"].is_object()) c.params["transition"] = json::object();
                    c.params["transition"][side] = std::string(TT[idx]);
                };
                int ii = curType("in"), oi = curType("out");
                ImGui::SetNextItemWidth(110);
                if (ImGui::Combo("in##tr", &ii, TT, 9)) setSide("in", ii);
                ImGui::SameLine(); ImGui::SetNextItemWidth(110);
                if (ImGui::Combo("out##tr", &oi, TT, 9)) setSide("out", oi);
                ImGui::SameLine(); ImGui::TextDisabled("transition");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("swap = force the host pose-swap dip on that edge (avatar rows;\nfires automatically when the sprite or auto-placement changes)");
            }
            // ── Look: common adjustable effects (outer glow · auto color-grade) for visual clips ──
            if (c.type != "tts" && c.type != "music") {
                json& LP = c.params;
                if (!LP.is_object()) LP = json::object();
                auto glowOn = [&]() -> bool {
                    if (!LP.contains("glow")) return false;
                    const json& g = LP["glow"];
                    if (g.is_boolean()) return g.get<bool>();
                    if (g.is_object()) return g.value("enabled", true);
                    return false;
                };
                bool gOn = glowOn();
                if (ImGui::Checkbox("outer glow", &gOn)) {
                    if (gOn) { if (!LP["glow"].is_object()) LP["glow"] = json{{"size", 26.0}, {"strength", 0.7}}; LP["glow"]["enabled"] = true; }
                    else if (LP["glow"].is_object()) LP["glow"]["enabled"] = false; else LP["glow"] = false;
                }
                if (gOn && LP["glow"].is_object()) {
                    float gsz = (float)LP["glow"].value("size", 26.0), gst = (float)LP["glow"].value("strength", 0.7);
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("glow size", &gsz, 2.f, 90.f, "%.0f")) LP["glow"]["size"] = (double)gsz;
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("glow strength", &gst, 0.f, 2.f, "%.2f")) LP["glow"]["strength"] = (double)gst;
                    float gcol[3] = {235 / 255.f, 240 / 255.f, 255 / 255.f};
                    if (LP["glow"].contains("color") && LP["glow"]["color"].is_array() && LP["glow"]["color"].size() == 3)
                        for (int k = 0; k < 3; k++) gcol[k] = (float)LP["glow"]["color"][k].get<int>() / 255.f;
                    if (ImGui::ColorEdit3("glow color", gcol, ImGuiColorEditFlags_NoInputs))
                        LP["glow"]["color"] = json::array({(int)(gcol[0] * 255), (int)(gcol[1] * 255), (int)(gcol[2] * 255)});
                }
                // border ("frame"): tri-state. AUTO = the default rule (on for a non-fullscreen inset
                // with no glow); ON forces it at any size/with glow; OFF kills it. Forced states are
                // the manual override the auto rule can't express (the user's 220s missing-border ask).
                if (c.type == "image" || c.type == "video") {
                    int fsel = 0;
                    if (LP.contains("frame")) {
                        const json& fj = LP["frame"];
                        bool off = (fj.is_boolean() && !fj.get<bool>()) ||
                                   (fj.is_object() && fj.contains("enabled") && fj["enabled"].is_boolean() && !fj["enabled"].get<bool>());
                        fsel = off ? 2 : 1;
                    }
                    static const char* FR[] = {"auto", "on", "off"};
                    ImGui::SetNextItemWidth(110);
                    if (ImGui::Combo("border", &fsel, FR, 3)) {
                        if (fsel == 0) LP.erase("frame");
                        else if (LP.contains("frame") && LP["frame"].is_object()) LP["frame"]["enabled"] = (fsel == 1);
                        else LP["frame"] = (fsel == 1);   // keep an authored style object; else plain bool
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("the pro inset border + drop shadow.\nauto = on for a non-fullscreen inset with no glow\non = always (any size, even with glow) · off = never");
                }
                if (c.type == "image" || c.type == "video" || c.type == "avatar") {
                    bool ag = LP.value("auto_grade", c.type == "avatar");
                    if (ImGui::Checkbox("auto color-grade (match bg)", &ag)) LP["auto_grade"] = ag;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("grade toward the bottom bg plate like the host does\n(image: desat + warm/cool · video: warm/cool only)");
                }
                if (c.type == "avatar") {                    // explicit foot/ground contact shadow (forces it on)
                    bool fs = false; float fsAmt = 0.45f;
                    if (LP.contains("foot_shadow")) {
                        const json& v = LP["foot_shadow"];
                        fs = v.is_boolean() ? v.get<bool>() : (v.is_number() ? v.get<double>() > 0.01 : false);
                        if (v.is_number()) fsAmt = (float)v.get<double>();
                    }
                    if (ImGui::Checkbox("foot contact-shadow", &fs)) { if (fs) LP["foot_shadow"] = (double)fsAmt; else LP.erase("foot_shadow"); }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("ground shadow under her feet — FORCES it on even for floating/bust poses\n(the auto contact shadow otherwise shows only on non-floating full-body shots)");
                    if (fs) {
                        ImGui::SetNextItemWidth(150);
                        if (ImGui::SliderFloat("shadow strength", &fsAmt, 0.05f, 1.0f, "%.2f")) LP["foot_shadow"] = (double)fsAmt;
                    }
                }
                if (c.type == "image" || c.type == "video") {
                    // layout = render-time adaptive placement (any source size lands well-framed);
                    // the transform below nudges/rescales ON TOP of it.
                    static const char* LAYS[] = {"(manual)", "inset", "inset-left", "fullscreen", "fit", "cover"};
                    std::string cl = LP.value("layout", std::string());
                    int li = 0; for (int k = 1; k < 6; k++) if (cl == LAYS[k]) li = k;
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::Combo("layout", &li, LAYS, 6)) {
                        if (li == 0) LP.erase("layout"); else LP["layout"] = LAYS[li];
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("adaptive placement, computed at render time:\ninset = fit ~half-frame beside the host (portrait: the top band) · fullscreen = cover\n(degrades to contain on an extreme-aspect source) · cover = ALWAYS cover (backdrops)\nfit = contain (letterbox on the filler) · (manual) = authored transform only");
                }
                if (c.type == "image" || c.type == "video") {
                    // NON-DESTRUCTIVE crop: sample a sub-rect of the SOURCE (compositor clip_crop →
                    // UVs); the raw asset is never touched. Stored params.crop = [x,y,w,h] fractions
                    // 0..1 (x,y = top-left corner). layout/transform apply ON TOP of the cropped
                    // region. Below is a VISUAL handle: a source thumbnail with a draggable crop
                    // rectangle (drag a corner to resize, drag inside to move). The main composite
                    // updates live as you drag; undo is automatic at gesture settle.
                    auto cl = [](float v, float lo, float hi){ return v < lo ? lo : v > hi ? hi : v; };
                    float cr[4] = {0.f, 0.f, 1.f, 1.f};
                    bool hasCrop = LP.contains("crop") && LP["crop"].is_array() && LP["crop"].size() == 4;
                    if (hasCrop) for (int k = 0; k < 4; k++) cr[k] = (float)LP["crop"][k].get<double>();

                    // resolve a source texture + native aspect for the preview
                    ImTextureID srcTex = 0; float srcW = 0, srcH = 0;
                    if (c.type == "image") {
                        auto au = p.asset_uri.find(c.asset);
                        if (au != p.asset_uri.end()) { Tex* t = get_texture(resolve_asset(au->second));
                            if (t && t->srv) { srcTex = (ImTextureID)(intptr_t)t->srv; srcW = (float)t->w; srcH = (float)t->h; } }
                    }
#ifdef SLOP_LIBAV
                    else if (c.type == "video") {
                        auto vmIt = p.asset_video.find(c.asset);
                        if (vmIt != p.asset_video.end() && g_videoDirect && !vmIt->second.src.empty()) {
                            VideoDecoder* d = get_decoder(resolve_asset(vmIt->second.src));
                            if (d) { double T = cl((float)st.playhead, (float)c.start, (float)(c.start + c.dur) - 0.05f);
                                FrameTex* ft = get_decoded_frame_tex(vmIt->second.src, video_frame_index(vmIt->second, c, T), d);
                                if (ft && ft->srv) { srcTex = (ImTextureID)(intptr_t)ft->srv; srcW = (float)ft->w; srcH = (float)ft->h; } }
                        }
                    }
#endif
                    ImGui::SeparatorText("crop — drag the handles (non-destructive)");
                    float aspect = (srcW > 0 && srcH > 0) ? srcW / srcH : 16.f / 9.f;
                    float BW = 280.f, BH = BW / aspect; if (BH > 210.f) { BH = 210.f; BW = BH * aspect; }
                    ImVec2 org = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##cropbox", ImVec2(BW, BH));
                    ImDrawList* idl = ImGui::GetWindowDrawList();
                    if (srcTex) idl->AddImage(srcTex, org, ImVec2(org.x + BW, org.y + BH));
                    else idl->AddRectFilled(org, ImVec2(org.x + BW, org.y + BH), IM_COL32(38, 38, 46, 255));
                    ImVec2 r0(org.x + cr[0] * BW, org.y + cr[1] * BH), r1(org.x + (cr[0] + cr[2]) * BW, org.y + (cr[1] + cr[3]) * BH);
                    ImU32 dimc = IM_COL32(0, 0, 0, 150);
                    idl->AddRectFilled(org, ImVec2(org.x + BW, r0.y), dimc);                                   // above
                    idl->AddRectFilled(ImVec2(org.x, r1.y), ImVec2(org.x + BW, org.y + BH), dimc);             // below
                    idl->AddRectFilled(ImVec2(org.x, r0.y), ImVec2(r0.x, r1.y), dimc);                         // left
                    idl->AddRectFilled(ImVec2(r1.x, r0.y), ImVec2(org.x + BW, r1.y), dimc);                    // right
                    idl->AddRect(r0, r1, IM_COL32(120, 200, 255, 255), 0, 0, 2.f);
                    ImVec2 corners[4] = { r0, ImVec2(r1.x, r0.y), ImVec2(r0.x, r1.y), r1 };
                    for (auto& cp : corners) idl->AddRectFilled(ImVec2(cp.x - 5, cp.y - 5), ImVec2(cp.x + 5, cp.y + 5), IM_COL32(255, 255, 255, 255));

                    static int grab = -1; static float fixX = 0, fixY = 0;   // corner 0..3, 4 = move body
                    if (ImGui::IsItemActivated()) {
                        ImVec2 m = ImGui::GetIO().MousePos; grab = -1; float best = 11.f;
                        for (int k = 0; k < 4; k++) { float dd = fabsf(m.x - corners[k].x) + fabsf(m.y - corners[k].y); if (dd < best) { best = dd; grab = k; } }
                        if (grab < 0 && m.x > r0.x && m.x < r1.x && m.y > r0.y && m.y < r1.y) grab = 4;
                        if (grab >= 0 && grab < 4) { fixX = (grab == 0 || grab == 2) ? cr[0] + cr[2] : cr[0]; fixY = (grab == 0 || grab == 1) ? cr[1] + cr[3] : cr[1]; }
                    }
                    if (ImGui::IsItemActive() && grab >= 0) {
                        if (grab == 4) { ImVec2 dpx = ImGui::GetIO().MouseDelta; cr[0] += dpx.x / BW; cr[1] += dpx.y / BH; }
                        else {
                            ImVec2 m = ImGui::GetIO().MousePos;
                            float mfx = cl((m.x - org.x) / BW, 0.f, 1.f), mfy = cl((m.y - org.y) / BH, 0.f, 1.f);
                            cr[0] = mfx < fixX ? mfx : fixX; cr[2] = fabsf(mfx - fixX);
                            cr[1] = mfy < fixY ? mfy : fixY; cr[3] = fabsf(mfy - fixY);
                        }
                        cr[0] = cl(cr[0], 0.f, 0.98f); cr[1] = cl(cr[1], 0.f, 0.98f);
                        cr[2] = cl(cr[2], 0.02f, 1.f - cr[0]); cr[3] = cl(cr[3], 0.02f, 1.f - cr[1]);
                        LP["crop"] = json::array({(double)cr[0], (double)cr[1], (double)cr[2], (double)cr[3]});
                    }
                    if (ImGui::IsItemDeactivated()) grab = -1;

                    // numeric fine-tune + reset
                    bool cChg = false;
                    ImGui::SetNextItemWidth(130);
                    if (ImGui::DragFloat2("pos x,y", cr, 0.002f, 0.f, 0.98f, "%.3f")) cChg = true;
                    ImGui::SameLine(); ImGui::SetNextItemWidth(130);
                    if (ImGui::DragFloat2("size w,h", cr + 2, 0.002f, 0.02f, 1.f, "%.3f")) cChg = true;
                    if (cChg) {
                        cr[2] = cl(cr[2], 0.02f, 1.f - cr[0]); cr[3] = cl(cr[3], 0.02f, 1.f - cr[1]);
                        LP["crop"] = json::array({(double)cr[0], (double)cr[1], (double)cr[2], (double)cr[3]});
                    }
                    if (hasCrop) { ImGui::SameLine(); if (ImGui::SmallButton("reset")) LP.erase("crop"); }
                }
            }
            if (!c.asset.empty()) ImGui::Text("asset: %s", c.asset.c_str());
            // image clips: file picker + a live preview of the current image (high up so it's visible)
            if (c.type == "image") {
                if (ImGui::SmallButton("Browse image…")) {
                    std::string f = pick_image_file();
                    if (!f.empty()) { set_clip_image_file(p, c, f); g_bufFor.clear(); }
                }
                auto au = p.asset_uri.find(c.asset);
                if (au != p.asset_uri.end()) {
                    Tex* t = get_texture(resolve_asset(au->second));
                    if (t && t->srv) {
                        float pw = 180.f, ph = pw * (t->h / (float)(t->w > 0 ? t->w : 1));
                        ImGui::Image((ImTextureID)(intptr_t)t->srv, ImVec2(pw, ph));
                    }
                }
            }
            // edit ops (deferred — they rewrite + reload the project). Split cuts at the playhead.
            bool inside = st.playhead > c.start + 0.02 && st.playhead < c.start + c.dur - 0.02;
            if (!inside) ImGui::BeginDisabled();
            if (ImGui::Button("Split @ playhead (S)")) splitReq = c.id;
            if (!inside) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Delete clip")) delReq = c.id;

            // ── audio: per-clip volume · per-lane (track) volume · loudness normalize · volume ENVELOPE · audition ──
            // (grouped HIGH — above the rarely-used generation panel — so a music bed's volume ramp is a
            // scroll-free glance away, mirroring the inline envelope drawn on the timeline lane.)
            if (c.type == "tts" || c.type == "music") {
                ImGui::SeparatorText("audio");
                float volc = (float)c.params.value("gain_db", 0.0);
                ImGui::SetNextItemWidth(180);
                // -48 floor: a loud bed can still be too hot even at -24 in a quiet context (owner ask).
                if (ImGui::SliderFloat("clip volume (dB)", &volc, -48.f, 24.f, "%+.1f")) c.params["gain_db"] = (double)volc;
                auto rit = p.rows.find(c.row);
                if (rit != p.rows.end()) {
                    float volr = (float)rit->second.params.value("gain_db", 0.0);
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::SliderFloat("lane volume (dB)", &volr, -48.f, 24.f, "%+.1f")) {
                        if (!rit->second.params.is_object()) rit->second.params = json::object();
                        rit->second.params["gain_db"] = (double)volr; g_undoDirty = true;
                    }
                }
                // playback speed (pitch-preserved time-stretch) — pace-match a slow/fast line.
                // A tts clip with no explicit rate inherits the project default (meta.speech_rate).
                double inheritRate = (c.type == "tts") ? p.speechRate : 1.0;
                float rate = (float)c.params.value("rate", inheritRate);
                ImGui::SetNextItemWidth(180);
                if (ImGui::SliderFloat("speed (rate)", &rate, 0.5f, 2.0f, "%.2fx")) {
                    double oldRate = c.params.value("rate", inheritRate);
                    if (rate < 0.5f) rate = 0.5f; if (rate > 2.0f) rate = 2.0f;
                    double span = c.dur * (oldRate > 0.01 ? oldRate : 1.0);   // source seconds shown (rate-invariant)
                    c.params["rate"] = (double)rate;
                    c.dur = span / rate;                                      // keep the same audio, just faster/slower
                    g_undoDirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("time-stretch, no pitch shift; resizes the clip to fit the same audio.\nlip-sync bob follows the sped-up audio (rate-aware).");
                // loudness normalize: clip param overrides the lane's; auto-levels a quiet line to a target RMS.
                bool laneNorm = (rit != p.rows.end()) && rit->second.params.value("normalize", false);
                bool norm = c.params.value("normalize", laneNorm);
                if (ImGui::Checkbox("normalize loudness", &norm)) c.params["normalize"] = norm;
                if (norm) {
                    double laneTgt = (rit != p.rows.end()) ? rit->second.params.value("normalize_db", -20.0) : -20.0;
                    float ntgt = (float)c.params.value("normalize_db", laneTgt);
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::SliderFloat("target (dBFS)", &ntgt, -30.f, -10.f, "%.0f")) c.params["normalize_db"] = (double)ntgt;
                    auto au2 = p.asset_uri.find(c.asset);
                    Pcm* pcm = (au2 != p.asset_uri.end()) ? get_pcm(au2->second) : nullptr;
                    if (pcm) ImGui::TextDisabled("auto gain: %+.1f dB", normalize_gain_db(pcm, c.params.value("normalize_db", (double)ntgt)));
                }
                std::string auri;
                auto aau = p.asset_uri.find(c.asset);
                if (aau != p.asset_uri.end()) auri = aau->second;
                if (!auri.empty()) {
                    std::string apath = resolve_asset(auri);
                    if (ImGui::Button("Play")) PlaySoundA(apath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
                    ImGui::SameLine();
                    if (ImGui::Button("Stop")) PlaySoundA(nullptr, nullptr, 0);
                }
                draw_gain_envelope(p, c, st.playhead);   // volume automation graph (the ramp/slope over time)
            }

            // ── song metadata (music clips): title / artist / credit — auto-filled from the file's ID3
            //    tags on import, editable here. The credit line feeds the export DESCRIPTION credits;
            //    title/artist feed the on-screen "now playing" chip. Writes the ASSET meta (shared by
            //    every clip of this song), so a looped bed edits once. ──
            if (c.type == "music" && !c.asset.empty() && p.doc.contains("assets")
                && p.doc["assets"].contains(c.asset) && p.doc["assets"][c.asset].is_object()) {
                ImGui::SeparatorText("song metadata");
                json& am = p.doc["assets"][c.asset];
                if (!am.contains("meta") || !am["meta"].is_object()) am["meta"] = json::object();
                json& mm = am["meta"];
                static std::string mFor, mTitle, mArtist, mCredit;   // edit buffers, refreshed on asset change
                if (mFor != c.asset) {
                    mFor = c.asset;
                    mTitle = mm.value("title", std::string());
                    mArtist = mm.value("artist", std::string());
                    mCredit = (mm.contains("attribution") && mm["attribution"].is_object())
                                ? mm["attribution"].value("attribution_text", std::string()) : std::string();
                }
                if (ImGui::InputText("title", &mTitle)) { if (mTitle.empty()) mm.erase("title"); else mm["title"] = mTitle; }
                if (ImGui::InputText("artist", &mArtist)) { if (mArtist.empty()) mm.erase("artist"); else mm["artist"] = mArtist; }
                if (ImGui::InputText("credit (description)", &mCredit)) {
                    if (mCredit.empty()) mm.erase("attribution");
                    else mm["attribution"] = json{{"attribution_text", mCredit}};
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("the full attribution line for the YouTube DESCRIPTION (e.g. CC BY credit).\n"
                                      "The on-screen chip shows title — artist; this line is the written credit.");
                if (ImGui::SmallButton("Re-detect from file tags")) {   // re-read ID3 (overwrites the fields)
                    auto uu = p.asset_uri.find(c.asset);
                    if (uu != p.asset_uri.end()) { json d = audio_meta_from_tags(uu->second); if (!d.empty()) { mm = d; mFor.clear(); } }
                    g_undoDirty = true;
                }
                ImGui::SameLine();
                bool showChip = c.params.value("credit", true);
                if (ImGui::Checkbox("show chip", &showChip)) {          // per-instance opt-out (params.credit:false)
                    if (showChip) c.params.erase("credit"); else c.params["credit"] = false; g_undoDirty = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("show the on-screen now-playing chip when THIS song starts.\n"
                                      "(global toggle + corner live in the Project panel.)");
            }

            // ── SFX cue: an authored one-shot (boom/awkward/…) fired at clip.start + `at`; the music
            // DUCKS around it. Any clip type. Also shows/moves via the orange flag on the timeline clip. ──
            {
                static const char* SFX_OPTS[] = {"(none)", "boom", "awkward", "pop", "pop-blip", "whoosh", "whoosh-sharp"};
                std::string cur = c.params.is_object() ? jstr(c.params, "sfx_cue") : std::string();
                int idx = 0; for (int i = 1; i < IM_ARRAYSIZE(SFX_OPTS); i++) if (cur == SFX_OPTS[i]) { idx = i; break; }
                ImGui::SeparatorText("SFX cue");
                ImGui::SetNextItemWidth(150);
                if (ImGui::Combo("cue##sfx", &idx, SFX_OPTS, IM_ARRAYSIZE(SFX_OPTS))) {
                    if (!c.params.is_object()) c.params = json::object();
                    if (idx == 0) c.params.erase("sfx_cue");
                    else { c.params["sfx_cue"] = std::string(SFX_OPTS[idx]); if (!c.params.contains("sfx_at")) c.params["sfx_at"] = 0.0; }
                    g_undoDirty = true;
                }
                if (idx != 0) {
                    float at = (float)c.params.value("sfx_at", 0.0);
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("at (s)##sfx", &at, 0.0f, (float)std::max(0.1, c.dur), "%.2f")) c.params["sfx_at"] = (double)at;
                    ImGui::SameLine(); ImGui::TextDisabled("fires @ %.2fs", c.start + c.params.value("sfx_at", 0.0));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("absolute timeline time. sfx_at is in TIMELINE seconds from the clip start\n(the same units the clip plays at) — drag the orange flag on the clip too.");
                    float g = (float)c.params.value("sfx_gain_db", -3.0);
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("gain (dB)##sfx", &g, -36.f, 12.f, "%+.1f")) c.params["sfx_gain_db"] = (double)g;
                    bool duck = c.params.value("sfx_duck", true);
                    if (ImGui::Checkbox("duck music##sfx", &duck)) c.params["sfx_duck"] = duck;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("dip the music around this cue (fade 0.3s -> floor -> back 0.6s).");
                }
            }

            // ── generation: edit the inputs + (re)generate through the provider ──
            std::string provKey, capType;
            bool generable = map_type(c.type, provKey, capType);
            if (generable) {
                ImGui::Separator();
                ImGui::TextDisabled("generation [%s]", provKey.c_str());
                if (g_bufFor != c.id) {  // refresh edit buffers on selection change
                    g_bufFor = c.id;
                    std::string t = (c.type == "image") ? jstr(c.params, "prompt")
                                  : (c.type == "music") ? jstr(c.params, "mood")
                                                        : jstr(c.params, "text");
                    if (c.type == "tts") t = wrap_text(t, ImGui::GetContentRegionAvail().x - 14.0f);  // soft-wrap to the box width
                    strncpy(g_textBuf, t.c_str(), sizeof g_textBuf - 1); g_textBuf[sizeof g_textBuf - 1] = 0;
                    std::string tr = wrap_text(jstr(c.params, "transcript"), ImGui::GetContentRegionAvail().x - 14.0f);
                    strncpy(g_trBuf, tr.c_str(), sizeof g_trBuf - 1); g_trBuf[sizeof g_trBuf - 1] = 0;
                    std::string e = jstr(c.params, "emotion");
                    strncpy(g_emoBuf, e.c_str(), sizeof g_emoBuf - 1); g_emoBuf[sizeof g_emoBuf - 1] = 0;
                    if (c.type == "tts") g_voicePresets = list_voice_presets();
                }
                if (c.type == "tts") {
                    if (ImGui::InputTextMultiline("text", g_textBuf, sizeof g_textBuf, ImVec2(-1, 76))) {
                        std::string clean = unwrap_text(g_textBuf);          // strip soft-wrap newlines for the provider
                        c.params["text"] = clean; c.label = snippet(clean);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {               // re-flow to the box width once editing settles
                        std::string w = wrap_text(jstr(c.params, "text"), ImGui::GetContentRegionAvail().x - 14.0f);
                        strncpy(g_textBuf, w.c_str(), sizeof g_textBuf - 1); g_textBuf[sizeof g_textBuf - 1] = 0;
                    }
                    // caption-display override (params.transcript): on-screen captions show THIS, not the
                    // spoken text — surfaced here because an invisible override silently kept old wording
                    // when the owner reworded a line (luckymas-short4 b01, 2026-07-08). `slop.py lint`
                    // flags a diverged pair as STALE-CAPTION.
                    if (!jstr(c.params, "transcript").empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 200, 90, 255));
                        ImGui::TextWrapped("captions display the override below, NOT the text above — reword both:");
                        ImGui::PopStyleColor();
                        if (ImGui::InputTextMultiline("caption##tr", g_trBuf, sizeof g_trBuf, ImVec2(-1, 56)))
                            c.params["transcript"] = unwrap_text(g_trBuf);
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            std::string w = wrap_text(jstr(c.params, "transcript"), ImGui::GetContentRegionAvail().x - 14.0f);
                            strncpy(g_trBuf, w.c_str(), sizeof g_trBuf - 1); g_trBuf[sizeof g_trBuf - 1] = 0;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("what the viewer READS (true spellings, digits) while the text above is what\n"
                                              "the TTS SPEAKS (phonetic respells, spelled-out numbers). After editing, hit\n"
                                              "Regenerate transcript (Project panel) to re-chunk the on-screen captions.");
                        if (ImGui::SmallButton("clear override##tr")) c.params.erase("transcript");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("captions fall back to showing the spoken text verbatim.");
                    } else if (ImGui::SmallButton("+ caption override")) {
                        c.params["transcript"] = jstr(c.params, "text");
                        std::string w = wrap_text(jstr(c.params, "transcript"), ImGui::GetContentRegionAvail().x - 14.0f);
                        strncpy(g_trBuf, w.c_str(), sizeof g_trBuf - 1); g_trBuf[sizeof g_trBuf - 1] = 0;
                    }
                    if (jstr(c.params, "transcript").empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("give the on-screen captions different text than the TTS speaks\n"
                                          "(true spellings, digits) — starts as a copy of the spoken text.");
                    if (ImGui::InputText("emotion / host pose", g_emoBuf, sizeof g_emoBuf))
                        c.params["emotion"] = std::string(g_emoBuf);
                    // voice preset selector — pick a designed voice, then Regenerate + Play to
                    // audition. Sets a per-clip override (falls back to the row's voice_preset).
                    std::string curVoice = jstr(c.params, "voice_preset");
                    if (curVoice.empty()) {
                        auto rr = p.rows.find(c.row);
                        if (rr != p.rows.end()) curVoice = jstr(rr->second.params, "voice_preset");
                    }
                    if (ImGui::BeginCombo("voice", curVoice.empty() ? "(none)" : curVoice.c_str())) {
                        for (auto& vp : g_voicePresets) {
                            bool sel = (vp == curVoice);
                            if (ImGui::Selectable(vp.c_str(), sel)) c.params["voice_preset"] = vp;
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    json curPreset = curVoice.empty() ? json::object() : load_voice_preset(curVoice);
                    if (!jstr(curPreset, "ref").empty() && !jstr(curPreset, "ref_text").empty())
                        ImGui::TextDisabled("cloned voice: emotion is used for host pose/script metadata, not TTS delivery.");
                } else if (c.type == "image") {
                    if (ImGui::InputTextMultiline("prompt", g_textBuf, sizeof g_textBuf, ImVec2(-1, 76))) {
                        c.params["prompt"] = std::string(g_textBuf); c.label = snippet(std::string(g_textBuf));
                    }
                    int seed = c.params.value("seed", 0);
                    if (ImGui::InputInt("seed", &seed)) c.params["seed"] = seed;
                } else if (c.type == "music") {
                    if (ImGui::InputTextMultiline("mood", g_textBuf, sizeof g_textBuf, ImVec2(-1, 76)))
                        c.params["mood"] = std::string(g_textBuf);
                } else if (c.type == "avatar") {
                    if (ImGui::InputText("emotion", g_emoBuf, sizeof g_emoBuf))
                        c.params["emotion"] = std::string(g_emoBuf);  // drives the expression
                    auto rit = p.rows.find(c.row);
                    std::string dby = (rit != p.rows.end()) ? jstr(rit->second.params, "driven_by") : "";
                    ImGui::TextDisabled("talk-react from: %s", dby.empty() ? "(no driven_by)" : dby.c_str());
                    // static pose + tweakable pngtuber bob + light-up (instant; no regen)
                    float bob = (float)c.params.value("bob", 1.0);
                    if (ImGui::DragFloat("bob", &bob, 0.01f, 0.0f, 4.0f)) c.params["bob"] = bob;
                    float bspd = (float)c.params.value("bob_speed", 1.8);
                    if (ImGui::DragFloat("bob speed", &bspd, 0.01f, 0.0f, 12.0f)) c.params["bob_speed"] = bspd;
                    float tbob = (float)c.params.value("talk_bob", 0.6);   // talk up/down (translation)
                    if (ImGui::DragFloat("talk bob (up/down)", &tbob, 0.01f, 0.0f, 3.0f)) c.params["talk_bob"] = tbob;
                    float tscl = (float)c.params.value("talk_scale", 0.0);  // talk scale (separate)
                    if (ImGui::DragFloat("talk scale", &tscl, 0.01f, 0.0f, 3.0f)) c.params["talk_scale"] = tscl;
                    float tatk = (float)c.params.value("talk_attack", 30.0);  // envelope rise rate (1/s; higher=snappier)
                    if (ImGui::DragFloat("talk attack", &tatk, 0.2f, 1.0f, 60.0f)) c.params["talk_attack"] = tatk;
                    float tdec = (float)c.params.value("talk_decay", 10.0);   // envelope fall rate (1/s; higher=snappier)
                    if (ImGui::DragFloat("talk decay", &tdec, 0.2f, 1.0f, 60.0f)) c.params["talk_decay"] = tdec;
                    float lu = (float)c.params.value("lightup", 0.35);
                    if (ImGui::DragFloat("light-up", &lu, 0.01f, 0.0f, 1.0f)) c.params["lightup"] = lu;
                    float dim = (float)c.params.value("dim", 1.0);  // <1 = dimmed when silent, full while talking
                    if (ImGui::DragFloat("idle dim", &dim, 0.01f, 0.0f, 1.0f)) c.params["dim"] = dim;
                }
                // (keyframe editor moved to draw_keyframes_panel() below, so the native
                //  clips code/caption/shape get it too — it was trapped in this generable block.)
                // ── generation history (this session): re-pick past gens; thumbnails for images ──
                auto& hist = g_genHist[c.id];
                if (!c.asset.empty() && std::find(hist.begin(), hist.end(), c.asset) == hist.end()) hist.push_back(c.asset);
                if (hist.size() > 1) {
                    ImGui::TextDisabled("history (%d) — click to restore:", (int)hist.size());
                    for (size_t i = 0; i < hist.size(); i++) {
                        ImGui::PushID((int)i);
                        bool cur = (hist[i] == c.asset);
                        std::string huri; auto hau = p.asset_uri.find(hist[i]); if (hau != p.asset_uri.end()) huri = hau->second;
                        Tex* ht = (c.type == "image" && !huri.empty()) ? get_texture(resolve_asset(huri)) : nullptr;
                        if (ht && ht->srv) {
                            ImVec4 tint = cur ? ImVec4(1,1,1,1) : ImVec4(0.7f,0.7f,0.7f,1);
                            if (ImGui::ImageButton("h", (ImTextureID)(intptr_t)ht->srv, ImVec2(64, 36), ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint))
                                { c.asset = hist[i]; g_bufFor.clear(); }
                        } else {
                            if (ImGui::Button((std::string(cur ? "*v" : "v") + std::to_string(i + 1)).c_str())) { c.asset = hist[i]; g_bufFor.clear(); }
                        }
                        if ((i + 1) % 5 != 0) ImGui::SameLine();
                        ImGui::PopID();
                    }
                    ImGui::NewLine();
                }
                auto gi = gen.find(c.id);
                bool active = (gi != gen.end() && gi->second.state == 1);
                if (ImGui::Button(c.asset.empty() ? "Generate" : "Regenerate (fresh)") && !active) {
                    if (!c.asset.empty()) c.params["seed"] = (int)c.params.value("seed", 0) + 1;  // bump seed → a genuinely NEW gen, not the cached one
                    start_generate(p, c.id);
                }
                ImGui::SameLine();
                if (active)
                    ImGui::Text("%s  %.0f%%", gi->second.message.c_str(), gi->second.progress * 100.0f);
                else if (gi != gen.end() && gi->second.state == 3)
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "ERR: %s", gi->second.message.c_str());
            } else {
                draw_native_params(p, c);  // code / caption / shape — the native compositing clips
            }
            // keyframe editor for every animatable (visual) clip type
            if (c.type != "tts" && c.type != "music") draw_keyframes_panel(c, st.playhead);

            // ── video playback: speed + loop mode. pingpong bounces the source forward/backward —
            //    the loop seam disappears on moving b-roll; retimed clips play silent (mixer rule). ──
            if (c.type == "video") {
                ImGui::SeparatorText("playback");
                float spd = (float)c.params.value("speed", 1.0);
                ImGui::SetNextItemWidth(180);
                if (ImGui::SliderFloat("speed", &spd, 0.25f, 2.0f, "%.2fx")) c.params["speed"] = (double)spd;
                int lm = 0;   // 0 loop · 1 hold last · 2 pingpong
                if (c.params.contains("loop")) {
                    const json& lj = c.params["loop"];
                    if (lj.is_boolean() && !lj.get<bool>()) lm = 1;
                    else if (lj.is_string() && lj.get<std::string>() == "pingpong") lm = 2;
                }
                ImGui::SetNextItemWidth(180);
                if (ImGui::Combo("loop mode", &lm, "loop\0hold last frame\0pingpong\0"))
                    c.params["loop"] = (lm == 0) ? json(true) : (lm == 1) ? json(false) : json("pingpong");
                if (spd < 0.999f || spd > 1.001f || lm == 2)
                    ImGui::TextDisabled("retimed: this clip's own audio is muted (would desync).");
            }

            // ── video audio: a video clip carries its OWN sound (NOT a separate track), low default
            //    volume (12%). "Split to audio track" (tools/slop.py splitaudio) for advanced editing. ──
            if (c.type == "video") {
                ImGui::SeparatorText("video audio");
                bool muted = c.params.value("mute_audio", false);
                if (ImGui::Checkbox("mute", &muted)) c.params["mute_audio"] = muted;
                if (!muted) {
                    float vol = (float)(c.params.value("video_volume", 0.12) * 100.0);
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::SliderFloat("volume", &vol, 0.f, 100.f, "%.0f%%")) c.params["video_volume"] = (double)(vol / 100.0);
                    bool duckM = c.params.value("duck_music", true);
                    if (ImGui::Checkbox("duck music under this clip", &duckM)) c.params["duck_music"] = duckM;
                    auto vmi = p.asset_video.find(c.asset);
                    std::string vsrc = (vmi != p.asset_video.end() && !vmi->second.src.empty()) ? vmi->second.src : std::string();
                    Pcm* vpc = vsrc.empty() ? nullptr : get_video_pcm(resolve_asset(vsrc));
                    if (vpc && vpc->rms >= VIDEO_AUDIO_SILENCE_RMS)
                             ImGui::TextDisabled("has audio (%.1fs). plays at 12%% by default, not as a track.", vpc->dur);
                    else if (vpc) ImGui::TextDisabled("silent audio track — treated as audio-less (won't duck the music).");
                    else     ImGui::TextDisabled("no decodable audio in this clip's source.");
                }
                ImGui::TextDisabled("advanced -> split to an audio track:\n  tools/slop.py splitaudio P.slop.json %s", c.id.c_str());
            }

            ImGui::Separator();
            ImGui::TextUnformatted("params:");
            if (c.params.is_object())
                for (auto& pr : c.params.items()) {
                    std::string v = pr.value().is_string() ? pr.value().get<std::string>() : pr.value().dump();
                    ImGui::TextWrapped("%s = %s", pr.key().c_str(), v.c_str());
                }
        } else {
            ImGui::TextDisabled("select a clip in the timeline");
        }
            ImGui::EndTabItem();
        }
        if (g_showViewer && ImGui::BeginTabItem("Asset detail")) {
            draw_viewer_contents();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Project")) {           // docked project-global settings
            draw_project_settings(p);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    h_splitter("##splitTop", &g_topH, ImGui::GetContentRegionAvail().x, 140.f, 1e5f);   // drag the preview/inspector ↕ timeline divider

    // ── transport: Play/Pause + Stop + time readout. Advances the playhead in real time and
    //    plays the timeline's audio in sync (see the waveOut mixer + the main loop). ──
    {
        double dur = p.duration();
        auto toggle = [&]() {
            if (!st.playing && st.playhead >= dur - 1e-4) st.playhead = 0.0;  // replay from start
            st.playing = !st.playing;
        };
        if (ImGui::Button(st.playing ? "Pause" : "Play", ImVec2(76, 0))) toggle();
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(60, 0))) { st.playing = false; st.playhead = 0.0; }
        if (ImGui::IsKeyPressed(ImGuiKey_Space) && !ImGui::GetIO().WantTextInput) toggle();
        if (ImGui::IsKeyPressed(ImGuiKey_S) && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && !st.selected.empty())
            splitReq = st.selected;   // S = split selected clip at playhead (Ctrl+S stays Save)
        if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) &&
            !ImGui::GetIO().WantTextInput && !st.selected.empty())
            delReq = st.selected;     // Del / Backspace = delete the selected clip (never while typing in a field)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !g_placeType.empty()) g_placeType.clear();   // disarm the placement tool
        if (ImGui::IsKeyPressed(ImGuiKey_A) && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt)
            g_placeType = (g_placeType == "__generic__") ? std::string() : std::string("__generic__");   // A = generic add mode
        ImGui::SameLine();
        ImGui::Text("%6.2f / %.2f s", st.playhead, dur);
        ImGui::SameLine();
        ImGui::TextDisabled("(space = play/pause)");
        // ── placement palette: A = generic (match neighbours); or arm a type, then click empty timeline ──
        ImGui::SameLine(); ImGui::TextDisabled("  +");
        { ImGui::SameLine(); bool on = (g_placeType == "__generic__");
          if (on) { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0x8a, 0x84, 0xd0, 255));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0x14, 0x12, 0x20, 255)); }
          if (ImGui::SmallButton("A")) g_placeType = on ? std::string() : std::string("__generic__");
          if (on) ImGui::PopStyleColor(2);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Generic add (A): click a lane → a clip that matches its neighbours"); }
        struct PT { const char* label; const char* kind; };
        static const PT PTS[] = { {"Host","host"}, {"Voice","voice"}, {"Sound","sound"}, {"Music","music"},
                                  {"BG","backdrop"}, {"Caption","caption"}, {"Code","code"}, {"Shape","shape"} };
        for (auto& pt : PTS) {
            ImGui::SameLine();
            bool on = (g_placeType == pt.kind);
            if (on) { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0x7f, 0xd8, 0xa8, 255));
                      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0x14, 0x12, 0x20, 255)); }
            if (ImGui::SmallButton(pt.label)) g_placeType = on ? std::string() : pt.kind;
            if (on) ImGui::PopStyleColor(2);
        }
        if (!g_placeType.empty()) { ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.66f, 1),
                g_placeType == "__generic__" ? "→ click a lane (matches neighbours; Esc cancels)"
                                             : "→ click/drag empty timeline (Esc cancels)"); }
    }

    ImGui::BeginChild("timeline", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollWithMouse);  // wheel = zoom only; lanes scroll via scrollbar / Ctrl+wheel
    DrawTimeline(p, st, gen);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::End();

    // deferred edit ops — safe now that no Clip& into p is held (split/delete/undo/redo reassign p).
    // undo/redo win over split/delete if somehow both fire in a frame. g_undoDirty: the S-key split
    // ends outside the ImGui active-item model, so flag it → Ctrl+Z right after still has the step.
    // A Ctrl+drag on the timeline requests a duplicate via a global (DrawTimeline can't see dupReq).
    if (dupReq.empty() && !g_dragDupReq.empty()) { dupReq = g_dragDupReq; g_dragDupReq.clear(); }
    if (!g_placeReq.empty()) {   // placement tool released → add the clip (overlap-aware; may add a track)
        std::string nid = (g_placeReq == "__generic__") ? add_generic_clip(p, g_placeReqRow, g_placeReqT, g_placeReqDur)
                                                        : add_quick_clip(p, g_placeReq, g_placeReqT, g_placeReqDur, g_placeReqRow);
        if (!nid.empty()) { st.selected = nid; g_bufFor.clear(); g_undoDirty = true; g_placeType.clear(); }  // auto-select the new clip + auto-exit add mode
        g_placeReq.clear(); g_placeReqRow.clear();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_ctrlDupDragId.clear();   // re-arm for the next Ctrl+drag
    if (doUndo)      do_undo(p);
    else if (doRedo) do_redo(p);
    else if (!splitReq.empty()) { split_clip(p, splitReq, st.playhead); g_bufFor.clear(); g_undoDirty = true; }
    else if (!dupReq.empty())   { bool dragDup = g_dupAtStart > -1e17;      // Ctrl+drag keeps dragging the original; Ctrl+D selects the copy
                                  std::string nid = duplicate_clip(p, dupReq, g_dupAtStart); g_dupAtStart = -1e18;
                                  if (!nid.empty() && !dragDup) st.selected = nid; g_bufFor.clear(); g_undoDirty = true; }
    else if (!delReq.empty())   { delete_clip(p, delReq); st.selected.clear(); g_bufFor.clear(); g_undoDirty = true; }

    lib_poll_gen_done();         // land finished library gens → invalidate caches + rescan
    rembg_poll_done();           // land a finished rembg cut-out → re-key the texture + reload sidecar
    DrawVoiceEditor(p);  // floating tool window (Tools ▸ Voice preset editor)
    draw_sprite_window();        // floating Sprite-sheet processor (Tools ▸ Sprite sheet)

    // AUTOMATIC undo checkpoint — last thing each frame, after every widget/panel + the deferred
    // ops, so IsAnyItemActive() reflects the whole frame. Self-gates to gesture boundaries.
    undo_checkpoint(p);
    g_placeSigEpoch++;   // avatar_place_sig memo turns over per UI frame (edits show next frame)
}

// ═══════════════════════════════ export ═══════════════════════════════════
// Deterministic full-res render → ffmpeg (docs/ARCHITECTURE.md §11). The editor owns the
// video graph walk: each frame is the SAME composite_frame the preview uses (so preview and
// export can't diverge), rendered to an offscreen full-res RT and streamed as raw RGBA to
// stdout. tools/export.sh pipes that into ffmpeg, which encodes + muxes the audio assembled
// from the export PLAN below. Audio stays in ffmpeg's hands (it decodes wav/mp3; the editor
// has no audio decoder) — the editor just lists the clips, offsets, and gains.
static ID3D11Texture2D*        g_expTex = nullptr;
static ID3D11RenderTargetView* g_expRTV = nullptr;
static ID3D11Texture2D*        g_expStaging = nullptr;

static bool create_export_targets(int w, int h) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_expTex))) return false;
    if (FAILED(g_dev->CreateRenderTargetView(g_expTex, nullptr, &g_expRTV))) return false;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &g_expStaging))) return false;
    return true;
}

// Whole-frame `filler` backdrop pre-pass (defined below; used by both the export + preview composites).
static bool filler_backdrop_needed(Project& p, double t, double& blurOut);
static bool render_fill_backdrop(Project& p, double t, double blur);

// Render the composite at time t into the offscreen RT; read it back into out (w*h*4 RGBA).
static void render_export_frame(Project& p, double t, int w, int h, std::vector<unsigned char>& out) {
    g_fillReady = false;   // whole-frame `filler` backdrop (same pre-pass as the live preview → identical export)
    { double fb = 0.0; if (p.width > 0 && p.height > 0 && filler_backdrop_needed(p, t, fb)) g_fillReady = render_fill_backdrop(p, t, fb); }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);   // drive ImGui at full res (no win32 backend)
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    UIState st; st.playhead = t;
    composite_frame(p, st, ImGui::GetBackgroundDrawList(), ImVec2(0, 0), (float)w, (float)h);
    ImGui::Render();

    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    const float clear[4] = {0, 0, 0, 1};
    g_ctx->OMSetRenderTargets(1, &g_expRTV, nullptr);
    g_ctx->ClearRenderTargetView(g_expRTV, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_ctx->CopyResource(g_expStaging, g_expTex);
    D3D11_MAPPED_SUBRESOURCE m;
    out.assign((size_t)w * h * 4, 0);
    if (SUCCEEDED(g_ctx->Map(g_expStaging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < h; ++y)
            memcpy(&out[(size_t)y * w * 4], (unsigned char*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        g_ctx->Unmap(g_expStaging, 0);
    }
}

// ── preview whole-frame blur: offscreen targets + an isolated composite pass (like render_export_frame,
//    but its own RT + a DYNAMIC texture the preview samples). Rendered at HALF project res — it's blurred
//    anyway, so half-res keeps the interactive loop smooth during the transition. ──
static ID3D11Texture2D*          g_pvTex = nullptr;      // RT color
static ID3D11RenderTargetView*   g_pvRTV = nullptr;
static ID3D11Texture2D*          g_pvStaging = nullptr;  // GPU→CPU readback
static ID3D11Texture2D*          g_pvDyn = nullptr;      // CPU→GPU dynamic (what the preview samples via g_pvBlurSrv)
static int g_pvW = 0, g_pvH = 0;
static ImGuiContext* g_mainCtx = nullptr;   // the interactive editor context
static ImGuiContext* g_blurCtx = nullptr;   // private context for the offscreen blur pre-pass (own input queue)

static bool ensure_preview_blur_targets(int w, int h) {
    if (g_pvW == w && g_pvH == h && g_pvTex) return true;
    if (g_pvTex) { g_pvTex->Release(); g_pvTex = nullptr; }
    if (g_pvRTV) { g_pvRTV->Release(); g_pvRTV = nullptr; }
    if (g_pvStaging) { g_pvStaging->Release(); g_pvStaging = nullptr; }
    if (g_pvDyn) { g_pvDyn->Release(); g_pvDyn = nullptr; }
    if (g_pvBlurSrv) { g_pvBlurSrv->Release(); g_pvBlurSrv = nullptr; }
    g_pvW = g_pvH = 0;
    D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_pvTex))) return false;
    if (FAILED(g_dev->CreateRenderTargetView(g_pvTex, nullptr, &g_pvRTV))) return false;
    D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &g_pvStaging))) return false;
    D3D11_TEXTURE2D_DESC dy = d; dy.Usage = D3D11_USAGE_DYNAMIC; dy.BindFlags = D3D11_BIND_SHADER_RESOURCE; dy.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateTexture2D(&dy, nullptr, &g_pvDyn))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(g_pvDyn, nullptr, &g_pvBlurSrv))) return false;
    g_pvW = w; g_pvH = h; return true;
}

// Render the FULL composite at t into g_pvTex, read it back, CPU-blur by sigmaProj, upload → g_pvBlurSrv.
// Runs its OWN ImGui frame (like export), so it must be called BEFORE the main UI frame's NewFrame.
// Returns true on success (→ the preview shows g_pvBlurSrv this frame instead of the live composite).
static bool render_preview_blur(Project& p, double t, float sigmaProj) {
    int w = std::max(2, (int)p.width / 2), h = std::max(2, (int)p.height / 2);   // half-res (blurred anyway)
    if (!ensure_preview_blur_targets(w, h)) return false;
    // Run the offscreen composite on a SEPARATE ImGui context. ImGui::NewFrame() DRAINS the shared
    // input-event queue, so doing this pre-pass on the main context (once per interactive frame) ATE the
    // editor's own mouse drags/clicks — drag the playhead onto a blur clip and the scrubber stopped
    // getting input, so it couldn't leave the clip → the pre-pass ran forever → "frozen indefinitely".
    // A private context has its own input queue + frame state (it shares the font atlas + D3D device).
    ImGuiContext* prev = ImGui::GetCurrentContext();
    if (g_blurCtx) ImGui::SetCurrentContext(g_blurCtx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h); io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
    UIState st; st.playhead = t;
    composite_frame(p, st, ImGui::GetBackgroundDrawList(), ImVec2(0, 0), (float)w, (float)h);
    ImGui::Render();
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1}; g_ctx->RSSetViewports(1, &vp);
    const float clear[4] = {0, 0, 0, 1};
    g_ctx->OMSetRenderTargets(1, &g_pvRTV, nullptr); g_ctx->ClearRenderTargetView(g_pvRTV, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (g_blurCtx) ImGui::SetCurrentContext(prev);   // restore BEFORE the main frame builds — readback/blur below are ImGui-agnostic
    g_ctx->CopyResource(g_pvStaging, g_pvTex);
    std::vector<unsigned char> buf((size_t)w * h * 4, 0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_pvStaging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < h; ++y) memcpy(&buf[(size_t)y * w * 4], (unsigned char*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        g_ctx->Unmap(g_pvStaging, 0);
    }
    blur_rgba(buf, w, h, sigmaProj * (float)w / (float)p.width);   // project-px sigma → this buffer's px
    D3D11_MAPPED_SUBRESOURCE mw;
    if (SUCCEEDED(g_ctx->Map(g_pvDyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
        for (int y = 0; y < h; ++y) memcpy((unsigned char*)mw.pData + (size_t)y * mw.RowPitch, &buf[(size_t)y * w * 4], (size_t)w * 4);
        g_ctx->Unmap(g_pvDyn, 0);
    }
    return true;
}

// ── whole-frame `filler` backdrop pre-pass ──────────────────────────────────
// Renders the FULL foreground composite (every layer EXCEPT fillers) at half res, reads it back, and runs
// the SAME heavy separable-gaussian (`blur_rgba`) the old plate-filler + the `blur` clip use — so the
// backdrop is a SMOOTH gradient (not a pixelated bilinear-upscale), the content colours bleed out to fill
// the whole frame, and it tracks every on-screen layer instead of a single guessed plate. (Bilinear-
// upscaling a tiny RT was pixelated + left the content's silhouette as a dark "shadow" over black.)
static ID3D11Texture2D*        g_fillTex = nullptr;   // half-res RT the composite is drawn into
static ID3D11RenderTargetView* g_fillRTV = nullptr;
static ID3D11Texture2D*        g_fillStaging = nullptr;  // GPU→CPU readback
static ID3D11Texture2D*        g_fillDyn = nullptr;      // CPU→GPU dynamic (blurred result the filler samples via g_fillSrv)
static bool ensure_fill_targets(int w, int h) {
    if (g_fillW == w && g_fillH == h && g_fillTex) return true;
    if (g_fillTex) { g_fillTex->Release(); g_fillTex = nullptr; }
    if (g_fillRTV) { g_fillRTV->Release(); g_fillRTV = nullptr; }
    if (g_fillStaging) { g_fillStaging->Release(); g_fillStaging = nullptr; }
    if (g_fillDyn) { g_fillDyn->Release(); g_fillDyn = nullptr; }
    if (g_fillSrv) { g_fillSrv->Release(); g_fillSrv = nullptr; }
    g_fillW = g_fillH = 0;
    D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_fillTex))) return false;
    if (FAILED(g_dev->CreateRenderTargetView(g_fillTex, nullptr, &g_fillRTV))) return false;
    D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &g_fillStaging))) return false;
    D3D11_TEXTURE2D_DESC dy = d; dy.Usage = D3D11_USAGE_DYNAMIC; dy.BindFlags = D3D11_BIND_SHADER_RESOURCE; dy.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateTexture2D(&dy, nullptr, &g_fillDyn))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(g_fillDyn, nullptr, &g_fillSrv))) return false;
    g_fillW = w; g_fillH = h; return true;
}
// Any `filler` active at t? (+ the blurriest active filler's `blur`, which sets the gaussian sigma.)
static bool filler_backdrop_needed(Project& p, double t, double& blurOut) {
    bool any = false; blurOut = 30.0;
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        if (c.type != "filler" || !clip_active(c, t)) continue;
        double b = anim_param(c, "blur", t, 30.0);
        blurOut = any ? std::max(blurOut, b) : b; any = true;
    }
    return any;
}
// Render the foreground composite (fillers excluded) at HALF res → readback → heavy gaussian → g_fillDyn.
// Runs its OWN ImGui frame on the private context, so it MUST be called before the main NewFrame.
static bool render_fill_backdrop(Project& p, double t, double blur) {
    int w = std::max(2, (int)p.width / 2), h = std::max(2, (int)p.height / 2);
    if (!ensure_fill_targets(w, h)) return false;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    if (g_blurCtx) ImGui::SetCurrentContext(g_blurCtx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h); io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
    UIState st; st.playhead = t;
    g_compositeSkipFillers = true;                       // fillers draw nothing in the pre-pass (no feedback)
    composite_frame(p, st, ImGui::GetBackgroundDrawList(), ImVec2(0, 0), (float)w, (float)h);
    g_compositeSkipFillers = false;
    ImGui::Render();
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1}; g_ctx->RSSetViewports(1, &vp);
    const float clear[4] = {0, 0, 0, 1};
    g_ctx->OMSetRenderTargets(1, &g_fillRTV, nullptr); g_ctx->ClearRenderTargetView(g_fillRTV, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (g_blurCtx) ImGui::SetCurrentContext(prev);       // restore BEFORE the main frame builds — readback/blur below are ImGui-agnostic
    g_ctx->CopyResource(g_fillStaging, g_fillTex);
    std::vector<unsigned char> buf((size_t)w * h * 4, 0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_fillStaging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < h; ++y) memcpy(&buf[(size_t)y * w * 4], (unsigned char*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        g_ctx->Unmap(g_fillStaging, 0);
    }
    // HEAVY gaussian: dissolves shapes into a smooth colour gradient + bleeds content across the frame
    // (a bg fill wants a much stronger blur than a scene-cut `blur` clip). sigma in THIS buffer's px.
    float sigma = (float)std::max(blur * 1.6, 42.0) * (float)w / (float)p.width;
    blur_rgba(buf, w, h, sigma);
    D3D11_MAPPED_SUBRESOURCE mw;
    if (SUCCEEDED(g_ctx->Map(g_fillDyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
        for (int y = 0; y < h; ++y) memcpy((unsigned char*)mw.pData + (size_t)y * mw.RowPitch, &buf[(size_t)y * w * 4], (size_t)w * 4);
        g_ctx->Unmap(g_fillDyn, 0);
    }
    return true;
}

// The export plan: dimensions + the audio assembly recipe + auto-assembled credits.
static void write_export_plan(Project& p, const std::string& path, int W, int H, int fps, int frames, double dur) {
    json plan;
    plan["title"] = p.title;
    plan["width"] = W; plan["height"] = H; plan["fps"] = fps; plan["frames"] = frames; plan["duration"] = dur;
    json audio = json::array();
    auto duckWins = collect_duck_windows(p);   // music dips around gag cues (same as preview)
    for (auto& tk : p.tracks) {
        if (tk.kind != "audio") continue;
        for (auto& rid : tk.rows) {
            auto rit = p.rows.find(rid);
            if (rit == p.rows.end()) continue;
            for (auto& cid : rit->second.clips) {
                auto cit = p.clips.find(cid);
                if (cit == p.clips.end() || cit->second.asset.empty()) continue;
                Clip& c = cit->second;
                auto au = p.asset_uri.find(c.asset);
                if (au == p.asset_uri.end()) continue;
                std::string ap = resolve_asset(au->second);
                std::ifstream f(ap);
                if (!f.good()) { fprintf(stderr, "export: skip missing audio %s\n", ap.c_str()); continue; }
                double gdb = c.params.value("gain_db", 0.0);
                gdb += rit->second.params.value("gain_db", 0.0);   // per-lane/track volume (same as the preview mixer)
                // loudness normalization (WAV only — measurable here; mp3 keeps its gain_db and
                // ffmpeg handles it). Clip param, or inherited from the row — same as the preview mixer.
                bool norm = c.params.value("normalize", false);
                double ntgt = c.params.value("normalize_db", -20.0);
                if (!c.params.contains("normalize")) norm = rit->second.params.value("normalize", false);
                if (!c.params.contains("normalize_db")) ntgt = rit->second.params.value("normalize_db", -20.0);
                if (norm) { Pcm* pc = get_pcm(au->second); if (pc) gdb += normalize_gain_db(pc, ntgt); }
                // GLOBAL speech boost (meta.speech_gain_db, +8 default) — a fixed dB on every tts clip on top
                // of normalize + clip/lane gain (relative dynamics unchanged). Stays in the static `gain_db`
                // even with a ramp (the kf subtracts only the clip's OWN gain below), same as the mixer.
                if (rit->second.type == "tts") gdb += p.speechGainDb;
                // tts clips take the project default speech rate when they carry none (shorts ~1.3x)
                double defRate = rit->second.type == "tts" ? p.speechRate : 1.0;
                // A clip nudged past timeline zero (start < 0) must NOT reach ffmpeg raw: adelay
                // rejects a negative delay and the whole filtergraph dies (the hook-footage export
                // failure). Clamp = trim the head: shift the in-point by the clipped span (at
                // source rate) and shorten dur — exactly what the preview mixer audibly plays.
                double crate = c.params.value("rate", defRate);
                double cstart = c.start, cdur = c.dur, cin = c.params.value("in", 0.0);
                if (cstart < 0) { cin += -cstart * crate; cdur += cstart; cstart = 0.0; }
                if (cdur <= 1e-6) continue;                        // entirely before t=0
                // keyframed gain_db = the music-lane volume RAMP → a piecewise-linear ffmpeg volume
                // EXPRESSION in clip-local time (the exact envelope the preview mixer evaluates). The
                // static gain_db becomes lane+normalize only (the keyframes carry the clip gain).
                std::string envExpr;
                auto gkf = c.keyframes.find("params.gain_db");
                if (gkf != c.keyframes.end() && gkf->second.size() >= 2) {
                    auto& ks = gkf->second;
                    auto lin = [](double db){ return std::pow(10.0, db / 20.0); };
                    auto num = [](double v){ char b[48]; snprintf(b, sizeof b, "%.5f", v); return std::string(b); };
                    // nested if(), built with std::string (NOT a fixed buffer — the tail is the whole
                    // remaining expression and grows with the keyframe count; a snprintf buffer silently
                    // TRUNCATED it → a malformed expr that ffmpeg evaluated to 0 in spots = dropouts).
                    std::string tail = num(lin(ks.back().v.empty() ? 0.0 : ks.back().v[0]));
                    for (int i = (int)ks.size() - 2; i >= 0; --i) {
                        double T0 = ks[i].t - cstart, T1 = ks[i + 1].t - cstart;   // clip-local (post-clamp)
                        double G0 = lin(ks[i].v.empty() ? 0.0 : ks[i].v[0]);
                        double G1 = lin(ks[i + 1].v.empty() ? 0.0 : ks[i + 1].v[0]);
                        std::string body = (T1 - T0 < 1e-4)
                            ? num(G0)
                            : num(G0) + "+(" + num((G1 - G0) / (T1 - T0)) + ")*(t-(" + num(T0) + "))";
                        tail = "if(lt(t," + num(T1) + ")," + body + "," + tail + ")";
                    }
                    double first = lin(ks.front().v.empty() ? 0.0 : ks.front().v[0]);
                    envExpr = "if(lt(t," + num(ks.front().t - cstart) + ")," + num(first) + "," + tail + ")";
                    gdb -= c.params.value("gain_db", 0.0);          // static volume = lane+normalize; kf carries the rest
                }
                json entry = json{{"path", ap}, {"start", cstart}, {"dur", cdur},
                                  {"in", cin}, {"gain_db", gdb},
                                  {"rate", crate}};
                // music duck: an ffmpeg volume EXPRESSION in clip-local time, the exact
                // duck_factor shape the preview mixer plays (export.sh chains it after the gain)
                std::string duckExpr;
                if (rit->second.type == "music" && !duckWins.empty()) {
                    std::string dmax;
                    for (auto& w : duckWins) {
                        double A = w.a - cstart, B = w.b - cstart;
                        if (B + DUCK_IN < 0 || A > cdur) continue;   // window misses this clip
                        char term[200];   // commas survive: export.sh single-quotes the whole value
                        snprintf(term, sizeof term, "%.3f*clip(min((t-(%.3f))/%.2f,((%.3f)-t)/%.2f),0,1)",
                                 1.0 - w.floor, A, DUCK_OUT, B + DUCK_IN, DUCK_IN);
                        dmax = dmax.empty() ? std::string(term) : ("max(" + dmax + "," + term + ")");
                    }
                    if (!dmax.empty()) duckExpr = "1-(" + dmax + ")";
                }
                // ramp × duck (both are clip-local multipliers) → one vol_expr
                if (!envExpr.empty() && !duckExpr.empty()) entry["vol_expr"] = "(" + envExpr + ")*(" + duckExpr + ")";
                else if (!envExpr.empty())                 entry["vol_expr"] = envExpr;
                else if (!duckExpr.empty())                entry["vol_expr"] = duckExpr;
                audio.push_back(entry);
            }
        }
    }
    // video clips carry their own audio (default 12%) → point ffmpeg at the mp4 (it pulls the audio
    // stream); same start/in/dur/rate/gain recipe as an audio clip. `mute_audio:true` drops it. Keeps
    // export == the preview mixer (collect_audio).
    for (auto& kv : p.clips) {
        Clip& c = kv.second;
        if (c.asset.empty() || c.params.value("mute_audio", false)) continue;
        // retimed footage (speed ≠ 1 / pingpong loop) goes silent — mirror the preview mixer
        if (std::fabs(c.params.value("speed", 1.0) - 1.0) > 1e-3) continue;
        if (c.params.is_object() && c.params.contains("loop") && c.params["loop"].is_string()) continue;
        auto rit = p.rows.find(c.row);
        if (rit == p.rows.end() || rit->second.type != "video") continue;
        double vol = c.params.value("video_volume", 0.12);
        if (vol <= 0.0) continue;
        auto vmi = p.asset_video.find(c.asset);
        std::string src = (vmi != p.asset_video.end() && !vmi->second.src.empty()) ? vmi->second.src : std::string();
        if (src.empty()) { auto au2 = p.asset_uri.find(c.asset); if (au2 != p.asset_uri.end()) src = au2->second; }
        if (src.empty()) continue;
        std::string ap = resolve_asset(src);
        std::ifstream f(ap); if (!f.good()) continue;
        double gdb = c.params.value("gain_db", 0.0) + 20.0 * std::log10(std::max(1e-4, vol))
                   + rit->second.params.value("gain_db", 0.0);
        // clamp a past-zero start (same recipe as the audio rows — adelay rejects negatives)
        double vrate = c.params.value("rate", 1.0);
        double vstart = c.start, vdur = c.dur, vin = c.params.value("in", 0.0);
        if (vstart < 0) { vin += -vstart * vrate; vdur += vstart; vstart = 0.0; }
        if (vdur <= 1e-6) continue;
        audio.push_back(json{{"path", ap}, {"start", vstart}, {"dur", vdur},
                             {"in", vin}, {"gain_db", gdb},
                             {"rate", vrate}, {"from_video", true}});
    }
    // built-in transition SFX — the SAME event list the preview mixer plays (meta.sfx gates inside)
    for (auto& e : collect_sfx_events(p)) {
        std::string ap = resolve_asset("library/sfx/" + e.wav);
        std::ifstream f(ap); if (!f.good()) continue;         // stock pack not fetched → none
        Pcm* pc = get_pcm("library/sfx/" + e.wav); if (!pc) continue;
        double st = e.t, sdur = pc->dur, sin2 = 0.0;          // clamp a pre-zero cue (adelay ≥ 0)
        if (st < 0) { sin2 = -st; sdur += st; st = 0.0; }
        if (sdur <= 1e-6) continue;
        audio.push_back(json{{"path", ap}, {"start", st}, {"dur", sdur}, {"in", sin2},
                             {"gain_db", e.gainDb}, {"rate", 1.0}});
    }
    plan["audio"] = audio;
    plan["master_gain_db"] = p.masterGainDb;   // final-mix gain — export.sh applies it after amix
    json credits = json::array();  // auto-credits — every asset actually USED by a clip, deduped
    std::set<std::string> usedAssets, seenTxt;
    for (auto& kv : p.clips) if (!kv.second.asset.empty()) usedAssets.insert(kv.second.asset);
    json assetsj = p.doc.value("assets", json::object());
    for (auto& kv : assetsj.items()) {
        if (!usedAssets.count(kv.key())) continue;                 // imported but not placed → don't credit an unheard song
        json att = kv.value().value("meta", json::object()).value("attribution", json());
        if (att.is_object()) {
            std::string txt = att.value("attribution_text", std::string());
            if (!txt.empty() && seenTxt.insert(txt).second) credits.push_back(txt);   // dedupe reused songs
        }
    }
    plan["credits"] = credits;
    std::ofstream out(path);
    out << plan.dump(2) << "\n";
    fprintf(stderr, "export plan: %dx%d @%dfps, %d frames, %zu audio, %zu credits → %s\n",
            W, H, fps, frames, audio.size(), credits.size(), path.c_str());
}

// Stream `frames` of raw RGBA to stdout (binary). tools/export.sh pipes this into ffmpeg.
static int run_export_stream(Project& p, int W, int H, int fps, int frames) {
    if (!create_export_targets(W, H)) { fprintf(stderr, "export: render target alloc failed\n"); return 1; }
    setmode(fileno(stdout), _O_BINARY);  // no CRLF translation on the frame bytes
    std::vector<unsigned char> buf;
    for (int i = 0; i < frames; ++i) {
        double t = (double)i / fps;
        render_export_frame(p, t, W, H, buf);
        float sig = 0.f; if (frame_blur_strength(p, t, sig) > 0.003f) blur_rgba(buf, W, H, sig * (float)W / (float)p.width);  // whole-frame blur transition
        if (fwrite(buf.data(), 1, buf.size(), stdout) != buf.size()) {
            fprintf(stderr, "export: stdout write failed at frame %d\n", i);
            return 1;
        }
        if (i % 30 == 0) fprintf(stderr, "export: frame %d/%d\r", i, frames);
    }
    fflush(stdout);
    fprintf(stderr, "\nexport: streamed %d frames %dx%d @%dfps\n", frames, W, H, fps);
    return 0;
}

// ─────────────────────────────── main ─────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static UINT g_resizeW = 0, g_resizeH = 0;

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_DROPFILES: {                         // drag an image file onto the timeline
        HDROP hd = (HDROP)wp;
        wchar_t wb[2048] = {0};
        if (DragQueryFileW(hd, 0, wb, 2048)) {
            char ub[2048] = {0};
            WideCharToMultiByte(CP_UTF8, 0, wb, -1, ub, sizeof ub, nullptr, nullptr);
            g_dropPath = ub; DragQueryPoint(hd, &g_dropPt); g_hasDrop = true;
        }
        DragFinish(hd);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Editor chrome theme — the cosmic2d palette (deep-purple base, mint accent, periwinkle focus) +
// tightened rounded metrics, applied over StyleColorsDark(). CHROME ONLY: the video-content fonts and
// compositor colors are untouched (they render into the export, not the editor UI).
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
    // metrics — rounded, breathable, thin borders (the cosmic2d feel)
    s.WindowRounding=8; s.ChildRounding=6; s.FrameRounding=6; s.PopupRounding=6; s.GrabRounding=5; s.TabRounding=6; s.ScrollbarRounding=8;
    s.WindowBorderSize=1; s.ChildBorderSize=1; s.PopupBorderSize=1; s.FrameBorderSize=0; s.SeparatorTextBorderSize=2;
    s.WindowPadding=ImVec2(10,10); s.FramePadding=ImVec2(8,4); s.CellPadding=ImVec2(6,4);
    s.ItemSpacing=ImVec2(8,6); s.ItemInnerSpacing=ImVec2(6,5); s.IndentSpacing=18;
    s.ScrollbarSize=13; s.GrabMinSize=11;
}

int main(int argc, char** argv) {
    std::string projectPath = "examples/signature-opener.slop.json";
    std::string shotPath, shotFramePath, configPath, genClip, exportPlan, splitClip, delClip, dupClip, selectClip, libSelect;
    int seqN = 1;   // --seq-n: render N consecutive frames from --time in ONE process (verify eased motion)
    bool exportStream = false, maskModeAuto = false;   // --mask-mode: open the Viewer's paint tool (for shots)
    int shotFrames = 5;
    double initialTime = 0.0, splitTime = 0.0;
    std::string addAvatarRig; double addAvatarTime = 0.0;   // --add-avatar <rig> [time] (headless place-from-scratch)
    std::vector<std::string> spriteCut;   // --sprite-cut <sheet> <RRGGBB> <fuzz> <prefix> <x,y,w,h>... (headless)
    std::vector<std::string> libGen;      // --lib-gen <image|voice> <name> <prompt|text> [voice-preset] (headless)
    std::string libRegen;                 // --lib-regen <library-file> — regenerate a gen item (fresh seed), keep history
    std::string libRembgFile, libRembgModel;  // --lib-removebg <library-file> [model] — provider bg-removal (headless)
    std::vector<std::string> libCrop, libMask;  // --lib-crop <file> x,y,w,h[|clear] · --lib-mask <file> erase|restore|clear [x,y,w,h]
    std::vector<std::string> clipSize;    // --clip-size <clipId> <wpx> [hpx] — resize a clip (rescales scale keyframes)
    std::string clipMoveId, clipMoveRow;  // --clip-move <clipId> <rowId> — move a clip to another (same-type) row
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sprite-cut") { for (int j = i + 1; j < argc; j++) spriteCut.push_back(argv[j]); i = argc; }
        else if (a == "--clip-size") { if (i + 2 < argc) { clipSize.push_back(argv[++i]); clipSize.push_back(argv[++i]); if (i + 1 < argc && argv[i + 1][0] != '-') clipSize.push_back(argv[++i]); } }
        else if (a == "--clip-move" && i + 2 < argc) { clipMoveId = argv[++i]; clipMoveRow = argv[++i]; }
        else if (a == "--lib-gen") { for (int j = i + 1; j < argc; j++) libGen.push_back(argv[j]); i = argc; }
        else if (a == "--lib-regen" && i + 1 < argc) libRegen = argv[++i];
        else if (a == "--lib-removebg" && i + 1 < argc) { libRembgFile = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') libRembgModel = argv[++i]; }
        else if (a == "--lib-crop") { for (int j = i + 1; j < argc && argv[j][0] != '-'; j++) libCrop.push_back(argv[++i]); }
        else if (a == "--lib-mask") { for (int j = i + 1; j < argc && argv[j][0] != '-'; j++) libMask.push_back(argv[++i]); }
        else if (a == "--sprite-load" && i + 1 < argc) g_spriteAutoload = argv[++i];  // open the sprite panel on a sheet
        else if (a == "--shot" && i + 1 < argc) shotPath = argv[++i];
        else if (a == "--shot-frame" && i + 1 < argc) shotFramePath = argv[++i];  // full-res composite PNG
        else if (a == "--seq-n" && i + 1 < argc) seqN = atoi(argv[++i]);           // render N consecutive frames (test animation)
        else if (a == "--frames" && i + 1 < argc) shotFrames = atoi(argv[++i]);
        else if (a == "--tl-vscroll" && i + 1 < argc) g_tlVScroll = (float)atof(argv[++i]);  // pin lane scroll (shots)
        else if (a == "--cache" && i + 1 < argc) g_cacheDir = argv[++i];
        else if (a == "--time" && i + 1 < argc) initialTime = atof(argv[++i]);
        else if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (a == "--generate" && i + 1 < argc) genClip = argv[++i];
        else if (a == "--select" && i + 1 < argc) selectClip = argv[++i];  // pre-select a clip (inspector shots)
        else if (a == "--lib-select" && i + 1 < argc) libSelect = argv[++i];  // focus a library item in the Viewer (shots)
        else if (a == "--mask-mode") maskModeAuto = true;                       // open the Viewer paint tool (shots)
        else if (a == "--split" && i + 2 < argc) { splitClip = argv[++i]; splitTime = atof(argv[++i]); }
        else if (a == "--delete" && i + 1 < argc) delClip = argv[++i];
        else if (a == "--duplicate" && i + 1 < argc) dupClip = argv[++i];   // copy a clip → right after it
        else if (a == "--add-avatar" && i + 1 < argc) { addAvatarRig = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') addAvatarTime = atof(argv[++i]); }
        else if (a == "--export-plan" && i + 1 < argc) exportPlan = argv[++i];
        else if (a == "--export") exportStream = true;
#ifdef SLOP_LIBAV
        else if (a == "--no-video-decode") g_videoDirect = false;  // force the proxy fallback (A/B)
#endif
        else if (a.rfind("--", 0) != 0) projectPath = a;
    }
    {   // per-project library root: <projdir>/assets/<stem>/ (the examples/assets/luckymas convention).
        // Set even if the dir doesn't exist yet — the first project-scoped import/gen creates it.
        size_t sl = projectPath.find_last_of("/\\");
        std::string pdir = sl == std::string::npos ? "." : projectPath.substr(0, sl);
        std::string stem = sl == std::string::npos ? projectPath : projectPath.substr(sl + 1);
        size_t d = stem.find('.'); if (d != std::string::npos) stem = stem.substr(0, d);   // strip .slop.json
        if (!stem.empty()) g_projLibDir = pdir + "/assets/" + stem;
        g_projDir = pdir;                                    // project-relative uri fallback
    }
    // Anchor the repo-relative roots (library/ beds, cache/ gens) to the code repo independent of the
    // launch CWD — the dashboard launches at repo-root, the standalone launcher at the project dir,
    // and the two asset families (library/ vs assets/) want opposite roots. Falls back to CWD-relative.
    g_repoRoot = derive_repo_root();
    if (g_repoRoot != ".") {
        if (g_libraryDir == "library") g_libraryDir = g_repoRoot + "/library";
        bool cacheRel = !g_cacheDir.empty() && g_cacheDir[0] != '/' && g_cacheDir[0] != '\\' &&
                        g_cacheDir.find(':') == std::string::npos;
        if (cacheRel) g_cacheDir = g_repoRoot + "/" + g_cacheDir;
    }
    // Headless sprite-sheet cut (verification + agent use; no window/D3D — pure CPU + stb write).
    // --sprite-cut <sheet.png> <RRGGBB-key> <fuzz> <prefix> <x,y,w,h> [<x,y,w,h>...]  → library/images/<prefix>-NN.png
    if (!spriteCut.empty()) {
        if (spriteCut.size() < 5) { fprintf(stderr, "usage: --sprite-cut <sheet> <RRGGBB> <fuzz> <prefix> <x,y,w,h>...\n"); return 2; }
        int w, h, n; unsigned char* d = stbi_load(spriteCut[0].c_str(), &w, &h, &n, 4);
        if (!d) { fprintf(stderr, "sprite-cut: load failed %s\n", spriteCut[0].c_str()); return 1; }
        std::vector<unsigned char> src(d, d + (size_t)w * h * 4); stbi_image_free(d);
        unsigned kc = (unsigned)strtoul(spriteCut[1].c_str(), nullptr, 16);
        float fuzz = (float)atof(spriteCut[2].c_str());
        std::string prefix = spriteCut[3];
        std::vector<SpriteRect> rects;
        for (size_t k = 4; k < spriteCut.size(); k++) {
            int x, y, rw, rh;
            if (sscanf(spriteCut[k].c_str(), "%d,%d,%d,%d", &x, &y, &rw, &rh) == 4) rects.push_back({x, y, rw, rh});
        }
        int cnt = sprite_export_to_library(src, w, h, (kc >> 16) & 255, (kc >> 8) & 255, kc & 255, fuzz, rects, prefix);
        fprintf(stderr, "sprite-cut: wrote %d/%d sprites → %s/images/%s-NN.png\n", cnt, (int)rects.size(), g_libraryDir.c_str(), prefix.c_str());
        return cnt > 0 ? 0 : 1;
    }

    // Headless clip resize (verify + agent use; no window/D3D — native px via stbi/libav, then the
    // SAME clip_rescale the inspector uses, so a keyframed scale is rescaled whole). Aspect-locked
    // when only W is given. --clip-size <clipId> <wpx> [hpx]
    if (!clipSize.empty()) {
        Project sp = load_project(projectPath);
        if (!sp.ok) { fprintf(stderr, "clip-size: load failed: %s\n", sp.error.c_str()); return 1; }
        auto cit = sp.clips.find(clipSize[0]);
        if (cit == sp.clips.end()) { fprintf(stderr, "clip-size: no such clip '%s'\n", clipSize[0].c_str()); return 1; }
        Clip& c = cit->second;
        float nw = 0, nh = 0;                          // native px without D3D: stbi_info / libav decoder
        if (c.type == "image") { auto au = sp.asset_uri.find(c.asset); int iw, ih, n; if (au != sp.asset_uri.end() && stbi_info(resolve_asset(au->second).c_str(), &iw, &ih, &n)) { nw = (float)iw; nh = (float)ih; } }
#ifdef SLOP_LIBAV
        else if (c.type == "video") { auto vm = sp.asset_video.find(c.asset); if (vm != sp.asset_video.end() && !vm->second.src.empty()) { VideoDecoder* d = get_decoder(resolve_asset(vm->second.src)); if (d && d->w > 0) { nw = (float)d->w; nh = (float)d->h; } } }
#endif
        if (nw <= 0) { fprintf(stderr, "clip-size: can't determine native px for clip '%s' (type %s)\n", clipSize[0].c_str(), c.type.c_str()); return 1; }
        double targW = atof(clipSize[1].c_str()), targH = clipSize.size() > 2 ? atof(clipSize[2].c_str()) : 0.0;
        double repX = clip_rep_scale(c, 0, c.start), repY = clip_rep_scale(c, 1, c.start);
        double rx = targW / (nw * (repX > 1e-9 ? repX : 1));
        double ry = (targH > 0) ? targH / (nh * (repY > 1e-9 ? repY : 1)) : rx;   // aspect-locked when H omitted
        clip_rescale(c, rx, ry);
        save_project(sp);
        fprintf(stderr, "clip-size OK: %s → %.0f×%.0f px (native %.0f×%.0f, scale ×%.3f,%.3f)\n",
                clipSize[0].c_str(), nw * clip_rep_scale(c, 0, c.start), nh * clip_rep_scale(c, 1, c.start), nw, nh, clip_rep_scale(c, 0, c.start), clip_rep_scale(c, 1, c.start));
        return 0;
    }

    // Headless move a clip to another (same-type) row — verify + agent use. --clip-move <clipId> <rowId>
    if (!clipMoveId.empty()) {
        Project mp = load_project(projectPath);
        if (!mp.ok) { fprintf(stderr, "clip-move: load failed: %s\n", mp.error.c_str()); return 1; }
        auto cit = mp.clips.find(clipMoveId);
        auto rit = mp.rows.find(clipMoveRow);
        if (cit == mp.clips.end()) { fprintf(stderr, "clip-move: no such clip '%s'\n", clipMoveId.c_str()); return 1; }
        if (rit == mp.rows.end()) { fprintf(stderr, "clip-move: no such row '%s'\n", clipMoveRow.c_str()); return 1; }
        if (rit->second.type != cit->second.type) { fprintf(stderr, "clip-move: type mismatch (clip '%s' is %s, row '%s' is %s)\n", clipMoveId.c_str(), cit->second.type.c_str(), clipMoveRow.c_str(), rit->second.type.c_str()); return 1; }
        if (!move_clip_to_row(mp, clipMoveId, clipMoveRow)) { fprintf(stderr, "clip-move: no-op (already on '%s')\n", clipMoveRow.c_str()); return 1; }
        save_project(mp);
        fprintf(stderr, "clip-move OK: %s → row %s\n", clipMoveId.c_str(), clipMoveRow.c_str());
        return 0;
    }

    const bool shot = !shotPath.empty();
    const int CW = 1600, CH = 900;

    // generation infra: provider endpoints (config) + thread-shared job/health state.
    InitializeCriticalSection(&g_genCS);
    InitializeCriticalSection(&g_healthCS);
    InitializeCriticalSection(&g_emoThumbCS);   // async emotion-picker thumbnails
    if (configPath.empty()) {  // prefer config.toml, fall back to the committed example
        if (load_config("config.toml")) configPath = "config.toml";
        else if (load_config("config.example.toml")) configPath = "config.example.toml";
    } else {
        load_config(configPath);
    }
    fprintf(stderr, "config: %s (%zu providers)\n",
            configPath.empty() ? "<none>" : configPath.c_str(), g_providers.size());

    // Headless generate (LLM/automation + smoke test): no window/D3D needed — run the
    // same submit→poll→download→persist path as the Generate button, then exit.
    if (!genClip.empty()) {
        Project gp = load_project(projectPath);
        if (!gp.ok) { fprintf(stderr, "generate: project load failed: %s\n", gp.error.c_str()); return 1; }
        if (!gp.clips.count(genClip)) { fprintf(stderr, "generate: no such clip '%s'\n", genClip.c_str()); return 1; }
        start_generate(gp, genClip);
        for (int i = 0; i < 2400; ++i) {  // up to ~4 min
            apply_generations(gp, g_bufFor);
            EnterCriticalSection(&g_genCS);
            auto it = g_gen.find(genClip);
            int state = (it == g_gen.end()) ? -1 : it->second.state;
            std::string msg = (it == g_gen.end()) ? "" : it->second.message;
            LeaveCriticalSection(&g_genCS);
            if (state == -1) {  // applied + cleared → success
                auto cit = gp.clips.find(genClip);
                save_project(gp);   // headless: persist the generation to disk (interactive defers to Ctrl+S/undo checkpoint)
                fprintf(stderr, "generate OK: %s → asset %s\n", genClip.c_str(),
                        cit != gp.clips.end() ? cit->second.asset.c_str() : "?");
                return 0;
            }
            if (state == 3) { fprintf(stderr, "generate FAILED: %s\n", msg.c_str()); return 1; }
            Sleep(100);
        }
        fprintf(stderr, "generate: timed out\n");
        return 1;
    }

    // Headless library gen (automation + smoke test): create a NEW gen library item via a provider
    // (no window/D3D). image → library/images, voice → library/audio, each + a .meta.json sidecar.
    if (!libGen.empty()) {
        if (libGen.size() < 3) { fprintf(stderr, "usage: --lib-gen <image|voice> <name> <prompt|text> [voice-preset]\n"); return 1; }
        std::string kind = libGen[0], name = libGen[1], text = libGen[2];
        LibType t; std::string prov, cap; json recipe;
        if (kind == "image") {
            t = LIB_IMAGE; prov = "image"; cap = "text2image";
            recipe = {{"prompt", text}, {"seed", 1234}, {"width", 1024}, {"height", 1024},
                      {"arch", "anima"}, {"lora", "gemma-san-anima"}, {"lora_strength", 0.9}};
        } else if (kind == "voice") {
            t = LIB_AUDIO; prov = "tts"; cap = "speech";
            std::string preset = libGen.size() > 3 ? libGen[3] : "gemma-san-deep-jp";
            recipe = {{"text", text}, {"voice_preset", preset}, {"seed", 7}};
        } else { fprintf(stderr, "lib-gen: kind must be image|voice\n"); return 1; }
        std::string base = lib_unique_base(t, name);
        std::string key = lib_dest_root() + "/" + lib_subdir(t) + "/" + base;   // project lib when a project was passed
        start_lib_gen(t, base, prov, cap, lib_job_body(cap, recipe), recipe, false);
        for (int i = 0; i < 2400; ++i) {  // up to ~4 min
            LibGenState s = libgen_get(key);
            if (s.state == 2) { fprintf(stderr, "lib-gen OK → %s.* (+ .meta.json)\n", key.c_str()); return 0; }
            if (s.state == 3) { fprintf(stderr, "lib-gen FAILED: %s\n", s.message.c_str()); return 1; }
            Sleep(100);
        }
        fprintf(stderr, "lib-gen: timed out\n");
        return 1;
    }

    // Headless library REgen (automation + verify): regenerate an existing gen item in place with a
    // fresh seed, demoting the prior gen into the sidecar history. (Same path as the Viewer button.)
    if (!libRegen.empty()) {
        json side = lib_load_sidecar(libRegen);
        if (!lib_is_gen(side)) { fprintf(stderr, "lib-regen: %s has no gen sidecar\n", libRegen.c_str()); return 1; }
        std::string fn = libRegen.substr(libRegen.find_last_of("/\\") + 1);
        LibType t = lib_type_of(ext_lower(fn));
        std::string base = strip_ext(fn);
        std::string cap = side.value("cap", std::string(t == LIB_IMAGE ? "text2image" : "speech"));
        std::string prov = side.value("provider", std::string(t == LIB_IMAGE ? "image" : "tts"));
        json recipe = side.value("params", json::object());
        recipe["seed"] = recipe.value("seed", 0) + 1;  // fresh
        std::string ownDir = libRegen.substr(0, libRegen.find_last_of("/\\"));   // regen in place
        std::string key = ownDir + "/" + lib_safe_name(base);
        start_lib_gen(t, base, prov, cap, lib_job_body(cap, recipe), recipe, true, ownDir);
        for (int i = 0; i < 2400; ++i) {
            LibGenState s = libgen_get(key);
            if (s.state == 2) { fprintf(stderr, "lib-regen OK → %s (history kept)\n", key.c_str()); return 0; }
            if (s.state == 3) { fprintf(stderr, "lib-regen FAILED: %s\n", s.message.c_str()); return 1; }
            Sleep(100);
        }
        fprintf(stderr, "lib-regen: timed out\n");
        return 1;
    }

    // Headless rembg cut-out (automation + verify): submit a library image to the rembg provider and
    // record the cutout in its sidecar removebg block (same path as the Viewer's Cut out button).
    if (!libRembgFile.empty()) {
        std::string model = libRembgModel.empty() ? "isnet-anime" : libRembgModel;
        auto pc = g_providers.find("rembg");
        if (pc == g_providers.end() || !pc->second.enabled || pc->second.url.empty()) {
            fprintf(stderr, "lib-removebg: provider 'rembg' not configured (config.toml)\n"); return 1;
        }
        std::string err;
        auto prog = [](float p, const std::string& m) { fprintf(stderr, "  %.0f%% %s\n", p * 100.f, m.c_str()); };
        if (rembg_cut_sync(pc->second.url, g_cacheDir, libRembgFile, model, prog, err)) {
            fprintf(stderr, "lib-removebg OK → %s (removebg.method=rembg, model=%s)\n", libRembgFile.c_str(), model.c_str());
            return 0;
        }
        fprintf(stderr, "lib-removebg FAILED: %s\n", err.c_str());
        return 1;
    }

    // Headless crop/mask (verify + agent use; no window/D3D — pure CPU + stb). Non-destructive: the
    // crop rect + the painted mask ride the item's sidecar, applied on the fly in get_texture.
    // --lib-crop <file> x,y,w,h | clear     --lib-mask <file> erase|restore|clear [x,y,w,h]
    if (!libCrop.empty()) {
        if (libCrop.size() < 2) { fprintf(stderr, "usage: --lib-crop <file> x,y,w,h | clear\n"); return 1; }
        json side = lib_load_sidecar(libCrop[0]);
        if (libCrop[1] == "clear") { side.erase("crop"); }
        else { int x, y, w2, h2; if (sscanf(libCrop[1].c_str(), "%d,%d,%d,%d", &x, &y, &w2, &h2) != 4) { fprintf(stderr, "lib-crop: bad rect '%s'\n", libCrop[1].c_str()); return 1; } side["crop"] = json::array({x, y, w2, h2}); }
        lib_save_sidecar(libCrop[0], side); invalidate_texture(libCrop[0]);
        fprintf(stderr, "lib-crop OK: %s %s\n", libCrop[0].c_str(), libCrop[1].c_str());
        return 0;
    }
    if (!libMask.empty()) {
        if (libMask.size() < 2) { fprintf(stderr, "usage: --lib-mask <file> erase|restore|clear [x,y,w,h]\n"); return 1; }
        if (libMask[1] == "clear") { json side = lib_load_sidecar(libMask[0]); side.erase("mask"); lib_save_sidecar(libMask[0], side);
                                     fprintf(stderr, "lib-mask OK: cleared %s\n", libMask[0].c_str()); return 0; }
        int iw, ih, in; if (!stbi_info(resolve_asset(libMask[0]).c_str(), &iw, &ih, &in)) { fprintf(stderr, "lib-mask: can't read %s\n", libMask[0].c_str()); return 1; }
        std::vector<unsigned char> m = mask_load_or_blank(libMask[0], iw, ih);
        int val = (libMask[1] == "restore") ? 255 : 0;
        if (libMask.size() >= 3) { int x, y, w2, h2; if (sscanf(libMask[2].c_str(), "%d,%d,%d,%d", &x, &y, &w2, &h2) != 4) { fprintf(stderr, "lib-mask: bad rect '%s'\n", libMask[2].c_str()); return 1; } mask_box(m, iw, ih, x, y, w2, h2, val); }
        else for (auto& v : m) v = (unsigned char)val;   // whole-image (rare; mostly for clearing to a known state)
        mask_save(libMask[0], m, iw, ih); invalidate_texture(libMask[0]);
        fprintf(stderr, "lib-mask OK: %s %s%s\n", libMask[0].c_str(), libMask[1].c_str(), libMask.size() >= 3 ? (" " + libMask[2]).c_str() : "");
        return 0;
    }

    // Headless edit ops (automation / LLM-driving, no window): split a clip at a time, or
    // delete one, then persist + exit. (Same path as the editor's S / Delete buttons.)
    if (!splitClip.empty() || !delClip.empty() || !dupClip.empty()) {
        Project sp = load_project(projectPath);
        if (!sp.ok) { fprintf(stderr, "edit: project load failed: %s\n", sp.error.c_str()); return 1; }
        if (!splitClip.empty()) {
            if (!sp.clips.count(splitClip)) { fprintf(stderr, "split: no such clip '%s'\n", splitClip.c_str()); return 1; }
            split_clip(sp, splitClip, splitTime);
        } else if (!dupClip.empty()) {
            if (!sp.clips.count(dupClip)) { fprintf(stderr, "duplicate: no such clip '%s'\n", dupClip.c_str()); return 1; }
            std::string nid = duplicate_clip(sp, dupClip);
            if (nid.empty()) { fprintf(stderr, "duplicate: failed for '%s'\n", dupClip.c_str()); return 1; }
        } else {
            if (!sp.clips.count(delClip)) { fprintf(stderr, "delete: no such clip '%s'\n", delClip.c_str()); return 1; }
            delete_clip(sp, delClip);
        }
        if (!save_project(sp)) { fprintf(stderr, "edit: save failed for %s\n", sp.path.c_str()); return 1; }
        return 0;
    }

    // Headless place-an-avatar-clip-from-scratch (automation + the from-scratch path's smoke test):
    // ensures an avatar row referencing <rig>, adds a static-pose clip, persists.
    if (!addAvatarRig.empty()) {
        Project ap = load_project(projectPath);
        if (!ap.ok) { fprintf(stderr, "add-avatar: project load failed: %s\n", ap.error.c_str()); return 1; }
        std::string id = add_avatar_clip_at(ap, addAvatarRig, addAvatarTime);
        if (id.empty()) { fprintf(stderr, "add-avatar: failed (no avatar row could be created)\n"); return 1; }
        if (!save_project(ap)) { fprintf(stderr, "add-avatar: save failed\n"); return 1; }
        fprintf(stderr, "add-avatar OK: %s on rig '%s' @ %.2fs\n", id.c_str(), addAvatarRig.c_str(), addAvatarTime);
        return 0;
    }

    // Headless export plan (no window/D3D needed): dimensions + audio recipe + credits.
    if (!exportPlan.empty()) {
        Project ep = load_project(projectPath);
        if (!ep.ok) { fprintf(stderr, "export: project load failed: %s\n", ep.error.c_str()); return 1; }
        double dur = ep.duration();
        int fps = ep.fps > 0 ? ep.fps : 60;
        write_export_plan(ep, exportPlan, (int)ep.width, (int)ep.height, fps, (int)std::ceil(dur * fps), dur);
        return 0;
    }

    if (!shot && !exportStream && shotFramePath.empty()) {  // background provider health polling drives the status dots
        HANDLE h = CreateThread(nullptr, 0, health_worker, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr),
                       nullptr, nullptr, nullptr, nullptr, L"slopstudio", nullptr };
    RegisterClassExW(&wc);
    RECT rc = { 0, 0, CW, CH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"slopstudio", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
    DragAcceptFiles(hwnd, TRUE);   // accept image files dragged onto the window/timeline
    if (!CreateDeviceD3D(hwnd, CW, CH)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        fprintf(stderr, "D3D11 init failed\n");
        return 1;
    }
    if (!shot && !exportStream && shotFramePath.empty()) { ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd); }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& fio = ImGui::GetIO();
    fio.IniFilename = nullptr;
    apply_editor_theme();
    // Load a CJK-capable system font so Japanese (ふふ~, mini-lessons) + non-ASCII render
    // instead of '???'. The first available Windows JP font wins; ranges cover ASCII + kana +
    // common kanji. Falls back to the built-in ASCII font if none is found.
    {
        const char* fonts[] = {"C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\meiryo.ttc",
                               "C:\\Windows\\Fonts\\msgothic.ttc", "C:\\Windows\\Fonts\\msmincho.ttc"};
        const char* chosen = nullptr;
        for (const char* fp : fonts) { FILE* tf = fopen(fp, "rb"); if (!tf) continue; fclose(tf); chosen = fp; break; }
        // JP ranges + the on-screen symbols GetGlyphRangesJapanese omits: ☆★ (らき☆マス /
        // Lucky☆Star), smart quotes/dashes, ♥♪ (Gemma's "fufu~ ♥"), arrows. Without these
        // they render as '?'. The range vector must outlive atlas build → static.
        static ImVector<ImWchar> jpRanges;
        {
            ImFontGlyphRangesBuilder gb;
            gb.AddRanges(fio.Fonts->GetGlyphRangesJapanese());
            gb.AddText(reinterpret_cast<const char*>(u8"…“”‘’—–•·★☆♥♡♪→←⇒×÷「」『』〜～！？"));
            gb.BuildRanges(&jpRanges);
        }
        // UI CHROME font: Inter (cosmic2d) with the CJK face merged so Japanese UI text (asset names,
        // JP-lesson editing) still renders; falls back to the system CJK font if Inter isn't bundled.
        // The VIDEO fonts (g_captionFont / g_monoFont) are UNCHANGED — they render into exports, not chrome.
        std::string interPath = g_repoRoot + "/assets-src/fonts/InterVariable.ttf";
        bool interOk = false; { FILE* itf = fopen(interPath.c_str(), "rb"); if (itf) { fclose(itf); interOk = true; } }
        if (interOk) {
            ImFontConfig ic; ic.RasterizerMultiply = 1.12f;                                        // a touch heavier = crisper UI
            fio.Fonts->AddFontFromFileTTF(interPath.c_str(), 17.0f, &ic);                          // UI default (Latin)
            if (chosen) { ImFontConfig mc; mc.MergeMode = true;
                          fio.Fonts->AddFontFromFileTTF(chosen, 17.0f, &mc, jpRanges.Data); }      // + CJK / symbols
        } else if (chosen) {
            fio.Fonts->AddFontFromFileTTF(chosen, 17.0f, nullptr, jpRanges.Data);                  // fallback: system CJK
        }
        if (chosen)
            g_captionFont = fio.Fonts->AddFontFromFileTTF(chosen, 48.0f, nullptr, jpRanges.Data);  // large captions / JP lessons (video, unchanged)
        // Monospace font for `code` clips (decompilation/source cards). Loaded at a large size so
        // AddText can scale it to any on-screen font_px crisply. Consolas → Lucida Console fallback.
        const char* mono[] = {"C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\lucon.ttf", "C:\\Windows\\Fonts\\cour.ttf"};
        // code-card TITLES use real punctuation (em/en dash, ·, …, arrows) the default ASCII
        // range renders as '?'; add them so "FUN_1234 — the class-name check" reads right. The
        // Win-1252 block covers mojibake demos (a cp1252 view of Shift-JIS bytes: ‚ç‚«™ƒ}ƒX).
        static ImVector<ImWchar> monoRanges;
        ImFontGlyphRangesBuilder mgb;
        mgb.AddRanges(fio.Fonts->GetGlyphRangesDefault());
        mgb.AddText(reinterpret_cast<const char*>(u8"—–·…→←⇒“”‘’"));
        mgb.AddText(reinterpret_cast<const char*>(u8"‚ƒ„†‡ˆ‰Š‹ŒŽ•˜™š›œžŸ¡¢£¤¥¦§¨©ª«¬®¯°±²³´µ¶¸¹º»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞßàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ�"));
        mgb.BuildRanges(&monoRanges);
        for (const char* fp : mono) {
            FILE* tf = fopen(fp, "rb"); if (!tf) continue; fclose(tf);
            g_monoFont = fio.Fonts->AddFontFromFileTTF(fp, 48.0f, nullptr, monoRanges.Data);
            break;
        }
        // Merge the JP face INTO the mono font so a code card can quote Japanese strings
        // (this cut literally demonstrates Shift-JIS text) — Consolas has no CJK glyphs.
        if (chosen && g_monoFont) {
            ImFontConfig mc; mc.MergeMode = true;
            fio.Fonts->AddFontFromFileTTF(chosen, 48.0f, &mc, jpRanges.Data);
        }
    }
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // A private ImGui context for the offscreen whole-frame-blur pre-pass (see render_preview_blur):
    // it runs its OWN NewFrame/Render each interactive frame, which on the main context would drain the
    // editor's input-event queue and freeze scrubbing over a blur clip. Shares the main font atlas
    // (many-contexts-one-atlas is supported); gets its own DX11 backend (no win32 backend — offscreen).
    g_mainCtx = ImGui::GetCurrentContext();
    g_blurCtx = ImGui::CreateContext(fio.Fonts);
    ImGui::SetCurrentContext(g_blurCtx);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    ImGui::SetCurrentContext(g_mainCtx);

    Project proj = load_project(projectPath);
    if (!proj.ok) fprintf(stderr, "project load error: %s\n", proj.error.c_str());
    else fprintf(stderr, "loaded %s: %zu rows, %zu clips, dur=%.2fs (cache=%s)\n",
                 projectPath.c_str(), proj.rows.size(), proj.clips.size(), proj.duration(),
                 g_cacheDir.c_str());
    g_projMtime = file_mtime(projectPath.c_str());
    undo_init(proj);   // baseline snapshot — the empty undo stack starts here
    UIState st;
    st.playhead = initialTime;
    st.selected = selectClip;  // pre-select for headless inspector shots (--select)
    if (!libSelect.empty()) { g_libSelected = libSelect; g_showViewer = true; }  // focus the Viewer (--lib-select)
    if (maskModeAuto) g_vMaskArm = true;                                           // open the paint tool (--mask-mode)

    if (exportStream) {  // headless: stream the full-res composite to stdout for ffmpeg
        int fps = proj.fps > 0 ? proj.fps : 60;
        int rc = run_export_stream(proj, (int)proj.width, (int)proj.height, fps,
                                   (int)std::ceil(proj.duration() * fps));
        if (g_expRTV) g_expRTV->Release();
        if (g_expTex) g_expTex->Release();
        if (g_expStaging) g_expStaging->Release();
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        CleanupDeviceD3D();
        return rc;
    }

    if (!shotFramePath.empty()) {  // headless: full-res composite frame(s) → PNG (verify/thumbnail; --seq-n>1 = motion)
        int W = (int)proj.width, H = (int)proj.height, rc = 1;
        if (create_export_targets(W, H)) {
            std::vector<unsigned char> buf;
            int n = seqN < 1 ? 1 : seqN;
            for (int fi = 0; fi < n; fi++) {   // consecutive frames in ONE process → the chip's eased motion advances
                double t = initialTime + (double)fi / (double)proj.fps;
                render_export_frame(proj, t, W, H, buf);
                float sig = 0.f; if (frame_blur_strength(proj, t, sig) > 0.003f) blur_rgba(buf, W, H, sig * (float)W / (float)proj.width);
                std::string path = shotFramePath;
                if (n > 1) { size_t dot = path.find_last_of('.'); char sfx[16]; snprintf(sfx, sizeof sfx, "_%03d", fi);
                             path = (dot == std::string::npos) ? path + sfx : path.substr(0, dot) + sfx + path.substr(dot); }
                if (stbi_write_png(path.c_str(), W, H, 4, buf.data(), W * 4)) {
                    if (n == 1 || fi == n - 1) fprintf(stderr, "shot-frame: %dx%d @ t=%.2fs (%d frame%s) → %s\n", W, H, t, n, n>1?"s":"", path.c_str());
                    rc = 0;
                } else { fprintf(stderr, "shot-frame: png write failed (%s)\n", path.c_str()); rc = 1; break; }
            }
        } else fprintf(stderr, "shot-frame: render target alloc failed\n");
        if (g_expRTV) g_expRTV->Release();
        if (g_expTex) g_expTex->Release();
        if (g_expStaging) g_expStaging->Release();
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        CleanupDeviceD3D();
        return rc;
    }

    bool running = true;
    int frame = 0;
    LARGE_INTEGER qpf, qpcLast;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpcLast);
    bool wasPlaying = false;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (g_resizeW && g_resizeH) {
            CleanupRTV();
            g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRTV();
        }

        // advance the playhead on the wall clock while playing (drives visuals; audio synced below)
        LARGE_INTEGER qpcNow; QueryPerformanceCounter(&qpcNow);
        double dt = (double)(qpcNow.QuadPart - qpcLast.QuadPart) / (double)qpf.QuadPart;
        qpcLast = qpcNow;
        if (dt > 0.25) dt = 0.25;   // ignore long stalls (resize, debugger) so audio can't lurch
        if (st.playing) {
            double dur = proj.duration();
            st.playhead += dt;
            if (st.playhead >= dur) { st.playhead = dur; st.playing = false; }  // stop at the end
        }

        // whole-frame blur transition (PRE-PASS): if a `blur` clip is active, render the FULL composite
        // offscreen + CPU-blur it → the preview shows that instead of the live frame, so the blur covers
        // EVERYTHING (host/captions/code/insets), not just one image plate. It runs its own ImGui frame,
        // so it MUST come before the main frame's NewFrame. Skipped (zero cost) when no blur is active.
        // whole-frame `filler` backdrop (PRE-PASS): render the foreground composite offscreen so any
        // filler samples the WHOLE frame (tracks motion, never a flat wash). Before the blur pre-pass so
        // a blur-over-filler still gets a proper fill; before the main NewFrame (it runs its own frame).
        g_fillReady = false;
        {
            double fb = 0.0;
            if (proj.width > 0 && proj.height > 0 && filler_backdrop_needed(proj, st.playhead, fb))
                g_fillReady = render_fill_backdrop(proj, st.playhead, fb);
        }

        g_pvBlurActive = false;
        {
            float sig = 0.f;
            if (proj.width > 0 && proj.height > 0 && frame_blur_strength(proj, st.playhead, sig) > 0.003f)
                g_pvBlurActive = render_preview_blur(proj, st.playhead, sig);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        bool reload = false;
        { static int rlCtr = 0;   // live-reload: auto-pick-up external/hand edits to the .slop.json
          if (++rlCtr >= 15) { rlCtr = 0; unsigned long long mt = file_mtime(projectPath.c_str());
              if (mt && g_projMtime && mt != g_projMtime) reload = true; } }
        std::map<std::string, GenLite> gen = gen_snapshot();
        DrawUI(proj, st, reload, gen);
        if (reload) {   // a failed parse (e.g. a mid-save partial file) keeps the current proj + retries
            Project np = load_project(projectPath);
            if (np.ok) { proj = std::move(np); g_bufFor.clear(); g_projMtime = file_mtime(projectPath.c_str());
                         undo_init(proj); }   // external/manual reload breaks the edit lineage → fresh baseline
        }
        apply_generations(proj, g_bufFor);  // land finished jobs → patch + persist + reload

        // reconcile the audio mixer with the transport (edge-triggered start/stop; reseek on scrub)
        if (!shot) {
            if (st.playing && !wasPlaying)      audio_start(st.playhead);
            else if (!st.playing && wasPlaying) audio_stop();
            else if (st.playing && st.scrubbed) audio_seek(st.playhead);
            if (st.playing) audio_pump(proj);
            wasPlaying = st.playing;
        }
        st.scrubbed = false;

        ImGui::Render();

        const float clear[4] = { 0.06f, 0.06f, 0.07f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (shot && ++frame >= shotFrames) {
            save_png_backbuffer(shotPath.c_str());
            fprintf(stderr, "wrote screenshot: %s\n", shotPath.c_str());
            running = false;
        }
        g_sc->Present(shot ? 0 : 1, 0);
    }

    audio_shutdown();
    if (g_blurCtx) {   // tear down the private blur context first (it borrows the main atlas → free it last)
        ImGui::SetCurrentContext(g_blurCtx);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(g_blurCtx);
        g_blurCtx = nullptr;
    }
    if (g_mainCtx) ImGui::SetCurrentContext(g_mainCtx);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
