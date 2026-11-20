// Per-pixel lighting prototype - see ppl.h.
#include "d3d/ppl.h"

#include "common.h"
#include "d3d/nv2a_vsh.h"
#include "d3d/state.h"
#include "d3d/xd3d.h"
#include "xbe.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

extern "C" volatile uint8_t g_fov_viewmodel;     // fov.cpp: set while the first-person list draws
extern "C" volatile uint8_t g_ppl_count_shots;   // cheats.cpp ammo thunk (the game's fire routine, once per round):
extern "C" volatile uint32_t g_ppl_player_shots; //   counts the player's rounds while the first is set
extern "C" volatile uint32_t g_ppl_shot_weapon;  //   and keeps the weapon object of the last one,
extern "C" volatile uint32_t g_ppl_enemy_shot[16]; // and everyone else's shooters (a ring)
extern "C" volatile uint32_t g_ppl_enemy_shot_n;

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
    float gain, reach, r, g, b, rise_ms, decay_ms, life_ms;
    float vis; // wall check: share of the light reaching the camera's side, smoothed (-1 = not tested yet)
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
    float down = t < l.rise_ms ? 1.0f : (float)exp(-(t - l.rise_ms) / l.decay_ms);
    return l.gain * up * down;
}

// Explosions: FUN_001e0970 (the game's core explosion: grenades, rockets, barrels, destructibles). coop.cpp may
// hook its first 6 bytes (the co-op relay, installed when a co-op mission starts, and it checks those bytes), so
// this hook takes the next instruction, sub esp, 0xA4 (6 bytes at 0x1E0976): the frame is set up there,
// [ebp+0xC] = the centre vec4*, XMM2 = the radius, EAX = the kind.
volatile uint32_t g_ex_queue_n;
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
        if (logged_shots++ < 60)
            logf("[ppl] enemy round: shooter %08x at %.1f %.1f %.1f", body, pos[0], pos[1], pos[2]);
    }
}

// This frame's world lights in view space, the 3 that matter most here (brightness x reach^2 / distance^2).
struct ViewLight { float pos[3], reach, col[3], wpos[3]; };
int g_vl_n;
ViewLight g_vl[3];

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

void frame_world_lights()
{
    static uint32_t frame = ~0u;
    uint32_t f = g_frame_count.load();
    if (f == frame) return;
    frame = f;
    double now = now_ms();
    collect_world_lights(now);
    struct Cand { ViewLight v; float score; } c[32];
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
        if (walls) {
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
        k.score = lv * l.reach * l.reach / (d2 > 1.0f ? d2 : 1.0f);
    }
    // slot 1 (the shadowed light, g_vl[0]) keeps the same light while it lives (within 0.5 m: a shooter's next
    // round too), unless another is 3x stronger: swapping the shadowed light frame to frame flickered
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
    for (int s = g_vl_n; s < 3 && n > 0; s++) {
        int best = 0;
        for (int i = 1; i < n; i++)
            if (c[i].score > c[best].score) best = i;
        g_vl[g_vl_n++] = c[best].v;
        c[best] = c[--n];
    }
    keep_ok = g_vl_n > 0;
    if (keep_ok) memcpy(keep_w, g_vl[0].wpos, 12);
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
    snprintf(b + w, sizeof b - w, "| tex 0x%x blend %u (%x/%x) zw %u vm %u | %s", rs[XRS_PSTEXTUREMODES], rs[XRS_ALPHABLENDENABLE],
             rs[XRS_SRCBLEND], rs[XRS_DESTBLEND], rs[XRS_ZWRITEENABLE], g_fov_viewmodel, verdict);
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
    // is lit like the base; effects, glass, the flash sprite (additive, or no depth write) are not.
    bool layer = rs[XRS_SRCBLEND] == 0x302 && rs[XRS_DESTBLEND] == 0x303 && rs[XRS_ZWRITEENABLE];
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

void ppl_upload_vs(IDirect3DDevice9 *d)
{
    // c201 1/P00, 1/P11; c202-c204 view -> world (rows); c205 the shadowed light in world space
    float k[5][4] = {{g_inv_p00, g_inv_p11, 0, 0}};
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
    d->SetVertexShaderConstantF(201, &k[0][0], 5);
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
    k[0][0] = 1.0f / (float)(w ? w : 1), k[0][1] = 1.0f / (float)(h ? h : 1), k[0][2] = bump;
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
    int slots = ppl_level() >= 2 ? 2 : 3;
    for (int i = 0; i < g_vl_n && i < slots; i++) {
        memcpy(L[1 + i], g_vl[i].pos, 12);
        L[1 + i][3] = g_vl[i].reach;
        C[1 + i][0] = g_vl[i].col[0] * gain, C[1 + i][1] = g_vl[i].col[1] * gain, C[1 + i][2] = g_vl[i].col[2] * gain;
        C[1 + i][3] = 0.0f; // .w of slot 1: its shadow cube is bound (below)
    }
    if (ppl_level() >= 2) {
        float hl = 0.45f * gain;
        L[3][0] = L[3][1] = L[3][2] = 0.0f, L[3][3] = 14.0f;
        C[3][0] = hl, C[3][1] = hl, C[3][2] = hl * 1.05f, C[3][3] = 0.8f;
    }
    if (g_vl_n && ppl_shadow_bind(d, g_vl[0].wpos)) C[1][3] = 1.0f; // slot 1 = g_vl[0], if the cube is for it
    d->SetPixelShaderConstantF(23, &k[0][0], 9);
}

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

const char *ppl_ps_decls()
{
    // ps_2_a has 32 constants; the game's combiner constants take c0-c22, so the pass lives in c23-c31.
    return "float4 pplTex : register(c23);\n"   // texel size of texture 0, bump height, this draw's light gain
           "float4 pplL[4] : register(c24);\n"  // view-space position, reach (0: the player's flash; 1-3 world lights / lamp)
           "float4 pplC[4] : register(c28);\n"  // colour x level; pplC[0].w = BLACK_PPL_DEBUG tint, pplC[1].w = shadowed
           "samplerCUBE pplShadow : register(s4);\n"; // slot 1's distance cube (view-space axes; ppl_shadow.cpp)
}

// Mikkelsen 2010 ("Bump Mapping Unparametrized Surfaces on the GPU"): no tangents needed. The height
// gradient is taken in texture space (central differences on the diffuse luminance) and carried to
// screen space with the UV derivatives, then applied with the position derivatives.
const char *ppl_ps_epilogue()
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
           "        float o = 0.015 * sl, ref = (sl - 0.08 - 0.02 * sl) / pplL[1].w;\n"
           "        float vis = (texCUBE(pplShadow, sd + t1 * o).r > ref) + (texCUBE(pplShadow, sd - t1 * o).r > ref)\n"
           "                  + (texCUBE(pplShadow, sd + t2 * o).r > ref) + (texCUBE(pplShadow, sd - t2 * o).r > ref);\n"
           "        sh = lerp(1, vis * 0.25, pplC[1].w);\n"
           "        shadowed = (1 - sh) * (a > 0) * (ndl > 0);\n"
           "      }\n"
           "      add += pplC[li].rgb * a * (t0.rgb * ndl + sp) * sh;\n"
           "    }\n"
           "    res.rgb = saturate(res.rgb + add * pplTex.w);\n"
           // BLACK_PPL_DEBUG=1: lit surfaces tinted magenta (c28.w > 0); =2: only what the shadow cube darkens (c28.w < 0)
           "    res.rgb = lerp(res.rgb, float3(1, 0, 1), pplC[0].w > 0 ? pplC[0].w : -pplC[0].w * 2 * shadowed);\n"
           "  }\n";
}

} // namespace xd3d
