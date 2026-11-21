// Per-pixel lighting prototype (BLACK_PPL=1 or black.ini "ppl = 1"; off by default and then a no-op).
// Lightmapped level geometry gets an extra per-pixel light pass inside its translated pixel shader:
// a face normal from screen-space derivatives of the object-space position, bumped by a height map
// taken from the diffuse texture's luminance (4 taps, central differences), Lambert + Blinn-Phong
// from camera-relative lights (the player's muzzle flash; BLACK_PPL=2 adds a dim head lamp).
#pragma once

#include <d3d9.h>

#include <cstdint>
#include <string>

namespace xd3d {

struct VertexShader;

int ppl_level();                              // 0 off, 1 muzzle flash, 2 + head lamp (demo)
bool ppl_world_vs(const VertexShader *vs);    // lightmapped level geometry: v0 FLOAT3 + v9 + v10, no normal
                                              // (also lit: the first-person gun and arms, g_fov_viewmodel)
bool ppl_begin_draw(const VertexShader *vs, bool has_ps); // this draw gets the light pass (sets the flag the PS key reads)
void ppl_cancel();
bool ppl_this_draw();
void ppl_upload(IDirect3DDevice9 *d);         // pixel shader c24..c29 for this draw
void ppl_upload_vs(IDirect3DDevice9 *d);
void ppl_frame_prepare(const VertexShader *vs); // once a frame, before a draw applies its state: world lights + shadow cube
bool ppl_world_rows(float rows[3][4], float inv[2]);
bool ppl_draw_in_world_space();
// ppl_shadow.cpp
void ppl_capture(const VertexShader *vs, uint32_t xprim, const uint16_t *idx, uint32_t count, uint32_t first, const float *conv,
                 uint32_t stride_bytes, uint64_t key);
bool ppl_shadow_render(const float wpos[3], const float vpos[3], float reach);
bool ppl_shadow_bind(IDirect3DDevice9 *d, const float wpos[3]);
void ppl_release_targets();
// the game's glows (lamp halos, fires) as lights: every programmable draw's converted vertices / immediate vertices
void ppl_glow_capture(const VertexShader *vs, const float *conv, uint32_t stride_bytes, uint32_t n);
void ppl_glow_sprites(const float *pos, uint32_t pos_stride, const float *col, uint32_t col_stride, uint32_t n);
void ppl_glow_immediate(const VertexShader *vs, const float *verts, uint32_t n); // 16 float4 slots per vertex      // vertex shader c201: 1/P00, 1/P11 (clip -> view space)
void ppl_install();                           // startup: the explosion hook (world lights)
void ppl_sun_sample(const VertexShader *vs); // the world light: the characters' irradiance direction (each draw)
void ppl_probe(const VertexShader *vs);       // BLACK_PPL_PROBE=1: log the game's own point-light registers

// HLSL pieces for nv2a_psh.cpp
bool ppl_ps_bump(uint32_t tex_modes);   // a stage bump-maps: c19-c22 are the game's
const char *ppl_ps_decls(int mode);    // 0 base, 1: world lights 4-5 in c19-c22, 2: ps_3_0 - lights 4-12 in c32-c49, fog c50
const char *ppl_ps_epilogue(int mode);
bool ppl_sm3_on();                     // BLACK_PPL_SM3 (default on): lit draws as a vs_3_0 / ps_3_0 pair
int ppl_max_slots();
bool ppl_med();                        // the CO-OP page's LIGHTING is MED (2) or more                   // world-light slots this frame (12 with shader model 3, else 5)
extern bool g_ppl_ps_sm3;
extern bool g_ppl_sm3_want;            // this draw may take the ps_3_0 light pass (draw.cpp: its vs_3_0 build exists)             // the bound pixel shader is the ps_3_0 light pass (pshader.cpp) // reads `res`, t0, I.tc0, I.tc4

} // namespace xd3d

