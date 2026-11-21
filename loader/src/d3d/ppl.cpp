// Per-pixel lighting prototype - see ppl.h.
#include "d3d/ppl.h"

#include "common.h"
#include "d3d/nv2a_vsh.h"
#include "d3d/state.h"
#include "d3d/vshader.h"
#include "d3d/xd3d.h"
#include "xbe.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <array>
#include <unordered_map>

extern "C" volatile uint8_t g_fov_viewmodel;     // fov.cpp: set while the first-person list draws
// Rounds fired, from the game's per-shot routine FUN_001DBD50 (ppl_thunk_shot below): the player's, with the
// weapon of the last one, and everyone else's shooters in a ring (enemies, and the partner's clone, whose shots
// coop.cpp fires through that routine directly).
volatile uint8_t g_ppl_count_shots;
volatile uint32_t g_ppl_player_shots;
volatile uint32_t g_ppl_shot_weapon;
volatile uint32_t g_ppl_player_shot_frame = 0x80000000u; // the frame of the player's last round
const void *g_glow_vs; // the sprite draw's vertex program (near-sprite log)
volatile uint32_t g_ppl_enemy_shot[16];
volatile uint32_t g_ppl_enemy_shot_n;

namespace xd3d {
namespace {

bool g_draw_on, g_draw_gun;

float env_f(const char *name, float def)
{
    const char *v = getenv(name);
    return v && *v ? (float)atof(v) : def;
}

double now_ms()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// Muzzle flash per weapon. The game's fire routine (cheats.cpp ammo thunk) counts each round the player fires
// and keeps that weapon object: byte 0 = weapon type (coop.cpp: 0 / 13 / 14 handguns, 14 = the mission-1 Glock,
// 3 SMG, 4 rifle, 8 the mission-1 rifle), byte +0x108 = suppressor on (the game then plays the silenced shot).
// A profile gives the light's view-space position (where that class's flash sprite shows), colour x brightness,
// reach and pulse length; a suppressor keeps a faint, short, small flash. Each new (type, suppressor) is logged
// once with its fire type and magazine size so the table can grow. Override one type for tuning with
// BLACK_PPL_WEAPON_<type>="gain,r,g,b,reach,x,y,z,decay_ms".
struct FlashProfile {
    const char *name;
    float gain, r, g, b, reach, x, y, z, decay_ms;
};
const FlashProfile kRifle = {"rifle", 1.0f, 1.6f, 1.15f, 0.65f, 7.0f, 0.13f, -0.03f, 1.0f, 18.0f};
const FlashProfile kSmg = {"SMG", 0.8f, 1.6f, 1.2f, 0.7f, 6.0f, 0.12f, -0.05f, 0.8f, 14.0f};
const FlashProfile kHandgun = {"handgun", 0.7f, 1.6f, 1.25f, 0.75f, 5.0f, 0.07f, -0.07f, 0.6f, 14.0f};
const FlashProfile kHeavy = {"heavy", 1.35f, 1.6f, 1.05f, 0.55f, 9.0f, 0.13f, -0.05f, 1.0f, 30.0f}; // shotgun-like

FlashProfile profile_for(int type)
{
    char key[40];
    snprintf(key, sizeof key, "BLACK_PPL_WEAPON_%d", type);
    if (const char *v = getenv(key)) {
        FlashProfile p = kRifle;
        p.name = "override";
        if (sscanf_s(v, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &p.gain, &p.r, &p.g, &p.b, &p.reach, &p.x, &p.y, &p.z, &p.decay_ms) >= 1) return p;
    }
    switch (type) {
    case 0: case 13: case 14: return kHandgun;
    case 3: case 8: return kSmg;
    case 4: return kRifle;
    default: return kRifle;
    }
}

uint32_t g_flash_pulses; // pulses started (the probe logs it)
FlashProfile g_flash = kRifle; // the last round's profile (suppressor applied)

float flash_intensity()
{
    static const float hold = env_f("BLACK_PPL_FLASH_HOLD", 0.0f); // tests: the flash always on at this intensity
    if (hold > 0) return hold;
    static uint32_t seen;
    static double pulse_at = -1e9;
    static float peak;
    double now = now_ms();
    uint32_t shots = g_ppl_player_shots;
    if (shots != seen) {
        seen = shots;
        if (now - pulse_at > 25.0) { // one pulse per frame-ish, whatever the round count
            pulse_at = now;
            uint32_t W = g_ppl_shot_weapon;
            int type = -1, silenced = 0, fire_type = -1, clip = -1;
            if (W >= 0x80000000 && W < 0x84000000) {
                type = *(volatile uint8_t *)(uintptr_t)W;
                silenced = *(volatile uint8_t *)(uintptr_t)(W + 0x108) != 0;
                uint32_t cp = *(volatile uint32_t *)(uintptr_t)(W + 0xF4);
                if (cp >= 0x80000000 && cp < 0x84000000) {
                    fire_type = *(volatile int *)(uintptr_t)cp;
                    clip = *(volatile int16_t *)(uintptr_t)(cp + 0x18);
                }
            }
            g_flash = profile_for(type);
            if (silenced) { // the suppressor: a faint, short, tight glow
                g_flash.gain *= 0.25f;
                g_flash.reach *= 0.5f;
                g_flash.decay_ms *= 0.6f;
            }
            static uint64_t logged_kinds[8]; // (type, silenced) seen, 512 bits
            uint32_t kind = ((uint32_t)(type & 0xFF) << 1) | (uint32_t)silenced;
            if (!(logged_kinds[kind >> 6] >> (kind & 63) & 1)) {
                logged_kinds[kind >> 6] |= 1ull << (kind & 63);
                logf("[ppl] weapon type %d%s fired (fire type %d, magazine now %d): %s flash, gain %.2f reach %.1f m decay %.0f ms",
                     type, silenced ? " SUPPRESSED" : "", fire_type, clip, g_flash.name, g_flash.gain, g_flash.reach, g_flash.decay_ms);
            }
            if (g_flash_pulses++ < 300) logf("[ppl] flash pulse %u: player rounds %u, weapon type %d%s", g_flash_pulses, shots, type, silenced ? " (suppressed)" : "");
            static uint32_t seed = 0x9E3779B9u;
            seed = seed * 1664525u + 1013904223u;
            peak = 0.75f + 0.25f * (float)(seed >> 8) / 16777216.0f;
        }
    }
    double t = now - pulse_at;
    return t > 6.0 * g_flash.decay_ms ? 0.0f : g_flash.gain * peak * (float)exp(-t / g_flash.decay_ms);
}

// The camera in the draw's object space: the point where clip x = y = w = 0 for the model->clip rows c10..c13.
bool eye_from_mvp(const float (*c)[4], float eye[3], float fwd[3])
{
    const float *r0 = c[10], *r1 = c[11], *r3 = c[13];
    float a[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r3[0], r3[1], r3[2]}};
    float b[3] = {-r0[3], -r1[3], -r3[3]};
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(fabsf(det) > 1e-12f)) return false;
    for (int i = 0; i < 3; i++) {
        float m[3][3];
        memcpy(m, a, sizeof m);
        for (int r = 0; r < 3; r++) m[r][i] = b[r];
        float d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        eye[i] = d / det;
    }
    // forward: the line x = y = 0, oriented so that w grows
    float f[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
    if (f[0] * r3[0] + f[1] * r3[1] + f[2] * r3[2] < 0) f[0] = -f[0], f[1] = -f[1], f[2] = -f[2];
    float n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(n > 0)) return false;
    for (int i = 0; i < 3; i++) fwd[i] = f[i] / n;
    return true;
}

} // namespace

void install_explosion_hook(); // below

// The CO-OP page's LIGHTING option (g_cfg.ppl, black.ini "ppl"), switchable in play; BLACK_PPL=<n> overrides it.
int ppl_level()
{
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("BLACK_PPL");
        env = e && *e ? (atoi(e) < 0 ? 0 : atoi(e)) : -1;
    }
    int lvl = env >= 0 ? env : g_cfg.ppl;
    static int last;
    if (lvl != last) {
        last = lvl;
        g_ppl_count_shots = lvl ? 1 : 0; // the fire routine counts rounds for the flashes (cheats.cpp)
        if (lvl) install_explosion_hook();
        logf("[ppl] per-pixel lighting %s (level %d%s)", lvl ? "on" : "off", lvl, env >= 0 ? ", BLACK_PPL" : "");
    }
    return lvl;
}

bool ppl_world_vs(const VertexShader *vs)
{
    if (!vs) return false;
    bool pos3 = false, uv0 = false, uv1 = false, nrm = false;
    for (uint32_t i = 0; i < vs->num_elements; i++) {
        const VertexElement &e = vs->elements[i];
        if (e.reg == 0 && e.xtype == 0x32) pos3 = true;
        if (e.reg == 9) uv0 = true;
        if (e.reg == 10) uv1 = true;
        if (e.reg == 2) nrm = true;
    }
    return pos3 && uv0 && uv1 && !nrm;
}

namespace {

// The projection's x/y scales (clip x = P00 * view x, clip y = P11 * view y, clip w = view z), taken from a level
// draw's model->clip rows: |row0.xyz| / |row3.xyz| holds for any rotation + uniform scale. Shared by every draw of
// the frame (the first-person gun is drawn with the same projection; fov.cpp scales its clip x/y afterwards).
float g_inv_p00 = 0.0f, g_inv_p11 = 0.0f;

// ---- World-space lights: explosions and everyone else's gunfire (v5) ----
// World -> view space: level geometry is drawn in world space, so a lightmapped draw's model->clip rows are the
// view-projection: view x = row0.X / P00, y = row1.X / P11, z = row3.X (capture_projection keeps them per frame).
float g_vp_rows[3][4];
float g_vp_eye[3], g_vp_fwd[3]; // the camera in world space (BLACK_PPL_TEST_BLAST)
bool g_vp_ok;
uint32_t g_vp_frame; // the frame the rows came from (world lights skip a frame without level draws)
float g_cons_eye[3]; // the camera most level draws of the last frame agreed on (validates the pin below)
bool g_cons_ok;

void capture_projection()
{
    const float (*c)[4] = g_st.vs_constants;
    auto len = [](const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    float l0 = len(c[10]), l1 = len(c[11]), l3 = len(c[13]);
    if (!(l0 > 1e-6f && l1 > 1e-6f && l3 > 1e-6f)) return;
    g_inv_p00 = l3 / l0;
    g_inv_p11 = l3 / l1;
    // The world -> view rows: not every lightmapped draw is in world space (the sky follows the camera; doors and
    // moving pieces have their own matrix), so the last one of a frame placed world lights 70+ m off. Each draw's
    // camera position in its own space is solved from its rows; the frame's world space is the one most draws
    // agree on (level geometry). Committed when the next frame starts: world lights lag one frame.
    struct Bucket { float eye[3], fwd[3]; uint32_t n; float rows[3][4]; };
    static Bucket b[8];
    static int nb;
    static uint32_t cur;
    uint32_t f = g_frame_count.load();
    if (f != cur) {
        int best = -1;
        for (int i = 0; i < nb; i++)
            if (best < 0 || b[i].n > b[best].n) best = i;
        if (best >= 0) {
            memcpy(g_cons_eye, b[best].eye, 12);
            g_cons_ok = true;
            if (g_vp_frame != f) { // not pinned this frame yet (ppl_frame_prepare): last frame's consensus meanwhile
                memcpy(g_vp_rows, b[best].rows, sizeof g_vp_rows);
                memcpy(g_vp_eye, b[best].eye, 12);
                memcpy(g_vp_fwd, b[best].fwd, 12);
                g_vp_ok = true;
                g_vp_frame = cur;
            }
            static uint32_t logged;
            if (nb > 1 && logged++ < 5)
                logf("[ppl] world space: frame %u, %d camera candidates; %u of the level draws agree (eye %.1f %.1f %.1f)", cur, nb, b[best].n,
                     b[best].eye[0], b[best].eye[1], b[best].eye[2]);
        }
        nb = 0;
        cur = f;
    }
    float eye[3], fwd[3];
    if (!eye_from_mvp(c, eye, fwd)) return;
    for (int i = 0; i < nb; i++) {
        float dx = b[i].eye[0] - eye[0], dy = b[i].eye[1] - eye[1], dz = b[i].eye[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz < 0.01f) { b[i].n++; return; }
    }
    if (nb < 8) {
        Bucket &k = b[nb++];
        memcpy(k.eye, eye, 12);
        memcpy(k.fwd, fwd, 12);
        k.n = 1;
        memcpy(k.rows[0], c[10], 16);
        memcpy(k.rows[1], c[11], 16);
        memcpy(k.rows[2], c[13], 16);
    }
}


bool world_to_view(const float X[3], float v[3])
{
    if (!g_vp_ok || g_frame_count.load() - g_vp_frame > 2) return false;
    auto dot = [&](const float *r) { return r[0] * X[0] + r[1] * X[1] + r[2] * X[2] + r[3]; };
    v[0] = dot(g_vp_rows[0]) * g_inv_p00;
    v[1] = dot(g_vp_rows[1]) * g_inv_p11;
    v[2] = dot(g_vp_rows[2]);
    return true;
}

bool safe_read(void *dst, uint32_t src, size_t n)
{
    if (src < 0x10000) return false;
    __try {
        memcpy(dst, (const void *)(uintptr_t)src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct WorldLight {
    float pos[3];
    double at;      // start, ms
    float gain, reach, r, g, b, rise_ms, decay_ms, life_ms, hold_ms; // hold_ms: full strength after the rise (sparks)
    float vis; // wall check: share of the light reaching the camera's side, smoothed (-1 = not tested yet)
    bool spark; // a ricochet flash: no wall check (it sits on the wall it hit; the test budget delayed it)
};
WorldLight g_wl[32]; // 0-15 explosions, 16-31 gunfire
uint32_t g_wl_blast_n, g_wl_shot_n;

void add_world_light(const WorldLight &l, bool blast)
{
    WorldLight &d = blast ? g_wl[g_wl_blast_n++ % 16] : g_wl[16 + g_wl_shot_n++ % 16];
    d = l;
    d.vis = -1.0f;
}

float world_light_level(const WorldLight &l, double now)
{
    double t = now - l.at;
    if (t < 0 || t > l.life_ms) return 0.0f;
    float up = l.rise_ms > 0 ? (float)(t < l.rise_ms ? t / l.rise_ms : 1.0) : 1.0f;
    float down = t < l.rise_ms + l.hold_ms ? 1.0f : (float)exp(-(t - l.rise_ms - l.hold_ms) / l.decay_ms);
    return l.gain * up * down;
}

// Explosions: FUN_001e0970 (the game's core explosion: grenades, rockets, barrels, destructibles). coop.cpp may
// hook its first 6 bytes (the co-op relay, installed when a co-op mission starts, and it checks those bytes), so
// this hook takes the next instruction, sub esp, 0xA4 (6 bytes at 0x1E0976): the frame is set up there,
// [ebp+0xC] = the centre vec4*, XMM2 = the radius, EAX = the kind.
volatile uint32_t g_ex_queue_n;
bool fire_hazard_note(const float p[3]); // below (the fire sites)
struct ExRec { float c[3], radius; uint32_t kind; };
ExRec g_ex_queue[16];

} // namespace

extern "C" void __cdecl ppl_on_explosion(uint32_t centre, uint32_t kind, float radius)
{
    float c[3];
    if (!safe_read(c, centre, 12)) return;
    ExRec &e = g_ex_queue[g_ex_queue_n % 16];
    memcpy(e.c, c, 12);
    e.radius = radius;
    e.kind = kind;
    g_ex_queue_n++;
}

extern "C" uint32_t g_ppl_ex_resume;
uint32_t g_ppl_ex_resume;

__declspec(naked) void ppl_thunk_explosion()
{
    __asm {
        pushfd
        pushad
        sub esp, 112                    // xmm1..xmm7 (the game keeps the radius in xmm2)
        movups [esp], xmm1
        movups [esp + 16], xmm2
        movups [esp + 32], xmm3
        movups [esp + 48], xmm4
        movups [esp + 64], xmm5
        movups [esp + 80], xmm6
        movups [esp + 96], xmm7
        sub esp, 4
        movss dword ptr [esp], xmm2     // radius
        push eax                        // kind
        push dword ptr [ebp + 0xC]      // centre*
        call ppl_on_explosion
        add esp, 12
        movups xmm1, [esp]
        movups xmm2, [esp + 16]
        movups xmm3, [esp + 32]
        movups xmm4, [esp + 48]
        movups xmm5, [esp + 64]
        movups xmm6, [esp + 80]
        movups xmm7, [esp + 96]
        add esp, 112
        popad
        popfd
        sub esp, 0xA4
        jmp dword ptr [g_ppl_ex_resume]
    }
}

void ppl_install() { ppl_level(); } // startup: installs the hook when the setting is already on

// Shots: FUN_001DBD50(shot ctx) runs once per round fired by anyone (the pellets inside; ctx+4 = the weapon,
// weapon+0xF0 = its owner). The normal fire routine calls it, and coop.cpp calls it directly for the partner's
// clone and for catch-up shots - so the clone's gunfire lights too (owner: "the lighting doesn't work on the
// clone's model"; v1-v10 counted at the magazine decrement in FUN_001EEFD0, which those shots skip). The hook
// takes sub esp, 0x2E4 (6 bytes at 0x1DBD56), after the prologue: [ebp+8] = the ctx.
uint32_t g_ppl_shot_resume;

volatile uint32_t g_ppl_last_shot_frame = 0x80000000u; // the frame of the last round fired by anyone (spark flashes)

extern "C" void __cdecl ppl_on_shot(uint32_t ctx)
{
    if (!g_ppl_count_shots) return;
    uint32_t W = 0, owner = 0;
    if (!safe_read(&W, ctx + 4, 4) || W < 0x80000000 || W >= 0x84000000) return;
    if (!safe_read(&owner, W + 0xF0, 4) || owner < 0x80000000 || owner >= 0x84000000) return;
    g_ppl_last_shot_frame = g_frame_count.load();
    uint32_t world = *(volatile uint32_t *)(uintptr_t)xbe_va(0x002D45D4);
    if (owner == world + 0x30) {
        g_ppl_shot_weapon = W;
        g_ppl_player_shot_frame = g_frame_count.load();
        g_ppl_player_shots++;
    } else {
        g_ppl_enemy_shot[g_ppl_enemy_shot_n & 15] = owner;
        g_ppl_enemy_shot_n++;
    }
}

__declspec(naked) void ppl_thunk_shot()
{
    __asm {
        pushfd
        pushad
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        push dword ptr [ebp + 8]        // the shot ctx
        call ppl_on_shot
        add esp, 4
        movups xmm0, [esp]
        movups xmm1, [esp + 16]
        movups xmm2, [esp + 32]
        movups xmm3, [esp + 48]
        movups xmm4, [esp + 64]
        movups xmm5, [esp + 80]
        movups xmm6, [esp + 96]
        movups xmm7, [esp + 112]
        add esp, 128
        popad
        popfd
        sub esp, 0x2E4
        jmp dword ptr [g_ppl_shot_resume]
    }
}

void install_shot_hook()
{
    uint32_t site = xbe_va(0x001DBD56);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x81, 0xEC, 0xE4, 0x02, 0x00, 0x00};
    if (memcmp(code, kExpect, 6) != 0) {
        logf("[ppl] shot hook: code at 0x%08x does not match - no muzzle flashes", site);
        return;
    }
    g_ppl_shot_resume = site + 6;
    DWORD old;
    VirtualProtect(code, 6, PAGE_EXECUTE_READWRITE, &old);
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)ppl_thunk_shot - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    VirtualProtect(code, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 6);
    logf("[ppl] shot hook installed at 0x%08x", site);
}

void install_explosion_hook()
{
    static bool done;
    if (done) return;
    done = true;
    uint32_t site = xbe_va(0x001E0976);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x81, 0xEC, 0xA4, 0x00, 0x00, 0x00};
    if (memcmp(code, kExpect, 6) != 0) {
        logf("[ppl] explosion hook: code at 0x%08x does not match - explosions not lit", site);
        return;
    }
    g_ppl_ex_resume = site + 6;
    DWORD old;
    VirtualProtect(code, 6, PAGE_EXECUTE_READWRITE, &old);
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)ppl_thunk_explosion - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    VirtualProtect(code, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 6);
    logf("[ppl] explosion hook installed at 0x%08x", site);
    install_shot_hook();
}

namespace {

float line_fraction(const float a[3], const float b[3], uint32_t world, uint32_t ignore); // the wall check, below

// New explosions and enemy rounds since the last frame -> world lights.
void collect_world_lights(double now)
{
    // tests: BLACK_PPL_TEST_BLAST=1 sets off an explosion light 4 m ahead of the camera every 3 s, placed in WORLD
    // space, so it goes through the same world -> view path as the game's explosions
    static const bool test_blast = getenv("BLACK_PPL_TEST_BLAST") != nullptr;
    static double last_test;
    if (test_blast && g_vp_ok && now - last_test > 3000.0) {
        last_test = now;
        ExRec &e = g_ex_queue[g_ex_queue_n % 16];
        // alternately 4 m ahead (in view) and 1.5 m BEHIND the first wall straight ahead (the wall check: the
        // room should stay dark); with no wall within 30 m, 4 m ahead again
        static int alt;
        float dir[3] = {g_vp_fwd[0], g_vp_fwd[1], g_vp_fwd[2]}, dist = 4.0f;
        if (alt++ & 1) { // the first solid wall within 15 m: ahead, behind, right, left (windows let rays through)
            uint32_t world = *(volatile uint32_t *)(uintptr_t)xbe_va(0x002D45D4);
            float rx = g_vp_fwd[2], rz = -g_vp_fwd[0], rl = sqrtf(rx * rx + rz * rz);
            if (rl > 1e-4f) rx /= rl, rz /= rl;
            const float dirs[4][3] = {{g_vp_fwd[0], 0, g_vp_fwd[2]}, {-g_vp_fwd[0], 0, -g_vp_fwd[2]}, {rx, 0, rz}, {-rx, 0, -rz}};
            const char *names[4] = {"ahead", "behind", "right", "left"};
            int pick = -1;
            for (int d = 0; d < 4 && pick < 0 && world >= 0x80000000 && world < 0x84000000; d++) {
                float far_pt[3] = {g_vp_eye[0] + dirs[d][0] * 15.0f, g_vp_eye[1], g_vp_eye[2] + dirs[d][2] * 15.0f};
                float fr = line_fraction(g_vp_eye, far_pt, world, world + 0x30);
                if (fr >= 2.0f / 15.0f && fr < 1.0f) { // a real wall, not a prop or edge right at the camera
                    pick = d;
                    memcpy(dir, dirs[d], 12);
                    dist = 15.0f * fr + 1.5f;
                }
            }
            logf("[ppl] test blast: %s%s at %.1f m", pick < 0 ? "no wall within 15 m, in view" : "behind the wall ", pick < 0 ? "" : names[pick], dist);
        }
        for (int i = 0; i < 3; i++) e.c[i] = g_vp_eye[i] + dir[i] * dist;
        e.c[1] -= 0.5f; // the light adds 0.5 m (explosions sit on the ground)
        e.radius = 3.0f;
        e.kind = 1;
        g_ex_queue_n++;
    }
    static uint32_t ex_seen, shot_seen;
    static uint32_t logged_ex, logged_shots;
    uint32_t ex_n = g_ex_queue_n;
    if (ex_n - ex_seen > 16) ex_seen = ex_n - 16;
    for (; ex_seen != ex_n; ex_seen++) {
        const ExRec &e = g_ex_queue[ex_seen % 16];
        WorldLight l = {};
        memcpy(l.pos, e.c, 12);
        l.pos[1] += 0.5f; // a little above the ground it often sits on
        l.at = now;
        float r = e.radius > 0 && e.radius < 100 ? e.radius : 4.0f;
        l.reach = r * 3.0f < 8.0f ? 8.0f : r * 3.0f > 30.0f ? 30.0f : r * 3.0f;
        l.gain = e.kind == 13 ? 0.35f : 0.6f;   // kind 13: a local effect only (no damage). 1.6 blew a room out at 4 m
        l.r = 2.4f, l.g = 1.4f, l.b = 0.55f;
        l.rise_ms = 40, l.decay_ms = 450, l.life_ms = 2200;
        // kind 13 at a known fire hazard: its glint carries it, no flash each time
        // (v12z took repeated kind-13 blasts for fire hazards - owner: "the glint appeared where scripted explosions
        // occured instead of environment flame emitters": off)
        add_world_light(l, true);
        if (logged_ex++ < 100)
            logf("[ppl] explosion kind %u at %.1f %.1f %.1f radius %.2f -> light %.0f m", e.kind, e.c[0], e.c[1], e.c[2], e.radius, l.reach);
    }
    uint32_t sh_n = g_ppl_enemy_shot_n;
    if (sh_n - shot_seen > 16) shot_seen = sh_n - 16;
    for (; shot_seen != sh_n; shot_seen++) {
        uint32_t body = g_ppl_enemy_shot[shot_seen % 16];
        float m[3][4]; // pose rows at body+0x70: r0, r1, r2 = forward (+0x90), position (+0xA0)
        float pos[3], fwd[3];
        if (body < 0x80000000 || body >= 0x84000000 || !safe_read(fwd, body + 0x90, 12) || !safe_read(pos, body + 0xA0, 12)) continue;
        (void)m;
        WorldLight l = {};
        // owner playtest 3: a shooter who sees you passes the wall check, and a 9 m flash then lit your side of the
        // walls around him; a short reach keeps it on his own surroundings (real shadows would be the full fix)
        l.pos[0] = pos[0] + fwd[0] * 0.4f;
        l.pos[1] = pos[1] + 1.3f + fwd[1] * 0.4f; // chest / gun height
        l.pos[2] = pos[2] + fwd[2] * 0.4f;
        l.at = now;
        static uint32_t seed = 0x2545F491u;
        seed = seed * 1664525u + 1013904223u;
        l.gain = 1.2f + 0.3f * (float)(seed >> 8) / 16777216.0f;
        l.reach = 4.5f;
        l.r = 1.6f, l.g = 1.15f, l.b = 0.65f;
        l.rise_ms = 0, l.decay_ms = 35, l.life_ms = 200;
        add_world_light(l, false);
        static uint32_t shooters[32], nshooters;   // each shooter named once (the partner's clone among them)
        bool known = false;
        for (uint32_t s = 0; s < nshooters; s++) known |= shooters[s] == body;
        if (!known && nshooters < 32) {
            shooters[nshooters++] = body;
            logf("[ppl] new shooter %08x at %.1f %.1f %.1f (rounds by others so far %u)", body, pos[0], pos[1], pos[2], sh_n);
        }
        if (logged_shots++ < 60)
            logf("[ppl] enemy round: shooter %08x at %.1f %.1f %.1f", body, pos[0], pos[1], pos[2]);
    }
}

// This frame's world lights in view space, the 3 that matter most here (brightness x reach^2 / distance^2).
struct ViewLight { float pos[3], reach, col[3], wpos[3]; bool steady, spec_only, spark; }; // steady: a lamp or a lasting glow
// spec_only: owner "more like some light can reflect on some surface" - lamps and lamp halos light nothing around
// them but show as reflections on glossy surfaces (sent with a negative reach; the shader drops the diffuse term)
int g_vl_n;

ViewLight g_vl[12]; // owner: "can we just increase the slot limits" - 5 (3 on bump-mapped surfaces); "Go for SM3": 12

// ---- Wall check (v7): a world light behind a wall should not light the player's side ----
// The game's own line test FUN_001DAFF0 (thiscall: ECX = end vec4*, EAX = start vec4*; stack: world (*0x2D45D4),
// mask, entity to ignore, flag, out* (+0x20 = hit fraction, 2.0 = untouched); mask 0x21 = the game's own LOS /
// bullet tests; coop.cpp uses it for enemy sight). Each live world light casts 5 rays toward the camera and
// points around it (1.2 m left / right, 1 m up, 2 m ahead at chest height); the light is scaled by the share that
// gets through, smoothed. One light is re-tested per frame (round robin), so ~5 rays a frame. BLACK_PPL_WALLS=0 = off.
}  // namespace

uint32_t g_ppl_rc_fn, g_ppl_rc_end, g_ppl_rc_start, g_ppl_rc_world, g_ppl_rc_ign, g_ppl_rc_out, g_ppl_rc_esp;

__declspec(naked) void ppl_thunk_raycast()
{
    __asm {
        pushad
        mov dword ptr [g_ppl_rc_esp], esp
        push dword ptr [g_ppl_rc_out]
        push 1                              // flag (as coop.cpp)
        push dword ptr [g_ppl_rc_ign]
        push 0x21                           // mask: the game's LOS / bullet tests
        push dword ptr [g_ppl_rc_world]
        mov ecx, dword ptr [g_ppl_rc_end]
        mov eax, dword ptr [g_ppl_rc_start]
        call dword ptr [g_ppl_rc_fn]
        mov esp, dword ptr [g_ppl_rc_esp]
        popad
        ret
    }
}

namespace {

// true = something blocks the segment a -> b
// the fraction of a -> b before the first hit (2.0 = nothing hit)
float line_fraction(const float a[3], const float b[3], uint32_t world, uint32_t ignore)
{
    __declspec(align(16)) static float s[4], e[4], res[16];
    s[0] = a[0], s[1] = a[1], s[2] = a[2], s[3] = 1.0f;
    e[0] = b[0], e[1] = b[1], e[2] = b[2], e[3] = 1.0f;
    memset(res, 0, sizeof res);
    res[8] = 2.0f;
    ((int32_t *)res)[12] = -1;
    g_ppl_rc_fn = xbe_va(0x001DAFF0);
    g_ppl_rc_world = world;
    g_ppl_rc_ign = ignore;
    g_ppl_rc_start = (uint32_t)(uintptr_t)s;
    g_ppl_rc_end = (uint32_t)(uintptr_t)e;
    g_ppl_rc_out = (uint32_t)(uintptr_t)res;
    __try {
        ppl_thunk_raycast();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2.0f;
    }
    return res[8];
}

// true = something blocks the segment a - b. The game's collision is one-sided (a wall stops a ray from its room
// side only: a test blast 1.5 m behind a wall found from the camera came back 80 % visible), so both directions are
// tested; a hit within the last 3 % of the way (the target's own surroundings) does not count.
bool line_blocked(const float a[3], const float b[3], uint32_t world, uint32_t ignore)
{
    float f = line_fraction(a, b, world, ignore);
    if (f >= 0.0f && f < 0.97f) return true;
    f = line_fraction(b, a, world, ignore);
    return f >= 0.03f && f < 1.0f;
}

// Share of rays from the light (moved 0.4 m toward the camera, out of the floor or wall it sits on) to the camera
// and the points around it that get through. 1 = in plain view.
float light_visibility(const float pos[3])
{
    uint32_t world = *(volatile uint32_t *)(uintptr_t)xbe_va(0x002D45D4);
    if (world < 0x80000000 || world >= 0x84000000) return 1.0f;
    uint32_t player = world + 0x30;
    const float *eye = g_vp_eye, *fwd = g_vp_fwd;
    // right = forward x up (y up), flattened
    float rx = fwd[2], rz = -fwd[0], rl = sqrtf(rx * rx + rz * rz);
    if (rl > 1e-4f) rx /= rl, rz /= rl;
    float targets[5][3] = {
        {eye[0], eye[1], eye[2]},
        {eye[0] + rx * 1.2f, eye[1], eye[2] + rz * 1.2f},
        {eye[0] - rx * 1.2f, eye[1], eye[2] - rz * 1.2f},
        {eye[0], eye[1] + 1.0f, eye[2]},
        {eye[0] + fwd[0] * 2.0f, eye[1] - 0.5f, eye[2] + fwd[2] * 2.0f},
    };
    int through = 0;
    for (auto &t : targets) {
        float d[3] = {t[0] - pos[0], t[1] - pos[1], t[2] - pos[2]};
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 0.5f) { through++; continue; }
        float k = 0.4f / len, a[3] = {pos[0] + d[0] * k, pos[1] + d[1] * k, pos[2] + d[2] * k};
        if (!line_blocked(a, t, world, player)) through++;
    }
    return through / 5.0f;
}

// Visibility per position, cached ~12 frames (the camera moves): gunfire from one shooter and an explosion's
// whole life reuse one test. At most 3 tests (15 rays) a frame; a light not tested yet stays dark for that frame.
struct VisEntry { float pos[3]; float vis; uint32_t frame; };
VisEntry g_vis[24];
uint32_t g_vis_rw, g_vis_tests, g_vis_blocked;

float vis_for(const float pos[3], uint32_t f, int &budget)
{
    for (auto &v : g_vis) {
        float dx = v.pos[0] - pos[0], dy = v.pos[1] - pos[1], dz = v.pos[2] - pos[2];
        if (v.frame && f - v.frame <= 12 && dx * dx + dy * dy + dz * dz < 1.0f) return v.vis;
    }
    if (budget <= 0) return -1.0f;
    budget--;
    VisEntry &v = g_vis[g_vis_rw++ % 24];
    memcpy(v.pos, pos, 12);
    v.vis = light_visibility(pos);
    v.frame = f;
    g_vis_tests++;
    if (v.vis < 0.5f) g_vis_blocked++;
    if (g_vis_tests <= 150)
        logf("[ppl] wall check at %.1f %.1f %.1f (camera %.1f %.1f %.1f): %.0f %% of the rays get through", pos[0], pos[1], pos[2], g_vp_eye[0],
             g_vp_eye[1], g_vp_eye[2], v.vis * 100.0f);
    return v.vis;
}

// The sprite's own vertex program run on the CPU for its position (glow sprites are small local quads that their
// program places with per-sprite constants, so the vertices alone say nothing): the NV2A MAC / ILU ops as in
// nv2a_vsh.cpp's translation, r12 = oPos. Returns the WORLD position: the program's screen position -> clip (the
// host viewport, as the translated shaders do) -> view (1/P00, 1/P11, w) -> world (the frame's rows, transposed).
bool vs_world_pos(const VertexShader *vs, const float (*vin)[4], float world[3])
{
    if (!vs || !vs->program || !g_vp_ok) return false;
    const float (*c)[4] = g_st.vs_constants;
    float r[13][4] = {};
    r[12][3] = 1.0f;
    int a0 = 0;
    auto fetch = [&](const VshInst &in, const VshSrc &s, float o[4]) {
        static const float zero[4] = {0, 0, 0, 0};
        const float *b = zero;
        if (s.mux == MUX_R) b = r[s.reg <= 12 ? s.reg : 0];
        else if (s.mux == MUX_V) b = vin[in.v & 15];
        else if (s.mux == MUX_C) {
            int k = (int)in.cnst + (in.a0x ? a0 : 0);
            b = c[k < 0 ? 0 : k > 191 ? 191 : k];
        }
        for (int i = 0; i < 4; i++) o[i] = s.neg ? -b[s.swz[i]] : b[s.swz[i]];
    };
    for (uint32_t i = 0; i < vs->program_insts && i < 136; i++) {
        VshInst in;
        vsh_decode(vs->program + i * 4, in);
        if (in.mac == MAC_NOP && in.ilu == ILU_NOP) {
            if (in.final) break;
            continue;
        }
        float A[4], B[4], C[4], M[4] = {}, L[4] = {};
        fetch(in, in.a, A), fetch(in, in.b, B), fetch(in, in.c, C);
        switch (in.mac) {
        case MAC_MOV: case MAC_ARL: memcpy(M, A, 16); break;
        case MAC_MUL: for (int k = 0; k < 4; k++) M[k] = A[k] * B[k]; break;
        case MAC_ADD: for (int k = 0; k < 4; k++) M[k] = A[k] + C[k]; break;
        case MAC_MAD: for (int k = 0; k < 4; k++) M[k] = A[k] * B[k] + C[k]; break;
        case MAC_DP3: M[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; break;
        case MAC_DPH: M[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + B[3]; break;
        case MAC_DP4: M[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3]; break;
        case MAC_DST: M[0] = 1, M[1] = A[1] * B[1], M[2] = A[2], M[3] = B[3]; break;
        case MAC_MIN: for (int k = 0; k < 4; k++) M[k] = A[k] < B[k] ? A[k] : B[k]; break;
        case MAC_MAX: for (int k = 0; k < 4; k++) M[k] = A[k] > B[k] ? A[k] : B[k]; break;
        case MAC_SLT: for (int k = 0; k < 4; k++) M[k] = A[k] < B[k] ? 1.0f : 0.0f; break;
        case MAC_SGE: for (int k = 0; k < 4; k++) M[k] = A[k] >= B[k] ? 1.0f : 0.0f; break;
        default: break;
        }
        float x = C[0];
        switch (in.ilu) {
        case ILU_MOV: memcpy(L, C, 16); break;
        case ILU_RCP: L[0] = L[1] = L[2] = L[3] = x != 0 ? 1.0f / x : 3.0e38f; break;
        case ILU_RCC: {
            float v = fabsf(x) > 1e-30f ? 1.0f / fabsf(x) : 1.884467e19f;
            v = v < 5.42101e-20f ? 5.42101e-20f : v > 1.884467e19f ? 1.884467e19f : v;
            L[0] = L[1] = L[2] = L[3] = x < 0 ? -v : v;
            break;
        }
        case ILU_RSQ: L[0] = L[1] = L[2] = L[3] = fabsf(x) > 0 ? 1.0f / sqrtf(fabsf(x)) : 3.0e38f; break;
        case ILU_EXP: L[0] = exp2f(floorf(x)), L[1] = x - floorf(x), L[2] = exp2f(x), L[3] = 1; break;
        case ILU_LOG: {
            float ax = fabsf(x) > 0 ? fabsf(x) : 1e-30f, e = floorf(log2f(ax));
            L[0] = e, L[1] = ax / exp2f(e), L[2] = log2f(ax), L[3] = 1;
            break;
        }
        case ILU_LIT: L[0] = 1, L[1] = x > 0 ? x : 0, L[2] = (x > 0 && C[1] > 0) ? powf(C[1], C[3]) : 0, L[3] = 1; break;
        default: break;
        }
        auto put = [&](float *dst, uint8_t mask, const float *v) {
            for (int k = 0; k < 4; k++)
                if (mask & (8 >> k)) dst[k] = v[k];
        };
        if (in.mac == MAC_ARL) a0 = (int)floorf(M[0] + 0.001f);
        else if (in.mac != MAC_NOP && in.out_mac_mask) put(r[in.out_r <= 12 ? in.out_r : 0], in.out_mac_mask, M);
        if (in.ilu != ILU_NOP && in.out_ilu_mask) put(r[in.mac != MAC_NOP ? 1 : (in.out_r <= 12 ? in.out_r : 0)], in.out_ilu_mask, L);
        if (in.out_o_mask && in.out_orb && in.out_address == 0) { // oPos
            bool from_ilu = in.out_mux != 0;
            if ((from_ilu && in.ilu != ILU_NOP) || (!from_ilu && in.mac != MAC_NOP && in.mac != MAC_ARL))
                put(r[12], in.out_o_mask, from_ilu ? L : M);
        }
        if (in.final) break;
    }
    // screen -> clip (as the translated vertex shaders: (oPos - vpOffset) / vpScale * w; w 0 -> 1)
    float vs_[4], vo[4];
    host_viewport_transform(vs_, vo);
    float w = r[12][3];
    // w exactly 1 (or 0 / unwritten): a screen-space draw - the game's lens-flare / glare sprites around bright lights.
    // Taken as world sprites they sat at exactly 1 m in front of the eye: the owner's "phantom muzzle flash" (a big
    // orange light on the wall, a spark flash whenever anyone fired anywhere). Not a world position.
    if (w == 1.0f || w == 0.0f || !(fabsf(w) < 3.0e38f)) return false;
    if (!(fabsf(vs_[0]) > 1e-12f && fabsf(vs_[1]) > 1e-12f)) return false;
    float cx = (r[12][0] - vo[0]) / vs_[0] * w, cy = (r[12][1] - vo[1]) / vs_[1] * w;
    float view[3] = {cx * g_inv_p00, cy * g_inv_p11, w};
    // view -> world: world = R^T (view - t), R rows = the frame's rows scaled (as ppl_upload_vs)
    const float s[3] = {g_inv_p00, g_inv_p11, 1.0f};
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = view[k] - g_vp_rows[k][3] * s[k];
    for (int i = 0; i < 3; i++) world[i] = g_vp_rows[0][i] * s[0] * d[0] + g_vp_rows[1][i] * s[1] * d[1] + g_vp_rows[2][i] * s[2] * d[2];
    return std::isfinite(world[0]) && std::isfinite(world[1]) && std::isfinite(world[2]);
}

// The average colour of a sprite's texture (its smallest mip level; DXT blocks by their two end colours), cached per
// host texture: tells fire (bright, warm) from smoke (grey), and gives glows their real colour.
bool texture_average(float out[3])
{
    Resource *r = g_st.textures[0];
    // why a sprite texture has no colour (mission 0: 80D6F400, alpha, drawn every frame - the fire?), once each
    auto why = [&](const char *what, int code) {
        static std::unordered_map<const void *, int> seen;
        if (seen.size() < 64 && seen.emplace((const void *)r, 1).second)
            logf("[ppl] sprite texture %p: no colour - %s (%d)", (const void *)r, what, code);
        return false;
    };
    if (!r) return false;
    if (resource_type(r) != RTYPE_TEXTURE) return why("resource type", (int)resource_type(r));
    IDirect3DBaseTexture9 *bt = host_texture(r, g_st.palettes[0]);
    if (!bt) return why("no host texture", 0);
    if (bt->GetType() != D3DRTYPE_TEXTURE) return why("host type", (int)bt->GetType());
    static std::unordered_map<IDirect3DBaseTexture9 *, std::array<float, 4>> cache;
    auto it = cache.find(bt);
    if (it == cache.end()) {
        std::array<float, 4> avg = {0, 0, 0, 0}; // [3] = 1 when known
        auto *t = (IDirect3DTexture9 *)bt;
        DWORD lv = t->GetLevelCount();
        D3DSURFACE_DESC dsc;
        D3DLOCKED_RECT lr;
        if (lv && SUCCEEDED(t->GetLevelDesc(lv - 1, &dsc)) && SUCCEEDED(t->LockRect(lv - 1, &lr, nullptr, D3DLOCK_READONLY))) {
            double s[3] = {0, 0, 0};
            uint32_t n = 0;
            auto c565 = [&](uint16_t c, double k) {
                s[0] += ((c >> 11) & 31) / 31.0 * k, s[1] += ((c >> 5) & 63) / 63.0 * k, s[2] += (c & 31) / 31.0 * k;
            };
            if (dsc.Format == D3DFMT_DXT1 || dsc.Format == D3DFMT_DXT3 || dsc.Format == D3DFMT_DXT5) {
                uint32_t bw = (dsc.Width + 3) / 4, bh = (dsc.Height + 3) / 4, bs = dsc.Format == D3DFMT_DXT1 ? 8 : 16;
                for (uint32_t y = 0; y < bh && y < 64; y++)
                    for (uint32_t x = 0; x < bw && x < 64; x++) {
                        const uint8_t *b = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch + (size_t)x * bs + (bs - 8);
                        uint16_t c0, c1;
                        memcpy(&c0, b, 2), memcpy(&c1, b + 2, 2);
                        c565(c0, 0.5), c565(c1, 0.5);
                        n++;
                    }
            } else if (dsc.Format == D3DFMT_A8R8G8B8 || dsc.Format == D3DFMT_X8R8G8B8) {
                for (uint32_t y = 0; y < dsc.Height && y < 64; y++)
                    for (uint32_t x = 0; x < dsc.Width && x < 64; x++) {
                        uint32_t p;
                        memcpy(&p, (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch + x * 4, 4);
                        s[0] += ((p >> 16) & 255) / 255.0, s[1] += ((p >> 8) & 255) / 255.0, s[2] += (p & 255) / 255.0;
                        n++;
                    }
            } else if (dsc.Format == D3DFMT_R5G6B5 || dsc.Format == D3DFMT_A4R4G4B4 || dsc.Format == D3DFMT_A1R5G5B5 ||
                       dsc.Format == D3DFMT_X1R5G5B5 || dsc.Format == D3DFMT_X4R4G4B4) {
                // 16-bit textures (owner: "every fire sprite" - a sprite texture in a format not read here had no
                // colour, and an unknown colour turned a plain-alpha sprite down)
                for (uint32_t y = 0; y < dsc.Height && y < 64; y++)
                    for (uint32_t x = 0; x < dsc.Width && x < 64; x++) {
                        uint16_t p;
                        memcpy(&p, (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch + x * 2, 2);
                        if (dsc.Format == D3DFMT_R5G6B5) c565(p, 1.0);
                        else if (dsc.Format == D3DFMT_A4R4G4B4 || dsc.Format == D3DFMT_X4R4G4B4)
                            s[0] += ((p >> 8) & 15) / 15.0, s[1] += ((p >> 4) & 15) / 15.0, s[2] += (p & 15) / 15.0;
                        else s[0] += ((p >> 10) & 31) / 31.0, s[1] += ((p >> 5) & 31) / 31.0, s[2] += (p & 31) / 31.0;
                        n++;
                    }
            }
            t->UnlockRect(lv - 1);
            if (n) avg = {(float)(s[0] / n), (float)(s[1] / n), (float)(s[2] / n), 1.0f};
            else {
                static int logged;
                if (logged++ < 30) logf("[ppl] sprite texture %p: no colour read (format %u, %ux%u)", (void *)bt, (unsigned)dsc.Format, dsc.Width, dsc.Height);
            }
        } else {
            static int logged;
            if (logged++ < 30) logf("[ppl] sprite texture %p: cannot be read (lock failed)", (void *)bt);
        }
        if (cache.size() > 4096) cache.clear();
        it = cache.emplace(bt, avg).first;
    }
    if (it->second[3] == 0.0f) return false;
    memcpy(out, it->second.data(), 12);
    return true;
}

float g_sprite_tint[3] = {1, 1, 1}; // the current sprite draw's texture colour (ppl_glow_capture / _immediate)
// Owner: "the fires are particles effects that obviously use fire textures". The current particle draw uses a fire
// texture: its average is a deep orange (red bright, green well below it, blue low) - not the whiter / yellower
// spark and muzzle textures. A glow drawn with one is a fire at once (no wait, never a spark burst).
bool g_sprite_fire;
// the orange (fire / spark texture) particles of the last two frames, for telling a fire emitter by what rises above it
float g_fq[2][512][3];
int g_fq_n[2];
uint32_t g_fq_frame[2];
void fire_quad_note(const float *p)
{
    const uint32_t f = g_frame_count.load();
    const int b = f & 1;
    if (g_fq_frame[b] != f) g_fq_frame[b] = f, g_fq_n[b] = 0;
    if (g_fq_n[b] < 512) memcpy(g_fq[b][g_fq_n[b]++], p, 12);
}

// ---- The game's own glows as lights (v12): lamp halos, fires, embers ----
// BLACK fakes its light sources with additive sprites (the census: v0 FLOAT3/4 + v3 colour + UVs, no normals,
// blend SRCALPHA or ONE onto ONE, depth write off). Each such draw in world space is caught as it is drawn: its
// vertices' centre, size and colour. Within a frame, sprites closer than 1.5 m merge into one glow (a lamp halo is
// one or two sprites; a fire is a cluster of particles). Glows are tracked across frames by position: a steady
// sprite gives a steady light, a fire's particles change every frame and so the light flickers by itself. They join
// the world lights (wall check, the strongest gets the shadow cube). BLACK_PPL_GLOWS=0 = off.
struct GlowAcc { float sum[3], col[3], w, lo[3], hi[3]; bool by_cam, firetex; }; // by_cam: within 3 m of the camera (flash only)
GlowAcc g_gacc[96]; // per particle now: more clusters a frame
int g_gacc_n;
uint32_t g_gacc_frame;

struct Glow { float pos[3], col[3], level, reach, vis, lastw, flick, gate, basey; uint32_t seen, born, flash_f, shot_f; bool used, sparky, follow, on, fire, firetex; };
uint32_t g_spark_stat[2];
uint32_t g_steady_swaps; // steady lights newly picked (per-second log) // spark flashes made, frames a spark flash held a light slot
// owner: "lets turn off the ppl on lamps and lamppost ... interior light ... turn them off too": lamps from light
// cones are off (BLACK_PPL_LAMPS=1 brings them back), and a lasting glow lights only when it flickers (a fire: its
// particle sum changes each frame; a lamp halo sprite does not)
bool lamps_on() { static const bool on = getenv("BLACK_PPL_LAMPS") && getenv("BLACK_PPL_LAMPS")[0] == '1'; return on; }
constexpr float kFireFlicker = 0.004f;
Glow g_glow[32];
uint32_t g_glow_stat[3]; // sprites caught, glows made, glows alive

// ---- Lamps from the game's light cones (v12) ----
// The dock lamps and the windows' sun shafts are drawn as additive, untextured cone meshes with normals and vertex
// colour (v0 v2 v3 v9, blend ONE onto ONE, depth write off); the lamps' own glow is baked into the level. Each cone
// mesh is split into its cones (vertices within 4 m of a cone's centre) and a lamp is placed at each cone's top
// (the light hangs where the beam starts), coloured by the cone's vertex colour. A mesh is analysed once (cached
// by its data); its lamps stay while it is drawn and fade 60 s after it stops (off screen for long, or a new level).
struct Lamp { float pos[3], col[3], level, reach, vis, gate; uint32_t seen; bool used, on; };
Lamp g_lamp[128]; // census: the docks and the forest both filled 32 (later cones were dropped)
uint32_t g_lamp_stat[2]; // cone meshes analysed, lamps made
std::unordered_map<uint64_t, std::vector<int>> g_cone_lamps;

// ---- The lamp map (v12d) ----
// Owner: "the new lights seems to not render at a distance". Only 3 world lights fit the pixel shader (its
// constants are full), so far lamps lost their slot. Every lamp and lasting glow is also painted on the CPU into a
// ground-plane texture around the camera (256 x 256 texels of 2 m: 512 m), colour plus the lights' mean height;
// each lit pixel reads it at its world position (no constants needed; flat shading, no shadow). A lamp in a slot
// takes its share out of the map as it fades in there, and puts it back when it leaves the slot.
constexpr int kMap = 256;
constexpr float kCell = 2.0f;
uint32_t g_map_px[kMap * kMap];
float g_map_o[3];           // the map's world x, z corner, the camera height it was made for
uint32_t g_map_frame;       // the frame it was made (0: none)
uint32_t g_map_sent = ~0u;  // the frame on the texture
IDirect3DTexture9 *g_map_tex;
bool g_map_failed;
uint32_t g_map_lights;

void cone_capture(const VertexShader *vs, const float *conv, uint32_t sf, uint32_t n, int pe, int ce)
{
    // only once this frame's world space is known (the cones are drawn in the level's first frames, before it is)
    if (n < 6 || !g_vp_ok || g_frame_count.load() - g_vp_frame > 1) return;
    uint32_t f = g_frame_count.load();
    // the key: the vertex count and a few positions (static level data: the same every frame)
    uint64_t key = n;
    for (uint32_t v = 0; v < n; v += (n / 7 + 1)) {
        uint32_t b[3];
        memcpy(b, conv + (size_t)v * sf + pe * 4, 12);
        key = key * 1000003ull ^ b[0] ^ ((uint64_t)b[1] << 21) ^ ((uint64_t)b[2] << 42);
    }
    auto it = g_cone_lamps.find(key);
    if (it != g_cone_lamps.end()) {
        for (int i : it->second) g_lamp[i].seen = f;
        return;
    }
    if (g_cone_lamps.size() > 256) g_cone_lamps.clear();
    struct Cl { float sum[3], col[3], top, low; uint32_t n; float apex[3]; uint32_t an; };
    static Cl cl[32];
    int ncl = 0;
    static float wp[2048][3], cc[2048][3];
    uint32_t m = 0;
    for (uint32_t v = 0; v < n && m < 2048; v++) {
        float vin[16][4] = {};
        for (int r = 0; r < 16; r++) vin[r][3] = 1.0f;
        const float *src = conv + (size_t)v * sf;
        for (uint32_t i = 0; i < vs->num_elements; i++) memcpy(vin[vs->elements[i].reg & 15], src + i * 4, 16);
        if (!vs_world_pos(vs, vin, wp[m])) continue;
        if (ce >= 0) for (int k = 0; k < 3; k++) cc[m][k] = src[ce * 4 + k] * src[ce * 4 + 3];
        else cc[m][0] = cc[m][1] = cc[m][2] = 1.0f;
        m++;
    }
    for (uint32_t v = 0; v < m; v++) {
        int best = -1;
        for (int i = 0; i < ncl; i++) {
            float dx = cl[i].sum[0] / cl[i].n - wp[v][0], dy = cl[i].sum[1] / cl[i].n - wp[v][1], dz = cl[i].sum[2] / cl[i].n - wp[v][2];
            if (dx * dx + dy * dy + dz * dz < 16.0f) { best = i; break; }
        }
        if (best < 0) {
            if (ncl >= 32) continue;
            best = ncl++;
            memset(&cl[best], 0, sizeof cl[best]);
            cl[best].top = -1e30f;
            cl[best].low = 1e30f;
        }
        Cl &c = cl[best];
        for (int k = 0; k < 3; k++) c.sum[k] += wp[v][k], c.col[k] += cc[v][k];
        c.n++;
        if (wp[v][1] > c.top) c.top = wp[v][1];
        if (wp[v][1] < c.low) c.low = wp[v][1];
    }
    for (uint32_t v = 0; v < m; v++) // each cone's top: the vertices within 0.3 m of its highest point
        for (int i = 0; i < ncl; i++) {
            Cl &c = cl[i];
            float dx = c.sum[0] / c.n - wp[v][0], dz = c.sum[2] / c.n - wp[v][2];
            if (dx * dx + dz * dz < 16.0f && wp[v][1] > c.top - 0.3f) {
                for (int k = 0; k < 3; k++) c.apex[k] += wp[v][k];
                c.an++;
                break;
            }
        }
    {
        static int dl;
        if (dl++ < 30) { // diagnostics: what a cone mesh gave
            char b[400];
            int w = snprintf(b, sizeof b, "[ppl] cone mesh: %u vertices, %u placed, %d cones", n, m, ncl);
            for (int i = 0; i < ncl && i < 6 && w < 340; i++)
                w += snprintf(b + w, sizeof b - w, " | n %u top-n %u at %.1f %.1f %.1f col %.2f %.2f %.2f", cl[i].n, cl[i].an, cl[i].sum[0] / cl[i].n,
                              cl[i].top, cl[i].sum[2] / cl[i].n, cl[i].col[0] / cl[i].n, cl[i].col[1] / cl[i].n, cl[i].col[2] / cl[i].n);
            logf("%s", b);
        }
    }
    std::vector<int> made;
    for (int i = 0; i < ncl; i++) {
        Cl &c = cl[i];
        if (c.n < 6 || !c.an) continue;
        // the light halfway down the cone, where its beam lands (floodlights stand 20 m above the ground: a light at
        // the top would not reach it), its reach from the cone's length
        float apex[3] = {c.apex[0] / c.an, c.apex[1] / c.an, c.apex[2] / c.an};
        float bottom[3] = {c.sum[0] / c.n, c.low, c.sum[2] / c.n};
        float len = sqrtf((apex[0] - bottom[0]) * (apex[0] - bottom[0]) + (apex[1] - bottom[1]) * (apex[1] - bottom[1]) +
                          (apex[2] - bottom[2]) * (apex[2] - bottom[2]));
        float p[3] = {(apex[0] + bottom[0]) * 0.5f, (apex[1] + bottom[1]) * 0.5f, (apex[2] + bottom[2]) * 0.5f};
        float reach = std::fmin(std::fmax(len * 0.9f, 6.0f), 25.0f);
        // owner playtest 6: the dock floodlights lit nothing - their cones are short beams that fade out in mid-air,
        // 15-20 m above the ground. Follow the cone's axis with the line test to the ground: the light goes 2.5 m
        // above where the beam lands, a 12 m pool.
        if (len > 0.5f) {
            uint32_t world = *(volatile uint32_t *)(uintptr_t)xbe_va(0x002D45D4);
            float dir[3] = {(bottom[0] - apex[0]) / len, (bottom[1] - apex[1]) / len, (bottom[2] - apex[2]) / len};
            float far_pt[3] = {apex[0] + dir[0] * 60.0f, apex[1] + dir[1] * 60.0f, apex[2] + dir[2] * 60.0f};
            float fr = (world >= 0x80000000 && world < 0x84000000) ? line_fraction(apex, far_pt, world, world + 0x30) : 2.0f;
            if (fr > 0.0f && fr < 1.0f) {
                float hit = 60.0f * fr, back = hit > 3.5f ? 2.5f : hit * 0.5f;
                for (int k = 0; k < 3; k++) p[k] = apex[k] + dir[k] * (hit - back);
                reach = 12.0f;
            }
        }
        float col[3] = {c.col[0] / c.n, c.col[1] / c.n, c.col[2] / c.n};
        float bright = std::fmax(std::fmax(col[0], col[1]), col[2]);
        if (bright < 0.02f) continue;
        int slot = -1;
        for (int k = 0; k < 128 && slot < 0; k++) { // the same lamp again (another mesh / LOD): reuse it
            if (!g_lamp[k].used) continue;
            float dx = g_lamp[k].pos[0] - p[0], dy = g_lamp[k].pos[1] - p[1], dz = g_lamp[k].pos[2] - p[2];
            if (dx * dx + dy * dy + dz * dz < 6.25f) slot = k; // within 2.5 m: the same lamp
        }
        for (int k = 0; k < 128 && slot < 0; k++)
            if (!g_lamp[k].used) slot = k;
        if (slot < 0) break;
        Lamp &L = g_lamp[slot];
        if (!L.used) g_lamp_stat[1]++;
        if (!L.used) L.vis = -1.0f, L.on = false, L.gate = 0.0f;
        L.used = true;
        memcpy(L.pos, p, 12);
        for (int k = 0; k < 3; k++) L.col[k] = col[k] / bright;
        float level = std::fmin(std::fmax(bright * 2.0f, 0.35f), 1.0f);
        if (L.seen == f && level < L.level) { made.push_back(slot); continue; } // keep the stronger of two pieces
        L.level = level;
        L.reach = reach;
        L.seen = f;
        made.push_back(slot);
        static int logged;
        if (logged++ < 40)
            logf("[ppl] lamp at %.1f %.1f %.1f from a light cone (%u vertices; colour %.2f %.2f %.2f)", p[0], p[1], p[2], c.n, col[0], col[1], col[2]);
    }
    g_lamp_stat[0]++;
    if (m) g_cone_lamps[key] = made; // nothing placed (no world position): try again next time
}

} // namespace

// pos / col: float4 per vertex at the given strides (in floats); col may be null (white)
namespace { bool near_blast(const float p[3], double ms, float r); } // below

static void glow_add(const float *pos, uint32_t pos_stride, const float *col, uint32_t col_stride, uint32_t n);

void ppl_glow_sprites(const float *pos, uint32_t pos_stride, const float *col, uint32_t col_stride, uint32_t n)
{
    if (!ppl_med()) return; // LIGHTING LOW: no glows / spark flashes (the 0.9.0 lighting)
    static const bool off = getenv("BLACK_PPL_GLOWS") && getenv("BLACK_PPL_GLOWS")[0] == '0';
    if (off || !ppl_level() || n < 1 || n > 4096 || g_fov_viewmodel) return;
    const uint32_t *rs = render_state();
    if (!rs[XRS_ALPHABLENDENABLE] || rs[XRS_ZWRITEENABLE]) return; // blended onto the scene only
    bool additive = rs[XRS_DESTBLEND] == 1 && (rs[XRS_SRCBLEND] == 0x302 || rs[XRS_SRCBLEND] == 1);
    bool alpha = rs[XRS_SRCBLEND] == 0x302 && rs[XRS_DESTBLEND] == 0x303;
    if (!additive && !alpha) return;
    if (!g_vp_ok || g_frame_count.load() - g_vp_frame > 2) return; // positions come in world space (vs_world_pos)
    uint32_t f = g_frame_count.load();
    if (f != g_gacc_frame) { g_gacc_frame = f; g_gacc_n = 0; }
    // owner: "find the point of origin of every flame emitters". One draw holds up to 64 particles (quads) over 2-3 m,
    // from several emitters, sparks and flames mixed (mission 0 log): taken as one cluster its middle jumped every
    // frame and no fire ever lasted. Each quad now joins the cluster within 1.5 m on its own, so each emitter
    // becomes a cluster of its own, with its lowest particle at the emitter.
    if (n >= 8 && n % 4 == 0) {
        for (uint32_t q = 0; q + 4 <= n; q += 4) glow_add(pos + (size_t)q * pos_stride, pos_stride, col ? col + (size_t)q * col_stride : nullptr, col_stride, 4);
        return;
    }
    glow_add(pos, pos_stride, col, col_stride, n);
}

static void glow_add(const float *pos, uint32_t pos_stride, const float *col, uint32_t col_stride, uint32_t n)
{
    const uint32_t *rs = render_state();
    float c[3] = {0, 0, 0}, lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, a = 0;
    for (uint32_t v = 0; v < n; v++) {
        const float *p = pos + (size_t)v * pos_stride;
        for (int k = 0; k < 3; k++) {
            if (p[k] < lo[k]) lo[k] = p[k];
            if (p[k] > hi[k]) hi[k] = p[k];
        }
        if (col) {
            const float *q = col + (size_t)v * col_stride;
            c[0] += q[0], c[1] += q[1], c[2] += q[2], a += q[3];
        }
    }
    if (col) c[0] /= n, c[1] /= n, c[2] /= n, a /= n;
    else c[0] = c[1] = c[2] = a = 1.0f;
    for (int k = 0; k < 3; k++) c[k] *= g_sprite_tint[k]; // the texture's colour (fire orange, spark white)
    float size = std::fmax(std::fmax(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
    if (!(size > 0.01f && size < 12.0f)) return; // a degenerate or a whole-screen effect
    float w = (c[0] + c[1] + c[2]) / 3.0f * (a > 0 ? a : 1.0f) * std::fmin(size, 3.0f);
    if (w < 0.01f) return;
    float mid[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    if (g_sprite_fire) fire_quad_note(mid);
    // effects that follow the player (the forest's fireflies, dust around the camera) are not light sources: a glow
    // within 3 m of the camera would light the scene as if the player carried a lamp. They were dropped; now they are
    // kept as "near" (owner: "every single bullet sparks should produce light" - close impacts were lost): a near
    // glow can flash after a shot but never lights steadily
    float ndx = mid[0] - g_vp_eye[0], ndy = mid[1] - g_vp_eye[1], ndz = mid[2] - g_vp_eye[2];
    const bool by_cam = ndx * ndx + ndy * ndy + ndz * ndz < 9.0f;
    // within 1.6 m of the eye: the player's own gun, not a glow at all. Owner playtest (solo, nobody firing at the
    // player): spark flashes ~1 m in front of the camera at level 0.8-2.6 - muzzle-flash sprites at the player's gun
    // without a shot (an old bug); enemies firing elsewhere made every frame count as "just after a shot"
    {   // BLACK_PPL_BLASTLOG=1: the particle draws within 12 m of an explosion for 10 s after it (every 6th frame) -
        // what a car's fire looks like to us (owner: "find the point of origin of every flame emitters")
        static const bool on = getenv("BLACK_PPL_BLASTLOG") && getenv("BLACK_PPL_BLASTLOG")[0] == '1';
        static uint32_t lines;
        const uint32_t fr = g_frame_count.load();
        if (on && lines < 1500 && fr % 6 == 0 && near_blast(mid, 10000.0, 12.0f)) {
            lines++;
            logf("[ppl] blast-area sprite f %u: mid %.1f %.1f %.1f lo-y %.1f hi-y %.1f size %.2f n %u w %.2f col %.2f %.2f %.2f a %.2f tex %p blend %x/%x",
                 fr, mid[0], mid[1], mid[2], lo[1], hi[1], size, n, w, c[0], c[1], c[2], a, (const void *)g_st.textures[0],
                 rs[XRS_SRCBLEND], rs[XRS_DESTBLEND]);
        }
    }
    if (by_cam) { // BLACK_PPL_NEARLOG=1: every sprite draw within 3 m of the eye (the phantom muzzle flash hunt)
        static const bool on = getenv("BLACK_PPL_NEARLOG") && getenv("BLACK_PPL_NEARLOG")[0] == '1';
        static uint32_t lines;
        if (on && lines < 1500) {
            lines++;
            float v[3] = {0, 0, 0};
            world_to_view(mid, v);
            uint32_t fr = g_frame_count.load();
            logf("[ppl] near sprite f %u: view %+.2f %+.2f %+.2f (%.2f m) size %.2f n %u w %.2f col %.2f %.2f %.2f a %.2f | vs %p tex %p blend %x/%x "
                 "| player shot %d frames ago (rounds %u), any shot %d frames ago",
                 fr, v[0], v[1], v[2], sqrtf(ndx * ndx + ndy * ndy + ndz * ndz), size, n, w, c[0], c[1], c[2], a, (const void *)g_glow_vs,
                 (const void *)g_st.textures[0], rs[XRS_SRCBLEND], rs[XRS_DESTBLEND], (int)(fr - g_ppl_player_shot_frame),
                 (unsigned)g_ppl_player_shots, (int)(fr - g_ppl_last_shot_frame));
        }
    }
    if (ndx * ndx + ndy * ndy + ndz * ndz < 2.56f) return;
    g_glow_stat[0]++;
    const uint32_t f = g_frame_count.load();
    (void)f;
    for (int i = 0; i < g_gacc_n; i++) { // into the cluster within 1.5 m
        GlowAcc &g = g_gacc[i];
        float cx = g.sum[0] / g.w - mid[0], cy = g.sum[1] / g.w - mid[1], cz = g.sum[2] / g.w - mid[2];
        if (cx * cx + cy * cy + cz * cz < 2.25f) {
            for (int k = 0; k < 3; k++) {
                g.sum[k] += mid[k] * w, g.col[k] += c[k] * w;
                g.lo[k] = std::fmin(g.lo[k], lo[k]), g.hi[k] = std::fmax(g.hi[k], hi[k]);
            }
            g.w += w;
            g.by_cam = g.by_cam || by_cam;
            g.firetex = g.firetex || g_sprite_fire;
            return;
        }
    }
    if (g_gacc_n < 96) {
        GlowAcc &g = g_gacc[g_gacc_n++];
        for (int k = 0; k < 3; k++) g.sum[k] = mid[k] * w, g.col[k] = c[k] * w, g.lo[k] = lo[k], g.hi[k] = hi[k];
        g.w = w;
        g.by_cam = by_cam;
        g.firetex = g_sprite_fire;
    }
}

// a sprite draw: each vertex through its program (vs_world_pos), at most 256
static bool glow_draw_ok(const VertexShader *vs)
{
    const uint32_t *rs = render_state();
    if (!(ppl_level() && vs && vs->program && !g_fov_viewmodel && rs[XRS_ALPHABLENDENABLE] && !rs[XRS_ZWRITEENABLE])) return false;
    return (rs[XRS_DESTBLEND] == 1 && (rs[XRS_SRCBLEND] == 0x302 || rs[XRS_SRCBLEND] == 1)) ||
           (rs[XRS_SRCBLEND] == 0x302 && rs[XRS_DESTBLEND] == 0x303);
}

// The sprite's texture colour into g_sprite_tint; for plain alpha sprites only fire passes (bright and warm: not
// grey smoke or dust). Owner playtest 6: the burning crane gave no light - its fire is drawn with transparency.
static bool glow_sprite_tint()
{
    float t[3];
    bool known = texture_average(t);
    // orange: green 35-80 % of red, blue under 45 % (mission 0: 81F2B100 0.89 0.63 0.37 = fire; 80D71500 0.83 0.12 0.02
    // = too red, not fire)
    g_sprite_fire = known && t[0] > 0.35f && t[1] > 0.35f * t[0] && t[1] < 0.8f * t[0] && t[2] < 0.45f * t[0];
    {   // census every 10 s: the particle textures, their colour, blend, and how often drawn just after a shot
        static std::unordered_map<const void *, std::array<float, 7>> cen; // draws, r, g, b, alpha draws, after-shot draws, fire
        static uint32_t last, lines;
        auto &e = cen[(const void *)g_st.textures[0]];
        const uint32_t fr = g_frame_count.load();
        e[0] += 1;
        if (known) e[1] = t[0], e[2] = t[1], e[3] = t[2];
        if (render_state()[XRS_DESTBLEND] == 0x303) e[4] += 1;
        if (fr - g_ppl_last_shot_frame <= 4) e[5] += 1;
        e[6] = g_sprite_fire ? 1.0f : 0.0f;
        if (fr - last >= 600 && lines < 60) {
            last = fr, lines++;
            char b[1200];
            int w = snprintf(b, sizeof b, "[ppl] particle textures (draws, colour, alpha %%, after a shot %%):");
            for (auto &kv : cen)
                if (w < 1100)
                    w += snprintf(b + w, sizeof b - w, " | %p: %.0f, %.2f %.2f %.2f, %.0f%%, %.0f%%%s", kv.first, kv.second[0], kv.second[1],
                                  kv.second[2], kv.second[3], 100 * kv.second[4] / kv.second[0], 100 * kv.second[5] / kv.second[0],
                                  kv.second[6] > 0 ? " FIRE" : "");
            logf("%s", b);
            cen.clear();
        }
    }
    const uint32_t *rs = render_state();
    bool alpha = rs[XRS_DESTBLEND] == 0x303;
    if (alpha && !(known && t[0] > 0.45f && t[0] > t[2] + 0.2f && t[0] >= t[1] * 0.9f)) {
        // diagnostics: plain-alpha sprite textures turned down (smoke, dust - or a fire the test misses), every 10 s
        static std::unordered_map<const void *, std::array<float, 4>> rej;
        static uint32_t last, lines;
        auto &e = rej[(const void *)g_st.textures[0]];
        e[0] += 1;
        if (known) e[1] = t[0], e[2] = t[1], e[3] = t[2];
        uint32_t f = g_frame_count.load();
        if (f - last >= 600 && lines < 40) {
            last = f, lines++;
            char b[700];
            int w = snprintf(b, sizeof b, "[ppl] alpha sprites turned down (texture: draws, colour):");
            for (auto &kv : rej)
                if (w < 640) w += snprintf(b + w, sizeof b - w, " | %p: %.0f, %.2f %.2f %.2f", kv.first, kv.second[0], kv.second[1], kv.second[2], kv.second[3]);
            logf("%s", b);
            rej.clear();
        }
        return false;
    }
    if (known) {
        float m = std::fmax(std::fmax(t[0], t[1]), std::fmax(t[2], 0.05f));
        for (int k = 0; k < 3; k++) g_sprite_tint[k] = t[k] / m;
    } else g_sprite_tint[0] = g_sprite_tint[1] = g_sprite_tint[2] = 1.0f;
    return true;
}

static void spot_log(const VertexShader *vs, const float *conv, uint32_t sf, uint32_t n, bool immediate);

void ppl_glow_immediate(const VertexShader *vs, const float *verts, uint32_t n)
{
    if (!ppl_med()) return;
    spot_log(vs, verts, 64, n, true);
    if (!glow_draw_ok(vs) || (vs_input_mask(vs) & 4)) return; // normals: a surface, not a glow
    static float wpos[256][4], col[256][4];
    uint32_t m = 0;
    bool has_col = (vs_input_mask(vs) & 8) != 0;
    for (uint32_t v = 0; v < n && m < 256; v++) {
        const float (*vin)[4] = (const float (*)[4])(verts + (size_t)v * 64);
        if (!vs_world_pos(vs, vin, wpos[m])) continue;
        memcpy(col[m], vin[3], 16);
        m++;
    }
    g_glow_vs = vs;
    if (m && glow_sprite_tint()) ppl_glow_sprites(&wpos[0][0], 4, has_col ? &col[0][0] : nullptr, 4, m);
}

// BLACK_PPL_SPOT=x,y,z: every programmable draw with a vertex within 6 m of that point, whatever its blend / depth
// state (owner: environment fires with no glint - not caught by the glow capture: how are they drawn?)
static void spot_log(const VertexShader *vs, const float *conv, uint32_t sf, uint32_t n, bool immediate = false)
{
    static float spot[3];
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("BLACK_PPL_SPOT");
        on = !v ? 0 : !strcmp(v, "cam") ? 2 : sscanf_s(v, "%f,%f,%f", &spot[0], &spot[1], &spot[2]) == 3 ? 1 : 0; // (was v && (..): a bool, "cam" became 1)
    }
    if (on == 2) memcpy(spot, g_vp_eye, 12); // BLACK_PPL_SPOT=cam: around the player (stand by a fire)
    if (!on) return;
    {   // why draws are not logged, once a second (the ghost run at the fire logged nothing after 72 s)
        static uint32_t calls, novs, novp, lastf;
        calls++;
        if (!vs || !vs->program) novs++;
        if (!g_vp_ok) novp++;
        const uint32_t fr = g_frame_count.load();
        if (fr - lastf >= 60) {
            logf("[ppl] spot: %u draws seen, %u without a program, %u without the view; camera %.1f %.1f %.1f", calls, novs, novp, spot[0], spot[1], spot[2]);
            lastf = fr, calls = novs = novp = 0;
        }
    }
    if (!vs || !vs->program || !conv || !g_vp_ok) return;
    static const float from_s = env_f("BLACK_PPL_SPOT_FROM", 0.0f); // start logging this many seconds in
    if (g_frame_count.load() < (uint32_t)(from_s * 60.0f)) return;
    static uint32_t lines;
    if (lines >= 4000) return;
    const uint32_t f = g_frame_count.load();
    if (f % 15) return; // a frame every quarter second
    // the distance from the spot to the box around the draw's vertices (a big mesh's corners can all be far)
    float w[4], lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, at[3] = {0, 0, 0};
    uint32_t got = 0;
    for (uint32_t v = 0; v < n && v < 128; v++) {
        float vin[16][4] = {};
        if (immediate) memcpy(vin, conv + (size_t)v * 64, sizeof vin);
        else {
            for (int r = 0; r < 16; r++) vin[r][3] = 1.0f;
            const float *src = conv + (size_t)v * sf;
            for (uint32_t i = 0; i < vs->num_elements; i++) memcpy(vin[vs->elements[i].reg & 15], src + i * 4, 16);
        }
        if (!vs_world_pos(vs, vin, w)) continue;
        for (int k = 0; k < 3; k++) lo[k] = std::fmin(lo[k], w[k]), hi[k] = std::fmax(hi[k], w[k]);
        got++;
    }
    if (!got) return;
    float best = 0;
    for (int k = 0; k < 3; k++) {
        float c = spot[k] < lo[k] ? lo[k] : spot[k] > hi[k] ? hi[k] : spot[k];
        best += (c - spot[k]) * (c - spot[k]);
        at[k] = (lo[k] + hi[k]) * 0.5f;
    }
    if (best > (on == 2 ? 100.0f : 36.0f)) return; // within 10 m of the camera, 6 m of a point
    const uint32_t *rs = render_state();
    float t[3] = {0, 0, 0};
    bool known = texture_average(t);
    char decl[120];
    int dw = 0;
    for (uint32_t i = 0; i < vs->num_elements && dw < 100; i++) dw += snprintf(decl + dw, sizeof decl - dw, "v%u:%02x ", vs->elements[i].reg, vs->elements[i].xtype);
    lines++;
    if (rs[XRS_ALPHABLENDENABLE] && !rs[XRS_ZWRITEENABLE] && (vs_input_mask(vs) & 4) && (rs[XRS_PSTEXTUREMODES] & 0x1F)) {
        // a blended particle batch with normals: where its vertices group (2 m), each group's centre and lowest point
        float cg[16][5] = {}; // x, y, z sums, count, low y
        int ng = 0;
        for (uint32_t v = 0; v < n && v < 256; v++) {
            float vin[16][4] = {}, q[4];
            if (immediate) memcpy(vin, conv + (size_t)v * 64, sizeof vin);
            else {
                for (int r = 0; r < 16; r++) vin[r][3] = 1.0f;
                const float *src = conv + (size_t)v * sf;
                for (uint32_t i = 0; i < vs->num_elements; i++) memcpy(vin[vs->elements[i].reg & 15], src + i * 4, 16);
            }
            if (!vs_world_pos(vs, vin, q)) continue;
            int g = 0;
            for (; g < ng; g++) {
                float dx = cg[g][0] / cg[g][3] - q[0], dz = cg[g][2] / cg[g][3] - q[2];
                if (dx * dx + dz * dz < 4.0f) break;
            }
            if (g == ng) {
                if (ng == 16) continue;
                ng++;
                cg[g][4] = 1e30f;
            }
            cg[g][0] += q[0], cg[g][1] += q[1], cg[g][2] += q[2], cg[g][3] += 1, cg[g][4] = std::fmin(cg[g][4], q[1]);
        }
        char gb[900];
        int gw = snprintf(gb, sizeof gb, "[ppl] spot batch f %u tex %p: %d groups", f, (const void *)g_st.textures[0], ng);
        for (int g = 0; g < ng && gw < 860; g++)
            gw += snprintf(gb + gw, sizeof gb - gw, " | %.1f %.1f %.1f low %.1f x%.0f", cg[g][0] / cg[g][3], cg[g][1] / cg[g][3], cg[g][2] / cg[g][3], cg[g][4], cg[g][3]);
        logf("%s", gb);
    }
    logf("[ppl] spot draw%s f %u: %u verts, box %.1f m away, centre %.1f %.1f %.1f size %.1f | blend %u (%x/%x) zwrite %u ztest %u alphatest %u ref %u | tex %p modes %x avg %s%.2f %.2f %.2f | vs %p %s",
         immediate ? " (immediate)" : "", f, n, sqrtf(best), at[0], at[1], at[2], std::fmax(std::fmax(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]), rs[XRS_ALPHABLENDENABLE], rs[XRS_SRCBLEND], rs[XRS_DESTBLEND], rs[XRS_ZWRITEENABLE], rs[XRS_ZENABLE],
         rs[XRS_ALPHATESTENABLE], rs[XRS_ALPHAREF], (const void *)g_st.textures[0], rs[XRS_PSTEXTUREMODES], known ? "" : "?", t[0], t[1], t[2],
         (const void *)vs, decl);
}

namespace { void fire_site_from_draw(const float p[3]); } // the fire sites (below)

static void env_fire_capture(const VertexShader *vs, const float *conv, uint32_t sf, uint32_t n, int pe)
{
    const uint32_t *rs = render_state();
    if (!rs[XRS_ALPHABLENDENABLE] || rs[XRS_ZWRITEENABLE] || pe < 0 || !g_vp_ok) return;
    float t[3];
    if (!texture_average(t)) return;
    if (!(t[0] > 0.3f && t[0] > t[1] && t[1] > t[2] && t[2] < 0.75f * t[0])) return; // warm: fire, not grey smoke
    float lo = 1e30f, sx = 0, sz = 0;
    uint32_t m = 0;
    for (uint32_t v = 0; v < n && v < 64; v++) {
        float vin[16][4] = {};
        for (int r = 0; r < 16; r++) vin[r][3] = 1.0f;
        const float *src = conv + (size_t)v * sf;
        for (uint32_t i = 0; i < vs->num_elements; i++) memcpy(vin[vs->elements[i].reg & 15], src + i * 4, 16);
        float w[3];
        if (!vs_world_pos(vs, vin, w)) continue;
        lo = std::fmin(lo, w[1]), sx += w[0], sz += w[2], m++;
    }
    if (!m) return;
    const float p[3] = {sx / m, lo, sz / m};
    const float dx = p[0] - g_vp_eye[0], dz = p[2] - g_vp_eye[2];
    if (dx * dx + dz * dz < 1.0f) return; // not the camera's own effects
    fire_site_from_draw(p);
}

void ppl_glow_capture(const VertexShader *vs, const float *conv, uint32_t stride_bytes, uint32_t n)
{
    if (!ppl_med()) return;
    spot_log(vs, conv, stride_bytes / 4, n);
    if (!conv || !glow_draw_ok(vs)) return;
    int pe = -1, ce = -1;
    bool normals = false;
    for (uint32_t i = 0; i < vs->num_elements; i++) {
        const VertexElement &e = vs->elements[i];
        if (e.reg == 2) normals = true;
        if (e.reg == 0 && (e.xtype == 0x32 || e.xtype == 0x42)) pe = (int)i;
        if (e.reg == 3) ce = (int)i;
    }
    if (pe < 0) return;
    uint32_t sf = stride_bytes / 4;
    if (normals) { // a surface; untextured additive ones are the light cones of lamps and windows
        // Owner's mission 0 fires with no glint (the burning wreck, the debris fires): BLACK_PPL_SPOT showed them as
        // alpha-blended particle meshes WITH a normal, depth write off and a warm texture (80CA0200, average
        // 0.42 0.33 0.21) - set aside here as "light cones" until now. Such a draw is a level fire: its lowest vertex
        // is the fire's origin, refreshed each frame it is drawn (fire_site_from_draw).
        // (v12za: OFF - owner: "those are enemies spawns". Texture 80CA0200 is not fire: soldiers fading in at their
        // spawn are drawn blended, with a normal, depth write off, and their camo averages warm - every "level fire"
        // was an enemy appearing, the moving one an enemy running in.)
        if ((render_state()[XRS_PSTEXTUREMODES] & 0x1F) != 0) return;
        if ((render_state()[XRS_PSTEXTUREMODES] & 0x1F) == 0) cone_capture(vs, conv, sf, n, pe, ce);
        return;
    }
    static float wpos[256][4], col[256][4];
    uint32_t m = 0;
    for (uint32_t v = 0; v < n && m < 256; v++) {
        float vin[16][4] = {};
        for (int r = 0; r < 16; r++) vin[r][3] = 1.0f;
        const float *src = conv + (size_t)v * sf;
        for (uint32_t i = 0; i < vs->num_elements; i++) memcpy(vin[vs->elements[i].reg & 15], src + i * 4, 16);
        if (!vs_world_pos(vs, vin, wpos[m])) continue;
        if (ce >= 0) memcpy(col[m], src + ce * 4, 16);
        m++;
    }
    g_glow_vs = vs;
    if (m && glow_sprite_tint()) ppl_glow_sprites(&wpos[0][0], 4, ce >= 0 ? &col[0][0] : nullptr, 4, m);
}

namespace {

// Last frame's sprite clusters -> tracked glows (called once a frame, before the lights are picked)
double g_glow_now; // frame_world_lights: this frame's time

bool near_blast(const float p[3], double ms, float r)
{
    for (int i = 0; i < 16; i++) {
        const WorldLight &l = g_wl[i];
        if (l.life_ms <= 0) continue;
        double age = g_glow_now - l.at;
        if (age < 0 || age > ms) continue;
        float dx = l.pos[0] - p[0], dy = l.pos[1] - p[1], dz = l.pos[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

bool near_effect_light(const float p[3])
{
    for (int i = 0; i < 32; i++) {
        const WorldLight &l = g_wl[i];
        if (l.spark || l.life_ms <= 0) continue;
        const bool blast = i < 16;
        double age = g_glow_now - l.at;
        if (age < 0 || age > (blast ? 1500.0 : 1000.0)) continue;
        float dx = l.pos[0] - p[0], dy = l.pos[1] - p[1], dz = l.pos[2] - p[2], r = blast ? 4.0f : 2.0f;
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

void update_glows(uint32_t f)
{
    static uint32_t done;
    if (done == f) return;
    done = f;
    if (g_gacc_frame + 1 == f || g_gacc_frame == f) {
        static uint32_t lines;
        if (f % 120 == 0 && lines++ < 60) { // diagnostics: last frame's clusters
            char b[400];
            int w = snprintf(b, sizeof b, "[ppl] glow clusters frame %u: %d (camera %.1f %.1f %.1f)", g_gacc_frame, g_gacc_n, g_vp_eye[0], g_vp_eye[1], g_vp_eye[2]);
            for (int i = 0; i < g_gacc_n && i < 5 && w < 330; i++) {
                const GlowAcc &a = g_gacc[i];
                w += snprintf(b + w, sizeof b - w, " | %.1f %.1f %.1f ext %.1f w %.2f", a.sum[0] / a.w, a.sum[1] / a.w, a.sum[2] / a.w,
                              std::fmax(std::fmax(a.hi[0] - a.lo[0], a.hi[1] - a.lo[1]), a.hi[2] - a.lo[2]), a.w);
            }
            logf("%s", b);
        }
        for (int i = 0; i < g_gacc_n; i++) {
            const GlowAcc &a = g_gacc[i];
            float p[3] = {a.sum[0] / a.w, a.sum[1] / a.w, a.sum[2] / a.w};
            float cm = std::fmax(std::fmax(a.col[0], a.col[1]), std::fmax(a.col[2], 1e-4f));
            float col[3] = {a.col[0] / cm, a.col[1] / cm, a.col[2] / cm}; // hue, brightest channel 1
            float ext = std::fmax(std::fmax(a.hi[0] - a.lo[0], a.hi[1] - a.lo[1]), a.hi[2] - a.lo[2]);
            float level = std::fmin(0.25f + 0.25f * a.w, 1.0f);
            float reach = std::fmin(std::fmax(3.0f + ext * 2.5f, 4.0f), 10.0f);
            int best = -1, free_slot = -1;
            float bd = 2.25f;
            for (int k = 0; k < 32; k++) {
                if (!g_glow[k].used) { if (free_slot < 0) free_slot = k; continue; }
                float dx = g_glow[k].pos[0] - p[0], dy = g_glow[k].pos[1] - p[1], dz = g_glow[k].pos[2] - p[2], d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bd) bd = d2, best = k;
            }
            // owner: "spark lights are still delayed" - under continuous fire the sparks hit the same spot and joined the
            // glow already there, so only its first burst flashed, and after 0.5 s the spark cluster became a steady
            // light that faded in. Now: a burst (a new glow, or more sprites than the last frame) within 4 frames of a
            // shot flashes; a glow born during gunfire is "sparky" and never steady until it glows on for a second
            // with no bursts (a real fire, a burning barrel)
            const bool recent_shot = f - g_ppl_last_shot_frame <= 4;
            bool burst = false;
            if (best < 0) {
                if (free_slot < 0) continue;
                best = free_slot;
                Glow &g = g_glow[best];
                g.used = true, g.born = f, g.level = level, g.vis = -1.0f, g.on = false, g.gate = 0.0f, g.basey = a.lo[1];
                g.lastw = 0, g.flash_f = 0, g.shot_f = f, g.sparky = recent_shot, g.flick = 0, g.follow = a.by_cam, g.fire = false, g.firetex = false;
                memcpy(g.pos, p, 12);
                g_glow_stat[1]++;
                burst = true;
            } else burst = a.w > g_glow[best].lastw + 0.02f; // any new sprite (owner: a single spark should flash too)
            // born at the camera and still there: it follows the player (fireflies, dust) - never a steady light. A glow
            // the player walks up to is not marked (owner playtest: a wall lamp stayed "sparky" and gave no glint)
            if (!a.by_cam) g_glow[best].follow = false;
            // owner: "its still flashing in some spot" - an enemy's muzzle-flash sprites under continuous fire made a
            // lasting glow that flipped between halo and fire (flick 0.004 / 0.02-0.04) and lit the room on and off;
            // the gunfire already has its own light. A glow within 2 m of a gunfire light of the last second, or 4 m of
            // an explosion of the last 1.5 s, is that effect's sprites: never a steady light.
            // (fire textures too: muzzle flashes use one - mission 0: a "fire" at an enemy shooter, -74 -3 36)
            const bool at_effect = near_effect_light(p);
            if (at_effect) g_glow[best].sparky = true, g_glow[best].shot_f = f;
            if (burst && recent_shot) g_glow[best].shot_f = f;
            // owner: "sparks doesnt produce flash anymore" - the spark texture IS the fire texture (81F2B100), so the
            // texture cannot tell them apart - and "fire particle effect ... spawn after a car explosion": a glow born
            // within 6 m of an explosion of the last 3 s is that explosion's fire. Once the fireball is over (at_effect
            // ends after 1.5 s) it is a fire at once: steady, strobing, no spark flashes of its own.
            {
                Glow &bg = g_glow[best];
                if (f - bg.born < 90 && near_blast(p, 3000.0, 6.0f)) bg.firetex = true;
                if (bg.firetex && !at_effect) bg.fire = true, bg.sparky = false;
            }
            bool flashed_near = false; // owner: "when one bullet sparks, many flash appears": one flash per 2.5 m / 150 ms
            for (int wi = 16; wi < 32 && !flashed_near; wi++) {
                const WorldLight &wl = g_wl[wi];
                // owner: "sparks still play flash twice": 300 ms / 3 m (was 150 ms / 2.5 m)
                if (!wl.spark || g_glow_now - wl.at > 300.0 || g_glow_now < wl.at) continue;
                float dx = wl.pos[0] - p[0], dy = wl.pos[1] - p[1], dz = wl.pos[2] - p[2];
                flashed_near = dx * dx + dy * dy + dz * dz < 9.0f;
            }
            const bool flashed_before = g_glow[best].flash_f && f - g_glow[best].flash_f < 24; // one flash per glow per 400 ms
            if (burst && recent_shot && !flashed_near && !flashed_before && !g_glow[best].firetex) {
                g_glow[best].flash_f = f;
                // owner: a ricochet should flash: a tiny, short flash in the sparks' colour
                WorldLight fl = {};
                memcpy(fl.pos, p, 12);
                fl.at = g_glow_now; // this frame's time: full strength in the frame it is first drawn
                // owner: "the sparks produce nearly nothing", "increase the values", then "the sparks are not instant
                // and the radius is too big": 1.5 m, full strength at once, then a very quick fade
                fl.gain = 3.0f; // owner: "bullet sparks are still missing" - made and in a slot, but too faint and too brief to see
                {   // close to the eye (rounds hitting the player's cover): a quarter at 0.8 m, full from 3 m
                    float ex = p[0] - g_vp_eye[0], ey = p[1] - g_vp_eye[1], ez = p[2] - g_vp_eye[2];
                    float de = sqrtf(ex * ex + ey * ey + ez * ez);
                    fl.gain *= std::fmin(std::fmax(0.25f + 0.75f * (de - 0.8f) / 2.2f, 0.25f), 1.0f);
                }
                fl.reach = 2.0f;
                fl.spark = true;
                fl.r = 0.6f + 0.8f * col[0], fl.g = 0.5f + 0.7f * col[1], fl.b = 0.3f + 0.5f * col[2];
                fl.rise_ms = 0, fl.hold_ms = 33, fl.decay_ms = 40, fl.life_ms = 170; // owner: "fade out slower" // full for 2 frames, then a very quick fade
                add_world_light(fl, false);
                g_spark_stat[0]++;
            }
            Glow &g = g_glow[best];
            if (f - g.seen > 3) g.born = f; // a gap: it starts over (continuous presence counts)
            for (int k = 0; k < 3; k++) g.pos[k] += (p[k] - g.pos[k]) * 0.3f, g.col[k] = col[k];
            // owner: "its still flashing in some spot" - enemy rounds hitting by a steady glow (mission 0's window,
            // -11 1 57) added their sparks to it: its level jumped and its flicker made it a "fire" that lit the room on
            // and off. While a glow gets bursts after shots (and 1 s after; shot_f), its kind and level stay as they
            // were; the sparks have their own flashes. A new glow starts that way too (shot_f = its birth).
            // owner: "make every fire sprite have a strobing orange glint, in mission 0 only the first fire with the car
            // had a glint": in a firefight the fires are born "after a shot" and their own flicker keeps counting as
            // spark bursts, so they stayed sparky. A glow present for 2 s on end is a fire or a lamp, shots or not.
            const bool burning = f - g.born >= 120 && !g.follow && !at_effect; // not at a shooter (muzzle) or a blast
            if (f - g.shot_f > 60 || burning) {
                if (f - g.born > 0) g.flick += (std::fabs(level - g.level) - g.flick) * 0.05f; // how much it flickers (fires: a lot)
                if (g.flick > 0.008f) g.fire = true; // fire or halo, with hysteresis: it cannot flip from frame to frame
                else if (g.flick < 0.002f && !g.firetex) g.fire = false; // fire particles stay a fire
                g.level += (level - g.level) * 0.5f; // fires: the particle sum changes each frame - a flicker of its own
            } else g.level += (level - g.level) * (level < g.level ? 0.5f : 0.02f); // under fire: falls, rises slowly
            g.reach = reach;
            g.seen = f;
            g.lastw = a.w;
            g.basey += (a.lo[1] - g.basey) * (a.lo[1] < g.basey ? 0.3f : 0.02f); // the emitter: the lowest particle (slow to rise)
            if (g.sparky && ((f - g.shot_f > 60 && f - g.born >= 30) || burning)) g.sparky = false; // glows on: a fire
        }
    }
    uint32_t alive = 0;
    for (Glow &g : g_glow) {
        if (!g.used) continue;
        if (f - g.seen > 2) g.level *= 0.7f; // its sprites stopped: fade out
        if (f - g.seen > 30 || g.level < 0.01f) { g.used = false; continue; }
        alive++;
    }
    g_glow_stat[2] = alive;
}

// The wall gate of lamps and lasting glows (owner: "the fire that is far off the room is causing glint flash inside
// the room"). The wall check's share (rays from the light to points around the camera) is smoothed, then switched
// with hysteresis - on above 0.35, off below 0.15 - and the light fades in / out over ~0.3 s. Unlike v12b's direct
// multiply, the share's frame-to-frame jumps as the camera moves never reach the light.
static float wall_gate(float t, float &vis, bool &on, float &gate)
{
    if (t >= 0.0f) vis = vis < 0.0f ? t : vis + (t - vis) * 0.05f; // owner: window glints flickered (lamps outside the room): slower
    if (vis >= 0.0f) {
        if (vis > 0.35f) on = true;
        else if (vis < 0.15f) on = false;
    }
    gate += ((on ? 1.0f : 0.0f) - gate) * 0.04f;
    return gate;
}


// ---- The game's own effect emitters (v12r) ----
// Owner: "flame glint didnt work at all, can we identify every emitters on the level". BLACK's effects (fires, smoke,
// fireballs, sparks, muzzle flashes) are instances in one manager, *0x2D1990 + 0x68AF0 (FUN_001780b0 -> FUN_00176950,
// ESI). Spawns are queued by FUN_0015f180 (+0x15000, count +0x1A018) and made live by FUN_00176620; the live list is
// the first [+0x1A008] (u16) entries of the u16 index array at [+0x1A000]; instance = manager + index * 0x70: up to 8
// sub-emitter pointers at +0x00, its 4x4 placement at +0x20 (position +0x50), its effect definition at +0x60. The
// manager's +0x1A00C points back at the system (checked).
// A FIRE is a live emitter older than 1 s with orange particles rising above it (within 3 m sideways, 1 m below to 6 m
// above): it gets a steady, strobing orange light 4 m above its origin (owner: "4m above the point of origin").
struct Emitter { uint32_t inst, def, born, seen, obj; float pos[3], score, maxscore, vis, gate; bool used, on, fire, impact, flame; char name[16]; };
Emitter g_em[128];
uint32_t g_em_live, g_em_fires, g_em_attached;
bool g_em_ok;
// Effect instances live only a few seconds (mission 0: all under 5 s) - a burning fire is effects respawned at one spot.
// A fire SITE is a place where orange-rising emitters keep appearing: it burns from 0.5 s after it starts, and fades
// out over 2 s after the last one (the light: 4 m above it, strobing).
// owner: "make it 3x less big" (a ground pool ~5.3 m across), "bring it ... to 6m high", then "from 6m to 4.5m":
// 4.5 m above the flames' base with a reach of 7 m keeps that pool (sqrt(7^2 - 4.5^2) = 5.4 m)
constexpr float kFireReach = 7.0f, kFireUp = 4.5f;
struct FireSite { float pos[3], vis, gate, lit; uint32_t born, seen, id, hold, hits; bool used, on; };
FireSite g_fs[32];
uint32_t g_fs_ids;

// A level fire drawn this frame (env_fire_capture): the site within 1.5 m (3 m up / down) is refreshed - its origin the
// lowest point seen - or a new one starts; kept 30 s after its last draw: a fire is drawn only while on screen, and its glow on what is in view must not go
// out when the fire itself is behind the camera (2 s made sites expire and come back: #289 in one run).
void fire_site_from_draw(const float p[3])
{
    const uint32_t f = g_frame_count.load();
    FireSite *site = nullptr, *fr = nullptr;
    for (FireSite &x : g_fs) {
        if (x.used) {
            float dx = x.pos[0] - p[0], dy = x.pos[1] - p[1], dz = x.pos[2] - p[2];
            if (dx * dx + dz * dz < 2.25f && dy > -3.0f && dy < 3.0f) { site = &x; break; }
        } else if (!fr) fr = &x;
    }
    if (!site) {
        if (!fr) return;
        site = fr;
        *site = FireSite{};
        site->used = true, site->born = f, site->vis = -1.0f, site->id = ++g_fs_ids, site->hold = 1800, site->hits = 1;
        memcpy(site->pos, p, 12);
        static int logged;
        if (logged++ < 60) logf("[ppl] level fire at %.1f %.1f %.1f (site #%u, texture %p)", p[0], p[1], p[2], site->id, (const void *)g_st.textures[0]);
    }
    // owner: "theres glint all over the place except the flame emitters" - burning debris flying from an explosion
    // left a trail of sites (one every 50 ms along its path). A site lights once drawn in place 90 frames (hits >= 90,
    // is the light's test: hits counts frames here, 2 = lit); a moving fire's sites never get there
    if (site->hits < 99 && site->seen != f) site->hits++;
    site->seen = f;
    if (site->hold < 1800) site->hold = 1800;
    site->pos[0] += (p[0] - site->pos[0]) * 0.05f, site->pos[2] += (p[2] - site->pos[2]) * 0.05f;
    if (p[1] < site->pos[1]) site->pos[1] = p[1];
}

// Owner's screenshots: fires with no glint (a burning wreck, debris fires). No flame effect there: they are level
// FIRE HAZARDS - their flames are drawn elsewhere, and the game sets off a small, invisible kind-13 explosion at the
// fire every few seconds (its burn). Owner's log: 35 kind-13 blasts in 300 s at a few fixed spots (-78 1 25 x8,
// -87 0 40 x5, -84 -3 40 x5, -153 -2 31 x3). Two of them within 2 m (60 s apart at most: some repeat only every
// ~40 s) make a fire site, kept 75 s after the last. Returns true when the spot is a known fire (its blast flash is then left out: no pop each time).
bool fire_hazard_note(const float p[3])
{
    const uint32_t f = g_frame_count.load();
    FireSite *site = nullptr, *fr = nullptr;
    for (FireSite &x : g_fs) {
        if (x.used) {
            float dx = x.pos[0] - p[0], dy = x.pos[1] - p[1], dz = x.pos[2] - p[2];
            if (dx * dx + dz * dz < 4.0f && dy > -2.5f && dy < 2.5f) { site = &x; break; }
        } else if (!fr) fr = &x;
    }
    if (site) {
        if (site->hits < 99) site->hits = f - site->seen <= 3600 ? site->hits + 1 : 1; // some repeat only every ~40 s
        site->seen = f;
        if (site->hold < 4500) site->hold = 4500;
        return site->hits >= 2;
    }
    if (!fr) return false;
    *fr = FireSite{};
    fr->used = true, fr->born = f, fr->seen = f, fr->vis = -1.0f, fr->id = ++g_fs_ids, fr->hold = 4500, fr->hits = 1;
    memcpy(fr->pos, p, 12);
    return false;
}

// Once per definition: the readable strings in its first 0x300 bytes and in the first 64 bytes behind each pointer
// it holds - the effect's name (level data lists fxfire, fxfiresmoke, fxfirelitsmoke, fxpetrolexp, fxgasbang ...)
void name_effect_def(uint32_t def)
{
    static uint32_t seen[96];
    static int ns;
    for (int i = 0; i < ns; i++)
        if (seen[i] == def) return;
    if (ns >= 96) return;
    seen[ns++] = def;
    static uint8_t buf[0x300];
    if (!safe_read(buf, def, sizeof buf)) return;
    char out[1500];
    int w = snprintf(out, sizeof out, "[ppl] effect def %08x:", def);
    auto strings = [&](const uint8_t *b, int n, uint32_t base) {
        int run = 0;
        for (int i = 0; i <= n && w < 1400; i++) {
            const bool pr = i < n && b[i] >= 0x20 && b[i] < 0x7F;
            if (pr) { run++; continue; }
            if (run >= 4) w += snprintf(out + w, sizeof out - w, " [%x] \"%.*s\"", base + (i - run), run > 40 ? 40 : run, (const char *)b + i - run);
            run = 0;
        }
    };
    strings(buf, sizeof buf, 0);
    for (int o = 0; o + 4 <= (int)sizeof buf && w < 1400; o += 4) {
        uint32_t ptr;
        memcpy(&ptr, buf + o, 4);
        if (ptr < 0x80000000u || ptr >= 0x84000000u) continue;
        uint8_t tb[64];
        if (!safe_read(tb, ptr, sizeof tb)) continue;
        const int before = w;
        w += snprintf(out + w, sizeof out - w, " | +%x->", o);
        const int mark = w;
        strings(tb, sizeof tb, 0);
        if (w == mark) w = before; // nothing readable there
    }
    logf("%s", out);
}

void update_emitters(uint32_t f)
{
    if (!ppl_med()) { g_em_ok = false; return; } // LIGHTING LOW: no fires
    static uint32_t done;
    if (done == f) return;
    done = f;
    g_em_ok = false;
    const uint32_t sys = *(volatile uint32_t *)(uintptr_t)xbe_va(0x002D1990); // the effects system (FUN_001edc20 / FUN_001f1080 pass it to FUN_001780b0)
    if (sys < 0x80000000u || sys >= 0x84000000u) {
        static int logged;
        if (f % 120 == 0 && logged++ < 8) logf("[ppl] emitters: no system (*0x2D1990 = %08x)", sys);
        return;
    }
    const uint32_t mgr = sys + 0x68AF0u;
    uint32_t back = 0, arr = 0;
    uint16_t cnt = 0;
    bool rb = safe_read(&back, mgr + 0x1A00C, 4);
    bool rc = safe_read(&cnt, mgr + 0x1A008, 2);
    bool ra = safe_read(&arr, mgr + 0x1A000, 4);
    if (!rb || back != sys || !rc || cnt > 768 || !ra) {
        static int logged;
        if (f % 120 == 0 && logged++ < 8)
            logf("[ppl] emitters: manager check failed - system %08x, manager %08x, back %08x (%d), count %u (%d), index array %08x (%d)", sys, mgr, back,
                 rb, cnt, rc, arr, ra);
        return;
    }
    static uint16_t idx[768];
    if (cnt && !safe_read(idx, arr, cnt * 2u)) return;
    g_em_ok = true;
    g_em_live = cnt;
    auto take = [&](uint32_t inst, uint32_t obj) {
        uint32_t def = 0;
        float m[3];
        if (!safe_read(&def, inst + 0x60, 4) || !safe_read(m, inst + 0x50, 12)) return;
        if (obj >= 0x80000000u && obj < 0x84000000u) { // attached: it rides on its object (position +0xA0)
            float op[3];
            if (safe_read(op, obj + 0xA0, 12) && std::isfinite(op[0])) memcpy(m, op, 12);
        } else obj = 0;
        if (!(std::isfinite(m[0]) && std::isfinite(m[1]) && std::isfinite(m[2]))) return;
        Emitter *e = nullptr, *fr = nullptr;
        for (Emitter &x : g_em) {
            if (x.used && x.inst == inst && x.def == def) { e = &x; break; }
            if (!x.used && !fr) fr = &x;
        }
        if (!e) {
            if (!fr) return;
            e = fr;
            *e = Emitter{};
            e->obj = obj;
            e->used = true, e->inst = inst, e->def = def, e->born = f, e->vis = -1.0f;
            name_effect_def(def); // owner: "do we know the name of which emitter is activated for the flame"
            // the definition's name at +0 (IM_G_MET_M90 = an impact on metal, EX_G_RPG = an explosion, ...)
            if (!safe_read(e->name, def, 15)) e->name[0] = 0;
            e->name[15] = 0;
            for (char &ch : e->name)
                if (ch && (ch < 0x20 || ch >= 0x7F)) ch = 0;
            // owner: "the flame effect seems to spawn near where the bullet lands": IM_* = bullet impacts, never fires
            e->impact = (e->name[0] == 'I' && e->name[1] == 'M' && e->name[2] == '_') ||
                        (e->name[0] == 'M' && e->name[1] == 'Z' && e->name[2] == '_'); // MZ_* = muzzle flashes (attached to shooters)
            // FL_* = FLAMES (owner's run: FL_W_CAR_M01 on each burning car, up to 249 s; FL_W_FIR_M05, a fire, 56 s)
            e->flame = e->name[0] == 'F' && e->name[1] == 'L' && e->name[2] == '_';
        }
        e->seen = f;
        memcpy(e->pos, m, 12);
    };
    for (uint32_t i = 0; i < cnt; i++)
        if (idx[i] < 768) take(mgr + idx[i] * 0x70u, 0);
    // The ATTACHED effects (owner: no car fire was in the list above): FUN_00169960 (the explosion's call when it
    // belongs to an object) queues them on a second manager, system + 0x65690, updated by FUN_00176bd0: live list =
    // the first [+0x3448] (u16) entries of the u16 index array at [+0x3440]; instance = A + index * 0x70 (as above),
    // its object at A + 0x2300 + index * 4 (position +0xA0), the attach mode at A + 0x2440 + index * 4.
    {
        const uint32_t A = sys + 0x65690u;
        uint16_t acnt = 0;
        uint32_t aarr = 0;
        static uint16_t aidx[80];
        if (safe_read(&acnt, A + 0x3448, 2) && acnt <= 80 && safe_read(&aarr, A + 0x3440, 4) && aarr >= 0x80000000u && aarr < 0x84000000u &&
            (!acnt || safe_read(aidx, aarr, acnt * 2u))) {
            g_em_attached = acnt;
            for (uint32_t i = 0; i < acnt; i++) {
                if (aidx[i] >= 80) continue;
                uint32_t obj = 0;
                safe_read(&obj, A + 0x2300 + aidx[i] * 4u, 4);
                take(A + aidx[i] * 0x70u, obj);
            }
        }
    }
    // what rose above each one last frame
    const int b = (f - 1) & 1;
    const bool have = g_fq_frame[b] == f - 1;
    uint32_t fires = 0;
    for (Emitter &e : g_em) {
        if (!e.used) continue;
        if (e.seen != f) { e.used = false; continue; }
        int nq = 0;
        if (have)
            for (int q = 0; q < g_fq_n[b]; q++) {
                const float *p = g_fq[b][q];
                float dx = p[0] - e.pos[0], dy = p[1] - e.pos[1], dz = p[2] - e.pos[2];
                if (dx * dx + dz * dz < 9.0f && dy > -1.0f && dy < 6.0f) nq++;
            }
        e.score += ((nq > 8 ? 8 : nq) / 8.0f - e.score) * 0.1f;
        e.maxscore = std::fmax(e.maxscore, e.score);
        if (f - e.born >= 60 && e.score > 0.15f) e.fire = true;
        else if (e.score < 0.05f) e.fire = false;
        if (e.fire) fires++;
        // a fire site: join the one within 3 m (6 m up / down), or start one. Only emitters 1 s old or more: the
        // impact effect (81f03a80 on mission 0: orange, never past 1.2 s) must not start sites where the player shoots
        // not at the player (the gun's own effects: mission 0 made a "fire" where the player stood) or at a shooter
        const float cx = e.pos[0] - g_vp_eye[0], cy = e.pos[1] - g_vp_eye[1], cz = e.pos[2] - g_vp_eye[2];
        const bool at_player = cx * cx + cy * cy + cz * cz < 9.0f;
        // owner: "the flame effect seems to spawn near where the bullet lands" - impact effects live up to ~4 s; the
        // owner's run: the car fires' instances lived ~40 s. A fire: alive 4 s or more, orange above it at some point.
        // a flame effect (FL_*) is a fire from its first 1/4 s; others need 4 s and orange above them
        // owner: "the glint show after almost every single explosion while it should appears at each flame emitters":
        // only FL_* (flame) effects make fire sites now - the orange / 4 s rule also took explosion effects (EX_G_GRD ...)
        if (e.flame && f - e.born >= 15 && !at_player) {
            FireSite *site = nullptr, *fr = nullptr;
            for (FireSite &x : g_fs) {
                if (x.used) {
                    float dx = x.pos[0] - e.pos[0], dz = x.pos[2] - e.pos[2], dy = x.pos[1] - e.pos[1];
                    // one site per flame emitter (owner: "at each flame emitters"): within 1 m only (was 3 m)
                    if (dx * dx + dz * dz < 1.0f && dy > -2.0f && dy < 2.0f) { site = &x; break; }
                } else if (!fr) fr = &x;
            }
            if (!site && fr) {
                site = fr;
                *site = FireSite{};
                site->used = true, site->born = f, site->vis = -1.0f, site->id = ++g_fs_ids, site->hold = 120, site->hits = 99;
                memcpy(site->pos, e.pos, 12);
            }
            if (site) {
                site->seen = f;
                for (int k = 0; k < 3; k++) site->pos[k] += (e.pos[k] - site->pos[k]) * 0.05f;
                if (e.pos[1] < site->pos[1]) site->pos[1] = e.pos[1]; // the origin: the lowest
            }
        }
    }
    for (FireSite &x : g_fs)
        if (x.used && f - x.seen > x.hold) x.used = false;
    g_em_fires = fires;
    static uint32_t last, lines;
    if (f - last >= 300 && lines < 200) { // census every 5 s
        last = f, lines++;
        char buf[1400];
        int w = snprintf(buf, sizeof buf, "[ppl] emitters: %u live + %u attached, %u fires |", g_em_live, g_em_attached, g_em_fires);
        int shown = 0;
        for (const Emitter &e : g_em)
            if (e.used && w < 1300 && shown < 14) {
                shown++;
                w += snprintf(buf + w, sizeof buf - w, " %s%s age %.1fs at %.1f %.1f %.1f orange %.2f%s |", e.name[0] ? e.name : "?",
                              e.obj ? " (on object)" : "", (f - e.born) / 60.0f, e.pos[0], e.pos[1], e.pos[2], e.score, e.fire ? " FIRE" : "");
            }
        logf("%s", buf);
        {   // the particle systems behind the effects (each instance: up to 8 at +0x00): pointer, its first 0x50 bytes,
            // and the name behind its definition pointer (+0x40) - to find the pool level fires live in
            static int dumps;
            for (const Emitter &e : g_em) {
                if (!e.used || dumps >= 40) continue;
                uint32_t sub[8];
                if (!safe_read(sub, e.inst, 32)) continue;
                for (int k = 0; k < 8 && dumps < 40; k++) {
                    if (sub[k] < 0x80000000u || sub[k] >= 0x84000000u) continue;
                    uint32_t q[20] = {};
                    if (!safe_read(q, sub[k], sizeof q)) continue;
                    char nm[17] = {};
                    if (q[16] >= 0x80000000u && q[16] < 0x84000000u) safe_read(nm, q[16], 16);
                    for (char &ch : nm)
                        if (ch && (ch < 0x20 || ch >= 0x7F)) ch = '.';
                    char hx[400];
                    int hw = 0;
                    for (int i = 0; i < 20; i++) hw += snprintf(hx + hw, sizeof hx - hw, "%08x ", q[i]);
                    logf("[ppl] particle system %08x (effect %s, slot %d): def name \"%s\" | %s", sub[k], e.name, k, nm, hx);
                    dumps++;
                }
            }
        }
        w = snprintf(buf, sizeof buf, "[ppl] attached effects:");
        for (const Emitter &e : g_em)
            if (e.used && e.obj && w < 1300)
                w += snprintf(buf + w, sizeof buf - w, " | %s on %08x age %.1fs at %.1f %.1f %.1f orange %.2f (max %.2f)%s", e.name[0] ? e.name : "?", e.obj,
                              (f - e.born) / 60.0f, e.pos[0], e.pos[1], e.pos[2], e.score, e.maxscore, e.fire ? " FIRE" : "");
        logf("%s", buf);
        // per definition: live count, oldest, mean orange score; fire sites
        struct DefSum { uint32_t def, n; float age, orange; const char *name; };
        DefSum ds[24];
        int nd = 0;
        for (const Emitter &e : g_em) {
            if (!e.used) continue;
            int j = 0;
            while (j < nd && ds[j].def != e.def) j++;
            if (j == nd) {
                if (nd == 24) continue;
                ds[nd++] = {e.def, 0, 0, 0, e.name};
            }
            ds[j].n++, ds[j].orange += e.score;
            ds[j].age = std::fmax(ds[j].age, (f - e.born) / 60.0f);
        }
        w = snprintf(buf, sizeof buf, "[ppl] emitter kinds:");
        for (int j = 0; j < nd && w < 1300; j++)
            w += snprintf(buf + w, sizeof buf - w, " | %s x%u oldest %.1fs orange %.2f", ds[j].name[0] ? ds[j].name : "?", ds[j].n, ds[j].age, ds[j].orange / ds[j].n);
        logf("%s", buf);
        w = snprintf(buf, sizeof buf, "[ppl] fire sites:");
        for (const FireSite &x : g_fs)
            if (x.used && w < 1300)
                w += snprintf(buf + w, sizeof buf - w, " | #%u at %.1f %.1f %.1f, %.1fs, last %.1fs ago", x.id, x.pos[0], x.pos[1], x.pos[2],
                              (f - x.born) / 60.0f, (f - x.seen) / 60.0f);
        logf("%s", buf);
    }
}

void frame_world_lights()
{
    static uint32_t frame = ~0u;
    uint32_t f = g_frame_count.load();
    if (f == frame) return;
    frame = f;
    double now = now_ms();
    collect_world_lights(now);
    // owner: "still delayed sparks": the glows (and their spark flashes) were updated after the world lights were
    // listed, so a flash missed its first frame and showed one frame later, already faded to ~30 %. First the glows.
    g_glow_now = now;
    update_glows(f);
    update_emitters(f);
    struct Cand { ViewLight v; float score; } c[192]; // 32 world lights + 24 glows + 128 lamps;
    int n = 0, live = 0, behind = 0, unplaced = 0;
    static const bool walls = !(getenv("BLACK_PPL_WALLS") && getenv("BLACK_PPL_WALLS")[0] == '0');
    int budget = 3;
    for (int i = 0; i < 32; i++) {
        WorldLight &l = g_wl[i];
        float lv = world_light_level(l, now);
        if (lv <= 0.01f) continue;
        live++;
        float v[3];
        if (!world_to_view(l.pos, v)) { unplaced++; continue; }
        float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (d2 > (l.reach + 60.0f) * (l.reach + 60.0f)) { behind++; continue; } // far beyond its reach from the eye
        if (walls && !l.spark) {
            float t = vis_for(l.pos, f, budget);
            if (t >= 0.0f) l.vis = l.vis < 0.0f ? t : l.vis + (t - l.vis) * 0.35f;
            if (l.vis <= 0.02f) continue; // behind a wall, or not tested yet
            lv *= l.vis;
        }
        Cand &k = c[n++];
        memcpy(k.v.pos, v, 12);
        memcpy(k.v.wpos, l.pos, 12);
        k.v.reach = l.reach;
        k.v.col[0] = l.r * lv, k.v.col[1] = l.g * lv, k.v.col[2] = l.b * lv;
        k.v.steady = false; // explosions and gunfire: no fade-in (a flash must be at full strength at once)
        k.v.spec_only = false;
        k.v.spark = l.spark;
        k.score = lv * l.reach * l.reach / (d2 > 1.0f ? d2 : 1.0f);
        // owner: "sometimes sparks doesnt produce light": with 1.5 m they lost the transient slot to the enemies'
        // muzzle flashes (2500 rounds by others in that playtest); the impact the player looks at comes first
        if (l.spark) k.score *= 4.0f;
    }
    // the game's glows (lamp halos, fires) as lights too (updated above)
    static const float glow_gain = env_f("BLACK_PPL_GLOW_GAIN", 1.0f);
    for (Glow &g : g_glow) {
        if (!g.used || g.level < 0.02f || n >= 192) continue;
        // owner playtest 6: "the ricochet sparks are too big and last too long": a glow lights only after half a
        // second of continuous presence - fires and lamps; spark bursts (and the muzzle / explosion sprites, which
        // have lights of their own) never get there
        if (f - g.born < (g.firetex ? 10u : 30u) || g.sparky || g.follow) continue;
        if (g.fire && g_em_ok) continue; // the emitter fires below replace the particle-cluster guess
        live++;
        // owner: "put the source of the light glint like 4m above the point of origin of the flame emitter": a fire's
        // light hangs 4 m above its emitter (the cluster's lowest particle), with a reach of at least 9 m
        float lp[3] = {g.pos[0], g.pos[1], g.pos[2]}, greach = g.reach;
        if (g.fire) lp[1] = g.basey + 4.0f, greach = std::fmax(greach, 9.0f);
        float v[3];
        if (!world_to_view(lp, v)) { unplaced++; continue; }
        float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (d2 > (greach + 40.0f) * (greach + 40.0f)) { behind++; continue; }
        float lv = g.level * glow_gain;
        if (walls) {
            lv *= wall_gate(vis_for(lp, f, budget), g.vis, g.on, g.gate);
            if (lv <= 0.01f) continue;
        }
        // owner: "the light source should stay on at all times to prevent diming and flashing bugs": lamps and lasting
        // glows are not dimmed by the wall check (its share moved with the camera); the shadow cube still darkens
        // what the strongest light cannot see
        Cand &k = c[n++];
        memcpy(k.v.pos, v, 12);
        memcpy(k.v.wpos, lp, 12);
        k.v.reach = greach;
        // the sprite's hue, warmed (the textures are mostly fire / sodium orange) and scaled by its brightness
        k.v.col[0] = (0.6f * g.col[0] + 0.4f * 1.0f) * 1.3f * lv;
        k.v.col[1] = (0.6f * g.col[1] + 0.4f * 0.7f) * 1.3f * lv;
        k.v.col[2] = (0.6f * g.col[2] + 0.4f * 0.4f) * 1.3f * lv;
        if (g.fire) { // a strobing orange glint: x 0.6-1.4 at random each frame (owner: "a strobing orange glint")
            uint32_t hsh = (f * 2654435761u) ^ ((uint32_t)(&g - g_glow) * 40503u);
            hsh ^= hsh >> 13, hsh *= 0x5bd1e995u, hsh ^= hsh >> 15;
            const float strobe = 0.6f + 0.8f * (float)(hsh & 0xFFFF) / 65535.0f;
            for (int q = 0; q < 3; q++) k.v.col[q] *= strobe;
        }
        k.score = lv * greach * greach / (d2 > 1.0f ? d2 : 1.0f);
        k.v.steady = true;
        k.v.spec_only = !g.fire; // a steady halo: reflections only; a fire (flickers): light
        k.v.spark = false;
    }
    // the game's fire sites (update_emitters): a strobing orange light 4 m above each origin, fading out over 2 s
    for (FireSite &e : g_fs) e.lit = 0.0f;
    for (FireSite &e : g_fs) {
        if (!e.used || f - e.born < 30 || e.hits < 2 || (e.hold == 1800 && e.hits < 90) || n >= 192) continue;
        live++;
        float lp[3] = {e.pos[0], e.pos[1] + kFireUp, e.pos[2]};
        // owner: "the flame glint can only be seen when very very close, we need to see them at a good distance":
        // reach 16 m (was 9), 1.6x brighter, ranked 2x for a slot, tested out to 100 m (the far ones light through the
        // lamp map below, with the same gate and pulse)
        const float reach = kFireReach;
        float v[3];
        if (!world_to_view(lp, v)) { unplaced++; continue; }
        float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (d2 > 100.0f * 100.0f) { behind++; continue; }
        // full, then fades from 0.5 s after the last effect (signed: f - seen - 30 in unsigned wrapped round while the
        // fire was being refreshed, made the level negative and dropped the light - the owner: "can only be seen very
        // very close and fade out")
        const int since = (int)(f - e.seen);
        float lv = std::fmin(1.0f, 1.0f - (float)(since - ((int)e.hold - 90)) / 90.0f) * glow_gain * 1.6f;
        if (walls) {
            lv *= wall_gate(vis_for(lp, f, budget), e.vis, e.on, e.gate);
            if (lv <= 0.01f) continue;
        }
        // owner: "the glint to stay on and have a slow pulse with brightness": 80-100 %, one cycle in ~1.8 s, each
        // site a little out of step with the others
        const float strobe = 0.9f + 0.1f * sinf((float)f / 60.0f * 6.2831853f / 1.8f + (float)e.id * 1.7f);
        Cand &k = c[n++];
        memcpy(k.v.pos, v, 12);
        memcpy(k.v.wpos, lp, 12);
        k.v.reach = reach;
        k.v.col[0] = 1.3f * lv * strobe, k.v.col[1] = 0.7f * lv * strobe, k.v.col[2] = 0.25f * lv * strobe;
        e.lit = lv * strobe; // for the lamp map
        k.score = 2.0f * lv * reach * reach / (d2 > 1.0f ? d2 : 1.0f);
        k.v.steady = true, k.v.spec_only = false, k.v.spark = false;
    }
    // lamps found in the light cones: steady lights
    for (Lamp &L : g_lamp) {
        if (!L.used) continue;
        if (f - L.seen > 3600) { L.used = false; continue; } // its cone not drawn for 60 s (a new level)
        // owner: "windows still flickers" - mission 0's cone "lamps" are the sun shafts through the windows, so the
        // lamp sat on the glass and its reflection slid across the pane as the camera moved. Off unless BLACK_PPL_LAMPS=1.
        if (!lamps_on()) continue;
        if (n >= 192) continue;
        live++;
        float v[3];
        if (!world_to_view(L.pos, v)) { unplaced++; continue; }
        float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        const float reach = L.reach;
        if (d2 > (reach + 40.0f) * (reach + 40.0f)) { behind++; continue; }
        float lv = L.level * glow_gain;
        if (walls) {
            lv *= wall_gate(vis_for(L.pos, f, budget), L.vis, L.on, L.gate);
            if (lv <= 0.01f) continue;
        }
        Cand &k = c[n++];
        memcpy(k.v.pos, v, 12);
        memcpy(k.v.wpos, L.pos, 12);
        k.v.reach = reach;
        for (int q = 0; q < 3; q++) k.v.col[q] = L.col[q] * 1.1f * lv;
        k.score = lv * reach * reach / (d2 > 1.0f ? d2 : 1.0f);
        k.v.steady = true;
        k.v.spec_only = !lamps_on(); // reflections only (BLACK_PPL_LAMPS=1: full light)
        k.v.spark = false;
    }
    // slot 1 (the shadowed light, g_vl[0]) keeps the same light while it lives (within 0.5 m: a shooter's next
    // round too), unless another is 3x stronger: swapping the shadowed light frame to frame flickered
    // the picks of the last frame: a light picked then keeps a 1.6x bonus (close calls do not swap every frame), and a
    // steady light that was not picked fades in over ~8 frames instead of appearing at once
    static float prev_w[12][3], prev_fade[12];
    static int prev_n;
    auto prev_of = [&](const float *w) {
        for (int q = 0; q < prev_n; q++) {
            float dx = prev_w[q][0] - w[0], dy = prev_w[q][1] - w[1], dz = prev_w[q][2] - w[2];
            if (dx * dx + dy * dy + dz * dz < 0.25f) return q;
        }
        return -1;
    };
    for (int i = 0; i < n; i++)
        if (prev_of(c[i].v.wpos) >= 0) c[i].score *= c[i].v.steady ? 3.0f : 1.6f; // steady lights: 3x (window glints swapped as the camera moved)
    static float keep_w[3];
    static bool keep_ok;
    g_vl_n = 0;
    if (keep_ok && n > 0) {
        int best = 0, kept = -1;
        for (int i = 0; i < n; i++) {
            if (c[i].score > c[best].score) best = i;
            float dx = c[i].v.wpos[0] - keep_w[0], dy = c[i].v.wpos[1] - keep_w[1], dz = c[i].v.wpos[2] - keep_w[2];
            if (dx * dx + dy * dy + dz * dz < 0.25f && (kept < 0 || c[i].score > c[kept].score)) kept = i;
        }
        if (kept >= 0 && c[best].score < 3.0f * c[kept].score) {
            g_vl[g_vl_n++] = c[kept].v;
            c[kept] = c[--n];
        }
    }
    // two slots for steady lights (lamps, fires) and one for transient ones (gunfire, explosions, sparks): a burst
    // of gunfire cannot push a lamp out (it went dark and came back); a group uses the other's slots it does not need
    {
        int st_tot = 0, tr_tot = 0, st = 0, tr = 0;
        for (int i = 0; i < g_vl_n; i++) (g_vl[i].steady ? st_tot : tr_tot)++, (g_vl[i].steady ? st : tr)++;
        for (int i = 0; i < n; i++) (c[i].v.steady ? st_tot : tr_tot)++;
        // 5 slots: 3 held for steady lights, 2 for transient ones (level 2: 4, the head lamp takes one)
        // shader model 3: 12 slots, 6 held for steady lights, 4 for transient ones (the rest to either)
        const int slots = ppl_max_slots();
        const int tr_keep = slots > 5 ? 4 : 2, st_keep = slots > 5 ? 6 : 3;
        const int st_max = slots - (tr_tot < tr_keep ? tr_tot : tr_keep), tr_max = slots - (st_tot < st_keep ? st_tot : st_keep);
        while (g_vl_n < slots && n > 0) {
            int best = -1;
            for (int i = 0; i < n; i++) {
                if (c[i].v.steady ? st >= st_max : tr >= tr_max) continue;
                if (best < 0 || c[i].score > c[best].score) best = i;
            }
            if (best < 0) break;
            (c[best].v.steady ? st : tr)++;
            g_vl[g_vl_n++] = c[best].v;
            c[best] = c[--n];
        }
    }
    for (int i = 0; i < g_vl_n; i++)
        if (g_vl[i].spark) { g_spark_stat[1]++; break; }
    {   // steady picks that changed since the last frame (window glints flickering: log per second)
        static float last_w[12][3];
        static int last_n;
        int changed = 0;
        for (int i = 0; i < g_vl_n; i++) {
            if (!g_vl[i].steady) continue;
            bool was = false;
            for (int q = 0; q < last_n; q++) {
                float dx = last_w[q][0] - g_vl[i].wpos[0], dy = last_w[q][1] - g_vl[i].wpos[1], dz = last_w[q][2] - g_vl[i].wpos[2];
                if (dx * dx + dy * dy + dz * dz < 0.25f) was = true;
            }
            if (!was) changed++;
        }
        last_n = 0;
        for (int i = 0; i < g_vl_n; i++)
            if (g_vl[i].steady) memcpy(last_w[last_n++], g_vl[i].wpos, 12);
        g_steady_swaps += changed;
    }
    keep_ok = g_vl_n > 0;
    if (keep_ok) memcpy(keep_w, g_vl[0].wpos, 12);
    float new_fade[12];
    for (int i = 0; i < g_vl_n; i++) {
        int q = prev_of(g_vl[i].wpos);
        float fd = g_vl[i].steady ? std::fmin((q >= 0 ? prev_fade[q] : 0.0f) + 0.125f, 1.0f) : 1.0f;
        for (int k = 0; k < 3; k++) g_vl[i].col[k] *= fd;
        new_fade[i] = fd;
    }
    for (int i = 0; i < g_vl_n; i++) memcpy(prev_w[i], g_vl[i].wpos, 12), prev_fade[i] = new_fade[i];
    prev_n = g_vl_n;
    // the lamp map (v12d): all lamps and lasting glows, near or far, minus the share a slot gives them
    static const bool map_on = !(getenv("BLACK_PPL_LAMPMAP") && getenv("BLACK_PPL_LAMPMAP")[0] == '0');
    if (map_on && g_vp_ok && ppl_med()) {
        static float acc[kMap * kMap][5];
        memset(acc, 0, sizeof acc);
        const float half = kMap * kCell * 0.5f;
        const float ox = floorf((g_vp_eye[0] - half) / kCell) * kCell, oz = floorf((g_vp_eye[2] - half) / kCell) * kCell;
        uint32_t painted = 0;
        auto splat = [&](const float *p, float r, const float *col) {
            float wgt = 1.0f;
            for (int i = 0; i < g_vl_n; i++) {
                if (!g_vl[i].steady) continue;
                float dx = g_vl[i].wpos[0] - p[0], dy = g_vl[i].wpos[1] - p[1], dz = g_vl[i].wpos[2] - p[2];
                if (dx * dx + dy * dy + dz * dz < 0.25f) wgt = 1.0f - new_fade[i];
            }
            if (wgt <= 0.01f || r <= 0.5f) return;
            painted++;
            int i0 = (int)floorf((p[0] - r - ox) / kCell), i1 = (int)ceilf((p[0] + r - ox) / kCell);
            int j0 = (int)floorf((p[2] - r - oz) / kCell), j1 = (int)ceilf((p[2] + r - oz) / kCell);
            if (i0 < 0) i0 = 0;
            if (j0 < 0) j0 = 0;
            if (i1 > kMap - 1) i1 = kMap - 1;
            if (j1 > kMap - 1) j1 = kMap - 1;
            const float r2 = r * r;
            for (int j = j0; j <= j1; j++)
                for (int i = i0; i <= i1; i++) {
                    float dx = ox + (i + 0.5f) * kCell - p[0], dz = oz + (j + 0.5f) * kCell - p[2];
                    float d2 = dx * dx + dz * dz;
                    if (d2 >= r2) continue;
                    float a = 1.0f - d2 / r2;
                    a *= a * wgt;
                    float *t = acc[j * kMap + i];
                    // owner: "a light source gets duplicated" (a wall burnt white): one light mesh gave 7 white lamps
                    // over 10 m and their pools added up; overlapping pools take the brightest, not the sum
                    t[0] = std::fmax(t[0], col[0] * a), t[1] = std::fmax(t[1], col[1] * a), t[2] = std::fmax(t[2], col[2] * a);
                    t[3] += p[1] * a, t[4] += a;
                }
        };
        for (Glow &g : g_glow) {
            if (!g.used || g.level < 0.02f || f - g.born < (g.firetex ? 10u : 30u) || g.sparky || g.follow || !g.fire) continue;
            float lv = g.level * glow_gain, col[3];
            col[0] = (0.6f * g.col[0] + 0.4f * 1.0f) * 1.3f * lv;
            col[1] = (0.6f * g.col[1] + 0.4f * 0.7f) * 1.3f * lv;
            col[2] = (0.6f * g.col[2] + 0.4f * 0.4f) * 1.3f * lv;
            if (walls) for (float &q : col) q *= g.gate; // the wall gate (frame_world_lights): no fire through walls
            splat(g.pos, g.reach, col);
        }
        for (FireSite &x : g_fs) { // the fire sites, far ones too (their level, gate and pulse from the slot pass: lit)
            if (!x.used || f - x.born < 30 || x.hits < 2 || (x.hold == 1800 && x.hits < 90) || x.lit <= 0.01f) continue;
            float lp[3] = {x.pos[0], x.pos[1] + kFireUp, x.pos[2]}, col[3] = {1.3f * x.lit, 0.7f * x.lit, 0.25f * x.lit};
            splat(lp, kFireReach, col);
        }
        for (Lamp &L : g_lamp) {
            if (!L.used || !lamps_on()) continue;
            float lv = L.level * glow_gain, col[3] = {L.col[0] * 1.1f * lv, L.col[1] * 1.1f * lv, L.col[2] * 1.1f * lv};
            splat(L.pos, L.reach, col);
        }
        static const float gain = env_f("BLACK_PPL_GAIN", 1.0f);
        const float camy = g_vp_eye[1];
        auto byte = [](float v) { return (uint32_t)(v <= 0.0f ? 0.0f : v >= 1.0f ? 255.0f : v * 255.0f + 0.5f); };
        for (int t = 0; t < kMap * kMap; t++) {
            const float *a = acc[t];
            if (a[4] <= 0.0f) { g_map_px[t] = 0x80000000u; continue; }
            float hn = (a[3] / a[4] - camy + 64.0f) / 128.0f; // the lights' mean height around the camera's (+-64 m)
            g_map_px[t] = byte(hn) << 24 | byte(a[0] * gain * 0.25f) << 16 | byte(a[1] * gain * 0.25f) << 8 | byte(a[2] * gain * 0.25f);
        }
        g_map_o[0] = ox, g_map_o[1] = oz, g_map_o[2] = camy;
        g_map_frame = f ? f : 1;
        g_map_lights = painted;
        if (!painted) g_map_frame = 0; // nothing to show: no texture update, nothing bound
    }
    // once a second while anything is lit: what the world lights are doing (playtest diagnostics)
    static uint32_t last_log, lines;
    if (live && f - last_log >= 60 && lines < 400) {
        last_log = f;
        lines++;
        char b[160] = "";
        if (g_vl_n) snprintf(b, sizeof b, " | top at view %+.1f %+.1f %+.1f (%.1f m), reach %.0f, level %.2f", g_vl[0].pos[0], g_vl[0].pos[1], g_vl[0].pos[2],
                             sqrtf(g_vl[0].pos[0] * g_vl[0].pos[0] + g_vl[0].pos[1] * g_vl[0].pos[1] + g_vl[0].pos[2] * g_vl[0].pos[2]), g_vl[0].reach, g_vl[0].col[0] / 1.6f);
        logf("[ppl] world lights frame %u: %d live, %d used, %d too far, %d unplaced (view rows %u frames old) | rounds by others %u, explosions %u | wall tests %u, %u blocked%s",
             f, live, g_vl_n, behind, unplaced, f - g_vp_frame, (unsigned)g_ppl_enemy_shot_n, (unsigned)g_ex_queue_n, g_vis_tests, g_vis_blocked, b);
        logf("[ppl] glows: %u sprites caught so far, %u glows made, %u alive now | lamps: %u cone meshes, %u lamps | lamp map: %u lights", g_glow_stat[0], g_glow_stat[1],
             g_glow_stat[2], g_lamp_stat[0], g_lamp_stat[1], g_map_lights);
        logf("[ppl] sparks: player rounds %u, spark flashes made %u, frames a spark held a slot %u | steady lights newly picked %u",
             (unsigned)g_ppl_player_shots, g_spark_stat[0], g_spark_stat[1], g_steady_swaps);
        {   // the steady picks now (kind, world position) - for the window glints
            char sb[400];
            int sw = snprintf(sb, sizeof sb, "[ppl] steady picks:");
            for (int i = 0; i < g_vl_n && sw < 340; i++)
                if (g_vl[i].steady)
                    sw += snprintf(sb + sw, sizeof sb - sw, " | %s at %.1f %.1f %.1f (%.1f m)", g_vl[i].spec_only ? "reflect" : "light", g_vl[i].wpos[0],
                                   g_vl[i].wpos[1], g_vl[i].wpos[2], sqrtf(g_vl[i].pos[0] * g_vl[i].pos[0] + g_vl[i].pos[1] * g_vl[i].pos[1] + g_vl[i].pos[2] * g_vl[i].pos[2]));
            logf("%s", sb);
        }
        // lasting glows and their flicker (fires light, lamp halos do not: kFireFlicker)
        char gb[1000] = "";
        int gw = 0;
        for (const Glow &g : g_glow)
            if (g.used && f - g.born >= 30 && gw < 900)
                gw += snprintf(gb + gw, sizeof gb - gw, " | %.0f %.0f %.0f lv %.2f flick %.4f hue %.2f %.2f %.2f%s%s", g.pos[0], g.pos[1], g.pos[2], g.level, g.flick, g.col[0], g.col[1], g.col[2],
                               g.sparky ? " sparky" : "", g.fire && !g.sparky ? (g.firetex ? " FIRE(blast)" : " FIRE") : "");
        if (gw) logf("[ppl] lasting glows%s", gb);
    }
}

// BLACK_PPL_PROBE=1: once every 600 frames, a census of one frame's draws by vertex layout, texture modes, blend
// and the selection's verdict (which surfaces the light pass takes, and why the others are skipped).
struct Census {
    uint32_t frame = 0;
    std::unordered_map<std::string, uint32_t> rows;
};
Census g_census;

void census(const VertexShader *vs, const char *verdict)
{
    static const bool on = getenv("BLACK_PPL_PROBE") && *getenv("BLACK_PPL_PROBE") == '1';
    if (!on) return;
    uint32_t f = g_frame_count.load();
    if (f != g_census.frame) {
        if (!g_census.rows.empty()) {
            logf("[ppl] census frame %u (%zu kinds):", g_census.frame, g_census.rows.size());
            for (auto &r : g_census.rows) logf("[ppl]   %4u x %s", r.second, r.first.c_str());
        }
        g_census.rows.clear();
        g_census.frame = f;
    }
    if (f % 600 != 300) return;
    const uint32_t *rs = render_state();
    char b[256];
    int w = 0;
    if (vs)
        for (uint32_t i = 0; i < vs->num_elements && w < 150; i++)
            w += snprintf(b + w, sizeof b - w, "v%u:%02x ", vs->elements[i].reg, vs->elements[i].xtype);
    else
        w += snprintf(b + w, sizeof b - w, "fvf/none ");
    // the model's origin from the camera (view space: row .w terms), to tell e.g. the partner's clone (a few m) from
    // the enemies; 5 m steps beyond 10 m
    const float (*c)[4] = g_st.vs_constants;
    float ox = c[10][3] * g_inv_p00, oy = c[11][3] * g_inv_p11, oz = c[13][3];
    int dist = (int)sqrtf(ox * ox + oy * oy + oz * oz);
    if (dist > 10) dist = dist / 5 * 5;
    snprintf(b + w, sizeof b - w, "| tex 0x%x blend %u (%x/%x) zw %u vm %u @%dm | %s", rs[XRS_PSTEXTUREMODES], rs[XRS_ALPHABLENDENABLE],
             rs[XRS_SRCBLEND], rs[XRS_DESTBLEND], rs[XRS_ZWRITEENABLE], g_fov_viewmodel, dist, verdict);
    g_census.rows[b]++;
}

} // namespace

void ppl_cancel() { g_draw_on = g_draw_gun = false; }

bool ppl_begin_draw(const VertexShader *vs, bool has_ps)
{
    g_draw_on = false;
    g_draw_gun = false;
    if (!ppl_level()) return false;
    if (!vs) { census(vs, "no vertex program"); return false; }
    if (!has_ps) { census(vs, "no pixel shader (fixed-function pixels)"); return false; }
    bool world = ppl_world_vs(vs), gun = g_fov_viewmodel != 0;
    if (world) capture_projection();
    // Any textured 3D draw: lightmapped level geometry, vertex-coloured ground (the cobbled quays: v0 v3 v9),
    // props and characters (v0 v2 v9, skinned too), multi-layer blends. 2D/screen draws use FLOAT2 positions.
    bool pos3 = false, uv = false;
    for (uint32_t i = 0; i < vs->num_elements; i++) {
        const VertexElement &e = vs->elements[i];
        if (e.reg == 0 && (e.xtype == 0x32 || e.xtype == 0x42)) pos3 = true;
        if (e.reg == 9) uv = true;
    }
    if (!(pos3 && uv) || !g_inv_p00) { census(vs, "not a textured 3D layout"); return false; }
    const uint32_t *rs = render_state();
    // Blended: a surface layer (plain alpha blend, writes depth: the cobbled ground's top layer over its lit base)
    // is lit like the base. So is a plain alpha-blended mesh WITH NORMALS even without depth writes: characters drawn
    // in the game's fade mode - the partner's clone among them (owner: "the lighting doesn't work on the clone's
    // model"; its v0 v9 v2 parts came blended, depth write off). Effects stay unlit: additive glows and light shafts,
    // and particles / sprites (no normals).
    bool normals = false;
    for (uint32_t i = 0; i < vs->num_elements; i++) normals |= vs->elements[i].reg == 2;
    bool plain_alpha = rs[XRS_SRCBLEND] == 0x302 && rs[XRS_DESTBLEND] == 0x303;
    bool layer = plain_alpha && (rs[XRS_ZWRITEENABLE] || normals);
    if (rs[XRS_ALPHABLENDENABLE] && !layer) { census(vs, "blended"); return false; }
    if ((rs[XRS_PSTEXTUREMODES] & 0x1F) != 1) { census(vs, "stage 0 not 2D"); return false; }
    Resource *t = g_st.textures[0];
    if (!t || resource_type(t) != RTYPE_TEXTURE || (((PixelContainer *)t)->Format & FMT_CUBEMAP)) { census(vs, "no 2D texture 0"); return false; }
    census(vs, "LIT");
    g_draw_on = true;
    g_draw_gun = gun;
    static uint32_t frame, n, ng, logged;
    uint32_t f = g_frame_count.load();
    if (f != frame) {
        if (f / 600 != frame / 600 && logged++ < 40) logf("[ppl] frame %u: %u lit draws (%u first-person); player rounds %u, flash pulses %u", frame, n, ng, (unsigned)g_ppl_player_shots, g_flash_pulses);
        frame = f;
        n = ng = 0;
    }
    n++;
    if (gun) ng++;
    return true;
}

bool ppl_this_draw() { return g_draw_on; }

bool g_ppl_ps_sm3, g_ppl_sm3_want;
// Lit draws as a vs_3_0 / ps_3_0 pair (224 pixel constants): 12 world lights, the fog done in the shader (shader model
// 3 has no fixed-function fog). BLACK_PPL_SM3=0: the shader model 2 pass (5 lights, 3 on bump-mapped surfaces).
bool ppl_sm3_on() { static const bool on = !(getenv("BLACK_PPL_SM3") && getenv("BLACK_PPL_SM3")[0] == '0'); return on && ppl_med(); }
int ppl_max_slots() { return ppl_sm3_on() ? (ppl_level() >= 3 ? 11 : 12) : (ppl_level() >= 3 ? 4 : 5); }
// The CO-OP page's LIGHTING (owner): 0 OFF; 1 LOW = the 0.9.0 lighting (muzzle flashes - the player's, the
// partner's, the enemies' - explosions, the wall check, shadows, bump; shader model 2); 2 MED = everything since:
// shader model 3 with 12 lights, spark flashes, flame-effect fires, lamp-halo glints, the world light, the lamp map;
// 3 = MED + the head lamp (tests, black.ini / BLACK_PPL only)
bool ppl_med() { return ppl_level() >= 2; }

// For ppl_shadow.cpp: the frame's world -> view rows, and whether the current draw is in world space (its
// camera position, solved from its model -> clip rows, is the frame's).
bool ppl_world_rows(float rows[3][4], float inv[2])
{
    if (!g_vp_ok || g_frame_count.load() - g_vp_frame > 2) return false;
    memcpy(rows, g_vp_rows, sizeof g_vp_rows);
    inv[0] = g_inv_p00, inv[1] = g_inv_p11;
    return true;
}

bool ppl_draw_in_world_space()
{
    if (!g_vp_ok || g_frame_count.load() - g_vp_frame > 2) return false;
    float eye[3], fwd[3];
    if (!eye_from_mvp(g_st.vs_constants, eye, fwd)) return false;
    float dx = eye[0] - g_vp_eye[0], dy = eye[1] - g_vp_eye[1], dz = eye[2] - g_vp_eye[2];
    return dx * dx + dy * dy + dz * dz < 0.01f;
}

// Once a frame, before a draw applies its state (draw.cpp begin_draw): the world lights, and the shadow cube
// for the strongest one (ppl_shadow.cpp). BLACK_PPL_SHADOWS=0 = no shadows.
void ppl_frame_prepare(const VertexShader *vs)
{
    // owner playtest 4 ("explosions cause lighting flickers"): the camera was last frame's, and explosions shake
    // it. This frame's camera is pinned at its first draw whose camera position (solved from its model -> clip rows)
    // lies within 2 m of the one most level draws agreed on last frame (world space; the sky and moving pieces
    // have their own spaces). Lights and the shadow cube are made then, before the draw applies its state.
    static uint32_t frame = ~0u;
    uint32_t f = g_frame_count.load();
    if (f == frame || !vs || !g_cons_ok) return;
    const float (*c)[4] = g_st.vs_constants;
    float eye[3], fwd[3];
    if (!eye_from_mvp(c, eye, fwd)) return;
    float dx = eye[0] - g_cons_eye[0], dy = eye[1] - g_cons_eye[1], dz = eye[2] - g_cons_eye[2];
    if (dx * dx + dy * dy + dz * dz > 4.0f) return;
    auto len = [](const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    float l0 = len(c[10]), l1 = len(c[11]), l3 = len(c[13]);
    if (!(l0 > 1e-6f && l1 > 1e-6f && l3 > 1e-6f)) return;
    frame = f;
    g_inv_p00 = l3 / l0, g_inv_p11 = l3 / l1;
    memcpy(g_vp_rows[0], c[10], 16);
    memcpy(g_vp_rows[1], c[11], 16);
    memcpy(g_vp_rows[2], c[13], 16);
    memcpy(g_vp_eye, eye, 12);
    memcpy(g_vp_fwd, fwd, 12);
    g_vp_ok = true;
    g_vp_frame = f;
    frame_world_lights();
    static const bool off = getenv("BLACK_PPL_SHADOWS") && getenv("BLACK_PPL_SHADOWS")[0] == '0';
    if (!off && g_vl_n) ppl_shadow_render(g_vl[0].wpos, g_vl[0].pos, g_vl[0].reach);
}

extern float g_sun[3], g_sun_w; // the world light (below)
bool g_draw_char; // this draw has a vertex normal: a character / prop (ppl_sun_sample)
namespace { void update_sun(uint32_t f); }

void ppl_upload_vs(IDirect3DDevice9 *d)
{
    // c201 1/P00, 1/P11; c202-c204 view -> world (rows); c205 the shadowed light in world space
    update_sun(g_frame_count.load());
    float k[7][4] = {{g_inv_p00, g_inv_p11, 0, 0}};
    // c206 the lamp map: its world x, z corner, 1 / its size, the camera height it was made for
    k[5][0] = g_map_o[0], k[5][1] = g_map_o[1], k[5][2] = 1.0f / (kMap * kCell), k[5][3] = g_map_o[2];
    if (g_vp_ok) {
        float R[3][3], t[3];
        const float s[3] = {g_inv_p00, g_inv_p11, 1.0f};
        for (int r = 0; r < 3; r++) {
            for (int i = 0; i < 3; i++) R[r][i] = g_vp_rows[r][i] * s[r];
            t[r] = g_vp_rows[r][3] * s[r];
        }
        for (int i = 0; i < 3; i++) { // world = R^T (view - t)
            k[1 + i][0] = R[0][i], k[1 + i][1] = R[1][i], k[1 + i][2] = R[2][i];
            k[1 + i][3] = -(R[0][i] * t[0] + R[1][i] * t[1] + R[2][i] * t[2]);
        }
    }
    if (g_vl_n) memcpy(k[4], g_vl[0].wpos, 12);
    // c207 the world light in view space (unit) and its strength
    static const float sun_gain_env = env_f("BLACK_PPL_SUN", 0.8f);
    const float sun_gain = ppl_med() ? sun_gain_env : 0.0f; // LIGHTING LOW: no world light
    if (g_vp_ok && sun_gain > 0.0f) {
        float v[3];
        for (int r = 0; r < 3; r++) v[r] = g_vp_rows[r][0] * g_sun[0] + g_vp_rows[r][1] * g_sun[1] + g_vp_rows[r][2] * g_sun[2];
        v[0] *= g_inv_p00, v[1] *= g_inv_p11;
        float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n > 1e-6f) k[6][0] = v[0] / n, k[6][1] = v[1] / n, k[6][2] = v[2] / n, k[6][3] = g_draw_char ? 0.0f : sun_gain * g_sun_w;
    }
    d->SetVertexShaderConstantF(201, &k[0][0], 7);
}

// The ps_3_0 light pass takes these as constants (c51 / c52): its vertex shader has no room left to pass them
void ppl_view_up_and_sun(float up[4], float sun[4])
{
    up[0] = 0, up[1] = 1, up[2] = 0, up[3] = 0;
    sun[0] = sun[1] = sun[2] = sun[3] = 0;
    if (!g_vp_ok) return;
    const float s[3] = {g_inv_p00, g_inv_p11, 1.0f};
    for (int r = 0; r < 3; r++) up[r] = g_vp_rows[r][1] * s[r]; // view-space up = R * (0, 1, 0)
    static const float sun_gain_env = env_f("BLACK_PPL_SUN", 0.8f);
    const float sun_gain = ppl_med() ? sun_gain_env : 0.0f; // LIGHTING LOW: no world light
    if (sun_gain <= 0.0f) return;
    float v[3];
    for (int r = 0; r < 3; r++) v[r] = (g_vp_rows[r][0] * g_sun[0] + g_vp_rows[r][1] * g_sun[1] + g_vp_rows[r][2] * g_sun[2]) * s[r];
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-6f) sun[0] = v[0] / n, sun[1] = v[1] / n, sun[2] = v[2] / n, sun[3] = g_draw_char ? 0.0f : sun_gain * g_sun_w;
}

// The lamp map onto sampler 5 (made / refreshed on the first lit draw of a frame)
void lampmap_bind(IDirect3DDevice9 *d)
{
    if (!g_map_frame || g_map_failed) { d->SetTexture(5, nullptr); return; }
    if (!g_map_tex && FAILED(d->CreateTexture(kMap, kMap, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_map_tex, nullptr))) {
        g_map_tex = nullptr, g_map_failed = true;
        logf("[ppl] lamp map: no texture (CreateTexture failed): far lamps off");
        d->SetTexture(5, nullptr);
        return;
    }
    if (g_map_sent != g_map_frame) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_map_tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
            for (int y = 0; y < kMap; y++) memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, g_map_px + y * kMap, kMap * 4);
            g_map_tex->UnlockRect(0);
            g_map_sent = g_map_frame;
        }
    }
    d->SetTexture(5, g_map_tex);
    d->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

void ppl_lampmap_release()
{
    if (g_map_tex) g_map_tex->Release();
    g_map_tex = nullptr;
    g_map_sent = ~0u;
}

// Everything is in view space (x right, y up, z forward, metres; the eye at 0): the vertex shader rebuilds it
// from its clip position, so skinned arms and the gun work like level geometry and no world matrix is needed.
void ppl_upload(IDirect3DDevice9 *d)
{
    static const float bump = env_f("BLACK_PPL_BUMP", 0.03f);   // height of full white over black, metres
    static const float gain = env_f("BLACK_PPL_GAIN", 1.0f);
    static const float gun_gain = env_f("BLACK_PPL_GUN", 0.5f); // the gun sits right at the flash
    static const float dbg_mode = env_f("BLACK_PPL_DEBUG", 0.0f);
    static const float dbg = dbg_mode >= 2 ? -0.35f : dbg_mode > 0 ? 0.35f : 0.0f; // c28.w: < 0 = shadows only
    frame_world_lights();
    float k[9][4] = {}; // c23..c31 (ps_2_a has 32 constants; the game's combiners use c0-c22)
    // c23 texel size of texture 0, bump height, this draw's light gain
    uint32_t w = 1, h = 1, dd, lv;
    pixel_container_dims((PixelContainer *)g_st.textures[0], &w, &h, &dd, &lv);
    k[0][0] = 1.0f / (float)(w ? w : 1), k[0][1] = 1.0f / (float)(h ? h : 1), k[0][2] = g_draw_char ? bump * 0.33f : bump;
    k[0][3] = g_draw_gun ? gun_gain : 1.0f;
    float (*L)[4] = &k[1], (*C)[4] = &k[5]; // c24..c27 position + reach, c28..c31 colour
    for (int i = 0; i < 4; i++) L[i][3] = 1.0f;  // unused slots: zero colour
    // slot 0: the player's muzzle flash, the fired weapon's profile (flash_intensity)
    float fl = flash_intensity() * gain;
    L[0][0] = g_flash.x, L[0][1] = g_flash.y, L[0][2] = g_flash.z, L[0][3] = g_flash.reach;
    if (g_draw_gun) { // the gun's visible faces look back at the eye: light it from above the barrel, between eye and muzzle
        L[0][0] = 0.10f, L[0][1] = 0.10f, L[0][2] = 0.5f, L[0][3] = 2.5f;
    }
    C[0][0] = g_flash.r * fl, C[0][1] = g_flash.g * fl, C[0][2] = g_flash.b * fl, C[0][3] = dbg; // .w: the debug tint
    // slots 1-3: the strongest world lights here (explosions, other characters' muzzle flashes); level 2 keeps
    // slot 3 for the head lamp (at the eye, 14 m reach, dim cool white)
    int slots = ppl_level() >= 3 ? 2 : 3;
    for (int i = 0; i < g_vl_n && i < slots; i++) {
        memcpy(L[1 + i], g_vl[i].pos, 12);
        L[1 + i][3] = g_vl[i].spec_only ? -g_vl[i].reach : g_vl[i].reach;
        C[1 + i][0] = g_vl[i].col[0] * gain, C[1 + i][1] = g_vl[i].col[1] * gain, C[1 + i][2] = g_vl[i].col[2] * gain;
        C[1 + i][3] = 0.0f; // .w of slot 1: its shadow cube is bound (below)
    }
    if (ppl_level() >= 3) {
        float hl = 0.45f * gain;
        L[3][0] = L[3][1] = L[3][2] = 0.0f, L[3][3] = 14.0f;
        C[3][0] = hl, C[3][1] = hl, C[3][2] = hl * 1.05f, C[3][3] = 0.8f;
    }
    if (g_vl_n && ppl_shadow_bind(d, g_vl[0].wpos)) C[1][3] = 1.0f; // slot 1 = g_vl[0], if the cube is for it
    // world lights 4-5 in c19-c22 (the bump-map matrices: free when no stage bump-maps; ppl_ps_decls(true)). Set
    // after upload_ps_constants, which sends c0-c22 again on every draw.
    if (g_ppl_ps_sm3) { // ps_3_0: world lights 4-12 in c32-c40 (position, reach) / c41-c49 (colour), the fog switch c50
        float x[19][4] = {};
        for (int i = 0; i < 9; i++) x[i][3] = 1.0f;
        for (int i = 0; i < 9 && slots + i < g_vl_n; i++) {
            const ViewLight &v = g_vl[slots + i];
            memcpy(x[i], v.pos, 12), x[i][3] = v.spec_only ? -v.reach : v.reach;
            x[9 + i][0] = v.col[0] * gain, x[9 + i][1] = v.col[1] * gain, x[9 + i][2] = v.col[2] * gain;
        }
        x[18][0] = render_state()[XRS_FOGENABLE] ? 1.0f : 0.0f;
        d->SetPixelShaderConstantF(32, &x[0][0], 19);
        float us[2][4] = {};
        ppl_view_up_and_sun(us[0], us[1]);
        d->SetPixelShaderConstantF(51, &us[0][0], 2);
    } else if (!ppl_ps_bump(render_state()[XRS_PSTEXTUREMODES])) {
        float x[4][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {}, {}}; // c19-20 position + reach, c21-22 colour
        for (int i = 0; i < 2 && slots + i < g_vl_n; i++) {
            const ViewLight &v = g_vl[slots + i];
            memcpy(x[i], v.pos, 12), x[i][3] = v.spec_only ? -v.reach : v.reach;
            x[2 + i][0] = v.col[0] * gain, x[2 + i][1] = v.col[1] * gain, x[2 + i][2] = v.col[2] * gain;
        }
        d->SetPixelShaderConstantF(19, &x[0][0], 4);
    }
    lampmap_bind(d);
    d->SetPixelShaderConstantF(23, &k[0][0], 9);
}


// ---- The world light (v12g) ----
// Owner: "just track the world light source that light up the whole map instead". BLACK has no runtime sun: level
// light is baked. Characters and props get a per-object quadratic irradiance in their vertex program:
//   light = dot(1, n * c4 + n.xx/yy/xy * c2 + n.xz/yz * c3) x colour c5      (n: the object-space normal, v2)
// c4.xyz, the linear term, points to where the light comes from (in object space). Each such draw's c4 goes to view
// space with its model->clip rows (c10 / P00, c11 / P11, c13), then to world space; the frame's mean is smoothed into
// g_sun. The lit pixel shader adds relief along it (bumped minus flat Lambert: no extra brightness) and a gloss
// highlight, scaled by the baked light. No light slot, no pixel constant (the direction rides in the .w of
// TEXCOORD4-7). BLACK_PPL_SUN = strength (0.8; 0 = off).
float g_sun[3] = {0.3f, 0.9f, 0.3f}; // world, unit: towards the light
float g_sun_w;                         // confidence 0..1 (rises while characters are drawn, falls slowly)
namespace {
float g_sun_acc[3];
uint32_t g_sun_acc_n, g_sun_frame, g_sun_total;
float g_sun_spread; // the last frame's mean resultant length (1: all samples agree)
}

void ppl_sun_sample(const VertexShader *vs)
{
    // owner: "bumps are way too extreme on characters": a draw with a vertex normal (characters, props, guns - level
    // geometry has none) gets no world-light relief and a third of the bump height (ppl_upload / ppl_upload_vs)
    g_draw_char = false;
    if (vs)
        for (uint32_t i = 0; i < vs->num_elements; i++)
            if (vs->elements[i].reg == 2) g_draw_char = true;
    if (!vs || !vs->program || !g_vp_ok) return;
    static std::unordered_map<const VertexShader *, int> reg_of;
    auto it = reg_of.find(vs);
    int reg = -1;
    if (it == reg_of.end()) {
        for (uint32_t i = 0; i < vs->program_insts; i++) { // "mul rX.xyz, v2, cK": K is the linear irradiance term
            VshInst in;
            vsh_decode(vs->program + i * 4, in);
            if (in.mac == MAC_MUL && in.a.mux == MUX_V && in.v == 2 && in.b.mux == MUX_C && !in.b.neg) { reg = in.cnst; break; }
            if (in.final) break;
        }
        reg_of[vs] = reg;
    } else reg = it->second;
    if (reg < 0 || reg >= 192) return;
    const float (*c)[4] = g_st.vs_constants;
    const float d[3] = {c[reg][0], c[reg][1], c[reg][2]};
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1e-6f) return;
    // object -> view (x / P00, y / P11, w), then view -> world (the frame's rows, as ppl_upload_vs)
    float dv[3] = {(c[10][0] * d[0] + c[10][1] * d[1] + c[10][2] * d[2]) * g_inv_p00,
                   (c[11][0] * d[0] + c[11][1] * d[1] + c[11][2] * d[2]) * g_inv_p11,
                   c[13][0] * d[0] + c[13][1] * d[1] + c[13][2] * d[2]};
    const float s[3] = {g_inv_p00, g_inv_p11, 1.0f};
    float w[3] = {0, 0, 0};
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 3; i++) w[i] += g_vp_rows[r][i] * s[r] * dv[r];
    float m = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (m < 1e-6f) return;
    for (int i = 0; i < 3; i++) g_sun_acc[i] += w[i] / m;
    g_sun_acc_n++;
    g_sun_total++;
}

namespace {
// Once a frame (ppl_frame_prepare): last frame's samples into g_sun
void update_sun(uint32_t f)
{
    if (g_sun_frame == f) return;
    g_sun_frame = f;
    if (g_sun_acc_n) {
        float m = sqrtf(g_sun_acc[0] * g_sun_acc[0] + g_sun_acc[1] * g_sun_acc[1] + g_sun_acc[2] * g_sun_acc[2]);
        g_sun_spread = m / g_sun_acc_n;
        if (m > 1e-6f) {
            // the first sample sets it, then ~5 s smoothing (owner: window glints "flickers when you move or look in
            // other directions" - the mean follows which characters / props are on screen)
            const float k = g_sun_w < 0.05f ? 1.0f : 0.003f;
            float n2 = 0;
            for (int i = 0; i < 3; i++) g_sun[i] += (g_sun_acc[i] / m - g_sun[i]) * k, n2 += g_sun[i] * g_sun[i];
            float n = sqrtf(n2);
            if (n > 1e-6f)
                for (int i = 0; i < 3; i++) g_sun[i] /= n;
            g_sun_w = std::fmin(g_sun_w + 0.05f, 1.0f);
        }
    } else g_sun_w *= 0.998f; // no characters drawn: keep the last direction, fade over ~10 s
    static uint32_t last, lines;
    if (f - last >= 120 && lines < 300) {
        last = f, lines++;
        logf("[ppl] sun: world dir %+.2f %+.2f %+.2f, confidence %.2f | %u samples last frame (agreement %.2f), %u so far", g_sun[0],
             g_sun[1], g_sun[2], g_sun_w, g_sun_acc_n, g_sun_spread, g_sun_total);
    }
    g_sun_acc[0] = g_sun_acc[1] = g_sun_acc[2] = 0;
    g_sun_acc_n = 0;
}
} // namespace

void ppl_probe(const VertexShader *vs)
{
    static const bool on = getenv("BLACK_PPL_PROBE") && *getenv("BLACK_PPL_PROBE") == '1';
    if (!on || !vs || !vs->program) return;
    static std::unordered_map<const VertexShader *, int> reg_of;
    auto it = reg_of.find(vs);
    int reg = -1;
    if (it == reg_of.end()) {
        for (uint32_t i = 0; i < vs->program_insts; i++) {   // "add rX.xyz, v0, -cK": K is a point light position
            VshInst in;
            vsh_decode(vs->program + i * 4, in);
            if (in.mac == MAC_ADD && in.a.mux == MUX_V && in.v == 0 && in.c.mux == MUX_C && in.c.neg) { reg = in.cnst; break; }
            if (in.final) break;
        }
        reg_of[vs] = reg;
    } else reg = it->second;
    if (reg < 0 || reg + 3 >= 192) return;
    static uint32_t last_frame;
    static int lines;
    uint32_t f = g_frame_count.load();
    if (f - last_frame < 30 || lines > 400) return;
    last_frame = f;
    lines++;
    const float (*c)[4] = g_st.vs_constants;
    float eye[3] = {}, fwd[3] = {};
    eye_from_mvp(c, eye, fwd);
    logf("[ppl] probe frame %u vs %p light c%d %+.2f %+.2f %+.2f %+.2f | c%d %+.3f %+.3f %+.3f %+.3f | c%d %.2f %.2f %.2f %.2f | c%d %.2f %.2f %.2f "
         "%.2f | eye %+.2f %+.2f %+.2f shots %u",
         f, vs, reg, c[reg][0], c[reg][1], c[reg][2], c[reg][3], reg + 1, c[reg + 1][0], c[reg + 1][1], c[reg + 1][2], c[reg + 1][3],
         reg + 2, c[reg + 2][0], c[reg + 2][1], c[reg + 2][2], c[reg + 2][3], reg + 3, c[reg + 3][0], c[reg + 3][1], c[reg + 3][2],
         c[reg + 3][3], eye[0], eye[1], eye[2], (unsigned)g_ppl_player_shots);
}

bool ppl_ps_bump(uint32_t tex_modes)
{
    for (int i = 0; i < 4; i++) {
        uint32_t m = (tex_modes >> (5 * i)) & 0x1F;
        if (m == 6 || m == 7) return true; // BUMPENVMAP (_LUM): uses bumpMat c19-c22
    }
    return false;
}

const char *ppl_ps_decls(int mode)
{
    if (mode == 2)
        return "float4 pplTex : register(c23);\n"
               "float4 pplL[4] : register(c24);\n"
               "float4 pplC[4] : register(c28);\n"
               "float4 pplL3[9] : register(c32);\n" // world lights 4-12: position, reach (ps_3_0)
               "float4 pplC3[9] : register(c41);\n" // their colour
               "float4 pplFog : register(c50);\n"   // x: the game's fog is on (shader model 3 has no fixed-function fog)
               "float4 pplUpS : register(c51);\n"   // the view-space up (the lamp map; shader model 2 takes it from TEXCOORD7)
               "float4 pplSunS : register(c52);\n"  // the world light: view direction, strength (shader model 2: TEXCOORD4-7 .w)
               "samplerCUBE pplShadow : register(s4);\n"
               "sampler2D pplMap : register(s5);\n";
    if (mode == 1)
        return "float4 pplTex : register(c23);\n"
               "float4 pplL[4] : register(c24);\n"
               "float4 pplC[4] : register(c28);\n"
               "float4 pplL2[2] : register(c19);\n" // world lights 4-5: position, reach (in the bump matrices' place)
               "float4 pplC2[2] : register(c21);\n" // their colour
               "samplerCUBE pplShadow : register(s4);\n"
               "sampler2D pplMap : register(s5);\n";
    // ps_2_a has 32 constants; the game's combiner constants take c0-c22, so the pass lives in c23-c31.
    return "float4 pplTex : register(c23);\n"   // texel size of texture 0, bump height, this draw's light gain
           "float4 pplL[4] : register(c24);\n"  // view-space position, reach (0: the player's flash; 1-3 world lights / lamp)
           "float4 pplC[4] : register(c28);\n"  // colour x level; pplC[0].w = BLACK_PPL_DEBUG tint, pplC[1].w = shadowed
           "samplerCUBE pplShadow : register(s4);\n" // slot 1's distance cube (view-space axes; ppl_shadow.cpp)
           "sampler2D pplMap : register(s5);\n";     // the lamp map (rgb / 4, a = the lights' height; I.tc6, I.tc7)
}

// Mikkelsen 2010 ("Bump Mapping Unparametrized Surfaces on the GPU"): no tangents needed. The height
// gradient is taken in texture space (central differences on the diffuse luminance) and carried to
// screen space with the UV derivatives, then applied with the position derivatives.
static const char *ppl_ps_epilogue_base();

const char *ppl_ps_epilogue(int mode)
{
    if (mode == 2) { // ps_3_0: 9 more lights, then the fog (vertex fog factor: 1 = clear)
        static const std::string sm3 = [] {
            std::string s = ppl_ps_epilogue_base();
            size_t at = s.find("    {\n      float4 lm = tex2D(pplMap");
            if (at == std::string::npos) return s;
            s.insert(at, "    [unroll] for (int lk = 0; lk < 9; lk++) {\n" // world lights 4-12: no shadow
                         "      float3 L = pplL3[lk].xyz - P; float d2 = dot(L, L); float a = saturate(1 - d2 / (pplL3[lk].w * pplL3[lk].w)); a *= a;\n"
                         "      L *= rsqrt(max(d2, 1e-6));\n"
                         "      float ndl = saturate(dot(Nb, L)) * saturate(dot(N, L) * 8);\n"
                         "      float sp = pow(saturate(dot(Nb, normalize(L + V))), 24) * 0.5 * gloss * (ndl > 0);\n"
                         "      add += pplC3[lk].rgb * a * (pplL3[lk].w > 0 ? t0.rgb * ndl + sp : sp * 2.5);\n"
                         "    }\n");
            s += "  if (pplFog.x > 0) res.rgb = lerp(fogColor.rgb, res.rgb, saturate(I.tc7.x));\n"; // the fog factor (TEXCOORD7.x)
            auto swap = [&](const char *a, const char *b) {
                size_t at = s.find(a);
                if (at != std::string::npos) s.replace(at, strlen(a), b);
            };
            swap("normalize(I.tc7.xyz)", "normalize(pplUpS.xyz)");
            swap("float3 Ls = float3(I.tc4.w, I.tc5.w, I.tc6.w); float sk = I.tc7.w;", "float3 Ls = pplSunS.xyz; float sk = pplSunS.w;");
            return s;
        }();
        return sm3.c_str();
    }
    if (mode != 1) return ppl_ps_epilogue_base();
    // not ppl_ps_epilogue(false) inside: re-entering this static's initialisation hung the game ("the game froze")
    static const std::string with2 = [] {
        std::string s = ppl_ps_epilogue_base();
        size_t at = s.find("    {\n      float4 lm = tex2D(pplMap");
        if (at == std::string::npos) return s;
        s.insert(at, "    [unroll] for (int lj = 0; lj < 2; lj++) {\n" // world lights 4-5: no shadow
                     "      float3 L = pplL2[lj].xyz - P; float d2 = dot(L, L); float a = saturate(1 - d2 / (pplL2[lj].w * pplL2[lj].w)); a *= a;\n"
                     "      L *= rsqrt(max(d2, 1e-6));\n"
                     "      float ndl = saturate(dot(Nb, L)) * saturate(dot(N, L) * 8);\n"
                     "      float sp = pow(saturate(dot(Nb, normalize(L + V))), 24) * 0.5 * gloss * (ndl > 0);\n"
                     "      add += pplC2[lj].rgb * a * (pplL2[lj].w > 0 ? t0.rgb * ndl + sp : sp * 2.5);\n"
                     "    }\n");
        return s;
    }();
    return with2.c_str();
}

static const char *ppl_ps_epilogue_base()
{
    return "  {\n"
           "    float3 P = I.tc4.xyz;\n"
           "    float2 uv = I.tc0.xy / (I.tc0.w == 0 ? 1 : I.tc0.w);\n"
           "    const float3 LUM = float3(0.299, 0.587, 0.114);\n"
           "    float hl = dot(tex2D(s2d0, uv - float2(pplTex.x, 0)).rgb, LUM), hr = dot(tex2D(s2d0, uv + float2(pplTex.x, 0)).rgb, LUM);\n"
           "    float hd = dot(tex2D(s2d0, uv - float2(0, pplTex.y)).rgb, LUM), hu = dot(tex2D(s2d0, uv + float2(0, pplTex.y)).rgb, LUM);\n"
           "    float2 dh = float2(hr - hl, hu - hd) / (2 * pplTex.xy) * pplTex.z;\n"
           "    float3 dp1 = ddx(P), dp2 = ddy(P);\n"
           "    float2 duv1 = ddx(uv), duv2 = ddy(uv);\n"
           "    float3 N = normalize(cross(dp1, dp2));\n"
           "    float3 V = -P;\n" // the eye is the view-space origin
           "    N = dot(N, V) < 0 ? -N : N;\n"
           "    float3 R1 = cross(dp2, N), R2 = cross(N, dp1);\n"
           "    float det = dot(dp1, R1);\n"
           "    float dBs = dot(dh, duv1), dBt = dot(dh, duv2);\n"
           "    float3 grad = sign(det) * (dBs * R1 + dBt * R2);\n"
           "    float3 Nb = normalize(abs(det) * N - grad);\n"
           "    V = normalize(V);\n"
           "    float gloss = dot(t0.rgb, LUM);\n"
           "    float3 add = 0; float shadowed = 0;\n"
           "    [unroll] for (int li = 0; li < 4; li++) {\n"
           "      float3 L = pplL[li].xyz - P; float d2 = dot(L, L); float a = saturate(1 - d2 / (pplL[li].w * pplL[li].w)); a *= a;\n"
           "      L *= rsqrt(max(d2, 1e-6));\n"
           "      float ndl = saturate(dot(Nb, L)) * saturate(dot(N, L) * 8);\n"
           "      float sp = pow(saturate(dot(Nb, normalize(L + V))), 24) * 0.5 * gloss * (ndl > 0);\n"
           "      float sh = 1;\n"
           "      if (li == 1) {\n"  // the shadow cube: 4 taps around the light -> pixel direction
           "        float3 sd = I.tc5.xyz; float sl = length(sd); float3 sn = sd / max(sl, 1e-4);\n"  // world axes
           "        float3 t1 = normalize(cross(sn, abs(sn.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0))); float3 t2 = cross(sn, t1);\n"
           "        float o = 0.015 * sl, ref = (sl - 0.08 - 0.02 * sl) / abs(pplL[1].w);\n"
           "        float vis = (texCUBE(pplShadow, sd + t1 * o).r > ref) + (texCUBE(pplShadow, sd - t1 * o).r > ref)\n"
           "                  + (texCUBE(pplShadow, sd + t2 * o).r > ref) + (texCUBE(pplShadow, sd - t2 * o).r > ref);\n"
           "        sh = lerp(1, vis * 0.25, pplC[1].w);\n"
           "        shadowed = (1 - sh) * (a > 0) * (ndl > 0);\n"
           "      }\n"
           "      add += pplC[li].rgb * a * (pplL[li].w > 0 ? t0.rgb * ndl + sp : sp * 2.5) * sh;\n"
           "    }\n"
           // the lamp map: every lamp and fire, far ones too; flat (the lights hang above: floors full, walls 40 %)
           "    {\n"
           "      float4 lm = tex2D(pplMap, I.tc6.xy);\n"
           "      float dh = abs(I.tc6.z - lm.a) * 128;\n"
           "      float hat = saturate(1 - max(dh - 3, 0) / 6);\n"
           "      float up = 0.4 + 0.6 * saturate(dot(Nb, normalize(I.tc7.xyz)));\n"
           "      add += lm.rgb * 4 * hat * up * t0.rgb;\n"
           "    }\n"
           "    res.rgb = saturate(res.rgb + add * pplTex.w);\n"
           // the world light (v12g): relief (bumped minus flat Lambert) and a highlight, scaled by the baked light
           "    {\n"
           "      float3 Ls = float3(I.tc4.w, I.tc5.w, I.tc6.w); float sk = I.tc7.w;\n"
           "      float fb = saturate(dot(Nb, Ls)), ff = saturate(dot(N, Ls));\n"
           "      float ssp = pow(saturate(dot(Nb, normalize(Ls + V))), 32) * gloss * (fb > 0);\n"
           "      float lum = dot(res.rgb, LUM);\n"
           "      res.rgb = saturate(res.rgb * (1 + sk * 2 * (fb - ff)) + sk * ssp * lum * float3(1, 0.96, 0.88));\n"
           "    }\n"
           // BLACK_PPL_DEBUG=1: lit surfaces tinted magenta (c28.w > 0); =2: only what the shadow cube darkens (c28.w < 0)
           "    res.rgb = lerp(res.rgb, float3(1, 0, 1), pplC[0].w > 0 ? pplC[0].w : -pplC[0].w * 2 * shadowed);\n"
           "  }\n";
}

} // namespace xd3d
