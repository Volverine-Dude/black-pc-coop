// Per-pixel lighting prototype - shadows for the strongest world light (NOTES-lighting.md 4b, option B).
//
// The port draws everything from CPU-converted vertices each frame (user-pointer draws), so replaying the frame
// from a light is too slow. Level geometry is static, though: opaque draws whose camera position matches the
// frame's world space (ppl.cpp) give world positions, kept once in a GPU vertex buffer (positions only,
// triangle lists) keyed by the game's vertex / index data. A distance cube map (R32F, 512^2) for the strongest
// world light is rendered from that cache each frame IN VIEW-SPACE AXES, so the lit pixel shader looks it up with
// the light -> pixel vector it already has (no new constants; ps_2_a has none left). Only static level
// geometry casts: enemies and props do not. (v10: the cube is in WORLD axes and kept while its light and the cache
// do not change - an explosion's cube is rendered once; the vertex shader passes the world light -> pixel vector.) Entries unseen for 30 s expire (destroyed walls); a pause in level
// drawing (a loading screen) empties the cache.
#include "d3d/ppl.h"

#include "common.h"
#include "d3d/compiler.h"
#include "d3d/draw.h"
#include "d3d/host.h"
#include "d3d/shader_cache.h"
#include "d3d/state.h"
#include "d3d/xd3d.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace xd3d {
namespace {

constexpr uint32_t kMaxTris = 400000;  // 14.4 MB of float3 positions
constexpr uint32_t kCube = 512;
constexpr uint32_t kExpireFrames = 1800; // 30 s at 60 fps

struct Entry {
    uint32_t first_tri, tris;
    uint32_t last_seen;
    float lo[3], hi[3];
};

std::unordered_map<uint64_t, Entry> g_entries;
std::vector<const Entry *> g_order; // by first_tri (append order)
IDirect3DVertexBuffer9 *g_vb;
uint32_t g_used_tris;
uint32_t g_last_capture_frame;
bool g_vb_failed;

IDirect3DCubeTexture9 *g_cube;
IDirect3DSurface9 *g_cube_depth;
IDirect3DVertexShader9 *g_svs;
IDirect3DPixelShader9 *g_sps;
bool g_targets_failed, g_shaders_failed;
bool g_cube_valid; // rendered this frame
uint32_t g_cube_frame;
float g_cube_wpos[3], g_cube_reach; // the light the cube holds
uint32_t g_cube_tris;                // the cache size it was made from

void reset_cache(const char *why)
{
    if (!g_entries.empty()) logf("[ppl] shadow cache cleared (%s): %zu pieces, %u triangles", why, g_entries.size(), g_used_tris);
    g_entries.clear();
    g_order.clear();
    g_used_tris = 0;
}

bool ensure_vb()
{
    if (g_vb) return true;
    if (g_vb_failed) return false;
    if (FAILED(host3d::dev()->CreateVertexBuffer(kMaxTris * 36, D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &g_vb, nullptr))) {
        g_vb_failed = true;
        logf("[ppl] shadow cache: CreateVertexBuffer failed - no shadows");
        return false;
    }
    return true;
}

// Triangles of an Xbox primitive as vertex numbers (relative to the converted block).
void triangulate(uint32_t xprim, uint32_t n, std::vector<uint32_t> &out)
{
    out.clear();
    switch (xprim) {
    case XPT_TRIANGLELIST:
        for (uint32_t i = 0; i + 2 < n; i += 3) out.insert(out.end(), {i, i + 1, i + 2});
        break;
    case XPT_TRIANGLESTRIP: case XPT_QUADSTRIP:
        for (uint32_t i = 0; i + 2 < n; i++) out.insert(out.end(), {i, i + 1, i + 2});
        break;
    case XPT_TRIANGLEFAN: case XPT_POLYGON:
        for (uint32_t i = 1; i + 1 < n; i++) out.insert(out.end(), {0u, i, i + 1});
        break;
    case XPT_QUADLIST:
        for (uint32_t q = 0; q + 3 < n; q += 4) out.insert(out.end(), {q, q + 1, q + 2, q, q + 2, q + 3});
        break;
    default: break;
    }
}

const char *kShadowVS =
    "float4 lightPos : register(c0);\n"  // world; w = reach (far)
    "float4 faceR : register(c1); float4 faceU : register(c2); float4 faceL : register(c3);\n"
    "struct O { float4 pos : POSITION; float4 rel : TEXCOORD0; };\n"
    "O main(float3 p : POSITION) {\n"
    "  float3 r = p - lightPos.xyz;\n"
    "  float x = dot(r, faceR.xyz), y = dot(r, faceU.xyz), z = dot(r, faceL.xyz);\n"
    "  float n = 0.05, f = lightPos.w;\n"
    "  O o; o.pos = float4(x, y, (z - n) * f / (f - n), z);\n"
    "  o.rel = float4(r, f);\n"
    "  return o;\n"
    "}\n";
const char *kShadowPS =
    "float4 main(float4 rel : TEXCOORD0) : COLOR0 { return (length(rel.xyz) / rel.w).xxxx; }\n";

bool ensure_targets()
{
    if (g_cube && g_cube_depth && g_svs && g_sps) return true;
    if (g_targets_failed || g_shaders_failed) return false;
    IDirect3DDevice9 *d = host3d::dev();
    if (!g_cube && FAILED(d->CreateCubeTexture(kCube, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_cube, nullptr))) {
        g_targets_failed = true;
        logf("[ppl] shadows: no R32F cube render target on this GPU - no shadows");
        return false;
    }
    if (!g_cube_depth && FAILED(d->CreateDepthStencilSurface(kCube, kCube, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &g_cube_depth, nullptr))) {
        g_targets_failed = true;
        logf("[ppl] shadows: CreateDepthStencilSurface failed - no shadows");
        return false;
    }
    if (!g_svs || !g_sps) {
        std::vector<uint8_t> code;
        std::string errors;
        if (!compile_cached(kShadowVS, "ppl_shadow_vs", {"vs_2_0"}, code, errors) ||
            FAILED(d->CreateVertexShader((const DWORD *)code.data(), &g_svs))) {
            g_shaders_failed = true;
            logf("[ppl] shadows: vertex shader failed: %s", errors.c_str());
            return false;
        }
        if (!compile_cached(kShadowPS, "ppl_shadow_ps", {"ps_2_0"}, code, errors) ||
            FAILED(d->CreatePixelShader((const DWORD *)code.data(), &g_sps))) {
            g_shaders_failed = true;
            logf("[ppl] shadows: pixel shader failed: %s", errors.c_str());
            return false;
        }
    }
    logf("[ppl] shadows: %ux%u R32F cube ready", kCube, kCube);
    return true;
}

} // namespace

void ppl_capture(const VertexShader *vs, uint32_t xprim, const uint16_t *idx, uint32_t count, uint32_t first, const float *conv,
                 uint32_t stride_bytes, uint64_t key)
{
    static const bool off = getenv("BLACK_PPL_SHADOWS") && getenv("BLACK_PPL_SHADOWS")[0] == '0';
    if (off || !ppl_level() || !vs || !conv || count < 3) return;
    const uint32_t *rs = render_state();
    if (rs[XRS_ALPHABLENDENABLE] || !rs[XRS_ZWRITEENABLE]) return; // casters: opaque surfaces only
    int pe = -1;
    for (uint32_t i = 0; i < vs->num_elements; i++)
        if (vs->elements[i].reg == 0) pe = vs->elements[i].xtype == 0x32 || vs->elements[i].xtype == 0x42 ? (int)i : -2;
    if (pe < 0 || !ppl_draw_in_world_space()) return;
    uint32_t f = g_frame_count.load();
    if (g_last_capture_frame && f - g_last_capture_frame > 90) reset_cache("level drawing paused (a load)");
    g_last_capture_frame = f;
    auto it = g_entries.find(key);
    if (it != g_entries.end()) {
        it->second.last_seen = f;
        return;
    }
    static std::vector<uint32_t> tri;
    static std::vector<uint32_t> list;
    triangulate(xprim, count, tri);
    if (tri.empty()) return;
    uint32_t ntri = (uint32_t)tri.size() / 3;
    if (ntri > 60000) return;
    if (!ensure_vb()) return;
    if (g_used_tris + ntri > kMaxTris) reset_cache("cache full");
    float *dst = nullptr;
    if (FAILED(g_vb->Lock(g_used_tris * 36, ntri * 36, (void **)&dst, 0)) || !dst) return;
    Entry e = {g_used_tris, ntri, f, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    uint32_t sf = stride_bytes / 4;
    for (uint32_t k = 0; k < tri.size(); k++) {
        uint32_t v = idx ? idx[tri[k]] - first : tri[k];
        const float *p = conv + (size_t)v * sf + pe * 4;
        for (int c = 0; c < 3; c++) {
            dst[k * 3 + c] = p[c];
            if (p[c] < e.lo[c]) e.lo[c] = p[c];
            if (p[c] > e.hi[c]) e.hi[c] = p[c];
        }
    }
    g_vb->Unlock();
    g_used_tris += ntri;
    auto ins = g_entries.emplace(key, e);
    g_order.push_back(&ins.first->second); // unordered_map nodes keep their addresses on rehash
}

// The shadow cube for a light at `wpos` (world) / `vpos` (view space) with reach `reach`. Called before a draw
// applies its state (draw.cpp begin_draw): the render target, depth, viewport and scissor are put back here, and
// the port's state caches are reset so the draw re-sends everything.
bool ppl_shadow_render(const float wpos[3], const float vpos[3], float reach)
{
    (void)vpos;
    if (g_order.empty() || !ensure_targets()) return false;
    {
        float dx = wpos[0] - g_cube_wpos[0], dy = wpos[1] - g_cube_wpos[1], dz = wpos[2] - g_cube_wpos[2];
        if (g_cube_valid && dx * dx + dy * dy + dz * dz < 0.0025f && reach == g_cube_reach && g_used_tris == g_cube_tris &&
            g_frame_count.load() - g_cube_frame < 300)
            return true; // the same light over the same level: the cube still holds
    }
    g_cube_valid = false;
    IDirect3DDevice9 *d = host3d::dev();
    host3d::begin_scene();
    IDirect3DSurface9 *old_rt = nullptr, *old_z = nullptr;
    D3DVIEWPORT9 old_vp;
    DWORD old_scissor = 0;
    d->GetRenderTarget(0, &old_rt);
    d->GetDepthStencilSurface(&old_z);
    d->GetViewport(&old_vp);
    d->GetRenderState(D3DRS_SCISSORTESTENABLE, &old_scissor);
    d->SetTexture(4, nullptr);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetVertexShader(g_svs);
    d->SetPixelShader(g_sps);
    d->SetFVF(D3DFVF_XYZ);
    d->SetStreamSource(0, g_vb, 0, 12);
    float k[4][4] = {};
    k[0][0] = wpos[0], k[0][1] = wpos[1], k[0][2] = wpos[2], k[0][3] = reach;
    // D3D cube faces (left-handed): +X, -X, +Y, -Y, +Z, -Z as (right, up, look)
    static const float face[6][3][3] = {
        {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},  {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}, {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},  {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
    };
    // the pieces within reach of the light (world-space boxes), merged into runs of the buffer
    uint32_t f = g_frame_count.load(), runs = 0, tris = 0;
    static std::vector<std::pair<uint32_t, uint32_t>> run;
    run.clear();
    for (const Entry *e : g_order) {
        if (f - e->last_seen > kExpireFrames) continue;
        float dx = std::fmax(std::fmax(e->lo[0] - wpos[0], 0.0f), wpos[0] - e->hi[0]);
        float dy = std::fmax(std::fmax(e->lo[1] - wpos[1], 0.0f), wpos[1] - e->hi[1]);
        float dz = std::fmax(std::fmax(e->lo[2] - wpos[2], 0.0f), wpos[2] - e->hi[2]);
        if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
        if (!run.empty() && run.back().first + run.back().second == e->first_tri) run.back().second += e->tris;
        else run.push_back({e->first_tri, e->tris});
        tris += e->tris;
    }
    D3DVIEWPORT9 vp = {0, 0, kCube, kCube, 0.0f, 1.0f};
    for (int fc = 0; fc < 6; fc++) {
        IDirect3DSurface9 *s = nullptr;
        if (FAILED(g_cube->GetCubeMapSurface((D3DCUBEMAP_FACES)fc, 0, &s))) continue;
        d->SetRenderTarget(0, s);
        d->SetDepthStencilSurface(g_cube_depth);
        d->SetViewport(&vp);
        d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
        for (int r = 0; r < 3; r++) memcpy(k[1 + r], face[fc][r], 12);
        d->SetVertexShaderConstantF(0, &k[0][0], 4);
        for (auto &rn : run) d->DrawPrimitive(D3DPT_TRIANGLELIST, rn.first * 3, rn.second);
        runs += (uint32_t)run.size();
        s->Release();
    }
    d->SetStreamSource(0, nullptr, 0, 0);
    d->SetRenderTarget(0, old_rt);
    d->SetDepthStencilSurface(old_z);
    d->SetViewport(&old_vp);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, old_scissor);
    if (old_rt) old_rt->Release();
    if (old_z) old_z->Release();
    forget_sent_state(); // every render / sampler / texture state and shader is sent again by the next draw
    g_cube_valid = true;
    g_cube_frame = f;
    memcpy(g_cube_wpos, wpos, 12);
    g_cube_reach = reach;
    g_cube_tris = g_used_tris;
    static uint32_t logged;
    if (logged++ < 30 || f % 600 == 0)
        logf("[ppl] shadows: frame %u, light at %.1f %.1f %.1f reach %.0f m: %u triangles in %zu runs (cache %zu pieces, %u triangles)",
             f, wpos[0], wpos[1], wpos[2], reach, tris, run.size(), g_entries.size(), g_used_tris);
    return true;
}

bool ppl_shadow_bind(IDirect3DDevice9 *d, const float wpos[3])
{
    float dx = wpos[0] - g_cube_wpos[0], dy = wpos[1] - g_cube_wpos[1], dz = wpos[2] - g_cube_wpos[2];
    if (!g_cube_valid || dx * dx + dy * dy + dz * dz > 0.0025f) return false; // only for the light it holds
    d->SetTexture(4, g_cube);
    d->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(4, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    return true;
}

void ppl_lampmap_release(); // ppl.cpp

void ppl_release_targets()
{
    ppl_lampmap_release();
    if (g_cube) g_cube->Release();
    if (g_cube_depth) g_cube_depth->Release();
    g_cube = nullptr;
    g_cube_depth = nullptr;
    g_cube_valid = false;
}

} // namespace xd3d
