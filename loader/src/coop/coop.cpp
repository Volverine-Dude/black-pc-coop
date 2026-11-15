// Co-op module (LAN): Stage 1 read bridge + Stage 2 marker demo (+ net hooks).
//
// BLACK is single-player only: no multiplayer session, lobby or netcode exists in the
// binary. LAN co-op is built on top of this loader as an additive module.
//
// Stage 1 (player bridge): FUN_001ec450 per-frame player update, this-call hook.
//   controller in ECX (prologue: push ebp / mov ebp,esp / and esp,-16 / sub esp,0x74 /
//   push ebx / mov ebx,ecx / mov eax,[ebx+0x8C]); +0x18 yaw, +0x1C pitch (deg),
//   +0xA0 pitch rate, +0xA4 yaw rate, +0xB8/+0xBC turn speeds (80/25 deg/s),
//   +0x8C = the player object pointer. Player object: +0xA0 exported position,
//   view matrix at +0xD0..+0x10C (rows +0xD0 left, +0xE0 up, +0xF0 forward, +0x100 eye).
//
// Stage 2 capture A: FUN_001df340, the raw entity spawn primitive (EDI = manager,
//   XMM0 = position, 7 stdcall stack args; ret 0x1C). Journals the game's own spawns.
// Stage 2 capture B: FUN_001eb440, the level-load driver, plus a loop-tail patch hook
//   between its two construction loops (FUN_001eb544). Only active with --coop-clone.
//
// Stage 2 route B (marker demo, --coop-listen N): the guest receives the other copy's
//   "PLAC" packets on UDP N and the port's Swap draws a colored dot at that world
//   position projected through the guest's camera (eye/player rows at +0x100/+0xD0/...);
//   the draw is a ColorFill on the host back buffer, so no game render state is touched.
#include "coop/coop.h"

#include "common.h"
#include "hooks.h"
#include "xbe.h"
#include "kernel/kexports.h"
#include "fe/coop_menu.h"
#include "cheats.h"

#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

// ---- Module-wide state the naked thunks and the d3d draw path reach. ----
extern "C" {
// Stage 1 telemetry socket + the other copy's packets (route B).
SOCKET g_coop_sock;          // the telemetry send socket (host role)
static uint64_t g_coop_t0;   // coop-init tick: the epoch for --downed-at (the enemy-sync tick's own first call is ~52 s late)
SOCKET g_coop_listens;       // the marker receive socket (guest role)
sockaddr_in g_coop_target;

// The marker snapshot: position, freshness and the drawn rect (device.cpp).
volatile float g_coop_host_pos[3];
volatile float g_coop_host_yaw;   // the remote yaw (.deg, packet) for the showcase drive
volatile float g_coop_host_pitch; // the remote view pitch (deg) - input for the upper-body aim step
volatile float g_coop_host_pawnmat[15]; // packet v3: the remote pawn's +0x70..0xAC body block
volatile uint32_t g_coop_host_pawn_ok;  // 1 = pawnmat carries valid data this stream
// The world-register capture hook's channels (spawner identity hunt: FUN_001ce1f0).
volatile uint32_t g_regcap_body, g_regcap_ret, g_regcap_world;
volatile void *g_regcap_resume;
// Pilot 12b: the pawn-spawner capture channels (FUN_001df440 = the character/pawn
// factory found by the registration census: it built all 4 mgr g-slot bodies).
volatile uint32_t g_pawncap_mgr, g_pawncap_owner, g_pawncap_def, g_pawncap_ctx, g_pawncap_owner_slot;
// The factory's only caller = FUN_001af880 (call site 0x1af967, ret 0x30). The factory
// does not push EBP, so at our entry hook EBP = the requester's frame. Its post-call
// steps on the returned body (0x1af96c..): +0x10 = [ebp+0x28], +0x14 = (u8)[ebp+0x2c],
// +0x31C = [ebp+0x18], +0x2F0 = float [0x295cec]; plus [edi]++ (its spawn counter).
volatile uint32_t g_pawncap_ebp;
volatile void *g_pawncap_resume;
// Pilot 13c: the pose-redirect state. At the natural spawn #3 entry the pose
// globals (ctor-only sources of the body transform) are rewritten; the body lands
// at our chosen position and the registration hook adopts it as the LAN puppet.
volatile uint32_t g_regc_quiet = 0;   // 1 = the capture loggers skip logf (reads stay)
volatile uint32_t g_regcap_on = 0;    // 0 = do NOT install the register hook (viewmodel gate)
volatile uint32_t g_pawncap_on = 0;   // 1 = install the FUN_001df440 pawn capture hook
volatile uint32_t g_p13_on = 0;       // 1 = arm the spawn-#4 pose redirect
volatile float g_rematch_pos[3];
volatile uint32_t g_rematch_hit = 0;
// Pilot 13: the factory-redirect state. The ctor pose globals (read by FUN_001da4f0:
// 0x2ace70..0x2ace9c = rotation block, 0x2f4390 = the position quad) get written by
// the logger before the real ctor runs, so the natural spawn lands at OUR pose.
// Pilot 13b: the pawn factory call channels (EAX = mgr, stack args = owner + def,
// stdcall RET 8 - the callee cleans both pushes; returns the new body in EAX).
volatile uint32_t g_fact_mgr, g_fact_a1, g_fact_def, g_fact_ctx, g_fact_out;
volatile void *g_fact_fn = (volatile void *)0x001DF440;
volatile uint32_t g_pawncap_n = 0;   // natural pawn spawn counter (the def capture)
volatile uint32_t g_coop_host_seq;
volatile uint32_t g_coop_marker_on; // device.cpp marker gate (coop.h)
uint64_t g_coop_host_ms;
float g_coop_hTan, g_coop_vTan;
uint32_t g_coop_last_ctrl;   // the player controller (updated by the Stage 1 thunk)
// HIT SYNC queues (see coop_on_pellet)
struct ShotRay { float o[3], d[3]; };
static uint64_t g_shot_in_ms[64];   // receive time per queued remote ray
static ShotRay g_shot_out[64]; static int g_shot_out_n;                // this player's rays -> network
static ShotRay g_shot_in[64];  static int g_shot_in_r, g_shot_in_w;    // remote rays -> clone
static uint32_t g_shot_stats[5];   // sent, received, used by clone pellets, dropped stale, extra replays
static void coop_shot_recv(const ShotRay *rays, int n);
static void coop_shot_catchup(uint32_t body);
struct EnemyRec { uint32_t body; float hp; float feet[3]; float byaw, lyaw, lpitch; uint32_t fire; uint32_t id; uint32_t tgt; float bear; };   // bear: the host brain's aim bearing (+0x10 - +0x14, deg)   // tgt: 1 = the host player, 2 = the joiner (clone on the host)
// ENEMY IDENTITY (day 10, owner play 20261112_183200: "still having enemies syncing issue").
// Pairing by body ADDRESS only holds while both copies spawn in the same order. In Veblensk the
// enemies come from level triggers that fire when EACH copy's player gets there -> different
// order -> different addresses (pairing misses, a "pair" 79 m apart, 61 wrong kills, a crash).
// Stable identity: every natural spawn carries a spawn-point context pointer (the def capture's
// ctx, level data - the same value on both copies) and the spawned body keeps it at +0x31C. An
// enemy's id = (that ctx, its ordinal among the spawns of that ctx). Both copies build the table
// locally; the host's packets carry ids, and the joiner translates every host body address to ITS
// body with that id when the packet arrives - the rest of the sync keeps using local addresses.
struct EnId { uint32_t body, ctx, id; uint8_t live, killing; uint64_t ms; };   // ms: when it appeared here   // killing: the joiner forced its hp to 0.01 for a host-ordered kill
static EnId g_enid[48]; static int g_enid_n;
static uint32_t g_class_objs[24]; static int g_class_objs_n;   // the class (brain) objects seen as spawn owners
static struct { uint32_t ctx, count; } g_spawn_ctx[128]; static int g_spawn_ctx_n;
static struct { uint32_t ctx, ord; } g_spawn_pend[32]; static int g_spawn_pend_n;
static struct { uint32_t host, local; } g_hmap[48]; static int g_hmap_n; static uint32_t g_hmap_w;   // joiner: host body -> our body
static struct { uint32_t id, host; } g_idhost[48]; static uint32_t g_idhost_w;   // joiner: enemy id -> the host's CURRENT body (latest enemy packet)
// KILL DIRECTION (play 20261113_191753: "enemies' death animation knockback is not synced to the joiner"):
// the joiner's forced kill shot flew from the host's clone to the victim -> the ragdoll / death fell away
// from the clone, not from the real killer. The host remembers every enemy's LAST hit (the damage call's
// arg 1 = hit point, arg 2 = shot direction - kd1) and sends it with the death ("CLKD"); the joiner's kill shot
// then comes along that direction through that point.
struct LastHit { uint32_t body; float dir[3], pt[3]; uint32_t cause; float bv[3][4]; uint32_t braw[7]; uint64_t ms; };   // cause 1 bullet, 2 blast (vt+0x2C args)
static LastHit g_lasthit[32]; static uint32_t g_lasthit_w;
struct KillDir { uint32_t id; float dir[3], pt[3]; uint32_t cause; float bv[3][4]; uint32_t braw[7]; };   // cause 1 bullet, 2 blast, 3 melee
struct MeleeMsg { uint32_t host_body; float hv[4], dir[4]; float dmg; };   // "CLML": the joiner's melee hit on a host enemy
static MeleeMsg g_ml_out[4]; static int g_ml_out_n;
static MeleeMsg g_ml_in[8]; static int g_ml_in_n;
static uint32_t g_ml_stats[3];   // sent, applied, dropped
static KillDir g_killdir[16]; static uint32_t g_killdir_w;       // host: recent deaths with their last hit; joiner: received
volatile uint32_t g_bk_fn, g_bk_this, g_bk_a[7], g_bk_esp;
__declspec(naked) static void coop_thunk_blast_kill()   // (body->vt+0x2C)(7 args), thiscall, RET 0x1C
{
    __asm {
        pushad
        mov dword ptr [g_bk_esp], esp
        push dword ptr [g_bk_a + 24]
        push dword ptr [g_bk_a + 20]
        push dword ptr [g_bk_a + 16]
        push dword ptr [g_bk_a + 12]
        push dword ptr [g_bk_a + 8]
        push dword ptr [g_bk_a + 4]
        push dword ptr [g_bk_a]
        mov ecx, dword ptr [g_bk_this]
        call dword ptr [g_bk_fn]
        mov esp, dword ptr [g_bk_esp]
        popad
        ret
    }
}
static uint32_t g_dead_ids[64]; static uint32_t g_dead_w;      // host: ids whose enemy is gone (ring, sent with CLEN); joiner: the received copy
static uint32_t coop_enid_of(uint32_t body)
{
    for (int i = 0; i < g_enid_n; i++) if (g_enid[i].live && g_enid[i].body == body) return g_enid[i].id;
    return 0;
}
static uint32_t coop_enid_local(uint32_t id)
{
    if (!id) return 0;
    for (int i = 0; i < g_enid_n; i++) if (g_enid[i].live && g_enid[i].id == id) return g_enid[i].body;
    return 0;
}
static uint32_t coop_host_to_local(uint32_t host)      // joiner: a host body address from any packet -> our body (0 = not here)
{
    for (int i = 0; i < g_hmap_n; i++) if (g_hmap[i].host == host) return g_hmap[i].local;
    return 0;
}
// every private link we put on a puppet body, with the class object it replaced (ar3: a spawner
// re-spawned with the dead body's +0x324 as the new enemy's brain = OUR private copy -> joiner AV
// 0x69955 in the renderer). Released (class put back) whenever that enemy ends.
static struct { uint32_t body, link, orig; } g_pl[48]; static int g_pl_n;
static struct { uint32_t link, orig; } g_pl_all[128]; static uint32_t g_pl_all_w;   // every private link ever made this level (never removed)
static void coop_pup_note(uint32_t body, uint32_t link, uint32_t orig)
{
    if (link && orig) { bool have = false; for (int i = 0; i < 128; i++) if (g_pl_all[i].link == link) { have = true; g_pl_all[i].orig = orig; }
                        if (!have) { g_pl_all[g_pl_all_w & 127].link = link; g_pl_all[g_pl_all_w & 127].orig = orig; g_pl_all_w++; } }
    for (int i = 0; i < g_pl_n; i++) if (g_pl[i].body == body) { g_pl[i].link = link; if (orig) g_pl[i].orig = orig; return; }
    if (g_pl_n < 48) { g_pl[g_pl_n].body = body; g_pl[g_pl_n].link = link; g_pl[g_pl_n].orig = orig; g_pl_n++; }
}
static void coop_pup_release(uint32_t body, const char *why)
{
    for (int i = 0; i < g_pl_n; i++) {
        if (g_pl[i].body != body) continue;
        uint32_t cur = *(volatile uint32_t *)(uintptr_t)(body + 0x324);
        if (cur == g_pl[i].link && g_pl[i].orig >= 0x80000000 && g_pl[i].orig < 0x84000000) {
            *(volatile uint32_t *)(uintptr_t)(body + 0x324) = g_pl[i].orig;
            static int rl;
            if (rl++ < 80) logf("coop: enemy sync: %08x class link put back %08x -> %08x (%s)", body, cur, g_pl[i].orig, why);
        }
        g_pl[i] = g_pl[--g_pl_n];
        return;
    }
}
static bool coop_enid_is_killing(uint32_t body)
{
    for (int i = 0; i < g_enid_n; i++) if (g_enid[i].live && g_enid[i].body == body && g_enid[i].killing) return true;
    return false;
}
static void coop_enid_mark_killing(uint32_t body)
{
    for (int i = 0; i < g_enid_n; i++) if (g_enid[i].live && g_enid[i].body == body) g_enid[i].killing = 1;
    // tg5: the flag was set while hp was still 100 (the kill shot lowers it later) and the liveness test
    // "killing && hp > 1" then read that as "a new enemy took the slot" - the kill was dropped as done
    // and the spawner behind it never spawned again. The hp goes to 0.01 at the ORDER.
    if (body >= 0x80000000 && body < 0x84000000) {
        float hp = *(volatile float *)(uintptr_t)(body + 0x2F0);
        if (hp > 0.01f) *(volatile float *)(uintptr_t)(body + 0x2F0) = 0.01f;
    }
}
static void coop_enid_reset(void)
{
    g_enid_n = 0; g_spawn_ctx_n = 0; g_spawn_pend_n = 0; g_hmap_n = 0; g_hmap_w = 0; g_class_objs_n = 0; g_pl_n = 0; memset(g_pl_all, 0, sizeof g_pl_all); g_pl_all_w = 0; memset(g_idhost, 0, sizeof g_idhost); g_idhost_w = 0; g_coop_mission_failed = 0;
    memset(g_dead_ids, 0, sizeof g_dead_ids); g_dead_w = 0;
}
static void coop_enid_update(uint32_t pobj);
static void coop_enemy_recv(const EnemyRec *e, int n);
static void coop_enemy_send(uint32_t pobj, uint32_t seq);
static void coop_enemy_tick(uint32_t pobj);
struct EnemyRay { uint32_t body; ShotRay r; };
// play 20261111_201304: arg 2 ("part") is a POINTER to a vec4 (FUN_001ed990 scales it at
// 0x1edb5f -> FUN_00011a70, movaps [edx]); the host's value 0343faf0 was host-process memory
// and the joiner crashed reading it. It now travels as a 16-byte copy (pvec, pvec_ok).
struct HitMsg { float vec[4]; uint32_t part, p5, p6, attacker; float dmg; float pvec[4]; uint32_t pvec_ok; };   // phase 3: host hit on the joiner's clone
static void coop_hit_recv(const HitMsg *h, int n);
static HitMsg g_hit_out[16]; static int g_hit_out_n;
static uint32_t g_hit_stats[3];   // sent, received, applied
static void coop_hit_apply(uint32_t pobj);
// EXPLOSION / MELEE relay (day 9): non-bullet hits on the joiner's clone (host) -> the
// joiner's player. slot = the damage vtable offset (0x2C generic/explosion, 7 args;
// 0x48 directional/melee, 4 args) - the SAME slot on the player's own vtable takes the
// same arguments. Pointer args travel as 32-byte copies (ptrmask); game OBJECT args
// (objmask) travel as addresses and are remapped on the joiner (host clone -> our
// player, host player -> our clone-of-host, an enemy -> the same-address puppet).
struct XdMsg { uint32_t slot, nargs, ptrmask, objmask; uint32_t raw[7]; float data[7][8]; uint32_t host_clone, host_player; };
static XdMsg g_xd_out[4]; static int g_xd_out_n;
// EXPLOSION RELAY "CLEX" (destructible sync, day 9): every explosion (FUN_001e0970 - grenades,
// rockets, destructibles) is captured where it happens and replayed on the other copy. owner_kind:
// 0 = same address there, 1 = the sender's player (-> the receiver's clone), 2 = the sender's clone
// (-> the receiver's player). Pointer args travel by value (centre, dir).
struct ExMsg { uint32_t kind, ecx, edx; float radius; float centre[4]; float dir[4]; uint32_t dir_ok, owner_kind; uint32_t raw[9]; };
static ExMsg g_ex_out[4]; static int g_ex_out_n;
static ExMsg g_ex_in[8]; static int g_ex_in_n;
static uint32_t g_ex_stats[4];   // sent, received, replayed, duplicates dropped/skipped
static void coop_ex_apply(uint32_t pobj);
static float g_ex_all[64][3]; static uint32_t g_ex_all_n;   // every explosion centre seen here (local or replayed; any role)
static XdMsg g_xd_in[8]; static int g_xd_in_n;
static uint32_t g_xd_stats[3];   // queued (host) / received / applied (joiner)
static void coop_xd_apply(uint32_t pobj);
static void coop_xd_test(void);
static void coop_ai_recv(const uint8_t *p, int len);
struct PupArm { uint32_t body, w; };
static PupArm g_pup_arm[16]; static int g_pup_narm;   // joiner: each puppet's last good weapon (enemy sync fills, weapon guard reads)
static bool coop_crosshair_point(uint32_t obj, float out[3], float *dist);
static void coop_squad_donor_step();   // squad donor (lobby step 4, part 3)
static void coop_load_recv(uint32_t seq);   // the loading barrier (day 15)
static void coop_checkpoint_section();      // a checkpoint section began (day 15)
static void coop_part_step();               // a level part the partner has and we do not (playtest #4)
static void coop_hm_hit(uint32_t enemy, int who);   // HIT MARKER (owner 20261115): who 1 = the host player, 2 = the joiner
static volatile int g_coop_teardown;        // the session ended inside a level: the level is being quit (until the next load)
static uint32_t g_jhit_n; static float g_jhit_dmg;   // host: the JOINER clone's hits on enemies / their damage (playtest #6)
static uint32_t g_own_brains[4]; static int g_own_brains_n;   // the brains our own spawn popped (the clone, the self model)
static uint32_t g_cp_sec, g_cp_peer_sec;    // level-part sections started since the level load: ours / the partner's highest
static uint64_t g_part_ms;                  // when our last section (level part) was set up
static float g_cp_tpos[3]; static bool g_cp_tpos_ok;
static uint32_t g_fj_obj; static uint64_t g_fj_ms;   // host: the last trigger its test fired at the JOINER's feet
static uint64_t g_part_by_joiner_ms;        // host: when a level-part switch was requested by the JOINER's position (its trigger test pass)
static float g_part_by_joiner_at[3];   // where the last synced trigger stands (who reached it: the closer player)
static void coop_cp_recv(const uint8_t *raw, int len);
static void coop_real_cp_recv(const uint8_t *raw, int len);
static void coop_real_cp_step();
static void coop_hm_recv(const uint8_t *raw, int len);
static void coop_hm_step();
static volatile uint64_t g_cp_popup_ms;      // when this copy last showed the checkpoint popup (coop_loc_lookup)
static int g_cp_who; static uint64_t g_cp_ms;   // who fired the last trigger (1 host, 2 joiner) and when
static uint16_t g_cp_req_id[8]; static uint64_t g_cp_req_ms[8]; static int g_cp_req_w;   // joiner: its recent own requests
static void coop_quit_recv();               // the partner left the session (day 15)
static void coop_quit_step();
static void coop_exit_recv(const uint8_t *raw, int len);   // the shared level exit (day 15)
static void coop_exit_step(uint32_t obj);
static void coop_exit_reset();
static void install_exit_sync();
extern volatile uint32_t g_exit_by_joiner;
static void coop_load_keepalive();
static uint32_t __stdcall coop_load_gate(uint32_t world);
extern volatile uint32_t g_orig_load;
extern uint32_t g_load_seq;   // level loads started (the loading barrier counts them)
static bool coop_spawn_own(const float *at, const char *where);   // the clone as its own entity (step 4, part 4)
static bool coop_own_spawn_on();
static bool coop_safe_copy(void *dst, uint32_t src, size_t n);
static void coop_downed_test_fire(uint32_t pobj);
static void coop_update_gates(void);
static void coop_downed_lifestate(void);
static void coop_aggro_log(void);
static void coop_clone_gunshot(void);
static EnemyRay g_enray_out[64]; static int g_enray_out_n;          // host: this frame's enemy pellets
static EnemyRay g_enray_in[128]; static uint64_t g_enray_ms[128]; static int g_enray_r, g_enray_w;   // joiner
static uint32_t g_enray_stats[4];   // sent, received, used by puppets, puppet pellets without a ray
static bool is_enemy(uint32_t b, uint32_t pobj);
static uint32_t enemy_slots(uint32_t out[16]);
static uint32_t g_kill_dbg_victim; static int g_kill_dbg_dmg;
static ShotRay g_kill_ray; static int g_kill_ray_on;   // enemy sync: one forced pellet (see coop_enemy_tick)
uint32_t g_clone_rec = 0;    // the fresh-construct clone record (the tail sets it)
uint32_t g_real_rec = 0;     // unit0, the real soldier (the tail sets it)

// Stage 2 capture/pilot globals (unchanged since the earlier rounds).
volatile float g_spawn_xmm[4];
volatile uint32_t g_spawn_edi;
volatile uint32_t g_spawn_args;
volatile uint32_t g_level_resume;
volatile uint32_t g_loop_resume;
volatile uint32_t g_level_obj;
volatile uint32_t g_level_frame;
volatile uint32_t g_s2_a1, g_s2_a2, g_s2_a3, g_s2_a4;
volatile void *g_s2_fn;
volatile uint32_t g_s2_alloc_size, g_s2_alloc_ret;
// The world-register thunk's channels (pilot 11: FUN_001ce1f0 = stdcall RET 0x4;
// EAX = the body, stack arg = the world value; the callee cleans its arg).
volatile uint32_t g_wreg_body, g_wreg_world;
volatile void *g_wreg_fn = (volatile void *)0x001CE1F0;   // FUN_001ce1f0 (game .text is flat-mapped)
volatile bool g_coop_clone;
// The clone's drawn world position (set when the clone lands; the overlay tracks it).
extern "C" volatile float g_coop_clone_pos[3];
volatile float g_coop_clone_pos[3] = {0, 0, 0};
volatile float g_gunmark[12];
volatile int g_gunmark_on;
volatile float g_gunmodel[30];
volatile float g_clone_mvp[16];
volatile int g_clone_mvp_ok;
extern "C" volatile uint32_t g_coop_clone_on;
volatile uint32_t g_coop_clone_on = 0;
}

namespace {
constexpr uint32_t kSite = 0x001EC450;      // FUN_001ec450, player update (this-call)
constexpr uint32_t kSpawnSite = 0x001DF340; // FUN_001df340, entity spawn
constexpr uint32_t kLevelSite = 0x001EB440; // FUN_001eb440, level-load driver
constexpr uint32_t kLoopTailSite = 0x001EB544;
const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
const uint8_t kSpawnExpect[12] = {0x8B, 0x87, 0xB4, 0x79, 0x00, 0x00, 0x8B, 0x8F, 0xB8, 0x79, 0x00, 0x00};
const uint8_t kLevelExpect[5] = {0x55, 0x8B, 0x6C, 0x24, 0x08};
const uint8_t kLoopTailExpect[5] = {0x8B, 0x45, 0x34, 0x33, 0xFF};

uint32_t g_coop_resume, g_spawn_resume, g_level_out, g_loop_out, g_interp_out;
uint32_t g_coop_calls, g_coop_log_every = 120;
std::atomic<uint32_t> g_spawn_calls{0};
float cpu_of(uint32_t addr) { return *(float *)(uintptr_t)addr; }

// Naked helpers used from the bodies above (the Stage 2 pilot's queue/register).
void stage2_queue_loop();
void stage2_wreg();

} // anon

// ---- Pilot 10: lazy group-body clone. The level-load tail is TOO EARLY: the
// external slot array (ptr value at mgr+0x79BC) is still all-null at that point;
// the enemy groups fill during gameplay (their bodies spawn on activation). So
// this runs per frame until the first live slot appears, then clones ONCE.
// v1's crash was a wrong-address write (mgr+0x79CC, an unrelated field); the
// registration here goes into the external array only. ----
void coop_thunk_wreg();    // pilot 11: EAX = body, push world, CALL FUN_001ce1f0
void coop_thunk_regcap();  // pilot 12: FUN_001ce1f0 entry capture (body/ret/world)
void coop_thunk_pawncap(); // pilot 12b: FUN_001df440 pawn-def capture
void coop_thunk_factory(); // pilot 13b: call the pawn factory
static uint32_t g_p10_done = 0;   // 0 = waiting, 1 = cloned + registered
static uint32_t g_p10_tries = 0;
static uint32_t g_p10_gdst = 0;   // the registered clone body (the observer reads it)
// SELF MODEL (owner, play 20261114_005119: "while in spectator mode, the downed player cant see his model
// crouching"): the local player is first-person only - BLACK has no body for it. A second chained factory
// pawn, LOCAL ONLY on each copy, stands for our own player: parked out of sight while we are up, crouched
// head-down at the spot where we went down while we are downed. It is no enemy (no id, no sync, no AI,
// no damage, never recycled/retired). Env COOP_SELF_MODEL=0 = off. Read by the perception thunk too.
extern "C" volatile uint32_t g_coop_self_body;
volatile uint32_t g_coop_self_body = 0;
static uint32_t g_self_link, g_self_link_src;
static float g_self_park[3];
static inline bool coop_is_self(uint32_t b) { return b && b == g_coop_self_body; }
// Pilot 13e: the factory-replay def cache (the last natural spawn's full def,
// captured by coop_log_pawn_def — the runtime links are refreshed from a live def,
// never stale), plus the replay state.
volatile uint32_t g_defcache[16] = {};
static void coop_pilot10_try(void)
{
    // Pilot 13f: the factory call chains at the natural spawn #4 ENTRY (inside
    // coop_log_pawn_def - the freshest def links). This per-frame hook only
    // reports the armed state once; the observer + LAN drive use g_p10_gdst.
    if (g_coop_clone && g_p13_on && !g_p10_tries) {
        g_p10_tries = 1;
        logf("coop: p13f: armed - the chained factory call fires at spawn #4 entry");
    }
}

// Pawn body layout (aimdump1 scan, 71 dumps of 5 seats; same basis family as the
// player object): +0x70 / +0x80 / +0x90 = BODY pose rows L / U / F (yaw only, y=0
// on L and F), +0xA0 = feet; +0xD0 / +0xE0 / +0xF0 = LOOK basis L / U / F (F carries
// the PITCH, y down to -0.45), +0x100 = eye; +0x110..+0x14F = animated root 4x4.
// Conventions verified on the samples: L = (Fz, 0, -Fx) / |..|, U = F x L.
static inline float dot3(const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void rd3(uint32_t a, float *o) { for (int k = 0; k < 3; k++) o[k] = *(volatile float *)(uintptr_t)(a + 4 * k); }
static void wr3(uint32_t a, const float *v) { for (int k = 0; k < 3; k++) *(volatile float *)(uintptr_t)(a + 4 * k) = v[k]; }

// Milestone 2: stand the adopted factory body at (bx,by,bz) = feet on the floor.
// fwd (optional, the controlling player's view forward incl. pitch): sets the body
// FACING (pose rows, horizontal) and the AIM look basis (+0xD0..+0xF0, pitched).
// The animated root block (+0x110..+0x14F) is taken from the nearest natural pawn
// RELATIVE to that donor's body frame and re-applied in ours, so the rendered body
// faces our way (not the donor's). Returns the donor (0 = none found).
static uint32_t g_last_donor;   // diagnostics: the donor of the last coop_place_body
static uint32_t coop_place_body(uint32_t body, float bx, float by, float bz, const float *fwd = nullptr)
{
    if (fwd) {
        float F[3] = {fwd[0], fwd[1], fwd[2]};
        float fl = sqrtf(dot3(F, F));
        float hl = sqrtf(F[0] * F[0] + F[2] * F[2]);
        if (fl > 1e-3f && hl > 1e-3f) {
            F[0] /= fl; F[1] /= fl; F[2] /= fl;
            float Fh[3] = {fwd[0] / hl, 0.0f, fwd[2] / hl};     // horizontal forward
            float L[3] = {Fh[2], 0.0f, -Fh[0]};                 // horizontal left
            float Uw[3] = {0.0f, 1.0f, 0.0f};
            // body pose: yaw only
            wr3(body + 0x70, L); wr3(body + 0x80, Uw); wr3(body + 0x90, Fh);
            // look/aim basis: pitched forward, U = F x L
            float U[3] = {F[1] * L[2] - F[2] * L[1], F[2] * L[0] - F[0] * L[2], F[0] * L[1] - F[1] * L[0]};
            wr3(body + 0xD0, L); wr3(body + 0xE0, U); wr3(body + 0xF0, F);
        }
    }
    // SPEED-SPIKE fix: the game's speed = (+0xA0 - +0x190)/dt inside the pawn tick, with
    // +0x190 snapshotted at tick start. A correction landing between that snapshot and
    // the measurement counted as movement (+0x2DC spikes to ~40 m/s at walk onset).
    // Shift +0x190 by the same delta so our corrections never read as velocity (a
    // between-ticks correction is unaffected: +0x190 is re-snapshotted next tick).
    {
        float ox = *(volatile float *)(uintptr_t)(body + 0xA0);
        float oy = *(volatile float *)(uintptr_t)(body + 0xA4);
        float oz = *(volatile float *)(uintptr_t)(body + 0xA8);
        *(volatile float *)(uintptr_t)(body + 0x190) += bx - ox;
        *(volatile float *)(uintptr_t)(body + 0x194) += by - oy;
        *(volatile float *)(uintptr_t)(body + 0x198) += bz - oz;
    }
    *(volatile float *)(uintptr_t)(body + 0xA0) = bx;
    *(volatile float *)(uintptr_t)(body + 0xA4) = by;
    *(volatile float *)(uintptr_t)(body + 0xA8) = bz;
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
    uint32_t arr = (mgr >= 0x80000000 && mgr < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(mgr + 0x79BC) : 0;
    // prefer the nearest SAME-floor pawn; fall back to the nearest on any floor
    // (m2a: the host walked to y 4.4 where no natural pawn stands -> no donor ->
    // stale render block). The root shift below carries the ~1.3 m root height.
    uint32_t donor = 0, any = 0; float bestd = 1e30f, besta = 1e30f;
    if (arr >= 0x80000000 && arr < 0x84000000) {
        for (int i = 0; i < 16; i++) {
            uint32_t b = *(volatile uint32_t *)(uintptr_t)(arr + i * 4);
            if (b < 0x80000000 || b >= 0x84000000 || b == body) continue;
            const volatile float *a = (const volatile float *)(uintptr_t)(b + 0xA0);
            float dx = a[0] - bx, dz = a[2] - bz, d = dx * dx + dz * dz;
            if (d < besta) { besta = d; any = b; }
            if (fabsf(a[1] - by) > 1.0f) continue;          // same floor
            if (d < bestd) { bestd = d; donor = b; }
        }
    }
    if (!donor) donor = any;
    g_last_donor = donor;
    if (donor) {
        // donor body frame Bd (rows L,U,F) and ours Bo; row-vector convention:
        // local = world . Bd^T, world' = local . Bo (Bd orthonormal)
        float Bd[3][3], Bo[3][3], dfeet[3];
        for (int r = 0; r < 3; r++) { rd3(donor + 0x70 + 0x10 * r, Bd[r]); rd3(body + 0x70 + 0x10 * r, Bo[r]); }
        rd3(donor + 0xA0, dfeet);
        float blk[16];
        memcpy(blk, (const void *)(uintptr_t)(donor + 0x110), 0x40);   // keeps the w lanes
        for (int i = 0; i < 4; i++) {
            float v[3] = {blk[4 * i], blk[4 * i + 1], blk[4 * i + 2]};
            if (i == 3) { v[0] -= dfeet[0]; v[1] -= dfeet[1]; v[2] -= dfeet[2]; }  // root offset
            float loc[3] = {dot3(v, Bd[0]), dot3(v, Bd[1]), dot3(v, Bd[2])};
            for (int k = 0; k < 3; k++)
                blk[4 * i + k] = loc[0] * Bo[0][k] + loc[1] * Bo[1][k] + loc[2] * Bo[2][k];
        }
        blk[12] += bx; blk[13] += by; blk[14] += bz;
        // play 20261114_083833 (host crash 0x69955 in the collision SAT FUN_00071cc0, garbage axis count): the
        // donor's root carried a NaN in a w lane (+0x14C); the game's SSE math multiplies that lane, and NaN
        // poisons the result. Junk w lanes are normal (finite); a non-finite one becomes 0 here.
        for (int i = 0; i < 16; i++) if (!std::isfinite(blk[i])) blk[i] = 0.0f;
        if (!std::isfinite(blk[12]) || !std::isfinite(blk[13]) || !std::isfinite(blk[14])) { blk[12] = bx; blk[13] = by + 1.2f; blk[14] = bz; }
        memcpy((void *)(uintptr_t)(body + 0x110), blk, 0x40);
    }
    g_coop_clone_pos[0] = bx; g_coop_clone_pos[1] = by; g_coop_clone_pos[2] = bz;
    return donor;
}

// Upper-body AIM. FUN_001dfd90 (per-tick, every pawn and the player) rebuilds the
// look basis +0xD0..+0x10F from the LINK OBJECT at [body+0x324]: yaw = [o+0x18]
// (deg; FUN_00014fe0 = sin/cos Y-rotation), pitch = [o+0x1C] (deg; FUN_00016260).
// Writers 0x1dff82 / 0x1dffb6 (watch, run 56). Same units as the player controller
// (+0x18 / +0x1C) = the packet's yaw/pitch. Guard: never write a link object that
// a natural pawn shares (it would steer that NPC too).
// aim57: the link objects ARE the 4 character-class objects (8014f220 / 8014d240 /
// 8014b260 / 80149280, 0x1FE0 apart = the factory's a1 roster); ours shared spawn
// #4's class object with natural #4 -> give our body a PRIVATE copy (self-pointers
// rebased, +0x8C back-pointer -> our body) and drive yaw/pitch there.
static int g_shot_rays_only = 1;   // the clone fires only from received rays (set by the fire block; COOP_CLONE_TRIGGER=1 clears it)
static float g_aim_pt[3];   // COOP_AIM_AT=x,y,z (test pilot point)
static uint32_t g_unit_arr, g_unit_cnt;       // this level's unit array (our swapped-in copy) - a unit's INDEX is the same on both copies
static volatile uint32_t g_coop_trig_latch;   // the peer's trigger was down in some packet since the last frame
static uint32_t g_clone_w[64];                // the weapon objects we made for the clone, by type (reset per level)
static uint32_t g_aim_link;   // our private link object (0 = not made yet)
static uint32_t g_aim_shared_checked, g_aim_shared_with;   // coop_aim_body's first-look state (reset per level)
static uint32_t g_aim_link_src;   // the class object it was copied from (the AI keeps writing that one)
// A private link is a FROZEN copy: fields the AI keeps updating on the class object go
// stale. +0xD8 = the voice-line index FUN_0017d130 uses as table[link+0xD8] (pain/death
// barks from the pawn tick); a stale copy held a pointer -> wild read (dn144 crash).
// Each frame: take the class object's current value when it is a sane small index,
// else 0 (a valid table entry).
static void link_sync_voice(uint32_t priv, uint32_t src)
{
    if (!priv) return;
    int v = (src >= 0x80000000 && src < 0x84000000) ? *(volatile int *)(uintptr_t)(src + 0xD8) : 0;
    *(volatile int *)(uintptr_t)(priv + 0xD8) = (v >= 0 && v < 64) ? v : 0;
}
static uint32_t coop_make_private_link(uint32_t body, uint32_t src)
{
    const uint32_t SZ = 0x1FE0;
    uint32_t cp = (uint32_t)(uintptr_t)_aligned_malloc(SZ, 16);
    if (!cp) return 0;
    memcpy((void *)(uintptr_t)cp, (const void *)(uintptr_t)src, SZ);
    int reb = 0;
    for (uint32_t off = 0; off < SZ; off += 4) {
        uint32_t v = *(volatile uint32_t *)(uintptr_t)(cp + off);
        if (v >= src && v < src + SZ) { *(volatile uint32_t *)(uintptr_t)(cp + off) = cp + (v - src); reb++; }
    }
    uint32_t back = *(volatile uint32_t *)(uintptr_t)(cp + 0x8C);
    if (back >= 0x801E3900 && back < 0x801EB900) *(volatile uint32_t *)(uintptr_t)(cp + 0x8C) = body;
    *(volatile uint32_t *)(uintptr_t)(body + 0x324) = cp;
    logf("coop: aim: PRIVATE link %08x <- class %08x (0x%x B, %d self-ptrs rebased, +8C was %08x)",
         cp, src, SZ, reb, back);
    if (body == g_p10_gdst) {   // ISOLATION survey: pointers the clone still SHARES with the donor's class
        char buf[1600]; int o = snprintf(buf, sizeof buf, "coop: clone isolation: shared pointers in the private link:");
        int n = 0;
        for (uint32_t off = 0; off < SZ && o < 1540; off += 4) {
            uint32_t v = *(volatile uint32_t *)(uintptr_t)(cp + off);
            if (v < 0x80000000 || v >= 0x84000000 || v == body) continue;
            o += snprintf(buf + o, sizeof buf - o, " +%X=%08x", off, v); n++;
        }
        logf("%s (%d)", buf, n);
        // which body owns each shared object? scan the donor body (back-pointer +0x8C) and
        // the clone body for the same value (or an object that contains it, within 0x200)
        uint32_t donor = back;
        for (uint32_t off = 0; off < SZ; off += 4) {
            uint32_t v = *(volatile uint32_t *)(uintptr_t)(cp + off);
            if (v < 0x80000000 || v >= 0x84000000 || v == body) continue;
            char who[200]; int wo = 0;
            for (int pass = 0; pass < 2; pass++) {
                uint32_t bb = pass ? body : donor;
                if (bb < 0x80000000 || bb >= 0x84000000) continue;
                for (uint32_t f = 0; f < 0x400; f += 4) {
                    uint32_t fv = *(volatile uint32_t *)(uintptr_t)(bb + f);
                    if (fv >= 0x80000000 && fv < 0x84000000 && v >= fv && v < fv + 0x200 && wo < 170)
                        wo += snprintf(who + wo, sizeof who - wo, " %s+%X%s", pass ? "CLONE" : "donor", f, v == fv ? "" : "(inside)");
                }
            }
            if (wo) logf("coop: clone isolation: +%X=%08x owned by%s", off, v, who);
        }
        // ISOLATE (owner: "have the clone with its own class, animation node and behaviour
        // tree to prevent conflict"): give the clone PRIVATE copies of the dynamic objects its
        // AI copy shares with the donor's live AI - nav path nodes +0xA4..+0x11C (the donor's
        // brain reallocates them: dangling otherwise), the 0x80Bxxxxx state objects and the
        // brain/group record. Each copy = [v - 0x200, v + 0x600) so negative and positive
        // offsets stay inside our own memory. Static data (0x8145xxxx assets), the clone's own
        // body and the factory donor body (+0xCE8) stay shared. Env COOP_CLONE_ISOLATE=0 = off.
        char e[8] = {0};
        bool iso = !(GetEnvironmentVariableA("COOP_CLONE_ISOLATE", e, sizeof e) && e[0] == '0');
        if (iso) {
            struct Map { uint32_t from, to; } map[64]; int nm = 0, nrep = 0;
            for (uint32_t off = 0; off < SZ; off += 4) {
                bool pick = (off >= 0xA4 && off <= 0x11C) || off == 0x910 || off == 0x970 || off == 0x9E0 ||
                            off == 0xD20 || off == 0xD8C || off == 0x1EF0 || off == 0x1FA0;
                if (!pick) continue;
                volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)(cp + off);
                uint32_t v = *slot;
                if (v < 0x80000000 || v >= 0x84000000) continue;
                uint32_t to = 0;
                for (int k = 0; k < nm; k++) if (map[k].from == v) to = map[k].to;
                if (!to && nm < 64) {
                    uint8_t *mem = (uint8_t *)_aligned_malloc(0x800, 16);
                    if (!mem) continue;
                    if (!coop_safe_copy(mem, v - 0x200, 0x800)) { _aligned_free(mem); continue; }
                    to = (uint32_t)(uintptr_t)mem + 0x200;
                    map[nm++] = {v, to};
                }
                if (to) { *slot = to; nrep++; }
            }
            logf("coop: clone isolation: %d shared pointers repointed to %d private copies", nrep, nm);
        }
    }
    return cp;
}

static void coop_aim_body(uint32_t body, float yaw, float pitch)
{
    if (g_aim_link) {   // private link already installed: just drive it
        uint32_t o = g_aim_link;
        if (*(volatile uint32_t *)(uintptr_t)(body + 0x324) != o) {
            static int relinks;
            if (relinks++ < 5) logf("coop: aim: body +324 changed to %08x - relinking", *(volatile uint32_t *)(uintptr_t)(body + 0x324));
            *(volatile uint32_t *)(uintptr_t)(body + 0x324) = o;
        }
        float py = *(volatile float *)(uintptr_t)(o + 0x18), pp = *(volatile float *)(uintptr_t)(o + 0x1C);
        // +0x10 = BODY FACING yaw (FUN_001f08d0: FUN_00014d80([o+0x10]*deg2rad) ->
        // FUN_001ce6c0 sets the drawn transform); +0x18/+0x1C = look yaw/pitch
        // (FUN_001dfd90). The AI keeps +0x10 = +0x18 (0x1d2d2e); our frozen
        // private copy needs it written or the body never turns (aim60).
        // owner: "the clone movement still tied to the other player viewport" ->
        // +0x10 (BODY facing) is NOT the view any more: coop_loco_body sets it from
        // the movement direction (held when idle). Only look yaw/pitch = view here.
        // DOWNED POSE (owner: "a death animation when a player is downed, restored when respawned"): BLACK's
        // deaths are ragdolls (no animation call during a death, da1/da2), which a player who gets up again
        // cannot use. While the partner this clone stands for is downed, the clone CROUCHES (+0x40 -> body
        // +0x3A1, the anim input "crouching") and hangs its head (aim pitch 70 deg = the anim input
        // "aim_pitch" + our spine bend); the respawn (peer downed -> 0) gives the drive back.
        static int downed_pose;
        extern volatile uint32_t g_coop_peer_downed;
        if (g_coop_peer_downed) {
            pitch = 70.0f;
            *(volatile uint8_t *)(uintptr_t)(o + 0x40) = 1;
            // sm3: the anim input "crouching" reads BODY +0x3A1; the copy from link +0x40 is the AI's job and
            // a private link has no AI -> write the body byte too (the self model stood upright without it)
            *(volatile uint8_t *)(uintptr_t)(body + 0x3A1) = 1;
            if (!downed_pose) { downed_pose = 1; logf("coop: downed pose: the partner is down - their clone %08x slumps (crouch, head down)", body); }
        } else {
            // play 20261114_100655 ("the host sees the joiner always in crouch mode"): the private link is a FROZEN
            // copy of the donor's class object - copied while that soldier crouched, +0x40 stayed 1 and the pawn
            // tick (FUN_001f08d0) copies it into body +0x3A1 every frame. The player we stand for never crouches
            // on its own here (the packet has no crouch): standing unless downed, every frame.
            if (*(volatile uint8_t *)(uintptr_t)(o + 0x40) || *(volatile uint8_t *)(uintptr_t)(body + 0x3A1)) {
                static int cl;
                if (cl++ < 5 && !downed_pose) logf("coop: clone %08x was crouching (link +0x40 %u, body +0x3A1 %u - a frozen donor state) - standing it up",
                                                   body, *(volatile uint8_t *)(uintptr_t)(o + 0x40), *(volatile uint8_t *)(uintptr_t)(body + 0x3A1));
            }
            *(volatile uint8_t *)(uintptr_t)(o + 0x40) = 0;
            *(volatile uint8_t *)(uintptr_t)(body + 0x3A1) = 0;
            if (downed_pose) {
                downed_pose = 0;
                logf("coop: downed pose: the partner is back - clone %08x restored", body);
            }
        }
        *(volatile float *)(uintptr_t)(o + 0x18) = yaw;
        *(volatile float *)(uintptr_t)(o + 0x1C) = pitch;
        { extern volatile float g_aim_pitch_deg, g_aim_yaw_deg; g_aim_pitch_deg = pitch; g_aim_yaw_deg = yaw; }   // skeleton bend input
        static uint32_t an;
        if ((an++ % 120) == 0)
            logf("coop: aim: private %08x write yaw %.1f pitch %.1f (found %.1f %.1f) | look F %.2f %.2f %.2f",
                 o, yaw, pitch, py, pp, *(volatile float *)(uintptr_t)(body + 0xF0),
                 *(volatile float *)(uintptr_t)(body + 0xF4), *(volatile float *)(uintptr_t)(body + 0xF8));
        return;
    }
    uint32_t o = *(volatile uint32_t *)(uintptr_t)(body + 0x324);
    uint32_t &shared_checked = g_aim_shared_checked, &shared_with = g_aim_shared_with;
    if (!shared_checked && o >= 0x80000000 && o < 0x84000000) {
        shared_checked = 1;
        uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
        uint32_t arr = (mgr >= 0x80000000 && mgr < 0x84000000)
            ? *(volatile uint32_t *)(uintptr_t)(mgr + 0x79BC) : 0;
        if (arr >= 0x80000000 && arr < 0x84000000)
            for (int i = 0; i < 16; i++) {
                uint32_t b = *(volatile uint32_t *)(uintptr_t)(arr + i * 4);
                if (b < 0x80000000 || b >= 0x84000000 || b == body) continue;
                uint32_t bo = *(volatile uint32_t *)(uintptr_t)(b + 0x324);
                logf("coop: aim: pawn[%d] %08x link %08x yaw %.1f pitch %.1f", i, b, bo,
                     (bo >= 0x80000000 && bo < 0x84000000) ? *(volatile float *)(uintptr_t)(bo + 0x18) : 0.0f,
                     (bo >= 0x80000000 && bo < 0x84000000) ? *(volatile float *)(uintptr_t)(bo + 0x1C) : 0.0f);
                if (bo == o) shared_with = b;
            }
        logf("coop: aim: OURS %08x link %08x yaw %.1f pitch %.1f -> %s", body, o,
             *(volatile float *)(uintptr_t)(o + 0x18), *(volatile float *)(uintptr_t)(o + 0x1C),
             shared_with ? "SHARED with a natural pawn -> making a private copy" : "private - driving yaw/pitch");
        // play 20261112 (Veblensk, first co-op run of that level): on the JOINER the donor's class link
        // was not shared with a live pawn at this first check -> no private copy -> g_aim_link stayed
        // 0 for the whole session, and everything gated on it was off: clone aim / fire / weapon,
        // shot catch-up, explosion replay, our grenade tracking and the ENTIRE enemy sync tick.
        // The clone always gets its private link now (a class object is shared sooner or later).
        g_aim_link_src = o; g_aim_link = coop_make_private_link(body, o);
        if (g_aim_link) return;
        logf("coop: aim: private link could not be made for %08x (class %08x) - driving the class link", body, o);
    }
    if (shared_with || o < 0x80000000 || o >= 0x84000000) return;
    float py = *(volatile float *)(uintptr_t)(o + 0x18), pp = *(volatile float *)(uintptr_t)(o + 0x1C);
    *(volatile float *)(uintptr_t)(o + 0x18) = yaw;    // look only; body facing = movement (coop_loco_body)
    *(volatile float *)(uintptr_t)(o + 0x1C) = pitch;
    static uint32_t an;
    if ((an++ % 120) == 0)
        logf("coop: aim: link %08x write yaw %.1f pitch %.1f (found %.1f %.1f) | look F now %.2f %.2f %.2f",
             o, yaw, pitch, py, pp, *(volatile float *)(uintptr_t)(body + 0xF0),
             *(volatile float *)(uintptr_t)(body + 0xF4), *(volatile float *)(uintptr_t)(body + 0xF8));
}

// LOCOMOTION (owner request: idle at 0 velocity, run anim speed = player speed).
// Link-object fields (cls66 dump of the live class object while natural #4 walks):
// +0x20 = desired speed (0 / 1.55 m/s), +0x84 = current speed (ramps 0..1.55),
// +0x60/+0x64/+0x68 and +0x70/+0x74/+0x78 = move direction (unit x/z). FUN_001f08d0
// feeds +0x60 * +0x20 * dt into physics only when body+0x3A7 == 0 (we set it), so
// these drive the ANIMATION without moving the body; position stays ours.
// Our private copy was snapshotted mid-walk (+0x20 = 1.55) = frozen walk state.
struct VelTrack { LARGE_INTEGER t; float x, z, vx, vz; int ok; };
static void vel_update(VelTrack &v, float x, float z)
{
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    if (v.ok) {
        float dt = (float)(now.QuadPart - v.t.QuadPart) / (float)f.QuadPart;
        if (dt > 0.004f && dt < 0.5f) {
            float ix = (x - v.x) / dt, iz = (z - v.z) / dt;
            if (ix * ix + iz * iz > 400.0f) { ix = 0; iz = 0; }   // teleport / respawn guard (>20 m/s)
            const float a = 0.25f;                                // EMA smoothing
            v.vx += a * (ix - v.vx); v.vz += a * (iz - v.vz);
        } else if (dt <= 0.004f) return;                          // same frame: keep the old sample
    }
    v.t = now; v.x = x; v.z = z; v.ok = 1;
}
// CLONE INTERPOLATION (owner 20261115: "the joiner might need animation interpolation for the host's clone, it is a bit
// jittering"). The clone stood on the partner's LATEST packet each frame: at 60 fps some frames brought no packet (the
// clone stopped) and some brought two (it jumped), and the walk animation follows the body's own per-frame movement
// (FUN_001DFD90: (+0xA0 - +0x190) / dt), so it stuttered too. Now each packet carries the sender's clock (tms, QPC ms);
// the receiver keeps the last 32 snapshots and shows the partner COOP_INTERP_MS (80) in the past, blended between the
// two snapshots around that moment (position, yaw on the short arc, pitch); a late packet is extrapolated at most 100 ms;
// a jump over 4 m (respawn, checkpoint move) snaps. Clock offset = the smallest (local - sender) of the buffer.
static double coop_qpc_ms()
{
    // ms since this process first asked. itpd: the raw counter in ms (~2.7e8) lost its fraction in 32-ms steps - D3D9
    // puts the game thread's x87 FPU in single precision, so doubles are computed as floats there; the interpolated time
    // then stood still every other frame. Small numbers keep sub-ms steps (< 1e6 ms for a long session).
    static LARGE_INTEGER base, f;
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    if (!base.QuadPart) { base = c; QueryPerformanceFrequency(&f); }
    return (double)((c.QuadPart - base.QuadPart) * 1000 / f.QuadPart) + (double)((c.QuadPart - base.QuadPart) * 1000 % f.QuadPart) / (double)f.QuadPart;
}
struct Snap { double st, lt; float x, y, z, yaw, pitch; };
static Snap g_snap[32]; static uint32_t g_snap_w;
static void coop_snap_push(double st, const float pos[3], float yaw, float pitch)
{
    if (!(st > 0.0)) return;
    if (g_snap_w && st <= g_snap[(g_snap_w - 1) & 31].st) return;   // old / repeated packet (UDP order)
    if (g_snap_w) {   // itpr1: the sender's state changes at the game's 30 Hz but its packets go out more often - an
                      // unchanged copy made every other snapshot pair still (86 of 180 frames standing); skip it (but
                      // keep one every 100 ms so a standing partner stays current)
        const Snap &l = g_snap[(g_snap_w - 1) & 31];
        if (l.x == pos[0] && l.y == pos[1] && l.z == pos[2] && l.yaw == yaw && l.pitch == pitch && st - l.st < 100.0) return;
    }
    Snap &n = g_snap[g_snap_w & 31];
    n.st = st; n.lt = coop_qpc_ms(); n.x = pos[0]; n.y = pos[1]; n.z = pos[2]; n.yaw = yaw; n.pitch = pitch;
    g_snap_w++;
}
static float coop_lerp_deg(float a, float b, float t)
{
    float d = fmodf(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}
// The partner's state at moment rt (the sender's clock) from the snapshot buffer: x y z yaw pitch.
static void coop_snap_sample(double rt, float out[5])
{
    uint32_t n = g_snap_w < 32 ? g_snap_w : 32;
    const Snap *a = nullptr, *b = nullptr;
    for (uint32_t i = 0; i + 1 < n; i++) {
        const Snap &q1 = g_snap[(g_snap_w - 1 - i) & 31], &q0 = g_snap[(g_snap_w - 2 - i) & 31];
        if (q0.st <= rt && rt <= q1.st) { a = &q0; b = &q1; break; }
    }
    float t;
    if (!a) {
        const Snap &q1 = g_snap[(g_snap_w - 1) & 31], &q0 = g_snap[(g_snap_w - 2) & 31];
        if (rt < g_snap[(g_snap_w - n) & 31].st) { a = b = &g_snap[(g_snap_w - n) & 31]; t = 0.0f; }   // older than the buffer
        else {                                                                                           // newer: extrapolate a little
            a = &q0; b = &q1;
            double span = q1.st - q0.st;
            t = span > 1.0 ? (float)(((std::min)(rt, q1.st + 100.0) - q0.st) / span) : 1.0f;
        }
    } else {
        double span = b->st - a->st;
        t = span > 1.0 ? (float)((rt - a->st) / span) : 1.0f;
    }
    float dx = b->x - a->x, dy = b->y - a->y, dz = b->z - a->z;
    if (dx * dx + dy * dy + dz * dz > 16.0f || b->st - a->st > 300.0) t = t < 0.5f ? 0.0f : 1.0f;   // a jump / a gap: snap
    out[0] = a->x + dx * t; out[1] = a->y + dy * t; out[2] = a->z + dz * t;
    out[3] = coop_lerp_deg(a->yaw, b->yaw, t);
    out[4] = a->pitch + (b->pitch - a->pitch) * t;
}
// out: x y z yaw pitch; vel: the partner's velocity (x, z) over the last 100 ms of the shown motion. false = no usable
// history (use the raw packet). strf1 (owner: "strafing left no longer turns the legs to the movement"): the velocity of
// the single snapshot pair alternated 0.6 / 3.2 m/s (the sender's state moves at 30 Hz, its packets come at 60) and
// dipped under the legs' 0.15 m/s idle threshold -> the legs swung back to the aim. Over 100 ms it is steady.
static bool coop_interp_remote(float out[5], float vel[2])
{
    static int mode = -1; static float delay = 80.0f;
    if (mode < 0) {
        char e[16] = {0};
        mode = !(GetEnvironmentVariableA("COOP_INTERP", e, sizeof e) && e[0] == '0');
        if (GetEnvironmentVariableA("COOP_INTERP_MS", e, sizeof e)) delay = (float)atof(e);
        logf("coop: clone interpolation %s (%.0f ms behind)", mode ? "ON" : "OFF", delay);
    }
    uint32_t n = g_snap_w < 32 ? g_snap_w : 32;
    if (!mode || n < 2) return false;
    double off = 1e300;
    for (uint32_t i = 0; i < n; i++) { const Snap &q = g_snap[(g_snap_w - 1 - i) & 31]; if (q.lt - q.st < off) off = q.lt - q.st; }
    double rt = coop_qpc_ms() - off - delay;   // the moment to show, on the sender's clock
    coop_snap_sample(rt, out);
    float back[5];
    coop_snap_sample(rt - 100.0, back);
    float dx = out[0] - back[0], dz = out[2] - back[2];
    bool jump = dx * dx + dz * dz > 16.0f;
    vel[0] = jump ? 0.0f : dx * 10.0f;   // per second (100 ms)
    vel[1] = jump ? 0.0f : dz * 10.0f;
    return true;
}
// loco67 result: idle OK, but writing +0x20/+0x84 does NOT play the walk cycle - the
// anim uses the game's own speed: FUN_001dfd90 computes (+0xA0 - +0x190)/dt into
// +0x1B0 and +0x2D8/+0x2DC, and FUN_001f08d0 sets +0x190 = +0xA0 at tick START. Our
// position writes land between ticks -> in-tick displacement 0 -> idle always.
// So: after a 2 s settle (the spawn fall-impact window), CLEAR +0x3A7 so the game's
// physics moves the body by +0x60 * +0x20 * dt INSIDE the tick (= real speed for the
// anim); our per-frame feet write remains the drift correction.
static uint32_t coop_anim_blk(uint32_t body);
static void coop_loco_body(uint32_t body, float vx, float vz)
{
    uint32_t o = g_aim_link;              // only our PRIVATE link (never a shared class object)
    if (!o) return;
    static uint32_t calls;
    // loco68: physics on -> the game's own speed matched ours (+2D8 3.94 = 3.94), but
    // the clone DIED 1 s later (health 0, speed spike 37..62) in FUN_001df630's
    // landing/fall check. FUN_001dfd90 runs that check only if +0x3A3 != 0, and with
    // +0x3A3 == 0 it zeroes the vertical velocity +0x2E4 every tick. So: +0x3A3 = 0
    // (no gravity, no fall death - we own the height) and +0x3A7 = 0 (horizontal
    // physics step = real in-tick motion for the walk cycle). Re-asserted per frame.
    if (++calls >= 120) {
        *(volatile uint8_t *)(uintptr_t)(body + 0x3A3) = 0;
        *(volatile uint8_t *)(uintptr_t)(body + 0x3A7) = 0;
        if (calls == 120)
            logf("coop: loco: physics ENABLED on %08x (+3A7 = 0, +3A3 = 0 no gravity/fall) - health %.1f", body,
                 *(volatile float *)(uintptr_t)(body + 0x2F0));
        static int deadlog;
        if (*(volatile float *)(uintptr_t)(body + 0x2F0) <= 0.0f && deadlog++ < 3)
            logf("coop: loco: WARNING clone health %.1f (dead)", *(volatile float *)(uintptr_t)(body + 0x2F0));
    }
    float sp = sqrtf(vx * vx + vz * vz);
    float dx = 0, dz = 0;
    // owner (20261115, screenshot): "the clone's legs are always facing to the clone's right instead of forward" - a
    // standing partner read 0.16-0.19 m/s (drift in the smoothed positions), just over the old 0.15 dead zone, so the
    // legs followed that drift's direction (e.g. move -107, aim -30). Walking is 3-4.5 m/s: moving from 0.6, standing
    // again below 0.35 (hysteresis).
    static bool moving;
    moving = moving ? sp >= 0.35f : sp >= 0.6f;
    if (!moving) sp = 0.0f;               // dead zone = idle
    else { dx = vx / sp; dz = vz / sp; }
    { extern volatile int g_clone_idle; g_clone_idle = sp == 0.0f; }
    // BODY facing (+0x10, FUN_001f08d0 -> drawn transform) = MOVEMENT direction, held
    // when idle; independent of the viewport (look yaw/pitch stay in coop_aim_body).
    // Yaw convention from the samples: F = (sin yaw, 0, cos yaw) -> yaw = atan2(x, z).
    static float body_yaw = 1000.0f;      // 1000 = not set yet: start from the current value
    if (body_yaw > 999.0f) body_yaw = *(volatile float *)(uintptr_t)(o + 0x18);
    if (sp > 0.0f) {
        body_yaw = 57.2957795f * atan2f(dx, dz);
        // play219: while moving the legs followed the walk direction and the spine twist
        // (clamp +-80) could not reach an aim far off it (strafe / backpedal / looking
        // around). Default (COOP_STRAFE=0 = off): when the walk is > 60 deg off the aim, face the AIM
        // and let the move direction (+0x60) drive the locomotion.
        static int strafe = -1;
        if (strafe < 0) { char e[8] = {0}; strafe = !(GetEnvironmentVariableA("COOP_STRAFE", e, sizeof e) && e[0] == '0'); }   // default ON
        if (strafe) {
            float aim = *(volatile float *)(uintptr_t)(o + 0x18), dd = aim - body_yaw;
            while (dd > 180.0f) dd -= 360.0f;
            while (dd < -180.0f) dd += 360.0f;
            // owner (day 14, first mission): "the model doesn't go sideways when a player is strafing, the legs keep
            // pointing where the player aims". Legs follow the MOVE up to COOP_STRAFE_DEG (default 135 - sf3: a "strafe" moved 116 deg off the aim; a sideways
            // strafe turns the legs, the upper body keeps the aim within the spine twist); only a backpedal
            // beyond that keeps the legs on the aim. Was 60.
            static float lim = -1.0f;
            if (lim < 0.0f) { char e[16] = {0}; lim = GetEnvironmentVariableA("COOP_STRAFE_DEG", e, sizeof e) ? (float)atof(e) : 135.0f; }
            if (fabsf(dd) > lim) body_yaw = aim;
            // owner (play 20261114_151810, all weapons): "strafing to the right rotates the upper body of the clone
            // while it should stay on his player aim". The spine twist the bend asks = (aim - legs) - blade (the held
            // pose's gun yaw, ~-20, the Naszran SMG -78), capped at 90: legs on a sideways move asked 110 one way
            // (and up to 155 on a diagonal) and the upper body stopped short. Keep the legs as close to the move as
            // the twist allows: |aim - legs - blade| <= COOP_TWIST_ROOM (default 85, under the 90 cap).
            extern volatile int g_hold_ok; extern volatile float g_hold_blade;
            static float room = -1.0f;
            if (room < 0.0f) { char e[16] = {0}; room = GetEnvironmentVariableA("COOP_TWIST_ROOM", e, sizeof e) ? (float)atof(e) : 85.0f; }
            if (g_hold_ok && room > 0.0f) {
                float need = aim - body_yaw - g_hold_blade;
                while (need > 180.0f) need -= 360.0f;
                while (need < -180.0f) need += 360.0f;
                if (need > room) body_yaw += need - room;
                else if (need < -room) body_yaw += need + room;
                while (body_yaw > 180.0f) body_yaw -= 360.0f;
                while (body_yaw < -180.0f) body_yaw += 360.0f;
            }
        }
    }
    if (sp > 0.0f) {   // legs probe (every 0.5 s while moving): move direction, aim, legs, speed
        static uint64_t lt; static uint32_t ln;
        uint64_t nw = GetTickCount64();
        if (nw - lt >= 500 && ln < 200) { lt = nw; ln++; logf("coop: legs: move %.1f aim %.1f legs %.1f speed %.2f (v %.2f %.2f)", 57.2957795f * atan2f(dx, dz), *(volatile float *)(uintptr_t)(o + 0x18), body_yaw, sp, vx, vz); }
    }
    // --p13-face (solo visual checks only): while idle, keep the body on the (reversed)
    // look yaw so the clone faces the camera and its weapon is in view
    else if (g_cfg.p13_face) {
        // COOP_FACE_OFS=<deg> (diagnostics): stand side-on to the camera instead of facing it
        static float fofs = 1000.0f;
        if (fofs > 999.0f) { char e[16] = {0}; fofs = GetEnvironmentVariableA("COOP_FACE_OFS", e, sizeof e) ? (float)atof(e) : 0.0f; }
        body_yaw = *(volatile float *)(uintptr_t)(o + 0x18) + fofs;
    }
    else {
        // TURN IN PLACE (owner: "his legs need to rotate after a certain upper body
        // rotation angle"; aim173: the joiner turned ~80 deg standing still, the legs kept
        // the old walk heading and the spine twist hit its clamp). Idle: once the aim is
        // more than 60 deg off the legs, the legs turn toward the aim at 240 deg/s until
        // they line up, then hold again (small aim moves stay upper-body only).
        static bool turning;
        static LARGE_INTEGER last;
        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)fq.QuadPart : 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        // dry188/189: the soldier's aim pose is BLADED - with the legs on the aim the gun
        // points ~70-90 deg to the right. So the legs follow the twist the gun still NEEDS
        // (g_need_twist from the bend, closed loop) rather than the raw aim: the legs turn
        // until the gun is on the aim with ~0 spine twist (the stance the AI soldiers use).
        // play213: a closed loop on the measured twist oscillated for the whole session (the
        // anim root lags the commanded legs). Open loop instead: legs target = aim - the slow
        // blade average (so the bladed gun lands on the aim), compared with the COMMANDED
        // legs; hysteresis 50 deg on / 8 deg off.
        // iso216/217: the blade is NOT a constant - the game's own aim layer turns the upper
        // body toward the look yaw, so the gun's angle to the legs follows the legs and any
        // target built from it chases itself (blade -80 .. -13 at a fixed aim). Stable rule:
        // the legs face the AIM itself once it is > 70 deg off; stop within 10 deg. The aim
        // layer + the spine bend place the gun.
        float aim = *(volatile float *)(uintptr_t)(o + 0x18);
        // with the HOLD pose the gun's angle to the body is a constant (no anim feedback):
        // legs = aim - that blade puts the gun on the aim with ~0 spine twist
        extern volatile int g_hold_ok; extern volatile float g_hold_blade;
        float target = g_hold_ok ? aim - g_hold_blade : aim;
        float d = target - body_yaw;
        while (d > 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        if (!turning && fabsf(d) > (g_hold_ok ? 45.0f : 70.0f)) {
            turning = true;
            logf("coop: loco: turn in place: aim %.1f, legs %.1f", aim, body_yaw);
        }
        if (turning) {
            float step = 240.0f * dt;
            if (fabsf(d) <= 10.0f) turning = false;
            else body_yaw += d > 0 ? (fabsf(d) < step ? fabsf(d) : step) : -(fabsf(d) < step ? fabsf(d) : step);
            while (body_yaw > 180.0f) body_yaw -= 360.0f;
            while (body_yaw < -180.0f) body_yaw += 360.0f;
        }
        last = now;
    }
    *(volatile float *)(uintptr_t)(o + 0x10) = body_yaw;
    float pre20 = *(volatile float *)(uintptr_t)(o + 0x20), pre84 = *(volatile float *)(uintptr_t)(o + 0x84);
    *(volatile float *)(uintptr_t)(o + 0x20) = sp;
    *(volatile float *)(uintptr_t)(o + 0x84) = sp;
    *(volatile float *)(uintptr_t)(o + 0x60) = dx; *(volatile float *)(uintptr_t)(o + 0x64) = 0.0f;
    *(volatile float *)(uintptr_t)(o + 0x68) = dz;
    *(volatile float *)(uintptr_t)(o + 0x70) = dx; *(volatile float *)(uintptr_t)(o + 0x74) = 0.0f;
    *(volatile float *)(uintptr_t)(o + 0x78) = dz;
    {   // STRAFE RE (owner: "add a strafing animation to the clones - right now it's only the forward animation"):
        // the anim input "player_direction" (FUN_00180350 -> FUN_001d2020) = the angle between link +0x70 (move)
        // and the body forward row +0x90, cached at link +0x80. COOP_STRAFE_LOG=1: clip / direction / speed.
        static int sl = -1, bear = -1; static uint32_t fr;
        if (sl < 0) { char e[8] = {0}; sl = GetEnvironmentVariableA("COOP_STRAFE_LOG", e, sizeof e) && e[0] == '1'; }
        if (bear < 0) { char e[8] = {0}; bear = GetEnvironmentVariableA("COOP_STRAFE_BEAR", e, sizeof e) && e[0] == '1'; }
        if (bear) *(volatile float *)(uintptr_t)(o + 0x14) = *(volatile float *)(uintptr_t)(o + 0x18);   // aim yaw -> "aim_bearing" = +0x10 - +0x14
        static int dumped;   // COOP_STRAFE_LOG=1: dump the clone's clip table once (the hash tree FUN_00094160 searches)
        if (sl && !dumped) {
            uint32_t blk = coop_anim_blk(body), holder = 0, root = 0;
            if (blk) coop_safe_copy(&holder, blk + 4, 4);
            if (holder >= 0x80000000 && holder < 0x84000000) coop_safe_copy(&root, holder, 4);
            if (root >= 0x80000000 && root < 0x84000000) {
                dumped = 1;
                uint32_t stack[256]; int sp2 = 0, n = 0; stack[sp2++] = root;
                while (sp2 > 0 && n < 600) {
                    uint32_t nd = stack[--sp2], l = 0, r = 0, key = 0, val = 0;
                    if (!coop_safe_copy(&l, nd + 8, 4) || !coop_safe_copy(&r, nd + 0xC, 4)) continue;
                    coop_safe_copy(&key, nd + 0x10, 4); coop_safe_copy(&val, nd + 0x14, 4);
                    uint32_t a8[8] = {0}; float f8[8] = {0};
                    if (val >= 0x80000000 && val < 0x84000000) { coop_safe_copy(a8, val, 32); memcpy(f8, a8, 32); }
                    logf("coop: clip table: #%d hash %08x asset %08x | %08x %08x %08x %08x %08x %08x %08x %08x | f %.2f %.2f %.2f %.2f",
                         n, key, val, a8[0], a8[1], a8[2], a8[3], a8[4], a8[5], a8[6], a8[7], f8[2], f8[3], f8[4], f8[5]);
                    n++;
                    if (l >= 0x80000000 && l < 0x84000000 && sp2 < 255) stack[sp2++] = l;
                    if (r >= 0x80000000 && r < 0x84000000 && sp2 < 255) stack[sp2++] = r;
                }
                logf("coop: clip table: %d clips for %08x (anim block %08x)", n, body, blk);
            }
        }
        if (sl && (fr++ % 15) == 0) {
            uint32_t blk = coop_anim_blk(body), h = 0; float t = 0;
            if (blk) { coop_safe_copy(&h, blk + 0x10, 4); coop_safe_copy(&t, blk + 0x14, 4); }
            logf("coop: strafe log: clip %08x t %.1f | move %.2f %.2f speed %.2f | facing %.1f (+10 %.1f) aim %.1f (+14 %.1f) | player_direction (+80) %.2f | +2D8 %.2f",
                 h, t, dx, dz, sp, body_yaw, *(volatile float *)(uintptr_t)(o + 0x10), *(volatile float *)(uintptr_t)(o + 0x18), *(volatile float *)(uintptr_t)(o + 0x14), *(volatile float *)(uintptr_t)(o + 0x80),
                 *(volatile float *)(uintptr_t)(body + 0x2D8));
        }
    }
    static uint32_t ln;
    if ((ln++ % 60) == 0)
        logf("coop: loco: speed %.2f m/s dir %.2f %.2f (found +20 %.2f +84 %.2f) | game speed +2D8 %.2f +2DC %.2f | health %.1f +3A7 %u",
             sp, dx, dz, pre20, pre84, *(volatile float *)(uintptr_t)(body + 0x2D8),
             *(volatile float *)(uintptr_t)(body + 0x2DC), *(volatile float *)(uintptr_t)(body + 0x2F0),
             *(volatile uint8_t *)(uintptr_t)(body + 0x3A7));
}

// SKELETON ARM AIM, step 1: map the clone's skeleton. Anim instance A = [body+0x328];
// bone matrices B = [A+0x30], 0x40 per bone (FUN_001c6060: render root = B+0x40 =
// bone 1, world space). Logs each bone's translation relative to the feet + its X row
// until a bone stops looking like a sane world matrix. Called a few times per run.
static void coop_log_skeleton(uint32_t body, const char *tag)
{
    uint32_t A = *(volatile uint32_t *)(uintptr_t)(body + 0x328);
    if (A < 0x80000000 || A >= 0x84000000) { logf("coop: skel[%s]: no anim instance (%08x)", tag, A); return; }
    uint32_t B = *(volatile uint32_t *)(uintptr_t)(A + 0x30);
    uint32_t D = *(volatile uint32_t *)(uintptr_t)(A + 0x50);
    const volatile float *feet = (const volatile float *)(uintptr_t)(body + 0xA0);
    logf("coop: skel[%s]: body %08x A %08x bones %08x desc %08x (desc+38 %d) feet %.2f %.2f %.2f",
         tag, body, A, B, D, (D >= 0x80000000 && D < 0x84000000) ? *(volatile int *)(uintptr_t)(D + 0x38) : -999,
         feet[0], feet[1], feet[2]);
    // skel73: [A+0x30] holds only the root pair (bone 0 local, bone 1 world); bone 2 is
    // not a matrix -> the skeleton palette lives elsewhere. Dump A and its desc raw.
    for (int o = 0; o < 0x100; o += 0x10) {
        const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)(A + o);
        const volatile float *f = (const volatile float *)(uintptr_t)(A + o);
        logf("coop: skel[%s]: A+%03x %08x %08x %08x %08x | %g %g %g %g", tag, o, w[0], w[1], w[2], w[3], f[0], f[1], f[2], f[3]);
    }
    if (D >= 0x80000000 && D < 0x84000000)
        for (int o = 0; o < 0x80; o += 0x10) {
            const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)(D + o);
            logf("coop: skel[%s]: D+%03x %08x %08x %08x %08x", tag, o, w[0], w[1], w[2], w[3]);
        }
    // skel74: A+0x30..+0x40 = FIVE frame records (0xA0 stride, local 4x4 at +0,
    // world 4x4 at +0x40?); A+0x54 = a 0x9D0-byte block ending at record 0 = the skin
    // palette candidate (~39 bones). Log the 5 records, then walk the palette.
    for (int k = 0; k < 5; k++) {
        uint32_t R = *(volatile uint32_t *)(uintptr_t)(A + 0x30 + 4 * k);
        if (R < 0x80000000 || R >= 0x84000000) continue;
        const volatile float *r = (const volatile float *)(uintptr_t)R;
        logf("coop: skel[%s]: rec%d %08x local t %+.2f %+.2f %+.2f | +40 t-feet %+.2f %+.2f %+.2f | X %+.2f %+.2f %+.2f",
             tag, k, R, r[12], r[13], r[14], r[28] - feet[0], r[29] - feet[1], r[30] - feet[2], r[0], r[1], r[2]);
        // recprobe: the gun is a separate rigid object - the records' extra 0x20 bytes
        // (+0x80..+0x9F) may hold the model / frame link it draws
        const volatile uint32_t *x = (const volatile uint32_t *)(uintptr_t)(R + 0x80);
        logf("coop: skel[%s]: rec%d +80 %08x %08x %08x %08x %08x %08x %08x %08x", tag, k, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
    }
    {   // the clone's current weapon object: tail fields (model / attachment candidates)
        uint32_t W = *(volatile uint32_t *)(uintptr_t)(body + 0x278 + 0x28);
        if (W >= 0x80000000 && W < 0x84000000)
            for (int o = 0xE0; o < 0x110; o += 0x10) {
                const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)(W + o);
                logf("coop: skel[%s]: W %08x +%03x %08x %08x %08x %08x", tag, W, o, w[0], w[1], w[2], w[3]);
            }
        // gun hunt round 3: the WEAPON TABLE entries (W+0xE8 = [*0x2D12D0]+4 + type*0x20)
        // and their +8 block (W+0xEC) for SMG 8 and shotgun 13 - model handle candidates
        {   // round 3 fix: [mgr+4] is the weapon POOL; read each weapon's OWN links
            // (W+0xE8 type entry, W+0xEC its block): clone SMG vs the PLAYER's shotgun
            uint32_t pobjw = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000)
                ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
            uint32_t parr = (pobjw >= 0x80000000 && pobjw < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(pobjw + 0x298) : 0;
            uint32_t Ws[2] = {W, (parr >= 0x80000000 && parr < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(parr + 4) : 0};
            const char *nm[2] = {"cloneSMG", "playerSlot1"};
            for (int k = 0; k < 2; k++) {
                uint32_t Wk = Ws[k];
                if (Wk < 0x80000000 || Wk >= 0x84000000) continue;
                uint32_t e = *(volatile uint32_t *)(uintptr_t)(Wk + 0xE8), s = *(volatile uint32_t *)(uintptr_t)(Wk + 0xEC);
                logf("coop: skel[%s]: wtab %s W %08x type %d entry %08x blk %08x", tag, nm[k], Wk,
                     (int)*(volatile uint8_t *)(uintptr_t)Wk, e, s);
                if (e >= 0x80000000 && e < 0x84000000) {
                    const volatile uint32_t *q = (const volatile uint32_t *)(uintptr_t)e;
                    logf("coop: skel[%s]: wtab %s entry: %08x %08x %08x %08x %08x %08x %08x %08x", tag, nm[k],
                         q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
                }
                if (s >= 0x80000000 && s < 0x84000000)
                    for (int o = 0; o < 0xE0; o += 0x20) {
                        const volatile uint32_t *q = (const volatile uint32_t *)(uintptr_t)(s + o);
                        logf("coop: skel[%s]: wtab %s blk +%03x %08x %08x %08x %08x %08x %08x %08x %08x", tag, nm[k], o,
                             q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
                    }
            }
        }
        // gun hunt round 2: the render instance at body+0x34C (sits right before the
        // clone's skin records) and the weapon object's head
        uint32_t RI = *(volatile uint32_t *)(uintptr_t)(body + 0x34C);
        if (RI >= 0x80000000 && RI < 0x84000000)
            for (int o = 0; o < 0xC0; o += 0x10) {
                const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)(RI + o);
                logf("coop: skel[%s]: RI %08x +%03x %08x %08x %08x %08x", tag, RI, o, w[0], w[1], w[2], w[3]);
            }
        if (W >= 0x80000000 && W < 0x84000000)
            for (int o = 0; o < 0xE0; o += 0x10) {
                const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)(W + o);
                logf("coop: skel[%s]: Whead %08x +%03x %08x %08x %08x %08x", tag, W, o, w[0], w[1], w[2], w[3]);
            }
        logf("coop: skel[%s]: body +29C %08x +2A0 %08x +348 %08x +34C %08x", tag,
             *(volatile uint32_t *)(uintptr_t)(body + 0x29C), *(volatile uint32_t *)(uintptr_t)(body + 0x2A0),
             *(volatile uint32_t *)(uintptr_t)(body + 0x348), *(volatile uint32_t *)(uintptr_t)(body + 0x34C));
    }
    B = *(volatile uint32_t *)(uintptr_t)(A + 0x54);   // walk the palette candidate below
    if (B < 0x10000 || B >= 0x84000000) return;
    // skel75: the block starts with zeros (not a plain 0x40 palette) -> raw dump once
    static int rawdone;
    if (!rawdone) {
        rawdone = 1;
        for (int o = 0; o < 0x9D0; o += 0x10) {
            const volatile float *f = (const volatile float *)(uintptr_t)(B + o);
            const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)(B + o);
            logf("coop: skelraw +%03x %08x %08x %08x %08x | %+.3f %+.3f %+.3f %+.3f", o, w[0], w[1], w[2], w[3], f[0], f[1], f[2], f[3]);
        }
    }
    for (int i = 0; i < 80; i++) {
        const volatile float *m = (const volatile float *)(uintptr_t)(B + i * 0x40);
        // skel71: bone matrices are MODEL space (bone 0 t = (-0.1, 1.15, 0.2), hip
        // height) - log raw translations, not feet-relative
        // skel72: bone 0 = local, bone 1 = WORLD (feet + 1.15 up = the render root) ->
        // accept either space; print feet-relative for world bones, raw for local
        float tx = m[12], ty = m[13], tz = m[14];
        float r0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        bool loc = fabsf(tx) < 5.0f && fabsf(ty) < 5.0f && fabsf(tz) < 5.0f;
        bool wld = fabsf(tx - feet[0]) < 5.0f && fabsf(ty - feet[1]) < 5.0f && fabsf(tz - feet[2]) < 5.0f;
        bool sane = r0 > 0.5f && r0 < 1.5f && (loc || wld);
        if (!sane) { logf("coop: skel[%s]: bone %d not sane (r0 %.2f t %.1f %.1f %.1f) - stop", tag, i, r0, tx, ty, tz); break; }
        if (wld && !loc) { tx -= feet[0]; ty -= feet[1]; tz -= feet[2]; }
        logf("coop: skel[%s]: bone %2d %s dt %+5.2f %+5.2f %+5.2f  X %+.2f %+.2f %+.2f  Y %+.2f %+.2f %+.2f",
             tag, i, (wld && !loc) ? "W" : "L", tx, ty, tz, m[0], m[1], m[2], m[4], m[5], m[6]);
    }
}

// SKELETON ARM AIM, step 2 (experiment, --aim-test <deg>): the anim instance's frame
// records (A+0x30..+0x40, 0xA0 stride: local 4x4 at +0, world 4x4 at +0x40). rec1 is
// a clean rotation frame at chest/neck height (1.56 m). Rotate rec1's WORLD rows about
// the body's left axis (pose row +0x70) by <deg> each frame: if the drawn torso bends,
// the upper body is skinned off rec1 and the real pitch can drive it.
static void rot_about(float *v, const float *k, float c, float s)
{   // Rodrigues: v' = v c + (k x v) s + k (k.v)(1-c), |k| = 1
    float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    float cx = k[1] * v[2] - k[2] * v[1], cy = k[2] * v[0] - k[0] * v[2], cz = k[0] * v[1] - k[1] * v[0];
    float r[3] = {v[0] * c + cx * s + k[0] * kv * (1 - c), v[1] * c + cy * s + k[1] * kv * (1 - c), v[2] * c + cz * s + k[2] * kv * (1 - c)};
    v[0] = r[0]; v[1] = r[1]; v[2] = r[2];
}
static void coop_aim_test_rec1(uint32_t body, float deg, int rec)
{
    uint32_t A = *(volatile uint32_t *)(uintptr_t)(body + 0x328);
    if (A < 0x80000000 || A >= 0x84000000) return;
    uint32_t R = *(volatile uint32_t *)(uintptr_t)(A + 0x30 + 4 * rec);
    if (R < 0x80000000 || R >= 0x84000000) return;
    float L[3]; rd3(body + 0x70, L);
    float ll = sqrtf(dot3(L, L)); if (ll < 1e-3f) return;
    L[0] /= ll; L[1] /= ll; L[2] /= ll;
    float a = deg * 0.0174532925f, c = cosf(a), s = sinf(a);
    for (int r = 0; r < 3; r++) {
        float v[3]; rd3(R + 0x40 + 0x10 * r, v);
        rot_about(v, L, c, s);
        wr3(R + 0x40 + 0x10 * r, v);
    }
    static uint32_t n;
    if ((n++ % 120) == 0) logf("coop: aimtest: rec%d %08x world rows rotated %.0f deg about L %.2f %.2f %.2f", rec, R, deg, L[0], L[1], L[2]);
}

// WEAPON MODEL SYNC (owner: "his weapon model should sync with his player").
// swap97: D-pad RIGHT switches the player's weapon; a weapon object's FIRST BYTE is its
// type (SMG 8, pistol 13). Body weapon array [body+0x298]: [0] primary, [4] secondary;
// the inventory (body+0x278) slot list +0x20 holds the same pointers, count (char)+0x42,
// current +0x28/+0x24, index +0x43. The clone has ONE weapon (SMG), the player two.
//   FUN_001cee10(body, typeIdx, flag) stdcall RET 0x0C, EDI = ECX = *0x2D12D0 (weapon
//     pool mgr; pool entries 0x110) = the weapon FACTORY (ctor 0x1da5e5, secondary
//     0x1deeef) -> returns the new weapon object;
//   FUN_001c2690(ECX = inventory, EDX = weapon) fastcall plain RET = EQUIP.
volatile uint32_t g_wf_body, g_wf_type, g_wf_inv, g_wf_out, g_wf_fn, g_eq_inv, g_eq_w, g_eq_fn;
void coop_thunk_wfactory();
__declspec(naked) void coop_thunk_wfactory()
{
    __asm {
        pushad
        mov edi, dword ptr ds:[0x002D12D0]
        mov ecx, edi
        mov esi, dword ptr [g_wf_inv]
        push 0
        push dword ptr [g_wf_type]
        push dword ptr [g_wf_body]
        call dword ptr [g_wf_fn]            // RET 0x0C: callee cleans
        mov dword ptr [g_wf_out], eax
        popad
        ret
    }
}
void coop_thunk_equip();
__declspec(naked) void coop_thunk_equip()
{
    __asm {
        pushad
        mov ecx, dword ptr [g_eq_inv]
        mov edx, dword ptr [g_eq_w]
        call dword ptr [g_eq_fn]
        popad
        ret
    }
}
static int weapon_type(uint32_t W) { return (W >= 0x80000000 && W < 0x84000000) ? *(volatile uint8_t *)(uintptr_t)W : -1; }
// DRAWN GUN: the soldier ctor (FUN_001da4f0 line 190) calls FUN_001c5cd0(hashLo, hashHi)
// with EBX = body, stdcall RET 8: hash' = FUN_000c09f0(weapon hash, 0x48000000_e69a1dd7)
// -> asset FUN_001f83e0(*0x2D45D8, hash') -> [body+0x258]+0x0C = the gun MODEL, +0x1A
// forced >= 1, body+0x3A5 = 1, +0x3AC = 0 ("AI gun model not found" on a miss). The
// weapon hash = the type entry W+0xE8 [0] lo / [4] hi (same order FUN_00014090 compares).
volatile uint32_t g_gm_body, g_gm_lo, g_gm_hi, g_gm_fn;
void coop_thunk_gunmodel();
__declspec(naked) void coop_thunk_gunmodel()
{
    __asm {
        pushad
        mov ebx, dword ptr [g_gm_body]
        push dword ptr [g_gm_hi]
        push dword ptr [g_gm_lo]
        call dword ptr [g_gm_fn]            // RET 8: callee cleans
        popad
        ret
    }
}
static void coop_clone_gunmodel(uint32_t body, uint32_t W)
{
    if (weapon_type(W) < 0) return;
    uint32_t ent = *(volatile uint32_t *)(uintptr_t)(W + 0xE8);
    if (ent < 0x10000 || ent >= 0x84000000) return;
    uint32_t att = *(volatile uint32_t *)(uintptr_t)(body + 0x258);
    if (att < 0x80000000 || att >= 0x84000000) { logf("coop: gun model: body+0x258 = %08x (no attachment)", att); return; }
    uint32_t before = *(volatile uint32_t *)(uintptr_t)(att + 0x0C);
    if (!g_gm_fn) g_gm_fn = xbe_va(0x001C5CD0);
    g_gm_body = body;
    g_gm_lo = *(volatile uint32_t *)(uintptr_t)ent;
    g_gm_hi = *(volatile uint32_t *)(uintptr_t)(ent + 4);
    coop_thunk_gunmodel();
    logf("coop: gun model: type %d hash %08x_%08x -> [+258]+0C %08x -> %08x (+1A %u, +3A5 %u)",
         weapon_type(W), g_gm_hi, g_gm_lo, before, *(volatile uint32_t *)(uintptr_t)(att + 0x0C),
         *(volatile uint8_t *)(uintptr_t)(att + 0x1A), *(volatile uint8_t *)(uintptr_t)(body + 0x3A5));
}
// Make the clone carry + equip a weapon of type `want` (equip if owned, else create it
// into slot 1 once per type, then equip). Called every frame; cheap when already equal.
static void coop_clone_weapon(uint32_t body, int want)
{
    // play (Naszran): the owner's switch went to weapon TYPE 0 (15-round clip) and the clone
    // never changed - type 0 was rejected as "no weapon". No weapon is -1 (weapon_type).
    {   // COOP_CLONE_WTYPE=<n> (calibration): force the clone's weapon type (face-on aim sweeps per weapon)
        static int fw = -2;
        if (fw == -2) { char e[8] = {0}; fw = GetEnvironmentVariableA("COOP_CLONE_WTYPE", e, sizeof e) ? atoi(e) : -1; }
        if (fw >= 0) want = fw;
    }
    if (want < 0 || want > 63 || body < 0x80000000 || body >= 0x84000000) return;
    uint32_t inv = body + 0x278;
    if (weapon_type(*(volatile uint32_t *)(uintptr_t)(inv + 0x28)) == want) return;
    uint32_t arr = *(volatile uint32_t *)(uintptr_t)(body + 0x298);
    if (arr < 0x80000000 || arr >= 0x84000000) return;
    if (!g_eq_fn) { g_eq_fn = xbe_va(0x001C2690); g_wf_fn = xbe_va(0x001CEE10); }
    for (int i = 0; i < 2; i++) {
        uint32_t Wi = *(volatile uint32_t *)(uintptr_t)(arr + 4 * i);
        if (weapon_type(Wi) == want) {
            g_eq_inv = inv; g_eq_w = Wi; coop_thunk_equip();
            logf("coop: weapon sync: clone EQUIPPED slot %d type %d (W %08x)", i, want, Wi);
            coop_clone_gunmodel(body, Wi);
            return;
        }
    }
    // owner (play 20261112): "sometimes it's not the right muzzle of the weapon he has equipped". Every
    // created weapon goes into slot 1 and replaces the previous one, and each type was created only
    // once -> after a third weapon, switching BACK to an earlier type found it in no slot and did
    // nothing (the clone kept the last gun and its flash). The objects we made stay alive: put the
    // remembered one back into slot 1 and equip it.
    if (g_clone_w[want] && weapon_type(g_clone_w[want]) == want) {
        uint32_t Wk = g_clone_w[want];
        *(volatile uint32_t *)(uintptr_t)(arr + 4) = Wk;
        g_eq_inv = inv; g_eq_w = Wk; coop_thunk_equip();
        logf("coop: weapon sync: clone RE-EQUIPPED its earlier type %d (W %08x) into slot 1", want, Wk);
        coop_clone_gunmodel(body, Wk);
        return;
    }
    static uint64_t tried_ms[64];               // a failed create is retried at most once per 2 s
    uint64_t nowms = GetTickCount64();
    if (tried_ms[want] && nowms - tried_ms[want] < 2000) return;
    tried_ms[want] = nowms;
    g_wf_body = body; g_wf_type = (uint32_t)want; g_wf_inv = inv; g_wf_out = 0;
    coop_thunk_wfactory();
    uint32_t Wn = g_wf_out;
    logf("coop: weapon sync: factory(type %d) -> W %08x (first byte %d)", want, Wn, weapon_type(Wn));
    if (weapon_type(Wn) != want) return;
    *(volatile uint32_t *)(uintptr_t)(arr + 4) = Wn;                         // slot 1
    g_clone_w[want] = Wn;
    if (*(volatile int8_t *)(uintptr_t)(inv + 0x42) < 2) *(volatile int8_t *)(uintptr_t)(inv + 0x42) = 2;
    *(volatile uint8_t *)(uintptr_t)(Wn + 0x108) = *(volatile uint8_t *)(uintptr_t)(body + 0x40F);
    g_eq_inv = inv; g_eq_w = Wn; coop_thunk_equip();
    logf("coop: weapon sync: clone GOT + EQUIPPED type %d into slot 1 (W %08x)", want, Wn);
    coop_clone_gunmodel(body, Wn);
}

// ALLY (owner: "the clone must be taken as an ally instead of an enemy"). team113
// pawn dump: body+0x39C = TEAM id - deck squadmates 0, dock enemies 1, and the clone 1
// (enemy spawn path: the stream spawn sets ent+0x39C = 1). A dozen AI routines test
// hostility as a+0x39C != b+0x39C (FUN_00186e30, 0018cbc0, 00191590, 00195120, ...).
// The clone takes the local PLAYER's team, re-asserted every frame.
static void coop_clone_team(uint32_t body, uint32_t pobj)
{
    if (body < 0x80000000 || body >= 0x84000000 || pobj < 0x80000000 || pobj >= 0x84000000) return;
    // a puppet: its real health lives on the other machine; enemies here may now shoot
    // it (team 0 is hostile to them) -> immortal like the deck squadmates (FLT_MAX)
    float hpnow = *(volatile float *)(uintptr_t)(body + 0x2F0);
    if (hpnow < 1.0e30f) {
        // dn144: the clone's hp was HARD-ZEROED mid-frame (a class-kill style write,
        // not a decrement from FLT_MAX) and the game's death processing crashed on
        // the private link's garbage voice index (FUN_0017d130 table[link+0xD8]).
        // Log the drop moment so the writer can be identified.
        static int drops;
        static uint64_t last;
        if (drops++ < 12 || GetTickCount64() - last > 5000) {
            last = GetTickCount64();
            logf("coop: ally: WARNING clone %08x hp dropped to %g (restoring FLT_MAX)", body, hpnow);
        }
    }
    *(volatile float *)(uintptr_t)(body + 0x2F0) = 3.4028235e38f;
    // (review: a per-frame "+0x384 = 0 alive" write lived here - a dn145 attempt that did
    // not stop the crash; the clone shield did. Removed: forcing the life state can
    // fight real engine states such as 2 = despawn / 4 = scripted.)
    uint32_t want = *(volatile uint32_t *)(uintptr_t)(pobj + 0x39C);
    uint32_t cur = *(volatile uint32_t *)(uintptr_t)(body + 0x39C);
    if (cur == want) return;
    *(volatile uint32_t *)(uintptr_t)(body + 0x39C) = want;
    static int n;
    if (n++ < 4) logf("coop: ally: clone %08x team +0x39C %u -> %u (the player's team)", body, cur, want);
}

// ---- DOWNED (day 6): --coop-downed. The owner design: nobody dies in co-op. ----
// A killing blow clamps to hp 1 (DOWNED, the death branch never runs, no frozen
// death screen); the downed player's input is muted (read_pad) and the spectator
// drive in coop_on_player_update parks their pawn beside the surviving player and
// copies their look; the survivor presses X -> "CLRS" -> respawn at their feet.
// Symmetric on both instances and it covers the joiner's RELAYED hits too (they
// enter the player through FUN_001ed990, the same function this hook guards).
volatile uint32_t g_coop_downed = 0;      // this instance's player is downed
volatile uint32_t g_coop_peer_downed = 0; // the peer is downed (our X can respawn them)
static float g_downed_pos[3]; static int g_downed_pos_ok;   // where we went down (our body stays there on the peer's screen)
static float g_downed_yaw;                                   // our view yaw when we went down (the self model faces it)
// The player's full health, learned from the live player object (the highest value
// seen on the current player object; 1200 at mission start on Normal) instead of a
// hardcoded constant - the respawn restores this. Reset when the player object changes.
static float g_player_max_hp = 0.0f;
static uint32_t g_player_max_obj = 0;
static void coop_track_max_hp(uint32_t po)
{
    if (po < 0x80000000 || po >= 0x84000000) return;
    if (po != g_player_max_obj) {
        g_player_max_obj = po; g_player_max_hp = 0.0f;
        // review step: which functions handle damage on the PLAYER object? (the downed guard
        // only covers FUN_001ed990; pawns use +0x20 bullets, +0x2C generic, +0x48 directional)
        uint32_t vt = *(volatile uint32_t *)(uintptr_t)po;
        if (vt >= 0x10000 && vt < 0x00400000)
            logf("coop: player %08x vtable %08x: +0x20 %08x +0x2C %08x +0x48 %08x +0x58 %08x", po, vt,
                 *(volatile uint32_t *)(uintptr_t)(vt + 0x20), *(volatile uint32_t *)(uintptr_t)(vt + 0x2C),
                 *(volatile uint32_t *)(uintptr_t)(vt + 0x48), *(volatile uint32_t *)(uintptr_t)(vt + 0x58));
    }
    float hp = *(volatile float *)(uintptr_t)(po + 0x2F0);
    if (hp > g_player_max_hp && hp < 1.0e6f) g_player_max_hp = hp;
}

// respawn our downed player at the survivor's feet (their "CLRS" packet), full
// health, oriented like them. The clone drive's 1 m min separation re-parks the
// survivor's model beside us on our screen after the teleport.
static void coop_respawn_at(const float *feet, float yaw, bool refill = true)
{
    uint32_t ctrl = g_coop_last_ctrl;
    if (ctrl < 0x80000000 || ctrl >= 0x84000000) return;
    uint32_t po = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
    if (po < 0x80000000 || po >= 0x84000000) return;
    volatile float *f = (volatile float *)(uintptr_t)(po + 0xA0);    // feet
    volatile float *snap = (volatile float *)(uintptr_t)(po + 0x190); // tick-start snapshot
    // play 20261114_083833 (host crash 0x69955 in the frame of its respawn): the survivor's clone stands on
    // exactly these feet here - two bodies on one point is the m2c crash (degenerate collision test). Land
    // 0.8 m behind the survivor (along their yaw) instead; the clone drive's 1 m separation does the rest.
    float yr = yaw * 0.0174532925f;
    float tgt[3] = {feet[0] - 0.8f * sinf(yr), feet[1], feet[2] - 0.8f * cosf(yr)};
    feet = tgt;
    float dx = feet[0] - f[0], dy = feet[1] - f[1], dz = feet[2] - f[2];
    f[0] += dx; f[1] += dy; f[2] += dz;
    snap[0] += dx; snap[1] += dy; snap[2] += dz;   // speed-spike fix: the game's speed = (+0xA0 - +0x190)/dt
    float full = (po == g_player_max_obj && g_player_max_hp > 1.0f) ? g_player_max_hp : 1200.0f;
    if (!refill) full = *(volatile float *)(uintptr_t)(po + 0x2F0);   // a checkpoint move of a player who is up: health stays
    *(volatile float *)(uintptr_t)(po + 0x2F0) = full;   // full health as learned from this player object
    *(volatile float *)(uintptr_t)(ctrl + 0x18) = yaw;      // look = the survivor's yaw
    g_coop_downed = 0;
    logf("coop: RESPAWNED at survivor feet %.1f %.1f %.1f yaw %.1f (health %.0f)", feet[0], feet[1], feet[2], yaw, full);
}

// MAIN OBJECTIVE RESPAWN (host): the objective list at world+0x930 = {records*, count}, records of 12 B {def*,
// progress, state 0..3 (3 = done)}; def +0x15 = flags (0x02/0x04/0x08 = target-count types, 0x10/0x20 other;
// FUN_001ba5c0 tallies them) - a MAIN objective has none of 0x3E (the game also checks "_PMO_" in its name).
// When one turns to state 3: a downed host respawns at the joiner's feet; a downed joiner gets "CLRS" with ours.
static void coop_main_obj_poll(uint32_t pobj)
{
    if (g_cfg.coop_role != 1) return;
    static uint8_t prev[64]; static uint32_t prev_arr;
    uint32_t W = *(volatile uint32_t *)(uintptr_t)0x002D45D4, arr = 0, n = 0;
    if (W < 0x80000000 || W >= 0x84000000 || !coop_safe_copy(&arr, W + 0x930, 4) || !coop_safe_copy(&n, W + 0x934, 4) || n > 64 || !arr) return;
    if (arr != prev_arr) { prev_arr = arr; for (uint32_t i = 0; i < n; i++) { uint32_t st = 0; coop_safe_copy(&st, arr + i * 12 + 8, 4); prev[i] = (uint8_t)st; } return; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t def = 0, st = 0; uint8_t fl = 0xFF;
        coop_safe_copy(&def, arr + i * 12, 4); coop_safe_copy(&st, arr + i * 12 + 8, 4);
        if (def) coop_safe_copy(&fl, def + 0x15, 1);
        if ((uint8_t)st == prev[i]) continue;
        logf("coop: objective %u (def %08x flags %02x) state %u -> %u%s", i, def, fl, prev[i], st, (st == 3 && !(fl & 0x3E)) ? " = MAIN OBJECTIVE COMPLETE" : "");
        prev[i] = (uint8_t)st;
        static int any = -1;   // COOP_MAIN_OBJ_ANY=1 (tests): any completed objective counts as a main one
        if (any < 0) { char e[8] = {0}; any = GetEnvironmentVariableA("COOP_MAIN_OBJ_ANY", e, sizeof e) && e[0] == '1'; }
        if (st != 3 || ((fl & 0x3E) && !any)) continue;
        if (g_coop_downed && g_coop_host_pawn_ok) {
            float feet[3] = {g_coop_host_pawnmat[12], g_coop_host_pawnmat[13], g_coop_host_pawnmat[14]};
            logf("coop: main objective done -> our downed player respawns at the joiner's feet");
            coop_respawn_at(feet, g_coop_host_yaw);
        }
        if (g_coop_peer_downed && pobj >= 0x80000000 && pobj < 0x84000000) {
            struct { uint32_t magic, seq; float feet[3]; float yaw; } rp;
            rp.magic = 0x53524C43; rp.seq = ++g_coop_calls;
            const volatile float *f = (const volatile float *)(uintptr_t)(pobj + 0xA0);
            rp.feet[0] = f[0]; rp.feet[1] = f[1]; rp.feet[2] = f[2];
            rp.yaw = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000) ? *(volatile float *)(uintptr_t)(g_coop_last_ctrl + 0x18) : 0.0f;
            for (int r = 0; r < 3; r++) sendto(g_coop_sock, (const char *)&rp, (int)sizeof rp, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            logf("coop: main objective done -> respawn sent to the downed joiner (our feet %.1f %.1f %.1f)", rp.feet[0], rp.feet[1], rp.feet[2]);
        }
    }
}
// ROUTE RECORD / REPLAY (owner: "learn the game layout so you can progress in any level to test").
// A route = text lines "x,y,z,yaw" (feet, world; '#' = comment), one file per level section.
//  COOP_ROUTE_REC=<file>  append our feet + look yaw every 250 ms while we move (play_coop.ps1 sets
//                         it, so every owner session teaches the route it walked).
//  COOP_ROUTE=<file>      GHOST RUN: from COOP_ROUTE_AT seconds after launch (default 75) the player
//                         is carried along the route at COOP_ROUTE_SPEED m/s (default 6) with the
//                         respawn teleport (feet +0xA0 and the tick snapshot +0x190 move together),
//                         looking along the recorded yaw, health held at full (a test pilot cannot
//                         fight). COOP_ROUTE_STOP=<m> stops that many metres before the end. The
//                         joiner trails the same route 2.5 m behind. Scripted input still works
//                         (fire, grenades) while riding.
// PROP PARTS (kind 4, vt 0028ed98; FUN_001d0e40): part count = byte +0x1F5; part i's matrix =
// [+0x19C] + idx * 0x40 with idx = int [+0x1A4][i] (idx < 0 -> the object's own +0x70 block);
// world position at matrix +0x30. Each part is its own explosion source.
static int coop_prop_parts(uint32_t o, float out[][3], int maxn)
{
    uint8_t np = 0; uint32_t mats = 0, ix = 0;
    if (!coop_safe_copy(&np, o + 0x1F5, 1)) return 0;
    coop_safe_copy(&mats, o + 0x19C, 4); coop_safe_copy(&ix, o + 0x1A4, 4);
    int n = 0;
    for (int i = 0; i < np && n < maxn; i++) {
        int32_t id = -1;
        if (ix >= 0x80000000 && ix < 0x84000000) coop_safe_copy(&id, ix + 4 * i, 4);
        uint32_t m = (id < 0 || mats < 0x80000000 || mats >= 0x84000000) ? o + 0x70 : mats + (uint32_t)id * 0x40;
        if (!coop_safe_copy(out[n], m + 0x30, 12)) continue;
        n++;
    }
    return n;
}
// LEVEL LOGIC TABLE (trigger sync RE, day 10). T = *0x2D45D0; FUN_001e7ff0 (every frame, ESI = T,
// args (dt?, &world+0xD0)) walks its class lists: a ushort COUNT at T + 2*k and the pointer array at
// T + 0x48 + 4*k. Lists 0, 1, 18, 19: TRIGGERS - armed byte +0x11C, skipped when int +0x120 == 3,
// test = vt+0x48(arg2, &playerptr) with playerptr = *0x2D45D4 + 0x30; true -> disarmed. Lists 14, 15:
// the same with the armed byte at +0x130. Lists 12, 13: SPAWNERS (FUN_001b3070: enabled +0x28,
// remaining count +0x2C, current body +0x24, timer +0x30). The other lists are ticked through vt+0
// / vt+0x10 while their byte +0x18 / +0x1C is set.
struct LogicList { int k; int armed_off; const char *name; };
static const LogicList kTrigLists[6] = {{0, 0x11C, "trigA"}, {1, 0x11C, "trigB"}, {14, 0x130, "trigC"}, {15, 0x130, "trigD"}, {18, 0x11C, "trigE"}, {19, 0x11C, "trigF"}};
static int coop_logic_list(int k, uint32_t *arr)       // count (and the array pointer) of class list k
{
    uint32_t T = *(volatile uint32_t *)(uintptr_t)0x002D45D0;
    if (T < 0x80000000 || T >= 0x84000000) return 0;
    uint16_t n = 0; uint32_t a = 0;
    if (!coop_safe_copy(&n, T + 2 * k, 2) || !coop_safe_copy(&a, T + 0x48 + 4 * k, 4)) return 0;
    if (a < 0x80000000 || a >= 0x84000000 || n > 2048) return 0;
    *arr = a;
    return n;
}
// COOP_LOGIC_CENSUS=1 (RE, objectives/destructibles): once per level table, every class list k (0..35):
// count + the distinct vtables of its objects
static void coop_logic_census(void)
{
    static int on = -1; static uint32_t lastT;
    if (on < 0) { char e[8] = {0}; on = GetEnvironmentVariableA("COOP_LOGIC_CENSUS", e, sizeof e) && e[0] == '1'; }
    if (!on || !g_coop_t0 || GetTickCount64() - g_coop_t0 < 60000) return;
    uint32_t T = *(volatile uint32_t *)(uintptr_t)0x002D45D0;
    if (T == lastT) return;
    lastT = T;
    for (int k = 0; k < 36; k++) {
        uint32_t arr = 0; int n = coop_logic_list(k, &arr);
        if (!n) continue;
        uint32_t vts[6] = {0}; int nv = 0, cnt[6] = {0};
        for (int i = 0; i < n; i++) {
            uint32_t o = 0, vt = 0; coop_safe_copy(&o, arr + 4 * i, 4); if (!o || !coop_safe_copy(&vt, o, 4)) continue;
            int f = -1; for (int q = 0; q < nv; q++) if (vts[q] == vt) f = q;
            if (f < 0 && nv < 6) { f = nv++; vts[f] = vt; }
            if (f >= 0) cnt[f]++;
        }
        char b[200]; int bp = 0; b[0] = 0;
        for (int q = 0; q < nv; q++) bp += snprintf(b + bp, sizeof b - bp, " vt %08x x%d", vts[q], cnt[q]);
        uint32_t o0 = 0; coop_safe_copy(&o0, arr, 4);
        logf("coop: logic census: list %2d: %4d objects (first %08x):%s", k, n, o0, b);
    }
}
// COOP_TRIG_LOG=1: list every trigger once the level runs, then log each disarm (= fired) with our position
static void coop_trig_trace(uint32_t obj)
{
    static int on = -1;
    if (on < 0) { char e[8] = {0}; on = GetEnvironmentVariableA("COOP_TRIG_LOG", e, sizeof e) && e[0] == '1'; }
    if (!on || obj < 0x80000000 || obj >= 0x84000000) return;
    static uint8_t prev[6][512], prevc[2][64]; static uint32_t seenT; static int listed;
    uint32_t T = *(volatile uint32_t *)(uintptr_t)0x002D45D0;
    if (T != seenT) { seenT = T; listed = 0; memset(prev, 0xFF, sizeof prev); }
    const volatile float *pf = (const volatile float *)(uintptr_t)(obj + 0xA0);
    for (int li = 0; li < 6; li++) {
        uint32_t arr = 0; int n = coop_logic_list(kTrigLists[li].k, &arr);
        for (int i = 0; i < n && i < 512; i++) {
            uint32_t o = 0, vt = 0, fn = 0, ty = 0; uint8_t armed = 0; float q[3] = {0, 0, 0};
            if (!coop_safe_copy(&o, arr + 4 * i, 4) || o < 0x80000000 || o >= 0x84000000) continue;
            coop_safe_copy(&armed, o + kTrigLists[li].armed_off, 1);
            if (!listed || prev[li][i] != armed) {
                coop_safe_copy(&vt, o, 4); coop_safe_copy(&ty, o + 0x120, 4); coop_safe_copy(q, o + 0xA0, 12);
                if (vt >= 0x10000 && vt < 0x00400000) coop_safe_copy(&fn, vt + 0x48, 4);
                if (!listed) logf("coop: trig list %s[%d] %08x vt %08x test %08x armed %u type %u at %.1f %.1f %.1f", kTrigLists[li].name, i, o, vt, fn, armed, ty, q[0], q[1], q[2]);
                if (!listed) {   // its TARGETS (FUN_001B48A0: obj +0x10 = array, count byte +0x14; each target->vt+8 runs)
                    uint32_t ta = 0; uint8_t tn = 0; char tb[400]; int to = 0; tb[0] = 0;
                    coop_safe_copy(&ta, o + 0x10, 4); coop_safe_copy(&tn, o + 0x14, 1);
                    for (int k = 0; k < tn && k < 8 && ta >= 0x80000000 && ta < 0x84000000; k++) {
                        uint32_t tg = 0, tv = 0, tf = 0;
                        coop_safe_copy(&tg, ta + 4 * k, 4);
                        if (tg >= 0x80000000 && tg < 0x84000000) { coop_safe_copy(&tv, tg, 4); if (tv >= 0x10000 && tv < 0x400000) coop_safe_copy(&tf, tv + 8, 4); }
                        to += snprintf(tb + to, sizeof tb - to, " %08x(vt %08x act %08x)", tg, tv, tf);
                    }
                    if (tn) logf("coop: trig list   targets %u:%s", tn, tb);
                }
                else logf("coop: TRIGGER %s[%d] %08x (test %08x, type %u, at %.1f %.1f %.1f) armed %u -> %u | player at %.1f %.1f %.1f", kTrigLists[li].name, i, o, fn, ty, q[0], q[1], q[2], prev[li][i], armed, pf[0], pf[1], pf[2]);
                prev[li][i] = armed;
            }
        }
    }
    for (int k = 14; k <= 15; k++) {   // class C: inside flag +0x131 (enter / leave)
        uint32_t arr = 0; int n = coop_logic_list(k, &arr);
        for (int i = 0; i < n && i < 64; i++) {
            uint32_t o = 0; uint8_t in = 0; float q[3] = {0, 0, 0};
            if (!coop_safe_copy(&o, arr + 4 * i, 4) || o < 0x80000000) continue;
            coop_safe_copy(&in, o + 0x131, 1);
            if (listed && prevc[k - 14][i] != in) {
                coop_safe_copy(q, o + 0xA0, 12);
                logf("coop: TRIG C L%d[%d] %08x inside %u -> %u (at %.1f %.1f %.1f) | player %.1f %.1f %.1f", k, i, o, prevc[k - 14][i], in, q[0], q[1], q[2], pf[0], pf[1], pf[2]);
            }
            prevc[k - 14][i] = in;
        }
    }
    if (!listed) {
        int tot = 0; uint32_t a2 = 0;
        for (int li = 0; li < 6; li++) tot += coop_logic_list(kTrigLists[li].k, &a2);
        if (tot) { listed = 1; logf("coop: trig list: table %08x, %d triggers; spawner lists %d + %d", T, tot, coop_logic_list(12, &a2), coop_logic_list(13, &a2)); }
    }
}
// TRIGGER SYNC v1 (day 10; owner: "sync all triggers between players ... the joiner sends a signal to
// the host so the host triggers events for all"). HOST-AUTHORITATIVE ACTIVATION of the one-shot
// triggers: classes A (vt 0028ef88, lists 0/1) and E (vt 0028ed40, lists 18/19) with type +0x120 == 0.
//  - Every trigger's action is its vt+4 = FUN_001bf670 (-> FUN_001b48a0, the generic "activate";
//    thiscall, 1 arg = &world+0x30, RET 4), called by the test (vt+0x48, thiscall (feet vec*, &world+0x30),
//    RET 8) when it passes, or by another object's action.
//  - HOST: the entry hook on FUN_001bf670 counts every activation of such a trigger; the counts ride
//    "CLTG". Each frame the host also runs the GAME'S OWN test of every armed class-A trigger with the
//    JOINER's feet (the clone's +0xA0; the swept test reads the previous feet from world+0x1C0, swapped
//    in for the call) - the joiner's position stream is its "signal".
//  - JOINER: its own activations of these triggers are suppressed in the hook (the local test may
//    still disarm the trigger); it performs exactly the host's activations (count catch-up).
// Env COOP_TRIG_SYNC=0 = off (each copy fires its own triggers, as before).
static uint16_t g_trig_cnt[512], g_trig_host_cnt[512];   // activations: ours (host: counted, joiner: replayed) / the host's last report
// (owner day 15: "is there a cap for trigger sync? raise it to the maximum": the counts were 1 byte - a trigger stopped
// being replayed after 255 activations - two-PC play 20261114: mission 1 trigger #36 (enter/leave, type 3) reached 255 at
// 117 s and from then on the joiner never replayed it again. Now 2 bytes that WRAP (no cap); the joiner replays while
// (uint16)(host - ours) is 1..32767, i.e. any number of activations as long as it is less than 32767 behind)
static uint32_t g_trig_obj[512]; static int g_trig_n; static uint32_t g_trig_T; static int g_trig_host_n;
static uint32_t g_trig_replay_this; static uint32_t g_trig_stats[4];   // host-fired-for-joiner, counted, suppressed, replayed
static int g_trig_sync = -1;
static void coop_trig_table(void)
{
    uint32_t T = *(volatile uint32_t *)(uintptr_t)0x002D45D0;
    if (T == g_trig_T && g_trig_n) return;
    if (!g_trig_T && g_part_ms && GetTickCount64() - g_part_ms < 300) return;   // a level part is being set up: index it after
    if (T != g_trig_T) { memset(g_trig_cnt, 0, sizeof g_trig_cnt); memset(g_trig_host_cnt, 0, sizeof g_trig_host_cnt); g_trig_host_n = 0; }
    g_trig_T = T; g_trig_n = 0;
    static const int ks[4] = {0, 1, 18, 19};
    for (int li = 0; li < 4; li++) {
        uint32_t arr = 0; int n = coop_logic_list(ks[li], &arr);
        for (int i = 0; i < n && g_trig_n < 512; i++) {
            uint32_t o = 0;
            coop_safe_copy(&o, arr + 4 * i, 4);
            g_trig_obj[g_trig_n++] = (o >= 0x80000000 && o < 0x84000000) ? o : 0;
        }
    }
    if (g_trig_n) logf("coop: trigger sync: table %08x, %d class A/E triggers indexed", T, g_trig_n);
}
static int coop_trig_index(uint32_t o)
{
    for (int i = 0; i < g_trig_n; i++) if (g_trig_obj[i] == o) return i;
    return -1;
}
static bool coop_trig_synced(uint32_t o)            // class A / E, ANY type (one-shot, area, enter/leave)
{
    uint32_t vt = 0;
    return o >= 0x80000000 && o < 0x84000000 && coop_safe_copy(&vt, o, 4) && (vt == 0x0028EF88 || vt == 0x0028ED40);
}
static uint16_t g_trig_req[256]; static int g_trig_req_n;        // joiner: activations to ask the host for (this frame)
static uint16_t g_trig_req_in[512]; static int g_trig_req_in_n; // host: requests received
static bool coop_trig_oneshot(uint32_t o)
{
    uint32_t vt = 0, ty = 1;
    if (o < 0x80000000 || o >= 0x84000000 || !coop_safe_copy(&vt, o, 4) || (vt != 0x0028EF88 && vt != 0x0028ED40)) return false;
    coop_safe_copy(&ty, o + 0x120, 4);
    return ty == 0;
}
volatile uint32_t g_tf_resume, g_tf_this, g_tf_skip;
extern "C" void coop_on_trigger_fire(void)
{
    g_tf_skip = 0;
    if (g_trig_sync != 1 || !g_cfg.coop_role) return;
    uint32_t o = g_tf_this;
    if (!coop_trig_synced(o)) {   // other classes: census only (COOP_TRIG_LOG=1)
        static int tl = -1;
        if (tl < 0) { char e[8] = {0}; tl = GetEnvironmentVariableA("COOP_TRIG_LOG", e, sizeof e) && e[0] == '1'; }
        uint32_t vt = 0, ty = 0, sub = 0; float q[3] = {0, 0, 0};
        coop_safe_copy(&vt, o, 4); coop_safe_copy(&ty, o + 0x120, 4); coop_safe_copy(&sub, o + 0x124, 4); coop_safe_copy(q, o + 0xA0, 12);
        static int ol;
        if (tl && ol++ < 200) logf("coop: TRIG ACT (not one-shot) %08x vt %08x type %u sub %u at %.1f %.1f %.1f", o, vt, ty, sub, q[0], q[1], q[2]);
        return;
    }
    int id = coop_trig_index(o);
    if (id < 0) return;
    uint32_t ty = 0, tvt = 0; coop_safe_copy(&ty, o + 0x120, 4); coop_safe_copy(&tvt, o, 4);
    if (ty == 4 && tvt == 0x0028EF88) {   // class A area triggers (FUN_001C9370 is class A's test)
        // two-PC play 20261114 (owner: "the host draws trees, a tree trunk and a sign post the joiner doesn't draw - a
        // zone culling effect that should be separate for host and joiner"; then "I went further and they drew"). An
        // AREA trigger (type 4, FUN_001C9370: the player entered area [2]/[3] -> world +0x5CA8/+0x5CAC, objects
        // flagged, then this activation) drives the zone's targets - the visibility of the areas around. Synced, the
        // HOST entering an area ran "hide the old zone" on the joiner too (and the joiner's own entries went through the
        // host). Area activations are LOCAL now: each copy runs its own player's, nothing is requested or replayed.
        static int al;
        if (al++ < 40) logf("coop: trigger sync: AREA trigger #%d %08x (type 4) - local to this player, not synced", id, o);
        return;
    }
    if (g_cfg.coop_role == 1) {
        g_trig_cnt[id]++;   // no cap: 16-bit wrap-around, the joiner compares the difference
        g_trig_stats[1]++;
        static int hl;
        if (hl++ < 5000) logf("coop: trigger sync: HOST trigger #%d %08x (type %u) activated (count %u)", id, o, ty, g_trig_cnt[id]);
        // realcp3: enter/leave trigger #36 (thousands of activations) overwrote who set the checkpoint off - only ONE-SHOT
        // activations (the checkpoints are one-shots) count for the checkpoint rule
        if (ty == 0) { g_cp_who = g_exit_by_joiner ? 2 : 1; g_cp_ms = GetTickCount64(); }   // the checkpoint rule: who set it off
        // realcp2: "HOST fired trigger #58 for the JOINER" - its activation came after the joiner block -> taken as the
        // host's own. The trigger the host's test fired at the joiner's feet in the last 1.5 s is the joiner's.
        if (ty == 0 && o == g_fj_obj && g_cp_ms - g_fj_ms < 1500) g_cp_who = 2;
        if (ty == 0) g_cp_tpos_ok = coop_safe_copy(g_cp_tpos, o + 0xA0, 12);
    } else if (o != g_trig_replay_this) {
        // the JOINER's own player set it off: do not fire it here - ask the host, which fires it for both
        // (owner: "have the joiner send a signal to the host so the host triggers events for all")
        g_tf_skip = 1; g_trig_stats[2]++;
        if (g_trig_req_n < 256) g_trig_req[g_trig_req_n++] = (uint16_t)id;
        g_cp_req_id[g_cp_req_w & 7] = (uint16_t)id; g_cp_req_ms[g_cp_req_w & 7] = GetTickCount64(); g_cp_req_w++;
        static int sl;
        if (sl++ < 5000) logf("coop: trigger sync: JOINER activation of trigger #%d %08x (type %u) -> request to the host (host count %u, ours %u)", id, o, ty, g_trig_host_cnt[id], g_trig_cnt[id]);
    }
}
__declspec(naked) void coop_thunk_trigger_fire()     // FUN_001bf670 entry: 83 B9 20 01 00 00 02 (cmp [ecx+0x120], 2) re-executed
{
    __asm {
        pushad
        mov dword ptr [g_tf_this], ecx
        call coop_on_trigger_fire
        popad
        cmp dword ptr [g_tf_skip], 0
        jne skipfire
        cmp dword ptr [ecx + 0x120], 2
        jmp dword ptr [g_tf_resume]
    skipfire:
        ret 4
    }
}
volatile uint32_t g_tc_fn, g_tc_this, g_tc_a1, g_tc_a2, g_tc_esp; volatile uint8_t g_tc_ret;
__declspec(naked) void coop_thunk_trig_test()         // vt+0x48: thiscall (feet*, &playerptr), RET 8
{
    __asm {
        pushad
        mov dword ptr [g_tc_esp], esp
        push dword ptr [g_tc_a2]
        push dword ptr [g_tc_a1]
        mov ecx, dword ptr [g_tc_this]
        call dword ptr [g_tc_fn]
        mov byte ptr [g_tc_ret], al
        mov esp, dword ptr [g_tc_esp]
        popad
        ret
    }
}
__declspec(naked) void coop_thunk_trig_fire()         // vt+4: thiscall (&playerptr), RET 4
{
    __asm {
        pushad
        mov dword ptr [g_tc_esp], esp
        push dword ptr [g_tc_a1]
        mov ecx, dword ptr [g_tc_this]
        call dword ptr [g_tc_fn]
        mov esp, dword ptr [g_tc_esp]
        popad
        ret
    }
}
static void install_trigger_sync(void)
{
    char e[8] = {0};
    g_trig_sync = (GetEnvironmentVariableA("COOP_TRIG_SYNC", e, sizeof e) && e[0] == '0') ? 0 : 1;
    if (!g_trig_sync) { logf("coop: trigger sync: OFF (COOP_TRIG_SYNC=0)"); return; }
    uint32_t site = xbe_va(0x001BF670);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[7] = {0x83, 0xB9, 0x20, 0x01, 0x00, 0x00, 0x02};
    if (memcmp(code, kExpect, 7) != 0) { logf("coop: trigger sync: 001bf670 bytes mismatch - not installed"); g_trig_sync = 0; return; }
    g_tf_resume = site + 7;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_trigger_fire - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90; code[6] = 0x90;
    logf("coop: trigger sync installed at FUN_001bf670 (host counts activations, joiner replays them)");
}
// COOP_SPAWNER_DUMP=<sec>[,<sec>..]: every spawner (logic lists 12 / 13) with its gates (RE of area spawning)
static void coop_spawner_dump(void)
{
    static float ts[4] = {-1, -1, -1, -1}; static int ti = -1;
    if (ti < 0) { ti = 0; char e[64] = {0}; if (GetEnvironmentVariableA("COOP_SPAWNER_DUMP", e, sizeof e)) sscanf(e, "%f,%f,%f,%f", &ts[0], &ts[1], &ts[2], &ts[3]); }
    if (ti >= 4 || ts[ti] <= 0 || !g_coop_t0 || (float)(GetTickCount64() - g_coop_t0) * 0.001f < ts[ti]) return;
    ti++;
    uint32_t world = *(volatile uint32_t *)(uintptr_t)0x002D45D4;
    uint32_t a5ca8 = 0, a5cac = 0;
    if (world >= 0x80000000) { coop_safe_copy(&a5ca8, world + 0x5CA8, 4); coop_safe_copy(&a5cac, world + 0x5CAC, 4); }
    logf("coop: SPAWNERS at %.0f s: area %08x %08x", ts[ti - 1], a5ca8, a5cac);
    for (int k = 12; k <= 13; k++) {
        uint32_t arr = 0; int n = coop_logic_list(k, &arr);
        for (int i = 0; i < n; i++) {
            uint32_t o = 0; coop_safe_copy(&o, arr + 4 * i, 4);
            if (o < 0x80000000 || o >= 0x84000000) continue;
            uint32_t w[24] = {0}; coop_safe_copy(w, o, 0x60);
            uint8_t b28 = (uint8_t)w[10], b29 = (uint8_t)(w[10] >> 8), b2a = (uint8_t)(w[10] >> 16), b2b = (uint8_t)(w[10] >> 24);
            float t30, t34; memcpy(&t30, &w[12], 4); memcpy(&t34, &w[13], 4);
            logf("coop: spawner L%d[%02d] %08x vt %08x | +18 %08x +1C %08x body %08x | en %u %u gates %u %u | left %d | t %.1f / %.1f | +38 %08x +3C %d +40 %08x +44 %08x",
                 k, i, o, w[0], w[6], w[7], w[9], b28, b29, b2a, b2b, (int)w[11], t30, t34, w[14], (int)w[15], w[16], w[17]);
        }
    }
}
// SPAWN SYNC (day 11). Spawners (logic lists 12 / 13, same objects on both copies) spawn their next
// enemy only after the current one (+0x24) is gone, through FUN_001b3030 (ESI = spawner, AL = ok;
// 0 -> the spawner retries after a short random delay). Split play (ar7): the joiner's local fights
// killed enemies the host still had alive -> the joiner's spawners ran ahead (71 enemies vs the
// host's 23). HOST-AUTHORITATIVE: the host counts each spawner's spawns (+0x24 changing to a new
// body) and sends the counts ("CLSP"); the joiner's spawner may only spawn while its own count is
// behind the host's. Env COOP_SPAWN_SYNC=0 = off.
static uint32_t g_sp_obj[256]; static int g_sp_n; static uint32_t g_sp_T;
static uint8_t g_sp_cnt[256], g_sp_host_cnt[256]; static uint32_t g_sp_last[256]; static int g_sp_host_n;
static int g_sp_sync = -1; static uint32_t g_sp_stats[3];   // denied, allowed (joiner), counted
volatile uint32_t g_sg_resume, g_sg_esi, g_sg_deny;
static void coop_spawn_table(void)
{
    uint32_t T = *(volatile uint32_t *)(uintptr_t)0x002D45D0;
    if (T == g_sp_T && g_sp_n) return;
    if (!g_sp_T && g_part_ms && GetTickCount64() - g_part_ms < 300) return;   // a level part is being set up: index it after
    if (T != g_sp_T) { memset(g_sp_cnt, 0, sizeof g_sp_cnt); memset(g_sp_host_cnt, 0, sizeof g_sp_host_cnt); memset(g_sp_last, 0, sizeof g_sp_last); g_sp_host_n = 0; }
    g_sp_T = T; g_sp_n = 0;
    for (int k = 12; k <= 13; k++) {
        uint32_t arr = 0; int n = coop_logic_list(k, &arr);
        for (int i = 0; i < n && g_sp_n < 256; i++) { uint32_t o = 0; coop_safe_copy(&o, arr + 4 * i, 4); g_sp_obj[g_sp_n++] = o; }
    }
    if (g_sp_n) logf("coop: spawn sync: %d spawners indexed (table %08x)", g_sp_n, T);
}
// POOL GUARD (play 20261113_175243 joiner crash 0x189a33): a spawn takes a kind-1 entity pool entry
// (FUN_001df440 -> FUN_001899f0; R+0x22C48+0x7C {entries, used flags, count 16}). The clone holds one
// for the whole level, so the pool can be full while a spawner fires -> FUN_001899f0 returns 0 and
// writes [0+0x20]. Both roles: a spawner may not spawn while the pool is full (it retries after its
// short random delay, the same as any refused spawn).
static bool coop_pool_full(void)
{
    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, pl[3] = {0};
    if (!coop_safe_copy(pl, R + 0x22C48 + 0x7C, 12) || pl[2] == 0 || pl[2] > 256) return false;
    uint8_t used[256]; if (!coop_safe_copy(used, pl[1], pl[2])) return false;
    for (uint32_t i = 0; i < pl[2]; i++) if (!used[i]) return false;
    return true;
}
static uint32_t g_pool_holds;
extern "C" void coop_spawn_gate_check(void)
{
    g_sg_deny = 0;
    if (g_cfg.coop_role && coop_pool_full()) {
        g_sg_deny = 1;
        if ((g_pool_holds++ % 300) == 0) logf("coop: pool guard: entity pool full - spawner %08x held (%u holds so far)", g_sg_esi, g_pool_holds);
        return;
    }
    if (g_sp_sync != 1 || g_cfg.coop_role != 2) return;
    uint32_t sp = g_sg_esi;
    int i = -1;
    for (int k = 0; k < g_sp_n; k++) if (g_sp_obj[k] == sp) { i = k; break; }
    if (i < 0) return;                                            // not an indexed spawner: leave it alone
    if (i >= g_sp_host_n || g_sp_cnt[i] >= g_sp_host_cnt[i]) {
        g_sg_deny = 1; g_sp_stats[0]++;
        static uint32_t dl;
        if ((dl++ % 600) == 0) logf("coop: spawn sync: JOINER spawner #%d %08x held (ours %u, host %u) - %u holds so far", i, sp, g_sp_cnt[i], i < g_sp_host_n ? g_sp_host_cnt[i] : 0, g_sp_stats[0]);
    } else g_sp_stats[1]++;
}
__declspec(naked) void coop_thunk_spawn_gate()       // FUN_001b3030 entry: 8B 46 44 8B 56 40 re-executed
{
    __asm {
        pushad
        mov dword ptr [g_sg_esi], esi
        call coop_spawn_gate_check
        popad
        cmp dword ptr [g_sg_deny], 0
        jne denyspawn
        mov eax, dword ptr [esi + 0x44]
        mov edx, dword ptr [esi + 0x40]
        jmp dword ptr [g_sg_resume]
    denyspawn:
        xor eax, eax
        ret
    }
}
static void install_spawn_gate(void)
{
    char e[8] = {0};
    g_sp_sync = (GetEnvironmentVariableA("COOP_SPAWN_SYNC", e, sizeof e) && e[0] == '0') ? 0 : 1;
    if (!g_sp_sync) logf("coop: spawn sync: OFF (COOP_SPAWN_SYNC=0) - the hook stays for the pool guard");
    uint32_t site = xbe_va(0x001B3030);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x8B, 0x46, 0x44, 0x8B, 0x56, 0x40};
    if (memcmp(code, kExpect, 6) != 0) { logf("coop: spawn sync: 001b3030 bytes mismatch - not installed"); g_sp_sync = 0; return; }
    g_sg_resume = site + 6;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_spawn_gate - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("coop: spawn sync installed at FUN_001b3030 (joiner spawners follow the host's spawn counts)");
}
// FORCED SPAWN (play 20261113_184706: "some enemies didn't sync with the joiner"): spawners #31..#41 of
// Veblensk started on the host at 196-263 s with NO synced trigger - a spawner also starts by itself
// when ITS copy's player is near (FUN_001b3070: +0x28 enabled, +0x2A / +0x2B conditions). The joiner
// was elsewhere, so its copies never ran and the host's enemies had no puppet ("not spawned here yet").
// Joiner: a spawner whose host count is ahead of ours and whose current enemy (+0x24) is gone (none,
// or life state not 0/1 - FUN_001b3070's own test) is made to spawn now through the game's spawn
// FUN_001b3030 (ESI = spawner; the spawn gate lets it through because we are behind). At most one per
// spawner per 0.5 s and 3 per frame. Env COOP_FORCE_SPAWN=0 = off.
volatile uint32_t g_fs_esi, g_fs_fn; volatile uint8_t g_fs_ret;
__declspec(naked) static void coop_call_spawn()
{
    __asm {
        pushad
        mov esi, dword ptr [g_fs_esi]
        call dword ptr [g_fs_fn]
        mov byte ptr [g_fs_ret], al
        popad
        ret
    }
}
static void coop_spawn_force(void)
{
    static int on = -1;
    if (on < 0) { char e[8] = {0}; on = (GetEnvironmentVariableA("COOP_FORCE_SPAWN", e, sizeof e) && e[0] == '0') ? 0 : 1; logf("coop: forced spawn %s", on ? "ON (joiner spawners follow the host's counts)" : "OFF"); }
    if (!on || g_sp_sync != 1 || g_cfg.coop_role != 2 || !g_sp_host_n) return;
    static uint64_t last[256];
    static uint32_t nforced;
    uint64_t now = GetTickCount64();
    int budget = 3;
    // m1part test (joiner crash 0x191FED in a spawn): the joiner replayed trigger #56 (its spawner #53 then spawns by
    // itself after its short delay) and 50 ms later the catch-up forced #53 too - two spawns of one spawner at once. A
    // spawner is forced only after it has been behind the host for 1.5 s, and not in the 2 s after a level part switch.
    static uint64_t behind[256], seen_part;
    if (seen_part != g_part_ms) { seen_part = g_part_ms; memset(behind, 0, sizeof behind); memset(last, 0, sizeof last); }
    if (g_part_ms && now - g_part_ms < 2000) return;
    for (int i = 0; i < g_sp_n && i < g_sp_host_n && budget > 0; i++) {
        if (g_sp_cnt[i] >= g_sp_host_cnt[i]) { behind[i] = 0; continue; }
        if (!behind[i]) behind[i] = now;
        if (now - behind[i] < 1500 || now - last[i] < 500) continue;
        uint32_t o = g_sp_obj[i], cur = 0;
        if (!o || !coop_safe_copy(&cur, o + 0x24, 4)) continue;
        if (cur) { uint32_t life = 0; if (!coop_safe_copy(&life, cur + 0x384, 4) || life == 0 || life == 1) continue; }   // its enemy still lives
        if (coop_pool_full()) return;
        {   // two-PC play 20261114 (joiner crash 0x1B2B09, [0 + 0x20]): the host had entered the next section, where these
            // spawners live; on the joiner that section was not set up yet - the spawner's spawn record (+0x18) had no
            // object at +4 (FUN_001B2BC0 -> FUN_001B2AF0 reads [[rec+4]+0x20]). The game never runs such a spawner;
            // the catch-up waits until it is live here.
            uint32_t rec = 0, ro = 0;
            if (!coop_safe_copy(&rec, o + 0x18, 4) || rec < 0x80000000 || rec >= 0x84000000 || !coop_safe_copy(&ro, rec + 4, 4) ||
                ro < 0x80000000 || ro >= 0x84000000) {
                static uint32_t nl;
                if (nl++ < 20) logf("coop: forced spawn: JOINER spawner #%d %08x is not live here yet (record %08x object %08x) - waits", i, o, rec, ro);
                last[i] = now;
                continue;
            }
        }
        last[i] = now; budget--;
        g_fs_esi = o; g_fs_ret = 0; g_fs_fn = xbe_va(0x001B3030);
        coop_call_spawn();
        nforced++;
        if (nforced <= 60 || (nforced % 50) == 0)
            logf("coop: forced spawn: JOINER spawner #%d %08x behind the host (ours %u, host %u) -> spawn %s (%u forced)", i, o, g_sp_cnt[i], g_sp_host_cnt[i], g_fs_ret ? "OK" : "refused", nforced);
    }
}
static void coop_spawn_track(void)   // both roles: count each spawner's spawns (+0x24 -> a new body)
{
    if (g_sp_sync != 1 || !g_cfg.coop_role) return;
    coop_spawn_table();
    for (int i = 0; i < g_sp_n; i++) {
        uint32_t o = g_sp_obj[i], b = 0;
        if (!o || !coop_safe_copy(&b, o + 0x24, 4)) continue;
        if (b && b != g_sp_last[i]) {
            if (g_sp_cnt[i] < 255) g_sp_cnt[i]++;
            g_sp_stats[2]++;
            static int cl;
            if (cl++ < 150) logf("coop: spawn sync: %s spawner #%d %08x spawned %08x (count %u)", g_cfg.coop_role == 1 ? "HOST" : "JOINER", i, o, b, g_sp_cnt[i]);
        }
        g_sp_last[i] = b;
    }
}
// COOP_REG_DUMP=<sec>[,<sec>]: the AI TARGET REGISTRY (*0x2D1988: entries of 12 B from +0xFB0 =
// {type, object, x}; FUN_001aed40 reads type 0/1/2) - which objects the enemies can target (RE)
// ENTITY POOL WATCH (play 20261113 joiner crash at 0x189a33): FUN_001df440 (spawn) registers the body
// (+0x378 = registry idx via FUN_001880c0) then FUN_001899f0 takes a per-kind pool entry (R+0x22C48
// +0x7C kind 1 / +0x88 kind 2: {entries, used flags, count}; entry+0x20 = body, body+0x344 = entry).
// A full pool returns 0 -> write to 0x20. FUN_001abcc0 (unregister) frees both. Logs usage every 10 s.
// REGISTRY VALVE (play 20261113_195507: the joiner's target registry (32 slots, mask R+0xFA8) went from
// 9 to 31 entries in 10 s at 221-231 s and stayed full; from then on new bodies got +0x378 = -1, which
// FUN_001abcc0 (unregister) skips - their pool entries leaked too, the pool filled with corpses and the
// joiner's spawns were held for the rest of the session -> "some enemies didn't sync"). Cause not yet
// identified (the dump above shows the entries when it happens again). Valve: with 30+ entries used, an
// entry whose body is not in the game's active pawn list and is not our player / the clone / a squad
// roster member is released - through the game's own FUN_001abcc0 when the body still points at it
// (+0x378 == index and +0x344 = its pool entry), otherwise by clearing the bit (type 5, as unregister does).
volatile uint32_t g_rv_fn, g_rv_body, g_rv_reg;
__declspec(naked) static void coop_call_unregister()
{
    __asm {
        pushad
        mov ebx, dword ptr [g_rv_body]
        push dword ptr [g_rv_reg]
        call dword ptr [g_rv_fn]
        popad
        ret
    }
}
static void coop_registry_valve(void)
{
    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, mask = 0;
    if (R < 0x80000000 || R >= 0x84000000 || !coop_safe_copy(&mask, R + 0xFA8, 4)) return;
    {   // COOP_REG_LEAK_TEST=1 (test only): at 90 s fill every free slot with a stale type-0 entry (object 0)
        static int lt = -1;
        if (lt < 0) { char e[8] = {0}; lt = GetEnvironmentVariableA("COOP_REG_LEAK_TEST", e, sizeof e) && e[0] == '1'; }
        if (lt == 1 && g_coop_t0 && GetTickCount64() - g_coop_t0 > 90000) {
            lt = 2;
            for (int i = 0; i < 32; i++) if (!(mask & (1u << i))) {
                *(volatile uint32_t *)(uintptr_t)(R + 0xFB0 + i * 0xC) = 0;
                *(volatile uint32_t *)(uintptr_t)(R + 0xFB4 + i * 0xC) = 0;
                mask |= 1u << i;
            }
            *(volatile uint32_t *)(uintptr_t)(R + 0xFA8) = mask;
            logf("coop: registry valve TEST: registry filled with stale entries");
        }
    }
    int used = 0; for (int i = 0; i < 32; i++) if (mask & (1u << i)) used++;
    if (used < 30) return;
    uint32_t act[16]; uint32_t na = enemy_slots(act);
    uint32_t keep[6] = {g_p10_gdst, 0, 0, 0, 0, 0};
    coop_safe_copy(&keep[1], R + 0x22974, 16);                 // squad roster (slot 3 = our player)
    int freed = 0;
    for (int i = 0; i < 32; i++) {
        if (!(mask & (1u << i))) continue;
        uint32_t e3[3] = {0, 0, 0}; coop_safe_copy(e3, R + 0xFB0 + i * 0xC, 12);
        uint32_t b = e3[1];
        if (e3[0] == 1) {   // a type-1 logic entry its object no longer owns (its +0x30 points elsewhere) = leaked
            uint32_t own = 0xFFFFFFFF;
            if (b >= 0x80000000 && b < 0x84000000 && coop_safe_copy(&own, b + 0x30, 4) && own == (uint32_t)i) continue;
            *(volatile uint32_t *)(uintptr_t)(R + 0xFB0 + i * 0xC) = 5;
            *(volatile uint32_t *)(uintptr_t)(R + 0xFA8) &= ~(1u << i);
            freed++;
            static int tl;
            if (tl++ < 60) logf("coop: registry valve: leaked type-1 entry %d of logic object %08x (it owns %d) released", i, b, (int)own);
            continue;
        }
        if (e3[0] != 0) continue;
        bool live = false;
        for (uint32_t k = 0; k < na; k++) if (act[k] == b) live = true;
        for (int k = 0; k < 5; k++) if (keep[k] && keep[k] == b) live = true;
        if (live) continue;
        uint32_t idx = 0xFFFFFFFF, pe = 0, back = 0;
        bool ok = b >= 0x80000000 && b < 0x84000000 && coop_safe_copy(&idx, b + 0x378, 4) && idx == (uint32_t)i &&
                  coop_safe_copy(&pe, b + 0x344, 4) && pe >= 0x80000000 && pe < 0x84000000 && coop_safe_copy(&back, pe + 0x20, 4) && back == b;
        if (ok) { g_rv_fn = xbe_va(0x001ABCC0); g_rv_body = b; g_rv_reg = R; coop_call_unregister(); }
        else {
            *(volatile uint32_t *)(uintptr_t)(R + 0xFB0 + i * 0xC) = 5;
            *(volatile uint32_t *)(uintptr_t)(R + 0xFA8) &= ~(1u << i);
            if (b >= 0x80000000 && b < 0x84000000 && idx == (uint32_t)i) *(volatile uint32_t *)(uintptr_t)(b + 0x378) = 0xFFFFFFFF;
        }
        freed++;
        static int vl;
        if (vl++ < 60) logf("coop: registry valve (%s): entry %d body %08x (idx %d, pool entry %08x) not an active pawn -> released %s",
                            g_cfg.coop_role == 1 ? "HOST" : "JOINER", i, b, (int)idx, pe, ok ? "via FUN_001abcc0" : "(bit cleared)");
    }
    if (freed) logf("coop: registry valve: %d of %d entries released", freed, used);
}
static void coop_pool_watch(void)
{
    coop_registry_valve();
    static uint32_t f;
    if ((++f % 600) != 0) return;
    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, pl[3] = {0}, regmask = 0;
    if (!coop_safe_copy(pl, R + 0x22C48 + 0x7C, 12) || pl[2] > 256) return;
    coop_safe_copy(&regmask, R + 0xFA8, 4);
    uint8_t used[256] = {0}; coop_safe_copy(used, pl[1], pl[2]);
    int nu = 0, dead = 0, gone = 0; char list[512]; int lp = 0; list[0] = 0;
    for (uint32_t i = 0; i < pl[2]; i++) {
        if (!used[i]) continue;
        nu++;
        uint32_t e = 0, b = 0, life = 9, idx = 0; coop_safe_copy(&e, pl[0] + 4 * i, 4); coop_safe_copy(&b, e + 0x20, 4);
        coop_safe_copy(&life, b + 0x384, 4); coop_safe_copy(&idx, b + 0x378, 4);
        if (life) dead++;
        if (g_cfg.coop_role == 1 && g_p10_gdst && b != g_p10_gdst) {   // an ally whose target is the clone?
            uint32_t t = 1, ct = 2, br = 0, ti = 0xFFFFFFFF, ci = 0xFFFFFFFE;
            coop_safe_copy(&t, b + 0x39C, 4); coop_safe_copy(&ct, g_p10_gdst + 0x39C, 4);
            coop_safe_copy(&br, b + 0x324, 4); if (br) coop_safe_copy(&ti, br + 0x2A8, 4);
            coop_safe_copy(&ci, g_p10_gdst + 0x378, 4);
            if (t == ct && ti == ci) logf("coop: ally check: ally %08x (team %u) TARGETS the joiner clone", b, t);
        }
        if (lp < 480) lp += snprintf(list + lp, sizeof list - lp, " %08x/%u/%d%s", b, life, (int)idx, b == g_p10_gdst ? "C" : "");
        { uint32_t A = 0, blk = 0; coop_safe_copy(&A, b + 0x328, 4); if (A) coop_safe_copy(&blk, A + 0x54, 4);
          static int al; if (al++ < 40) logf("coop: anim block: body %08x A %08x blk %08x", b, A, blk); }
    }
    (void)gone;
    int nreg = 0; for (int i = 0; i < 32; i++) if (regmask & (1u << i)) nreg++;
    if (nreg > 24) {   // play 20261113_195507: the joiner's registry filled with 22 non-soldier entries in 10 s - who?
        static uint64_t lastd;
        uint64_t now = GetTickCount64();
        if (now - lastd > 30000) {
            lastd = now;
            char rl[900]; int rp = 0; rl[0] = 0;
            for (int i = 0; i < 32 && rp < 860; i++) {
                if (!(regmask & (1u << i))) continue;
                uint32_t e3[3] = {0, 0, 0}, vt = 0, kind = 0, life = 9; float pos[3] = {0, 0, 0};
                coop_safe_copy(e3, R + 0xFB0 + i * 0xC, 12);
                if (e3[1] >= 0x80000000 && e3[1] < 0x84000000) { coop_safe_copy(&vt, e3[1], 4); coop_safe_copy(&kind, e3[1] + 0xC4, 4); coop_safe_copy(&life, e3[1] + 0x384, 4); coop_safe_copy(pos, e3[1] + 0xA0, 12); }
                rp += snprintf(rl + rp, sizeof rl - rp, " [%d t%u %08x vt%08x k%u L%u @%.0f,%.0f,%.0f]", i, e3[0], e3[1], vt, kind, life, pos[0], pos[1], pos[2]);
            }
            logf("coop: registry dump (%s, %d used):%s", g_cfg.coop_role == 1 ? "HOST" : "JOINER", nreg, rl);
        }
    }
    logf("coop: pool watch (%s): kind-1 pool %d / %u used (%d not alive), registry mask %08x (%d) | body/life/idx:%s",
         g_cfg.coop_role == 1 ? "HOST" : "JOINER", nu, pl[2], dead, regmask, nreg, list);
}
static void coop_reg_dump(uint32_t me)
{
    static float ts[3] = {-1, -1, -1}; static int ti = -1;
    if (ti < 0) { ti = 0; char e[48] = {0}; if (GetEnvironmentVariableA("COOP_REG_DUMP", e, sizeof e)) sscanf(e, "%f,%f,%f", &ts[0], &ts[1], &ts[2]); }
    if (ti >= 3 || ts[ti] <= 0 || !g_coop_t0 || (float)(GetTickCount64() - g_coop_t0) * 0.001f < ts[ti]) return;
    ti++;
    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x2D1988;
    if (R < 0x80000000 || R >= 0x84000000) return;
    uint32_t hdr[4] = {0}; coop_safe_copy(hdr, R + 0xFA0, 16);
    logf("coop: REGISTRY %08x at %.0f s: +FA0 %08x %08x %08x %08x | player %08x clone %08x", R, ts[ti - 1], hdr[0], hdr[1], hdr[2], hdr[3], me, g_p10_gdst);
    for (int i = 0; i < 4; i++) {   // the SIGHT CANDIDATE list FUN_001ad440 loops (registry +0x22974, 4 entries)
        uint32_t c = 0, k = 0, idx = 0, t = 0; coop_safe_copy(&c, R + 0x22974 + 4 * i, 4);
        if (c >= 0x80000000 && c < 0x84000000) { coop_safe_copy(&k, c + 0xC4, 4); coop_safe_copy(&idx, c + 0x378, 4); coop_safe_copy(&t, c + 0x39C, 4); }
        logf("coop: sight candidate[%d] %08x%s kind %u +378 %d team %u", i, c, c == me ? " = OUR PLAYER" : (c == g_p10_gdst ? " = THE CLONE" : ""), k, (int)idx, t);
    }
    { uint32_t ci = 0; if (g_p10_gdst) coop_safe_copy(&ci, g_p10_gdst + 0x378, 4); logf("coop: clone +378 = %d", (int)ci); }
    for (int i = 0; i < 48; i++) {
        uint32_t e3[3] = {0}; coop_safe_copy(e3, R + 0xFB0 + i * 0xC, 12);
        if (!e3[0] && !e3[1] && !e3[2]) continue;
        uint32_t k = 0, t = 0, vt = 0; float q[3] = {0, 0, 0};
        if (e3[1] >= 0x80000000 && e3[1] < 0x84000000) { coop_safe_copy(&vt, e3[1], 4); coop_safe_copy(&k, e3[1] + 0xC4, 4); coop_safe_copy(&t, e3[1] + 0x39C, 4); coop_safe_copy(q, e3[1] + 0xA0, 12); }
        logf("coop: registry[%02d] type %u obj %08x%s x %08x | vt %08x kind %u team %u at %.1f %.1f %.1f", i, e3[0], e3[1],
             e3[1] == me ? " = OUR PLAYER" : (e3[1] == g_p10_gdst ? " = THE CLONE" : ""), e3[2], vt, k, t, q[0], q[1], q[2]);
    }
}
// ENEMY ANIMATION SYNC (day 13; owner: "sync every enemy behaviour tree and every animation node to the
// joiner including scripted ones", "enemies also have hit reactions when shot").
// Every body animation of a soldier is requested through two calls of the anim library (blk = the anim
// block [[body+0x328]+0x54]; hash = the animation's id, the same value on both machines):
//   FUN_00091e00(blk, hash, char blend, char flag)  - play with a blend (-> FUN_00097240 / FUN_000970c0)
//   FUN_00091e70(blk, hash)                         - play at once (-> FUN_000970c0)
// (both cdecl, return AL; the internal sink FUN_00096fd0 sets blk+0x10 = hash, blk+0xC = clip; queued
// follow-ups FUN_00094c80 / FUN_00094da0 / FUN_00097150 run inside the local playback on both copies.)
// HOST: every call on an enemy's block is recorded ("CLAN" events, each sent in 2 packets: UDP) plus a
// snapshot of every enemy's current hash + time twice a second ("CLAS").
// JOINER: the puppets' OWN calls are dropped (their local AI would pick its own animations); the host's
// events are replayed on the puppet's block through the same call; a snapshot whose hash differs from
// the puppet's -> FUN_00091e70 + the host's time. Dying / dead puppets keep their local calls (the kill
// path plays the death). Behaviour (the brain) stays local + the CLAI whitelist (the broad brain mirror
// crashed the joiner, play213): what the joiner SEES is the animation, and that now comes from the host.
// Env COOP_ANIM_SYNC=0 = off.
static int g_an_on = -1;
volatile uint32_t g_an_blk, g_an_hash, g_an_a3, g_an_a4, g_an_which, g_an_skip, g_an_replay;
volatile uint32_t g_an_resume[2];
static uint32_t g_an_hblk[16], g_an_hbody[16]; static int g_an_hn;       // host: enemy blocks this frame
static uint32_t g_an_pblk[48], g_an_pbody[48]; static int g_an_pn;       // joiner: puppet blocks / bodies this frame
struct AnimEv { uint32_t body, hash, seq; uint8_t which, a3, a4, pad; };
static AnimEv g_an_out[64]; static int g_an_out_n; static AnimEv g_an_prev[64]; static int g_an_prev_n;
static uint32_t g_an_seq, g_an_last_in;
volatile uint32_t g_an_clone_lock;   // the clone's anim block while it holds a forced pose (downed): its own calls are dropped
static uint32_t g_an_stats[6];   // host events, joiner drops, joiner replays, snapshot fixes, replays with no puppet, snapshots
static uint32_t coop_anim_blk(uint32_t body)
{
    uint32_t A = 0, blk = 0;
    if (!coop_safe_copy(&A, body + 0x328, 4) || A < 0x80000000 || A >= 0x84000000) return 0;
    if (!coop_safe_copy(&blk, A + 0x54, 4) || blk < 0x80000000 || blk >= 0x84000000) return 0;
    return blk;
}
extern "C" void coop_anim_check(void)
{
    g_an_skip = 0;
    uint32_t blk = g_an_blk;
    if (g_cfg.coop_role == 1) {
        for (int i = 0; i < g_an_hn; i++) if (g_an_hblk[i] == blk) {
            if (g_an_out_n < 64) {
                AnimEv &e = g_an_out[g_an_out_n++];
                e.body = g_an_hbody[i]; e.hash = g_an_hash; e.seq = ++g_an_seq;
                e.which = (uint8_t)g_an_which; e.a3 = (uint8_t)g_an_a3; e.a4 = (uint8_t)g_an_a4; e.pad = 0;
            }
            g_an_stats[0]++;
            {   // COOP_DEATH_ANIM_LOG=1: the animations an enemy gets while dying (the downed pose source)
                static int dl = -1; if (dl < 0) { char e[8] = {0}; dl = GetEnvironmentVariableA("COOP_DEATH_ANIM_LOG", e, sizeof e) && e[0] == '1'; }
                float hp = 1; uint32_t life = 0; coop_safe_copy(&hp, g_an_hbody[i] + 0x2F0, 4); coop_safe_copy(&life, g_an_hbody[i] + 0x384, 4);
                static int n;
                uint8_t cr = 0; coop_safe_copy(&cr, g_an_hbody[i] + 0x3A1, 1);
                if (dl && n++ < 3000) logf("coop: death anim: enemy %08x (hp %.1f life %u crouch %u) plays %08x via %s (a3 %u a4 %u)", g_an_hbody[i], hp, life, cr, g_an_hash, g_an_which ? "FUN_00091e70" : "FUN_00091e00", g_an_a3, g_an_a4);
            }
            return;
        }
    } else if (g_cfg.coop_role == 2 && !g_an_replay) {
        for (int i = 0; i < g_an_pn; i++) if (g_an_pblk[i] == blk) { g_an_skip = 1; g_an_stats[1]++; return; }
    }
    if (g_an_clone_lock && !g_an_replay && blk == g_an_clone_lock) g_an_skip = 1;   // the clone holds a forced pose (downed)
}
__declspec(naked) void coop_thunk_anim_play()      // FUN_00091e00 entry: 56 8B 74 24 08 re-executed
{
    __asm {
        pushad
        mov eax, dword ptr [esp + 0x24]
        mov dword ptr [g_an_blk], eax
        mov eax, dword ptr [esp + 0x28]
        mov dword ptr [g_an_hash], eax
        movzx eax, byte ptr [esp + 0x2C]
        mov dword ptr [g_an_a3], eax
        movzx eax, byte ptr [esp + 0x30]
        mov dword ptr [g_an_a4], eax
        mov dword ptr [g_an_which], 0
        call coop_anim_check
        popad
        cmp dword ptr [g_an_skip], 0
        jne skipit
        push esi
        mov esi, dword ptr [esp + 8]
        jmp dword ptr [g_an_resume + 0]
    skipit:
        mov al, 1
        ret
    }
}
__declspec(naked) void coop_thunk_anim_now()       // FUN_00091e70 entry: 56 8B 74 24 08 re-executed
{
    __asm {
        pushad
        mov eax, dword ptr [esp + 0x24]
        mov dword ptr [g_an_blk], eax
        mov eax, dword ptr [esp + 0x28]
        mov dword ptr [g_an_hash], eax
        mov dword ptr [g_an_a3], 0
        mov dword ptr [g_an_a4], 0
        mov dword ptr [g_an_which], 1
        call coop_anim_check
        popad
        cmp dword ptr [g_an_skip], 0
        jne skipit2
        push esi
        mov esi, dword ptr [esp + 8]
        jmp dword ptr [g_an_resume + 4]
    skipit2:
        mov al, 1
        ret
    }
}
// ANIM EVENT GUARD (an1 joiner crash 0x180537): an animation carries events (type 2, id 0..9) that
// FUN_00179830 dispatches to the handler table [*0x2D12C8 + 0x948 + id*4] as handler(eax, edx, A),
// A = the anim instance (A+0 = body). Handler FUN_001804a0 reads the BRAIN state of the body
// (link +0x90 == 1 -> object [link + 0x1EA0 + [link+0x1EA8]*4] -> vt+0x18): a puppet's private link
// has index 2 there (out of range) - the host's animation reached a state the puppet's brain never
// did. The call site 0x179882 (FF D1 83 C4 0C = call ecx; add esp, 0xC) goes through
// coop_thunk_anim_event: puppets' events run under SEH; a fault skips that one event.
static uint32_t g_an_evfault;
extern "C" void __cdecl coop_anim_event_call(uint32_t fn, uint32_t a, uint32_t b, uint32_t A)
{
    typedef void(__cdecl *ev_fn)(uint32_t, uint32_t, uint32_t);
    uint32_t body = 0;
    if (A >= 0x80000000 && A < 0x84000000) coop_safe_copy(&body, A, 4);
    // play 20261113_182730: the same fault on a body the host had just killed and the joiner had just
    // re-spawned (not a puppet yet) -> the bad index is not puppet-only. FUN_001804a0's slot is checked
    // for every body on both roles; on the joiner every event runs under SEH.
    if (fn == xbe_va(0x001804A0) && body >= 0x80000000 && body < 0x84000000) {
        uint32_t link = 0, m = 0, idx = 0, obj = 0;
        coop_safe_copy(&link, body + 0x324, 4);
        if (link && coop_safe_copy(&m, link + 0x90, 4) && m == 1) {
            bool ok = coop_safe_copy(&idx, link + 0x1EA8, 4) && idx < 2 && coop_safe_copy(&obj, link + 0x1EA0 + idx * 4, 4) &&
                      ((obj >= 0x80000000 && obj < 0x84000000) || (obj >= 0x00100000 && obj < 0x10000000));
            if (!ok) {
                if (g_an_evfault++ < 30) logf("coop: anim event guard: body %08x link %08x state slot %u -> %08x is not an object - event %08x skipped", body, link, idx, obj, fn);
                return;
            }
        }
    }
    if (g_cfg.coop_role != 2) { ((ev_fn)(uintptr_t)fn)(a, b, A); return; }
    __try { ((ev_fn)(uintptr_t)fn)(a, b, A); }
    __except (1) {
        if (g_an_evfault++ < 30) logf("coop: anim event guard: puppet %08x event handler %08x faulted (its brain state is not the host's) - event skipped", body, fn);
    }
}
__declspec(naked) void coop_thunk_anim_event()      // replaces call ecx; add esp, 0xC at 0x179882
{
    __asm {
        push dword ptr [esp + 0xC]
        push dword ptr [esp + 0xC]
        push dword ptr [esp + 0xC]
        push ecx
        call coop_anim_event_call
        add esp, 0x10
        ret 0xC
    }
}
typedef char(__cdecl *an_play_fn)(uint32_t blk, uint32_t hash, int a3, int a4);
typedef char(__cdecl *an_now_fn)(uint32_t blk, uint32_t hash);
static void install_anim_sync(void)
{
    char e[8] = {0};
    g_an_on = (GetEnvironmentVariableA("COOP_ANIM_SYNC", e, sizeof e) && e[0] == '0') ? 0 : 1;
    if (!g_an_on) { logf("coop: anim sync: OFF (COOP_ANIM_SYNC=0)"); return; }
    static const uint8_t kEntry[5] = {0x56, 0x8B, 0x74, 0x24, 0x08};
    uint32_t va[2] = {0x00091E00, 0x00091E70}; void *th[2] = {(void *)coop_thunk_anim_play, (void *)coop_thunk_anim_now};
    for (int i = 0; i < 2; i++) if (memcmp((void *)(uintptr_t)xbe_va(va[i]), kEntry, 5) != 0) {
        logf("coop: anim sync: %08x bytes mismatch - not installed", va[i]); g_an_on = 0; return;
    }
    for (int i = 0; i < 2; i++) {
        uint32_t a = xbe_va(va[i]);
        auto *code = (uint8_t *)(uintptr_t)a;
        g_an_resume[i] = a + 5;
        code[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)th[i] - (a + 5));
        memcpy(code + 1, &rel, 4);
    }
    {
        uint32_t a = xbe_va(0x00179882);
        auto *code = (uint8_t *)(uintptr_t)a;
        static const uint8_t kCall[5] = {0xFF, 0xD1, 0x83, 0xC4, 0x0C};
        if (memcmp(code, kCall, 5) == 0) {
            code[0] = 0xE8;
            int32_t rel = (int32_t)((uintptr_t)coop_thunk_anim_event - (a + 5));
            memcpy(code + 1, &rel, 4);
        } else logf("coop: anim event guard: 00179882 bytes mismatch - not installed");
    }
    logf("coop: anim sync installed (FUN_00091e00 / FUN_00091e70: host records, joiner puppets replay; event guard at 0x179882)");
}
static bool coop_anim_puppet_live(uint32_t b)
{
    float hp = 0; uint32_t life = 1;
    coop_safe_copy(&hp, b + 0x2F0, 4); coop_safe_copy(&life, b + 0x384, 4);
    return hp > 0.0f && life == 0;
}
// joiner: replay one host animation call on our puppet
static void coop_anim_replay(uint32_t hostbody, uint32_t hash, int which, int a3, int a4)
{
    uint32_t b = coop_host_to_local(hostbody);
    if (!b || !coop_anim_puppet_live(b)) { g_an_stats[4]++; return; }
    uint32_t blk = coop_anim_blk(b);
    if (!blk) { g_an_stats[4]++; return; }
    g_an_replay = 1;
    if (which == 0) ((an_play_fn)(uintptr_t)xbe_va(0x00091E00))(blk, hash, a3, a4);
    else ((an_now_fn)(uintptr_t)xbe_va(0x00091E70))(blk, hash);
    g_an_replay = 0;
    g_an_stats[2]++;
}
// COOP_ANIM_TRY="h1,h2,..." (test, joiner): from 80 s, every 3 s play the next hash on the HOST's clone (our
// g_p10_gdst) with its own calls held - to pick a downed pose by screenshot
static void coop_anim_try(void)
{
    static int n = -1; static uint32_t h[24]; static int cur = -1;
    if (n < 0) {
        n = 0; char e[512] = {0};
        if (GetEnvironmentVariableA("COOP_ANIM_TRY", e, sizeof e)) for (char *t = strtok(e, ","); t && n < 24; t = strtok(nullptr, ",")) h[n++] = strtoul(t, nullptr, 16);
    }
    if (!n || g_cfg.coop_role != 2 || !g_coop_t0 || !g_p10_gdst) return;
    int64_t t = (int64_t)(GetTickCount64() - g_coop_t0) - 80000;
    if (t < 0) return;
    int k = (int)(t / 3000);
    if (k >= n) { g_an_clone_lock = 0; return; }
    if (k == cur) return;
    cur = k;
    uint32_t blk = coop_anim_blk(g_p10_gdst);
    if (!blk) return;
    g_an_clone_lock = blk;
    g_an_replay = 1; ((an_now_fn)(uintptr_t)xbe_va(0x00091E70))(blk, h[k]); g_an_replay = 0;
    logf("coop: anim try #%d: the clone plays %08x (frame %u)", k, h[k], (uint32_t)((GetTickCount64() - g_coop_t0) * 60 / 1000));
}
// every frame (both roles): the block tables; host: send the events + the 2 Hz snapshot
static void coop_anim_step(uint32_t obj)
{
    coop_anim_try();
    if (g_an_on != 1 || !g_cfg.coop_role || obj < 0x80000000 || obj >= 0x84000000) return;
    if (g_cfg.coop_role == 1) {
        uint32_t sl[16]; uint32_t ns = enemy_slots(sl);
        g_an_hn = 0;
        for (uint32_t i = 0; i < ns; i++) {
            if (!is_enemy(sl[i], obj)) continue;
            uint32_t blk = coop_anim_blk(sl[i]);
            if (blk) { g_an_hblk[g_an_hn] = blk; g_an_hbody[g_an_hn] = sl[i]; g_an_hn++; }
        }
        // events: this frame's + last frame's again (a lost packet still delivers each event once)
        if (g_an_out_n || g_an_prev_n) {
            struct { uint32_t magic, seq, n; AnimEv e[128]; } pk;
            pk.magic = 0x4E414C43; pk.seq = g_an_seq; pk.n = 0;
            for (int i = 0; i < g_an_prev_n; i++) pk.e[pk.n++] = g_an_prev[i];
            for (int i = 0; i < g_an_out_n; i++) pk.e[pk.n++] = g_an_out[i];
            sendto(g_coop_sock, (const char *)&pk, 12 + pk.n * (int)sizeof(AnimEv), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            memcpy(g_an_prev, g_an_out, g_an_out_n * sizeof(AnimEv)); g_an_prev_n = g_an_out_n; g_an_out_n = 0;
        }
        static uint32_t sf;
        if ((sf++ % 30) == 7 && g_an_hn) {                          // CLAS snapshot: {body, hash, time}
            struct { uint32_t magic, n; struct { uint32_t body, hash; float t; } e[16]; } sp;
            sp.magic = 0x53414C43; sp.n = 0;
            for (int i = 0; i < g_an_hn; i++) {
                uint32_t h = 0; float t = 0;
                coop_safe_copy(&h, g_an_hblk[i] + 0x10, 4); coop_safe_copy(&t, g_an_hblk[i] + 0x14, 4);
                sp.e[sp.n].body = g_an_hbody[i]; sp.e[sp.n].hash = h; sp.e[sp.n].t = t; sp.n++;
            }
            sendto(g_coop_sock, (const char *)&sp, 8 + sp.n * 12, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        }
    } else {
        g_an_pn = 0;
        for (int i = 0; i < g_pl_n && g_an_pn < 48; i++) {
            uint32_t b = g_pl[i].body;
            if (!b || !g_pl[i].link || b == g_p10_gdst || !coop_anim_puppet_live(b)) continue;
            uint32_t blk = coop_anim_blk(b);
            if (blk) { g_an_pblk[g_an_pn] = blk; g_an_pbody[g_an_pn] = b; g_an_pn++; }
        }
    }
    static uint64_t lastlog;
    uint64_t now = GetTickCount64();
    if (now - lastlog >= 10000) {
        lastlog = now;
        if (g_cfg.coop_role == 1) logf("coop: anim sync HOST: %u enemy anim calls sent (%d enemies)", g_an_stats[0], g_an_hn);
        else logf("coop: anim sync JOINER: %u puppet-local calls dropped, %u host calls replayed (%u with no puppet here), %u snapshot fixes / %u snapshots (%d puppets)",
                  g_an_stats[1], g_an_stats[2], g_an_stats[4], g_an_stats[3], g_an_stats[5], g_an_pn);
    }
}
static void coop_pj_step(void);
static void coop_pj_recv(const uint8_t *raw, int len);
static void coop_prop_step(void);
static void coop_prop_recv(const uint8_t *raw, int len);
// OBJECTIVE SYNC (owner: "sync objectives"). Objectives change ONLY through objective logic nodes (class vt
// 0028EB2C, Veblensk list 8 x17): their action vt+8 FUN_001c2290 (thiscall ECX = node, 1 stack arg, RET 4)
// reads the mode byte [[node+0x20]+0xC]: 0 = ACTIVATE (FUN_001ba770, EDI = world+0x930, objective id pair,
// 1 = show), 1 = PROGRESS (FUN_001ba710, EBX = world+0x930 -> FUN_001ba430 count / state 3 = done ->
// the FE_PRIMARYOBJ_* HUD messages via FUN_0012c1d0 / FUN_0012c250). Logic nodes have the same address on
// both copies. HOST: every node action runs and goes out as "CLOB" (3x, seq); JOINER: its own node actions
// are dropped, the host's are replayed on the same node. Env COOP_OBJ_SYNC=0 = off.
static int g_ob_on = -1;
volatile uint32_t g_ob_resume, g_ob_ecx, g_ob_arg, g_ob_skip, g_ob_replay;
static uint32_t g_ob_out[16]; static int g_ob_out_n; static uint32_t g_ob_seq, g_ob_last_in;
struct ObAct { uint32_t seq, node; };
static ObAct g_ob_hist[16]; static uint32_t g_ob_hist_w;   // host: last actions (resent)
static uint32_t g_ob_stats[3];   // host actions, joiner local dropped, joiner replayed
extern "C" void coop_ob_check(void)
{
    g_ob_skip = 0;
    if (g_ob_on != 1 || !g_cfg.coop_role || g_ob_replay) return;
    uint32_t node = g_ob_ecx, def = 0; uint8_t mode = 9;
    coop_safe_copy(&def, node + 0x20, 4); if (def) coop_safe_copy(&mode, def + 0xC, 1);
    if (g_cfg.coop_role == 1) {
        g_ob_hist[g_ob_hist_w & 15].seq = ++g_ob_seq; g_ob_hist[g_ob_hist_w & 15].node = node; g_ob_hist_w++;
        g_ob_stats[0]++;
        logf("coop: objective sync: HOST objective node %08x (mode %u: %s) -> joiner", node, mode, mode == 0 ? "activate" : mode == 1 ? "progress" : "other");
    } else {
        g_ob_skip = 1; g_ob_stats[1]++;
        logf("coop: objective sync: JOINER local objective node %08x (mode %u) dropped - the host's decides", node, mode);
    }
}
__declspec(naked) void coop_thunk_objective()      // FUN_001c2290 entry: 8B 41 20 0F BE 50 0C re-executed
{
    __asm {
        pushad
        mov dword ptr [g_ob_ecx], ecx
        call coop_ob_check
        popad
        cmp dword ptr [g_ob_skip], 0
        jne skipob
        mov eax, dword ptr [ecx + 0x20]
        movsx edx, byte ptr [eax + 0xC]
        jmp dword ptr [g_ob_resume]
    skipob:
        ret 4
    }
}
volatile uint32_t g_obr_fn, g_obr_ecx, g_obr_esp;
__declspec(naked) static void coop_thunk_ob_replay()
{
    __asm {
        pushad
        mov dword ptr [g_obr_esp], esp
        push 0
        mov ecx, dword ptr [g_obr_ecx]
        call dword ptr [g_obr_fn]
        mov esp, dword ptr [g_obr_esp]
        popad
        ret
    }
}
static void install_objective_sync(void)
{
    char e[8] = {0};
    g_ob_on = (GetEnvironmentVariableA("COOP_OBJ_SYNC", e, sizeof e) && e[0] == '0') ? 0 : 1;
    if (!g_ob_on) { logf("coop: objective sync: OFF (COOP_OBJ_SYNC=0)"); return; }
    uint32_t site = xbe_va(0x001C2290);
    auto *c = (uint8_t *)(uintptr_t)site;
    static const uint8_t k[7] = {0x8B, 0x41, 0x20, 0x0F, 0xBE, 0x50, 0x0C};
    if (memcmp(c, k, 7) != 0) { logf("coop: objective sync: 001c2290 bytes mismatch - not installed"); g_ob_on = 0; return; }
    g_ob_resume = site + 7;
    c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)coop_thunk_objective - (site + 5)); memcpy(c + 1, &rel, 4); c[5] = 0x90; c[6] = 0x90;
    logf("coop: objective sync installed at FUN_001c2290 (objective nodes: host decides, joiner replays)");
}
// PICKUP SYNC (owner day 14: "pickups next"). FUN_001bed90 marks a pickup item COLLECTED (+0x154 = 1; the
// item tick FUN_001e4580 then removes it). ESI = the item, ECX = the item pool (64 slots x 0x160, slot flag
// at pool +0x5804), stack arg = sound flag, RET 4. Its callers (FUN_001c9dd0 weapon swap, FUN_001eacd0 /
// FUN_001e4530 ...) hand out the ammo / weapon first; this only marks the item. Each copy tells the other
// which item went ("CLPU" {seq, slot, type +0x140, sub +0x148, pos +0xA0}, 3 sends); the other marks ITS
// item collected through the same function (sound flag 0) - nobody gets the partner's ammo.
// Entry F6 86 52 01 00 00 08 (test byte [esi+0x152],8) re-executed last (the next JE reads its flags).
// Env COOP_PICKUP_SYNC=0 = off.
volatile uint32_t g_pu_resume, g_pu_esi, g_pu_ecx, g_pu_snd, g_pu_replay, g_pu_fn;
static int g_pu_on;
struct PickMsg { uint32_t magic, seq; int32_t slot; uint32_t type; uint8_t sub, pad[3]; float pos[3]; };
static PickMsg g_pu_out[8]; static int g_pu_out_n; static int g_pu_out_sends;
static PickMsg g_pu_in[16]; static int g_pu_in_n; static uint8_t g_pu_in_age[16];   // frames waited (dr2: the pickup can beat its mirrored drop)
static uint32_t g_pu_seq, g_pu_stats[3];   // sent, applied, unmatched
static void pu_item_info(uint32_t it, uint32_t pool, int32_t *slot, uint32_t *type, uint8_t *sub, float pos[3])
{
    *slot = -1; *type = 0; *sub = 0; pos[0] = pos[1] = pos[2] = 0;
    if (pool >= 0x80000000 && pool < 0x84000000 && it >= pool && it < pool + 64 * 0x160 && (it - pool) % 0x160 == 0) *slot = (int32_t)((it - pool) / 0x160);
    coop_safe_copy(type, it + 0x140, 4); coop_safe_copy(sub, it + 0x148, 1); coop_safe_copy(pos, it + 0xA0, 12);
}
extern "C" void coop_pickup_note(void)
{
    if (g_pu_replay) return;
    uint32_t it = g_pu_esi, pool = g_pu_ecx;
    uint8_t done = 0; coop_safe_copy(&done, it + 0x154, 1);
    if (done) return;                                    // already collected
    PickMsg m = {}; m.magic = 0x55504C43; m.seq = ++g_pu_seq ^ (g_cfg.coop_role << 24);
    pu_item_info(it, pool, &m.slot, &m.type, &m.sub, m.pos);
    uint32_t gp = *(volatile uint32_t *)(uintptr_t)0x002D0D64, vt = 0; coop_safe_copy(&vt, it, 4);
    static int n;
    if (n++ < 60) logf("coop: pickup sync: WE collected item %08x (vt %08x, pool %08x%s, slot %d) type %u sub %u at %.1f %.1f %.1f (sound %u) -> peer",
                       it, vt, pool, pool == gp ? " = *0x2D0D64" : "", m.slot, m.type, m.sub, m.pos[0], m.pos[1], m.pos[2], g_pu_snd & 0xFF);
    if (g_pu_out_n < 8) { g_pu_out[g_pu_out_n++] = m; g_pu_out_sends = 3; }
    g_pu_stats[0]++;
}
__declspec(naked) void coop_thunk_pickup()
{
    __asm {
        pushad
        mov dword ptr [g_pu_esi], esi
        mov dword ptr [g_pu_ecx], ecx
        mov eax, dword ptr [esp + 0x24]
        mov dword ptr [g_pu_snd], eax
        call coop_pickup_note
        popad
        test byte ptr [esi + 0x152], 8
        jmp dword ptr [g_pu_resume]
    }
}
__declspec(naked) void coop_call_pickup()   // ECX = pool, ESI = item: FUN_001bed90(0) through the trampoline
{
    __asm {
        pushad
        mov ecx, dword ptr [g_pu_ecx]
        mov esi, dword ptr [g_pu_esi]
        push 0
        call dword ptr [g_pu_fn]
        popad
        ret
    }
}
static void install_pickup_sync(void)
{
    char e[8] = {0};
    if (GetEnvironmentVariableA("COOP_PICKUP_SYNC", e, sizeof e) && e[0] == '0') { logf("coop: pickup sync: OFF (COOP_PICKUP_SYNC=0)"); return; }
    static const uint8_t k[7] = {0xF6, 0x86, 0x52, 0x01, 0x00, 0x00, 0x08};
    uint32_t a = xbe_va(0x001BED90); auto *c = (uint8_t *)(uintptr_t)a;
    if (memcmp(c, k, 7) != 0) { logf("coop: pickup sync: FUN_001bed90 bytes mismatch - not installed"); return; }
    // the replay call must skip our entry jump: a trampoline = the original test + a jump to +7
    static uint8_t *tramp = (uint8_t *)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf("coop: pickup sync: no trampoline memory - not installed"); return; }
    memcpy(tramp, k, 7); tramp[7] = 0xE9; int32_t r2 = (int32_t)((a + 7) - ((uint32_t)(uintptr_t)tramp + 12)); memcpy(tramp + 8, &r2, 4);
    g_pu_fn = (uint32_t)(uintptr_t)tramp; g_pu_resume = a + 7;
    c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)coop_thunk_pickup - (a + 5)); memcpy(c + 1, &rel, 4);
    c[5] = 0x90; c[6] = 0x90;
    g_pu_on = 1;
    logf("coop: pickup sync installed at FUN_001bed90 (a collected item is collected on the other copy too)");
}
// DROP / SWAP MIRROR (owner day 14: "sync the enemy drops too AND weapon swapping"). Every DYNAMIC pickup
// is initialised by FUN_001ddd90 (thiscall ECX = type -> +0x140; stack: item, sub -> +0x148, model, +0x150,
// flags +0x152, +0x153; EAX = the 4x4 matrix -> +0x70..+0xAF). Level items use FUN_001ddef0 instead.
// Step 1 (discovery): log each init with the code addresses on the stack (which spawner / caller).
// Entry 55 8B EC 83 E4 F0 re-executed.
volatile uint32_t g_dr_resume, g_dr_ecx, g_dr_esp, g_dr_replay;
extern volatile uint32_t g_drs_fn;   // FUN_001e10d0 (set at install, used by the mirror spawn)
struct DropRec { uint32_t item, type, sub, model, stk[6]; };
static DropRec g_dr_new[16]; static int g_dr_new_n;
extern "C" void coop_drop_note(void)
{
    if (g_dr_replay || g_dr_new_n >= 16) return;
    DropRec &r = g_dr_new[g_dr_new_n++];
    const uint32_t *sp = (const uint32_t *)(uintptr_t)g_dr_esp;
    r.item = sp[1]; r.type = g_dr_ecx; r.sub = sp[2] & 0xFF; r.model = sp[3];
    int k = 0;
    for (int i = 0; i < 0x80 && k < 6; i++) { uint32_t v = 0; if (!coop_safe_copy(&v, g_dr_esp + 4 * i, 4)) break; if (v >= 0x00011000 && v < 0x00230000) r.stk[k++] = v; }
    for (; k < 6; k++) r.stk[k] = 0;
}
__declspec(naked) void coop_thunk_drop()
{
    __asm {
        pushad
        mov dword ptr [g_dr_ecx], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_dr_esp], eax
        call coop_drop_note
        popad
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_dr_resume]
    }
}
static void install_drop_sync(void)
{
    char e[8] = {0};
    if (GetEnvironmentVariableA("COOP_DROP_SYNC", e, sizeof e) && e[0] == '0') { logf("coop: drop sync: OFF (COOP_DROP_SYNC=0)"); return; }
    static const uint8_t k[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    uint32_t a = xbe_va(0x001DDD90); auto *c = (uint8_t *)(uintptr_t)a;
    if (memcmp(c, k, 6) != 0) { logf("coop: drop sync: FUN_001ddd90 bytes mismatch - not installed"); return; }
    g_dr_resume = a + 6;
    g_drs_fn = xbe_va(0x001E10D0);
    c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)coop_thunk_drop - (a + 5)); memcpy(c + 1, &rel, 4); c[5] = 0x90;
    logf("coop: drop sync installed at FUN_001ddd90 (dynamic pickup inits)");
}
// Step 2 (dr1 discovery -> rules). Stacks of the inits:
//   level items      1ddf4c (FUN_001ddef0) / 1e14b7 (FUN_001e1380)        -> identical on both copies: local
//   ENEMY DROPS      inside FUN_001ea8a0 (1eaa93 / 1eac9f) or FUN_001e7090 (1e7218 / 1e73b7), from the pawn
//                    tick 1f090e; ammo 0/1, weapon 2/x, 1/3. Each copy's puppet dies elsewhere (dr1: 4-5 m
//                    apart) -> HOST decides: the host mirrors its drops, the joiner removes its own.
//   WEAPON SWAP DROP inside FUN_001e2920 (1e2a4a) <- the player update (1f07aa): the old weapon, ~0.5 s after
//                    the swap's collect -> mirrored by whoever swapped.
//   anything else    local.
// Mirror = "CLDR" {seq, kind, type, sub, matrix 4x4} (3 sends); the peer spawns it with FUN_001e10d0
// (EAX = item pool; stack type, sub, matrix*, velocity*, angular velocity*; RET 0x14) at zero velocity.
volatile uint32_t g_drs_fn, g_drs_pool, g_drs_type, g_drs_sub;
__declspec(align(16)) static float g_drs_mat[16], g_drs_vel[4], g_drs_avel[4];
__declspec(naked) void coop_call_spawn_item()
{
    __asm {
        pushad
        mov eax, dword ptr [g_drs_pool]
        push offset g_drs_avel
        push offset g_drs_vel
        push offset g_drs_mat
        push dword ptr [g_drs_sub]
        push dword ptr [g_drs_type]
        call dword ptr [g_drs_fn]
        popad
        ret
    }
}
struct DropMsg { uint32_t magic, seq, kind, type, sub; float mat[16]; };
static DropMsg g_drm_out[8]; static int g_drm_out_n, g_drm_out_sends;
static DropMsg g_drm_in[16]; static int g_drm_in_n;
static uint32_t g_drm_seq, g_drm_stats[4];   // mirrored out, spawned from the peer, own enemy drops removed, local
static int dr_kind(const DropRec &r)
{
    for (int i = 0; i < 6; i++) {
        uint32_t v = r.stk[i];
        if (v == 0x1DDF4C) return 0;                                   // level item
        if ((v >= 0x1EA8A0 && v < 0x1EACD0) || (v >= 0x1E7090 && v < 0x1E74B0)) return 1;   // enemy drop
        if (v >= 0x1E2920 && v < 0x1E2B90) return 2;                   // weapon swap drop
    }
    return 0;
}
static void coop_drop_step(void)
{
    static int on = -1;
    if (on < 0) { char e[8] = {0}; on = !(GetEnvironmentVariableA("COOP_DROP_SYNC", e, sizeof e) && e[0] == '0'); }
    uint32_t pool = *(volatile uint32_t *)(uintptr_t)0x002D0D64;
    {   // the item model table, once per level (which drops can be mirrored here)
        static uint32_t dumped_for;
        uint32_t lvl = *(volatile uint32_t *)(uintptr_t)0x002D45D0;
        if (pool >= 0x80000000 && pool < 0x84000000 && lvl != dumped_for) {
            dumped_for = lvl;
            uint8_t c[4] = {0, 0, 0, 0}; uint32_t cp = 0, tb = 0, e[24] = {0};
            coop_safe_copy(&cp, pool + 0x5844, 4); if (cp >= 0x80000000 && cp < 0x84000000) coop_safe_copy(c, cp, 4);
            coop_safe_copy(&tb, pool + 0x5800, 4); if (tb >= 0x80000000 && tb < 0x84000000) coop_safe_copy(e, tb, sizeof e);
            char b[400]; int o = 0;
            for (int k = 0; k < 24; k++) o += snprintf(b + o, sizeof b - o, " %d:%08x", k, e[k]);
            logf("coop: drop sync: item models (counts %u %u %u %u):%s", c[0], c[1], c[2], c[3], b);
        }
    }
    for (int i = 0; i < g_dr_new_n; i++) {
        const DropRec &r = g_dr_new[i];
        int kind = dr_kind(r);
        float m[16] = {}; coop_safe_copy(m, r.item + 0x70, 64);
        if (!on || !g_cfg.coop_role || !kind) { g_drm_stats[3]++; continue; }
        if (kind == 1 && g_cfg.coop_role == 2) {          // the joiner's own enemy drop: the host's arrives instead
            g_pu_ecx = pool; g_pu_esi = r.item;
            g_pu_replay = 1; coop_call_pickup(); g_pu_replay = 0;
            g_drm_stats[2]++;
            static int jl; if (jl++ < 40) logf("coop: drop sync: JOINER's own enemy drop %08x (type %u sub %u at %.1f %.1f %.1f) removed - the host's comes", r.item, r.type, r.sub, m[12], m[13], m[14]);
            continue;
        }
        DropMsg d = {}; d.magic = 0x52444C43; d.seq = ++g_drm_seq ^ ((uint32_t)g_cfg.coop_role << 24); d.kind = (uint32_t)kind; d.type = r.type; d.sub = r.sub;
        memcpy(d.mat, m, 64);
        if (g_drm_out_n < 8) { g_drm_out[g_drm_out_n++] = d; g_drm_out_sends = 3; }
        g_drm_stats[0]++;
        static int ol; if (ol++ < 60) logf("coop: drop sync: %s %08x type %u sub %u at %.1f %.1f %.1f -> mirrored to the peer", kind == 1 ? "enemy drop" : "SWAP drop (our old weapon)", r.item, r.type, r.sub, m[12], m[13], m[14]);
    }
    g_dr_new_n = 0;
    if (g_drm_out_n && g_drm_out_sends > 0) {
        for (int i = 0; i < g_drm_out_n; i++) sendto(g_coop_sock, (const char *)&g_drm_out[i], (int)sizeof(DropMsg), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        if (--g_drm_out_sends == 0) g_drm_out_n = 0;
    }
    for (int q = 0; q < g_drm_in_n; q++) {
        const DropMsg &d = g_drm_in[q];
        if (pool < 0x80000000 || pool >= 0x84000000 || d.type > 4) continue;
        {   // play 20261114_171637 (the host crashed, AV 0x1CAA60 reading [0 + 0x48]): the joiner's SWAP drop of a weapon
            // this level has no pickup model for (type 2 sub 0). FUN_001E10D0 takes the item's model from
            // *(pool + 0x5800)[base + sub] (base = the counts *(pool + 0x5844) of the types before: 1 -> c0, 0 -> c0+c1,
            // 3 -> +c2, 4 -> +c3; weapons (2) -> 0) and FUN_001CAA50 reads it unchecked. The swapper's own drop used its
            // held weapon's model. No model here -> the drop is not mirrored.
            uint8_t c[4] = {0, 0, 0, 0}; uint32_t cp = 0, tb = 0, mdl = 0;
            coop_safe_copy(&cp, pool + 0x5844, 4); if (cp >= 0x80000000 && cp < 0x84000000) coop_safe_copy(c, cp, 4);
            int base = d.type == 1 ? c[0] : d.type == 0 ? c[0] + c[1] : d.type == 3 ? c[0] + c[1] + c[2] : d.type == 4 ? c[0] + c[1] + c[2] + c[3] : 0;
            coop_safe_copy(&tb, pool + 0x5800, 4);
            if (tb >= 0x80000000 && tb < 0x84000000) coop_safe_copy(&mdl, tb + (uint32_t)((base + (int)(d.sub & 0xFF)) & 0xFF) * 4, 4);
            if (!mdl) {
                static int nl; if (nl++ < 20) logf("coop: drop sync: the PEER's %s (type %u sub %u) has no item model in this level - not spawned here", d.kind == 1 ? "enemy drop" : "swap drop", d.type, d.sub);
                continue;
            }
        }
        memcpy(g_drs_mat, d.mat, 64);
        memset(g_drs_vel, 0, sizeof g_drs_vel); memset(g_drs_avel, 0, sizeof g_drs_avel);
        g_drs_pool = pool; g_drs_type = d.type; g_drs_sub = d.sub;
        g_dr_replay = 1; coop_call_spawn_item(); g_dr_replay = 0;
        g_drm_stats[1]++;
        static int il; if (il++ < 60) logf("coop: drop sync: the PEER's %s (type %u sub %u at %.1f %.1f %.1f) spawned here", d.kind == 1 ? "enemy drop" : "swap drop", d.type, d.sub, d.mat[12], d.mat[13], d.mat[14]);
    }
    g_drm_in_n = 0;
}
static void coop_drop_recv(const uint8_t *raw, int len)
{
    if (len < (int)sizeof(DropMsg)) return;
    const DropMsg &d = *(const DropMsg *)raw;
    static uint32_t seen[32]; static int sw;
    for (int i = 0; i < 32; i++) if (seen[i] == d.seq) return;
    seen[sw++ & 31] = d.seq;
    if (d.kind == 1 && g_cfg.coop_role != 2) return;     // enemy drops: host -> joiner only
    if (g_drm_in_n < 16) g_drm_in[g_drm_in_n++] = d;
}
static void coop_pickup_recv(const uint8_t *raw, int len)
{
    if (len < (int)sizeof(PickMsg)) return;
    const PickMsg &m = *(const PickMsg *)raw;
    static uint32_t seen[32]; static int sw;
    for (int i = 0; i < 32; i++) if (seen[i] == m.seq) return;
    seen[sw++ & 31] = m.seq;
    if (g_pu_in_n < 16) { g_pu_in_age[g_pu_in_n] = 0; g_pu_in[g_pu_in_n++] = m; }
}
static void coop_pickup_step(void)
{
    if (!g_pu_on || !g_cfg.coop_role) return;
    if (g_pu_out_n && g_pu_out_sends > 0) {
        for (int i = 0; i < g_pu_out_n; i++) sendto(g_coop_sock, (const char *)&g_pu_out[i], (int)sizeof(PickMsg), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        if (--g_pu_out_sends == 0) g_pu_out_n = 0;
    }
    uint32_t pool = *(volatile uint32_t *)(uintptr_t)0x002D0D64;
    PickMsg keep[16]; uint8_t keep_age[16]; int nkeep = 0;
    for (int q = 0; q < g_pu_in_n; q++) {
        const PickMsg &m = g_pu_in[q];
        // pu1: level items match their slot at 0 m; enemy DROPS (type 1 sub 3, dynamic slots) lie where each
        // copy's own physics put them (0.1-2 m apart, some more) -> nearest of the type within 5 m
        uint32_t best = 0; float bd = 25.0f, nearest = 1e30f;
        auto live = [&](uint32_t it) -> bool {   // in use (pool +0x5804 + slot; the collect clears it) and not collected
            uint8_t done = 1, used = 0; uint32_t vt = 0;
            if (it < pool || (it - pool) % 0x160 || (it - pool) / 0x160 >= 64) return false;
            coop_safe_copy(&used, pool + 0x5804 + (it - pool) / 0x160, 1);
            return used && coop_safe_copy(&vt, it, 4) && vt >= 0x10000 && vt < 0x00400000 && coop_safe_copy(&done, it + 0x154, 1) && !done;
        };
        auto score = [&](uint32_t it) {
            int32_t sl; uint32_t ty; uint8_t sb; float pp[3];
            pu_item_info(it, pool, &sl, &ty, &sb, pp);
            if (ty != m.type || sb != m.sub || !live(it)) return;
            float dx = pp[0] - m.pos[0], dy = pp[1] - m.pos[1], dz = pp[2] - m.pos[2], d = dx * dx + dy * dy + dz * dz;
            if (d < nearest) nearest = d;
            if (d < bd) { bd = d; best = it; }
        };
        if (pool >= 0x80000000 && pool < 0x84000000) {
            if (m.slot >= 0 && m.slot < 64) score(pool + (uint32_t)m.slot * 0x160);
            if (!best) for (uint32_t i = 0; i < 64; i++) score(pool + i * 0x160);
        }
        if (!best && ++g_pu_in_age[q] < 60) { keep[nkeep] = m; keep_age[nkeep++] = g_pu_in_age[q]; continue; }   // retry next frame (1 s)
        if (!best) {
            g_pu_stats[2]++;
            static int un; if (un++ < 30) logf("coop: pickup sync: the peer's item (slot %d type %u sub %u at %.1f %.1f %.1f) has no live match within 5 m here (nearest of that type %.1f m)", m.slot, m.type, m.sub, m.pos[0], m.pos[1], m.pos[2], nearest < 1e29f ? sqrtf(nearest) : -1.0f);
            continue;
        }
        g_pu_ecx = pool; g_pu_esi = best;
        g_pu_replay = 1; coop_call_pickup(); g_pu_replay = 0;
        g_pu_stats[1]++;
        static int ap; if (ap++ < 60) logf("coop: pickup sync: the PEER collected item %08x (slot %d type %u sub %u, %.2f m off%s) -> collected here too", best, m.slot, m.type, m.sub, sqrtf(bd), g_pu_in_age[q] ? ", after a wait" : "");
    }
    for (int i = 0; i < nkeep; i++) { g_pu_in[i] = keep[i]; g_pu_in_age[i] = keep_age[i]; }
    g_pu_in_n = nkeep;
}
static void coop_ob_step(void)
{
    if (g_ob_on != 1 || !g_cfg.coop_role) return;
    if (g_cfg.coop_role == 1 && g_ob_hist_w) {   // the last 16 actions, every 10th frame (UDP; the joiner dedupes by seq)
        static uint32_t f;
        if ((f++ % 10) == 0) {
            struct { uint32_t magic, n; ObAct a[16]; } pk;
            pk.magic = 0x424F4C43; pk.n = g_ob_hist_w < 16 ? g_ob_hist_w : 16;
            for (uint32_t i = 0; i < pk.n; i++) pk.a[i] = g_ob_hist[(g_ob_hist_w - pk.n + i) & 15];
            sendto(g_coop_sock, (const char *)&pk, 8 + pk.n * 8, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        }
    }
}
static void coop_ob_recv(const uint8_t *raw, int len)
{
    if (g_ob_on != 1 || g_cfg.coop_role != 2 || len < 8) return;
    uint32_t n = *(const uint32_t *)(raw + 4);
    if (n > 16 || len < (int)(8 + n * 8)) return;
    const uint32_t *a = (const uint32_t *)(raw + 8);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t seq = a[2 * i], node = a[2 * i + 1], vt = 0;
        if (seq <= g_ob_last_in) continue;
        g_ob_last_in = seq;
        if (!coop_safe_copy(&vt, node, 4) || vt != 0x0028EB2C) { logf("coop: objective sync: host node %08x is not an objective node here (vt %08x) - skipped", node, vt); continue; }
        g_obr_fn = *(volatile uint32_t *)(uintptr_t)(vt + 8); g_obr_ecx = node;
        g_ob_replay = 1; coop_thunk_ob_replay(); g_ob_replay = 0;
        g_ob_stats[2]++;
        logf("coop: objective sync: JOINER replayed the host's objective node %08x (seq %u)", node, seq);
    }
}
static void coop_melee_step(void);
static void coop_rt_apply(uint32_t which, uint32_t seq);
static void coop_state_scan(void);
static void coop_mission_fail_step(uint32_t obj);
static void coop_mf_apply(void);
static void coop_restart_reset(void);
// every frame, both roles (after the receive loop)
static void coop_trig_step(uint32_t obj)
{
    coop_anim_step(obj);
    coop_pj_step();
    coop_melee_step();
    coop_ob_step();
    coop_drop_step();     // first: a mirrored drop must exist before the peer's pickup of it is matched
    coop_pickup_step();
    coop_prop_step();
    coop_jp_frame(g_cfg.coop_role == 1 ? g_p10_gdst : 0);
    coop_pool_watch();
    coop_logic_census();
    coop_main_obj_poll(obj);
    coop_state_scan();
    coop_mission_fail_step(obj);
    {   // no-pause: how many world frames ran under the pause menu
        extern volatile uint32_t g_np_runs; static uint32_t last; static uint64_t lt;
        if (g_np_runs != last && GetTickCount64() - lt > 3000) { lt = GetTickCount64(); logf("coop: no-pause: the world ran %u frames under the pause menu so far", g_np_runs); last = g_np_runs; }
    }
    coop_reg_dump(obj);
    coop_spawn_track();
    coop_spawn_force();
    coop_spawner_dump();
    if (g_trig_sync != 1 || !g_cfg.coop_role || obj < 0x80000000 || obj >= 0x84000000) return;
    coop_trig_table();
    if (!g_trig_n) return;
    uint32_t world = *(volatile uint32_t *)(uintptr_t)0x002D45D4;
    if (world < 0x80000000 || world >= 0x84000000) return;
    if (g_cfg.coop_role == 1) {
        g_exit_by_joiner = 1;   // a level end requested inside this block is the JOINER's (cleared below)
        uint32_t part_req0 = *(volatile uint32_t *)(uintptr_t)(world + 0x5AE8);   // a part switch requested in this block = the joiner's
        // the joiner's requests: fire each for both (a one-shot only if it has not fired yet)
        for (int q = 0; q < g_trig_req_in_n; q++) {
            int i = g_trig_req_in[q];
            if (i >= g_trig_n) continue;
            uint32_t o = g_trig_obj[i], vt = 0, ty = 0; uint8_t armed = 0;
            if (!o || !coop_trig_synced(o)) continue;
            coop_safe_copy(&vt, o, 4); coop_safe_copy(&ty, o + 0x120, 4); coop_safe_copy(&armed, o + 0x11C, 1);
            if (ty == 0 && (g_trig_cnt[i] > 0 || !armed)) {
                static int dl;
                if (dl++ < 100) logf("coop: trigger sync: joiner request for one-shot #%d %08x - already fired here", i, o);
                continue;
            }
            g_tc_fn = *(volatile uint32_t *)(uintptr_t)(vt + 4); g_tc_this = o; g_tc_a1 = world + 0x30;
            coop_thunk_trig_fire();                                                   // the hook counts it
            if (ty == 0) *(volatile uint8_t *)(uintptr_t)(o + 0x11C) = 0;
            g_trig_stats[0]++;
            logf("coop: trigger sync: HOST fired trigger #%d %08x (type %u) on the JOINER's request", i, o, ty);
        }
        g_trig_req_in_n = 0;
        // the joiner's feet through the game's own test (class A one-shots that are still armed)
        uint32_t c = g_p10_gdst;
        static float prev[4]; static int prev_ok;
        if (c >= 0x80000000 && c < 0x84000000 && g_coop_host_ms && GetTickCount64() - g_coop_host_ms < 400) {
            __declspec(align(16)) static float cur[4];
            coop_safe_copy(cur, c + 0xA0, 12); cur[3] = 1.0f;
            if (prev_ok) {
                float keep[4];
                memcpy(keep, (const void *)(uintptr_t)(world + 0x1C0), 16);
                memcpy((void *)(uintptr_t)(world + 0x1C0), prev, 16);
                for (int i = 0; i < g_trig_n; i++) {
                    uint32_t o = g_trig_obj[i], vt = 0; uint8_t armed = 0;
                    if (!o || !coop_trig_oneshot(o)) continue;
                    coop_safe_copy(&vt, o, 4);
                    if (vt != 0x0028EF88) continue;                                   // class A (the swept volume test)
                    coop_safe_copy(&armed, o + 0x11C, 1);
                    if (!armed) continue;
                    g_tc_fn = *(volatile uint32_t *)(uintptr_t)(vt + 0x48); g_tc_this = o;
                    g_tc_a1 = (uint32_t)(uintptr_t)cur; g_tc_a2 = world + 0x30; g_tc_ret = 0;
                    coop_thunk_trig_test();
                    if (g_tc_ret) {
                        *(volatile uint8_t *)(uintptr_t)(o + 0x11C) = 0;              // as the game's loop does
                        g_trig_stats[0]++;
                        logf("coop: trigger sync: HOST fired trigger #%d %08x for the JOINER at %.1f %.1f %.1f", i, o, cur[0], cur[1], cur[2]);
                        g_fj_obj = o; g_fj_ms = GetTickCount64(); g_cp_who = 2; g_cp_ms = g_fj_ms;
                    }
                }
                memcpy((void *)(uintptr_t)(world + 0x1C0), keep, 16);
            }
            memcpy(prev, cur, 16); prev_ok = 1;
        } else prev_ok = 0;
        g_exit_by_joiner = 0;
        if (!part_req0 && *(volatile uint32_t *)(uintptr_t)(world + 0x5AE8)) {
            // m1part2: the host's trigger test at the JOINER's position (an area trigger with +0x124 == 1 / 2) asked for the
            // next level part -> the host switched 0.35 s after the joiner and both copies took themselves as the one who
            // got there (nobody moved). Remember it: the host's section that follows is the joiner's checkpoint.
            g_part_by_joiner_ms = GetTickCount64();
            uint32_t c2 = g_p10_gdst;
            if (c2 >= 0x80000000 && c2 < 0x84000000) coop_safe_copy(g_part_by_joiner_at, c2 + 0xA0, 12);
            logf("coop: level part: the switch to the next part was asked for at the JOINER's position (request %u)", *(volatile uint32_t *)(uintptr_t)(world + 0x5AE8));
        }
    } else {
        // perform the host's activations we have not done yet (at most 16 a frame)
        int budget = 16;
        for (int i = 0; i < g_trig_n && i < g_trig_host_n && budget > 0; i++) {
            while ((uint16_t)(g_trig_host_cnt[i] - g_trig_cnt[i]) - 1u < 32767u && budget > 0) {
                uint32_t o = g_trig_obj[i], vt = 0, ty = 0;
                g_trig_cnt[i]++; budget--;
                if (!o || !coop_trig_synced(o)) continue;
                coop_safe_copy(&vt, o, 4); coop_safe_copy(&ty, o + 0x120, 4);
                g_tc_fn = *(volatile uint32_t *)(uintptr_t)(vt + 4); g_tc_this = o; g_tc_a1 = world + 0x30;
                g_trig_replay_this = o;
                coop_thunk_trig_fire();
                g_trig_replay_this = 0;
                if (ty == 0) {   // the checkpoint rule (one-shots only): our own request coming back = we set it off; else the host's player did
                    uint64_t nw = GetTickCount64(); bool ours = false;
                    for (int q = 0; q < 8; q++) if (g_cp_req_id[q] == (uint16_t)i && g_cp_req_ms[q] && nw - g_cp_req_ms[q] < 3000) ours = true;
                    g_cp_who = ours ? 2 : 1; g_cp_ms = nw;
                    g_cp_tpos_ok = coop_safe_copy(g_cp_tpos, o + 0xA0, 12);
                }
                if (ty == 0) *(volatile uint8_t *)(uintptr_t)(o + 0x11C) = 0;         // a one-shot has fired: do not test it here any more
                g_trig_stats[3]++;
                logf("coop: trigger sync: JOINER replayed the host's activation of trigger #%d %08x (count %u)", i, o, g_trig_cnt[i]);
            }
        }
    }
    static uint32_t sl;
    if ((sl++ % 1800) == 0) logf("coop: trigger sync: %d triggers | host-fired-for-joiner %u, counted %u, suppressed %u, replayed %u",
                                 g_trig_n, g_trig_stats[0], g_trig_stats[1], g_trig_stats[2], g_trig_stats[3]);
}
static void coop_route_step(uint32_t ctrl, uint32_t obj)
{
    static int init = 0, npts = 0, arrived = 0;
    static FILE *rec = nullptr;
    static float (*pts)[4] = nullptr;          // x y z yaw
    static float *cum = nullptr;               // distance along the route at each point
    static float at = 75.0f, speed = 6.0f, stop = 0.0f, dist = 0.0f;
    static uint64_t last_ms = 0, rec_ms = 0;
    static float rl[3] = {1e9f, 0, 0};
    if (!init) {
        init = 1;
        char e[512] = {0};
        if (GetEnvironmentVariableA("COOP_ROUTE_REC", e, sizeof e) && e[0]) {
            rec = fopen(e, "a");
            if (rec) { fprintf(rec, "# route recording, role %d\n", g_cfg.coop_role); logf("coop: route: recording to %s", e); }
        }
        char v[32] = {0};
        if (GetEnvironmentVariableA("COOP_ROUTE_AT", v, sizeof v)) at = (float)atof(v);
        if (GetEnvironmentVariableA("COOP_ROUTE_SPEED", v, sizeof v)) speed = (float)atof(v);
        if (GetEnvironmentVariableA("COOP_ROUTE_STOP", v, sizeof v)) stop = (float)atof(v);
        e[0] = 0;
        {   // COOP_ROUTE_ROLE=host|joiner: only that copy rides the route (the other stays where it is) - split-player tests
            char rr[16] = {0};
            if (GetEnvironmentVariableA("COOP_ROUTE_ROLE", rr, sizeof rr) && rr[0]) {
                int want = (rr[0] == 'h' || rr[0] == 'H') ? 1 : 2;
                if (g_cfg.coop_role != want) { SetEnvironmentVariableA("COOP_ROUTE", ""); logf("coop: route: this copy does not ride (COOP_ROUTE_ROLE=%s)", rr); }
            }
        }
        {   // COOP_ROUTE_HOST / COOP_ROUTE_JOINER (absolute paths): each copy rides ITS OWN route (replaying both
            // recordings of an owner session, play_coop.ps1 rec_<time>_host / _joiner)
            char rp[512] = {0};
            if (GetEnvironmentVariableA(g_cfg.coop_role == 1 ? "COOP_ROUTE_HOST" : "COOP_ROUTE_JOINER", rp, sizeof rp) && rp[0]) {
                SetEnvironmentVariableA("COOP_ROUTE", rp);
                logf("coop: route: this copy rides its own route %s", rp);
            }
        }
        if (GetEnvironmentVariableA("COOP_ROUTE", e, sizeof e) && e[0]) {
            FILE *f = fopen(e, "r");
            if (!f) logf("coop: route: cannot open %s", e);
            else {
                pts = (float (*)[4])calloc(8192, sizeof *pts); cum = (float *)calloc(8192, sizeof *cum);
                char line[256];
                while (pts && cum && npts < 8192 && fgets(line, sizeof line, f)) {
                    if (line[0] == '#') continue;
                    float x, y, z, yw = 0.0f;
                    if (sscanf(line, "%f,%f,%f,%f", &x, &y, &z, &yw) < 3) continue;
                    pts[npts][0] = x; pts[npts][1] = y; pts[npts][2] = z; pts[npts][3] = yw;
                    if (npts) {
                        float dx = x - pts[npts - 1][0], dy = y - pts[npts - 1][1], dz = z - pts[npts - 1][2];
                        cum[npts] = cum[npts - 1] + sqrtf(dx * dx + dy * dy + dz * dz);
                    }
                    npts++;
                }
                fclose(f);
                logf("coop: route: %s loaded, %d points, %.0f m; ghost run from %.0f s at %.1f m/s (stop %.1f m early)",
                     e, npts, npts ? cum[npts - 1] : 0.0f, at, speed, stop);
            }
        }
    }
    if (obj < 0x80000000 || obj >= 0x84000000 || ctrl < 0x80000000 || ctrl >= 0x84000000) return;
    volatile float *f = (volatile float *)(uintptr_t)(obj + 0xA0);
    uint64_t now = GetTickCount64();
    if (rec && now - rec_ms >= 250) {
        float dx = f[0] - rl[0], dy = f[1] - rl[1], dz = f[2] - rl[2];
        if (dx * dx + dy * dy + dz * dz > 0.09f) {
            rec_ms = now; rl[0] = f[0]; rl[1] = f[1]; rl[2] = f[2];
            fprintf(rec, "%.2f,%.2f,%.2f,%.1f\n", f[0], f[1], f[2], *(volatile float *)(uintptr_t)(ctrl + 0x18));
            fflush(rec);
        }
    }
    if (npts < 2 || !g_coop_t0 || (float)(now - g_coop_t0) * 0.001f < at) { last_ms = now; return; }
    // the level must be the route's: start only when we stand within 30 m of its first point
    static int started = 0;
    if (!started) {
        float dx = f[0] - pts[0][0], dy = f[1] - pts[0][1], dz = f[2] - pts[0][2];
        if (dx * dx + dy * dy + dz * dz > 900.0f) {
            static int wl;
            if (wl++ % 600 == 0) logf("coop: route: waiting - we are %.0f m from the route start (%.1f %.1f %.1f)", sqrtf(dx * dx + dy * dy + dz * dz), pts[0][0], pts[0][1], pts[0][2]);
            last_ms = now; return;
        }
        started = 1; dist = 0.0f;
        logf("coop: route: GHOST RUN starts at %.1f %.1f %.1f", f[0], f[1], f[2]);
    }
    float dt = last_ms ? (float)(now - last_ms) * 0.001f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_ms = now;
    float total = cum[npts - 1] - stop;
    if (total < 0.0f) total = 0.0f;
    if (!arrived) {
        dist += speed * dt;
        if (dist >= total) { dist = total; arrived = 1; logf("coop: route: ARRIVED at the end of the route (%.0f m) - %.1f %.1f %.1f", total, f[0], f[1], f[2]); }
    }
    float d = dist - (g_cfg.coop_role == 2 ? 2.5f : 0.0f);     // the joiner trails behind the host
    if (d < 0.0f) d = 0.0f;
    int k = 0;
    while (k < npts - 2 && cum[k + 1] < d) k++;
    float seg = cum[k + 1] - cum[k], t = seg > 1e-4f ? (d - cum[k]) / seg : 0.0f;
    if (t > 1.0f) t = 1.0f;
    float want[3], yaw;
    for (int i = 0; i < 3; i++) want[i] = pts[k][i] + t * (pts[k + 1][i] - pts[k][i]);
    float dy = pts[k + 1][3] - pts[k][3];
    while (dy > 180.0f) dy -= 360.0f;
    while (dy < -180.0f) dy += 360.0f;
    yaw = pts[k][3] + t * dy;
    // keep full health (the pilot cannot fight); never while downed logic is being tested
    float hp = *(volatile float *)(uintptr_t)(obj + 0x2F0);
    float full = (obj == g_player_max_obj && g_player_max_hp > 1.0f) ? g_player_max_hp : 1200.0f;
    if (!g_cfg.downed_at && hp > 0.0f && hp < full) *(volatile float *)(uintptr_t)(obj + 0x2F0) = full;
    if (arrived) return;                                        // standing at the end: the script / we control it
    volatile float *snap = (volatile float *)(uintptr_t)(obj + 0x190);
    float mx = want[0] - f[0], my = want[1] - f[1], mz = want[2] - f[2];
    f[0] += mx; f[1] += my; f[2] += mz;
    snap[0] += mx; snap[1] += my; snap[2] += mz;
    *(volatile float *)(uintptr_t)(ctrl + 0x18) = yaw;
    static uint32_t pl;
    if ((pl++ % 180) == 0) logf("coop: route: %.0f / %.0f m, at %.1f %.1f %.1f yaw %.0f", dist, total, want[0], want[1], want[2], yaw);
}

// called from read_pad (input/xinput.cpp) every poll with the FULLY merged input (pad,
// scripted presses, keyboard/mouse bindings). While the peer is downed and we are up,
// X is the revive key: its press edge sends "CLRS", and X is eaten so it does not also
// fire its normal game action.
void coop_notify_x_press(unsigned char analogButtons[8])
{
    static unsigned char prev = 0;
    unsigned char now = analogButtons[2];   // X
    bool edge = now > 127 && prev <= 127;
    prev = now;
    // RESPAWN RULE (owner): a downed player now comes back when a MAIN objective is completed (coop_main_obj_poll),
    // not by the survivor's X. COOP_X_REVIVE=1 brings the old X revive back (tests).
    static int xr = -1;
    if (xr < 0) { char e[8] = {0}; xr = GetEnvironmentVariableA("COOP_X_REVIVE", e, sizeof e) && e[0] == '1'; }
    bool reviver = xr && !g_coop_downed && g_coop_peer_downed && g_coop_sock != INVALID_SOCKET;
    if (reviver) analogButtons[2] = 0;      // X belongs to the revive while the peer is down
    if (!edge || !reviver) return;
    uint32_t ctrl = g_coop_last_ctrl;
    if (ctrl < 0x80000000 || ctrl >= 0x84000000) return;
    uint32_t po = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
    if (po < 0x80000000 || po >= 0x84000000) return;
    struct { uint32_t magic, seq; float feet[3]; float yaw; } rp;
    rp.magic = 0x53524C43;   // "CLRS"
    rp.seq = ++g_coop_calls;
    const volatile float *f = (const volatile float *)(uintptr_t)(po + 0xA0);
    rp.feet[0] = f[0]; rp.feet[1] = f[1]; rp.feet[2] = f[2];
    rp.yaw = cpu_of(ctrl + 0x18);
    sendto(g_coop_sock, (const char *)&rp, (int)sizeof rp, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
    logf("coop: respawn request sent for the downed peer (our feet %.1f %.1f %.1f)", rp.feet[0], rp.feet[1], rp.feet[2]);
}

// SELF MODEL drive (see g_coop_self_body): every frame. Its own private link (never the class object a
// natural pawn shares: crouch / look writes would steer that enemy too), immortal, our team, no fall
// physics. Downed -> placed at the spot we went down, facing our view then, crouched with the head down
// (the same anim inputs as the partner clone's downed pose); otherwise parked 100 m below the level.
static void coop_self_model_step(uint32_t ctrl)
{
    {   // COOP_SELF_RESPAWN_AT=<sec> (test): a downed player respawns at the survivor's feet once, at that time
        static int ra = -2, done;
        if (ra == -2) { char e[16] = {0}; ra = GetEnvironmentVariableA("COOP_SELF_RESPAWN_AT", e, sizeof e) ? atoi(e) : -1; }
        if (ra > 0 && !done && g_coop_downed && g_coop_t0 && GetTickCount64() - g_coop_t0 >= (uint64_t)ra * 1000) {
            done = 1;
            float feet[3] = {g_coop_host_pawnmat[12], g_coop_host_pawnmat[13], g_coop_host_pawnmat[14]};
            logf("coop: self model test: forced respawn at %d s", ra);
            coop_respawn_at(feet, g_coop_host_yaw);
        }
    }
    uint32_t b = g_coop_self_body;
    if (!b) return;
    uint32_t vt = 0;
    if (!coop_safe_copy(&vt, b, 4) || vt != 0x0028C1E8) {
        logf("coop: self model: body %08x is no pawn any more (vt %08x) - dropped", b, vt);
        g_coop_self_body = 0; return;
    }
    if (!g_self_link) {
        uint32_t src = *(volatile uint32_t *)(uintptr_t)(b + 0x324);
        if (src < 0x80000000 || src >= 0x84000000) return;
        g_self_link_src = src;
        g_self_link = coop_make_private_link(b, src);
        logf("coop: self model: %08x private link %08x (class %08x)", b, g_self_link, src);
        if (!g_self_link) { g_coop_self_body = 0; return; }
    }
    uint32_t o = g_self_link;
    if (*(volatile uint32_t *)(uintptr_t)(b + 0x324) != o) *(volatile uint32_t *)(uintptr_t)(b + 0x324) = o;
    link_sync_voice(o, g_self_link_src);
    *(volatile float *)(uintptr_t)(b + 0x2F0) = 3.4028235e38f;
    *(volatile uint8_t *)(uintptr_t)(b + 0x3A7) = 1;   // no fall physics (sm12: the crouch shows with it on)
    uint32_t po = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
    if (po >= 0x80000000 && po < 0x84000000)
        *(volatile uint32_t *)(uintptr_t)(b + 0x39C) = *(volatile uint32_t *)(uintptr_t)(po + 0x39C);   // our team
    *(volatile float *)(uintptr_t)(o + 0x20) = 0.0f;   // no walk: desired / current speed 0
    *(volatile float *)(uintptr_t)(o + 0x84) = 0.0f;
    // keep the partner clone's diagnostics globals as they were (coop_place_body writes them)
    float keep[3] = {g_coop_clone_pos[0], g_coop_clone_pos[1], g_coop_clone_pos[2]}; uint32_t kd = g_last_donor;
    bool down = g_coop_downed && g_downed_pos_ok;
    // sm5: coop_place_body copies a DONOR pawn's animated root (+0x110 block) every frame - that root is a
    // standing pelvis (1.2 m above the feet), so our crouch never showed. The self model stands still: the
    // donor root is taken ONCE (the frame it moves), after that the game's own animation owns the root.
    static int placed = -1;   // -1 nowhere, 0 parked, 1 at the downed spot
    static uint32_t placed_body; if (placed_body != b) { placed_body = b; placed = -1; }
    if (down) {
        float yr = g_downed_yaw * 0.0174532925f;
        float F[3] = {sinf(yr), 0.0f, cosf(yr)};
        if (placed != 1) coop_place_body(b, g_downed_pos[0], g_downed_pos[1], g_downed_pos[2], F);
        placed = 1;
        *(volatile float *)(uintptr_t)(o + 0x10) = g_downed_yaw;   // body facing
        *(volatile float *)(uintptr_t)(o + 0x18) = g_downed_yaw;   // look yaw
        *(volatile float *)(uintptr_t)(o + 0x14) = g_downed_yaw;   // aim yaw (anim "aim_bearing" = +0x10 - +0x14 = 0)
        *(volatile float *)(uintptr_t)(o + 0x1C) = 70.0f;          // head down (anim "aim_pitch")
        *(volatile uint8_t *)(uintptr_t)(o + 0x40) = 1;            // crouching (the AI's copy)
        *(volatile uint8_t *)(uintptr_t)(b + 0x3A1) = 1;           // crouching (what the anim reads)
    } else {
        if (placed != 0) coop_place_body(b, g_self_park[0], g_self_park[1], g_self_park[2], nullptr);
        placed = 0;
        *(volatile uint8_t *)(uintptr_t)(o + 0x40) = 0;
        *(volatile uint8_t *)(uintptr_t)(b + 0x3A1) = 0;
        *(volatile float *)(uintptr_t)(o + 0x1C) = 0.0f;
    }
    if (down) {   // the crouch clip. cr1 (host enemy anim calls with the crouch byte): 138bc553 / 7bfe8a91 are
        // played only on CROUCHED enemies; sm8: 138bc553 hands over to 7bfe8a91 by itself (the move into the
        // crouch, then the crouch loop). Nobody else asks this AI-less body for clips: play the loop when the
        // current clip is neither. (The pawn tick itself copies link +0x40 -> body +0x3A1 and blends the
        // stance, sm11: pelvis 0.83 -> 0.47 m, head 1.31 -> 0.96 m.) Env COOP_SELF_ANIM=<hex>.
        static uint32_t want = 0;
        if (!want) { char e[16] = {0}; want = GetEnvironmentVariableA("COOP_SELF_ANIM", e, sizeof e) ? strtoul(e, nullptr, 16) : 0x7BFE8A91u; }
        uint32_t blk = coop_anim_blk(b), curh = 0;
        if (blk && coop_safe_copy(&curh, blk + 0x10, 4) && curh != want && curh != 0x138BC553u) {
            g_an_replay = 1; ((an_now_fn)(uintptr_t)xbe_va(0x00091E70))(blk, want); g_an_replay = 0;
            static int al;
            if (al++ < 20) logf("coop: self model: clip %08x -> %08x (crouch)", curh, want);
        }
    }
    {   // hold the feet every frame (no fall physics, but keep it exact) - feet only, the root stays the game's
        const float *P = down ? g_downed_pos : g_self_park;
        volatile float *f = (volatile float *)(uintptr_t)(b + 0xA0), *sn = (volatile float *)(uintptr_t)(b + 0x190);
        for (int k = 0; k < 3; k++) { float d = P[k] - f[k]; f[k] += d; sn[k] += d; }
    }
    {   // the root block is taken once and the game does not rewrite it for this body: keep it finite (a NaN
        // lane crashed the host's collision test, play 20261114_083833)
        volatile float *rt = (volatile float *)(uintptr_t)(b + 0x110);
        int bad = 0;
        for (int i = 0; i < 16; i++) if (!std::isfinite((float)rt[i])) { rt[i] = 0.0f; bad++; }
        if (bad) { static int bl; if (bl++ < 10) logf("coop: self model: %d non-finite lane(s) in the root block cleared", bad); }
    }
    g_coop_clone_pos[0] = keep[0]; g_coop_clone_pos[1] = keep[1]; g_coop_clone_pos[2] = keep[2]; g_last_donor = kd;
    {   // state readback every ~2 s while shown (did the game keep our writes?)
        static uint32_t fr;
        if (down && (fr++ % 120) == 0 && fr < 1200) {
            {   uint32_t bk = coop_anim_blk(b), a50 = 0, a54 = 0; float p50 = 0, p54 = 0, h50 = 0;
                if (bk) { coop_safe_copy(&a50, bk + 0x50, 4); coop_safe_copy(&a54, bk + 0x54, 4); }
                if (a50) { coop_safe_copy(&p50, a50 + 64 + 52, 4); coop_safe_copy(&h50, a50 + 15 * 64 + 52, 4); }
                if (a54) coop_safe_copy(&p54, a54 + 64 + 52, 4);
                logf("coop: self model: bones %08x/%08x pelvis y %.3f / %.3f head y %.3f | 3A2 %u 2E0 %.2f", a50, a54, p50, p54, h50,
                     *(volatile uint8_t *)(uintptr_t)(b + 0x3A2), *(volatile float *)(uintptr_t)(b + 0x2E0)); }
            logf("coop: self model: state %08x clip %08x t %.2f | feet %.2f %.2f %.2f root %.2f %.2f %.2f | 3A1 %u link+40 %u +1C %.0f | life %u hp %g vis %04x",
                 b, coop_anim_blk(b) ? *(volatile uint32_t *)(uintptr_t)(coop_anim_blk(b) + 0x10) : 0,
                 coop_anim_blk(b) ? *(volatile float *)(uintptr_t)(coop_anim_blk(b) + 0x14) : 0.0f, *(volatile float *)(uintptr_t)(b + 0xA0), *(volatile float *)(uintptr_t)(b + 0xA4), *(volatile float *)(uintptr_t)(b + 0xA8),
                 *(volatile float *)(uintptr_t)(b + 0x140), *(volatile float *)(uintptr_t)(b + 0x144), *(volatile float *)(uintptr_t)(b + 0x148),
                 *(volatile uint8_t *)(uintptr_t)(b + 0x3A1), *(volatile uint8_t *)(uintptr_t)(o + 0x40), *(volatile float *)(uintptr_t)(o + 0x1C),
                 *(volatile uint32_t *)(uintptr_t)(b + 0x384), *(volatile float *)(uintptr_t)(b + 0x2F0), *(volatile uint16_t *)(uintptr_t)(b + 0x13C));
        }
    }
    static int was = -1;
    if ((int)down != was) {
        was = (int)down;
        if (down) logf("coop: self model: SHOWN - %08x crouched at %.1f %.1f %.1f facing %.1f (where we went down)",
                       b, g_downed_pos[0], g_downed_pos[1], g_downed_pos[2], g_downed_yaw);
        else logf("coop: self model: parked - %08x at %.1f %.1f %.1f", b, g_self_park[0], g_self_park[1], g_self_park[2]);
    }
}

// play 20261114_100655 ("the joiner sees the host clone's gun flash and fire sound while the host does not
// fire"): 34 clone pellets with NO remote ray (own aim) at 79.3-86.1 s - before the clone drive's first frame
// (86 s; it waits for fresh peer packets). Until then the clone ran on the CLASS object it shares with natural
// pawn #4 (no private link yet) = that soldier's AI aimed and fired it. Now the private link is made on the
// first frame after the clone exists, with the trigger (+0x41) held at 0 until the drive takes over.
static void coop_clone_disarm_early(void)
{
    uint32_t b = g_p10_gdst;
    if (!g_p10_done || b < 0x80100000 || b >= 0x84000000) return;
    uint32_t vt = 0;
    if (!coop_safe_copy(&vt, b, 4) || vt != 0x0028C1E8) return;
    if (!g_aim_link) {
        uint32_t o = *(volatile uint32_t *)(uintptr_t)(b + 0x324);
        if (o < 0x80000000 || o >= 0x84000000) return;
        g_aim_shared_checked = 1; g_aim_link_src = o;
        g_aim_link = coop_make_private_link(b, o);
        logf("coop: clone %08x disarmed early: private link %08x (class %08x) before the drive - its donor AI no longer fires it", b, g_aim_link, o);
        if (!g_aim_link) return;
    }
    if (*(volatile uint32_t *)(uintptr_t)(b + 0x324) != g_aim_link) *(volatile uint32_t *)(uintptr_t)(b + 0x324) = g_aim_link;
    *(volatile uint8_t *)(uintptr_t)(g_aim_link + 0x41) = 0;   // the drive's fire block owns it once running (it writes 0 too)
}
// ---- Stage 1 bridge body + the marker's receive tick. ----
static void coop_pause_gate();
extern "C" void coop_on_player_update(uint32_t ctrl)
{
    coop_pause_gate();   // co-op: our pause menu holds our player's update
    uint32_t n = ++g_coop_calls;
    if (ctrl < 0x10000 || ctrl >= 0x84000000) return;
    g_coop_last_ctrl = ctrl;
    coop_update_gates();   // HUD / viewmodel gates (downed, or --hide-test stages)
    coop_downed_lifestate();   // a downed player is "not alive" to the AI (enemies drop it)
    {   // phase 3 diagnostics: the local player's health changes
        uint32_t po = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
        coop_track_max_hp(po);
        static float last_hp = -1.0f;
        if (po >= 0x80000000 && po < 0x84000000) {
            float hp = *(volatile float *)(uintptr_t)(po + 0x2F0);
            if (hp != last_hp) {
                static int hl;
                if (hl++ < 60 && last_hp >= 0.0f) logf("coop: PLAYER %08x health %.1f -> %.1f", po, last_hp, hp);
                last_hp = hp;
            }
        }
    }

    // Route B: consume the guest-side socket's packets each frame (non-blocking).
    // Packet v3 (magic 0x434C4151, 96 bytes): + the sender's PAWN body block
    // (+0x70..+0xAC = rotation cols + translation + the 4th float) so the receiving
    // side's puppet stands at the remote player's ACTUAL maintained pawn spot with
    // the remote's body orientation - no invented floor offsets.
    if (g_coop_listens != INVALID_SOCKET) {
        struct { uint32_t magic, seq, ctrl, obj; float yaw, pitch, pos[3], pawnmat[15]; uint32_t downed; double tms; } pkt;   // tms: the sender's clock (ms, QPC) - interpolation
        static uint32_t pkts;
        for (;;) {
            sockaddr_in from{};
            int fl = sizeof from;
            static union { uint8_t raw[65536]; uint32_t magic; } buf;   // CLAI packets reach tens of KB
            int r = recvfrom(g_coop_listens, (char *)buf.raw, sizeof buf.raw, 0, (sockaddr *)&from, &fl);
            if (r < 4) break;
            if (buf.magic == 0x54484C43 && r >= 12) {             // "CLHT" host-decided hits on our player (phase 3)
                uint32_t n = *(const uint32_t *)(buf.raw + 8);
                if (n <= 16 && r >= (int)(12 + n * sizeof(HitMsg)))
                    coop_hit_recv((const HitMsg *)(buf.raw + 12), (int)n);
                continue;
            }
            if (buf.magic == 0x49414C43 && r >= 12) {             // "CLAI" host enemy AI-state mirror
                coop_ai_recv(buf.raw, r);
                continue;
            }
            if (buf.magic == 0x44584C43 && r >= 12) {             // "CLXD" host explosion / melee hits on our player
                uint32_t n = *(const uint32_t *)(buf.raw + 8);
                if (n <= 4 && r >= (int)(12 + n * sizeof(XdMsg)))
                    for (uint32_t i = 0; i < n && g_xd_in_n < 8; i++) {
                        memcpy(&g_xd_in[g_xd_in_n++], buf.raw + 12 + i * sizeof(XdMsg), sizeof(XdMsg));
                        g_xd_stats[1]++;
                    }
                continue;
            }
            if (buf.magic == 0x51544C43 && r >= 12) {             // "CLTQ" the joiner's trigger requests (each sent 3 times)
                uint32_t qn = *(const uint32_t *)(buf.raw + 4), cnt = *(const uint32_t *)(buf.raw + 8);
                uint32_t qsec = cnt >> 16; cnt &= 0xFFFF;   // the joiner's level-part section: its indices are only ours when equal
                static uint32_t last_seq = 0xFFFFFFFF;
                if (g_cfg.coop_role == 1 && qn != last_seq && qsec == (g_cp_sec & 0xFFFF) && cnt <= 256 && r >= (int)(12 + cnt * 2)) {
                    last_seq = qn;
                    for (uint32_t i = 0; i < cnt && g_trig_req_in_n < 512; i++) g_trig_req_in[g_trig_req_in_n++] = ((const uint16_t *)(buf.raw + 12))[i];
                }
                continue;
            }
            if (buf.magic == 0x4A504C43 && r >= 12) { coop_pj_recv(buf.raw, r); continue; }   // "CLPJ" the peer's rockets
            if (buf.magic == 0x55504C43) { coop_pickup_recv(buf.raw, r); continue; }
            if (buf.magic == 0x52444C43) { coop_drop_recv(buf.raw, r); continue; }   // "CLDR" a mirrored drop   // "CLPU" the peer collected an item
            if (buf.magic == 0x424F4C43 && r >= 8) { coop_ob_recv(buf.raw, r); continue; }   // "CLOB" the host's objective node actions
            if (buf.magic == 0x54524C43 && r >= 12) { coop_rt_apply(*(const uint32_t *)(buf.raw + 4), *(const uint32_t *)(buf.raw + 8)); continue; }   // "CLRT" the host restarted
            if (buf.magic == 0x464D4C43 && r >= 4) { coop_mf_apply(); continue; }   // "CLMF" mission failed (both downed)
            if (buf.magic == 0x524C4C43 && r >= 8) { coop_load_recv(*(const uint32_t *)(buf.raw + 4)); continue; }
            if (buf.magic == 0x54514C43) { coop_quit_recv(); continue; }   // "CLQT" the partner left the session
            if (buf.magic == 0x50434C43 && r >= 20) { coop_cp_recv(buf.raw, r); continue; }   // "CLCP" the partner's level-part section
            if (buf.magic == 0x4B434C43 && r >= 20) { coop_real_cp_recv(buf.raw, r); continue; }   // "CLCK" the partner reached a real checkpoint
            if (buf.magic == 0x4D484C43 && r >= 8) { coop_hm_recv(buf.raw, r); continue; }          // "CLHM" the host saw our hit / kill
            if ((buf.magic == 0x54584C43 || buf.magic == 0x45584C43) && r >= 8) { coop_exit_recv(buf.raw, r); continue; }   // "CLXT" / "CLXE" the level exit (timer / end)   // "CLLR" the partner finished a level load
            if (buf.magic == 0x54494C43) { g_coop_peer_intro = 1; g_coop_peer_intro_ms = GetTickCount64(); continue; }   // "CLIT" the partner skipped the intro text
            if (buf.magic == 0x4B534C43 && r >= 4) { if (!g_coop_peer_skip) logf("coop: skip: the partner wants to skip the cinematic"); g_coop_peer_skip = 1; g_coop_peer_skip_ms = GetTickCount64(); continue; }   // "CLSK"
            if (buf.magic == 0x50524C43 && r >= 12) { coop_prop_recv(buf.raw, r); continue; }  // "CLRP" the host's prop damage
            if (buf.magic == 0x4C4D4C43 && r >= 12) {             // "CLML" the joiner's melee hits
                uint32_t mn = *(const uint32_t *)(buf.raw + 8);
                if (g_cfg.coop_role == 1 && mn <= 4 && r >= (int)(12 + mn * sizeof(MeleeMsg)))
                    for (uint32_t i = 0; i < mn && g_ml_in_n < 8; i++) g_ml_in[g_ml_in_n++] = ((const MeleeMsg *)(buf.raw + 12))[i];
                continue;
            }
            if (buf.magic == 0x444B4C43 && r >= (int)(8 + 16 * sizeof(KillDir))) {   // "CLKD" the host's kill directions
                if (g_cfg.coop_role == 2) memcpy(g_killdir, buf.raw + 8, sizeof g_killdir);
                continue;
            }
            if (buf.magic == 0x4E414C43 && r >= 12) {             // "CLAN" the host's enemy animation calls
                uint32_t an = *(const uint32_t *)(buf.raw + 8);
                if (g_cfg.coop_role == 2 && g_an_on == 1 && an <= 128 && r >= (int)(12 + an * sizeof(AnimEv))) {
                    const AnimEv *ev = (const AnimEv *)(buf.raw + 12);
                    for (uint32_t i = 0; i < an; i++) {
                        if (ev[i].seq <= g_an_last_in && g_an_last_in - ev[i].seq < 100000) continue;   // already played
                        g_an_last_in = ev[i].seq;
                        coop_anim_replay(ev[i].body, ev[i].hash, ev[i].which, ev[i].a3, ev[i].a4);
                    }
                }
                continue;
            }
            if (buf.magic == 0x53414C43 && r >= 8) {              // "CLAS" the host's enemy animation snapshot
                uint32_t an = *(const uint32_t *)(buf.raw + 4);
                if (g_cfg.coop_role == 2 && g_an_on == 1 && an <= 16 && r >= (int)(8 + an * 12)) {
                    g_an_stats[5]++;
                    for (uint32_t i = 0; i < an; i++) {
                        const uint32_t *q = (const uint32_t *)(buf.raw + 8 + i * 12);
                        uint32_t b = coop_host_to_local(q[0]);
                        if (!b || !coop_anim_puppet_live(b)) continue;
                        uint32_t blk = coop_anim_blk(b), cur = 0;
                        if (!blk || !coop_safe_copy(&cur, blk + 0x10, 4) || cur == q[1] || q[1] == 0 || q[1] == 0xFFFFFFFF) continue;
                        g_an_replay = 1;
                        ((an_now_fn)(uintptr_t)xbe_va(0x00091E70))(blk, q[1]);
                        g_an_replay = 0;
                        *(volatile float *)(uintptr_t)(blk + 0x14) = *(const float *)&q[2];
                        g_an_stats[3]++;
                        static int fl;
                        if (fl++ < 30) logf("coop: anim sync: puppet %08x showed %08x, host %08x -> fixed (t %.1f)", b, cur, q[1], *(const float *)&q[2]);
                    }
                }
                continue;
            }
            if (buf.magic == 0x50534C43 && r >= 12) {             // "CLSP" the host's spawn counts per spawner
                uint32_t sn = *(const uint32_t *)(buf.raw + 8), ssec = sn >> 16; sn &= 0xFFFF;
                if (g_cfg.coop_role == 2 && ssec == (g_cp_sec & 0xFFFF) && sn <= 256 && r >= (int)(12 + sn)) { memcpy(g_sp_host_cnt, buf.raw + 12, sn); g_sp_host_n = (int)sn; }
                continue;
            }
            if (buf.magic == 0x47544C43 && r >= 12) {             // "CLTG" the host's trigger activation counts
                uint32_t tn = *(const uint32_t *)(buf.raw + 8), tsec = tn >> 16; tn &= 0xFFFF;
                if (g_cfg.coop_role == 2 && tsec == (g_cp_sec & 0xFFFF) && tn <= 512 && r >= (int)(12 + tn * 2)) { memcpy(g_trig_host_cnt, buf.raw + 12, tn * 2); g_trig_host_n = (int)tn; }
                continue;
            }
            if (buf.magic == 0x58454C43 && r >= 12) {             // "CLEX" the peer's explosions
                uint32_t n = *(const uint32_t *)(buf.raw + 8);
                if (n <= 4 && r >= (int)(12 + n * sizeof(ExMsg)))
                    for (uint32_t i = 0; i < n && g_ex_in_n < 8; i++) {
                        memcpy(&g_ex_in[g_ex_in_n++], buf.raw + 12 + i * sizeof(ExMsg), sizeof(ExMsg));
                        g_ex_stats[1]++;
                    }
                continue;
            }
            if (buf.magic == 0x52454C43 && r >= 12) {             // "CLER" host enemy bullet rays (phase 3)
                uint32_t n = *(const uint32_t *)(buf.raw + 8);
                if (n <= 32 && r >= (int)(12 + n * sizeof(EnemyRay))) {
                    const EnemyRay *e = (const EnemyRay *)(buf.raw + 12);
                    uint64_t now = GetTickCount64();
                    for (uint32_t i = 0; i < n; i++) {
                        if (g_enray_w - g_enray_r >= 128) g_enray_r++;
                        g_enray_in[g_enray_w & 127] = e[i]; g_enray_ms[g_enray_w & 127] = now;
                        g_enray_in[g_enray_w & 127].body = coop_host_to_local(e[i].body);   // enemy identity: the host's shooter -> ours (0 = not here: the ray is dropped)
                        g_enray_w++;
                        g_enray_stats[1]++;
                    }
                }
                continue;
            }
            if (buf.magic == 0x4E454C43 && r >= 12) {             // "CLEN" host enemy state (enemy sync)
                uint32_t n = *(const uint32_t *)(buf.raw + 8);
                if (n <= 16 && r >= (int)(12 + n * sizeof(EnemyRec))) {
                    if (r >= (int)(12 + n * sizeof(EnemyRec) + sizeof g_dead_ids))
                        memcpy(g_dead_ids, buf.raw + 12 + n * sizeof(EnemyRec), sizeof g_dead_ids);
                    coop_enemy_recv((const EnemyRec *)(buf.raw + 12), (int)n);
                }
                continue;
            }
            if (buf.magic == 0x48534C43 && r >= 12) {             // "CLSH" shot rays (hit sync)
                uint32_t n = *(const uint32_t *)(buf.raw + 8);
                if (n <= 16 && r >= (int)(12 + n * sizeof(ShotRay)))
                    coop_shot_recv((const ShotRay *)(buf.raw + 12), (int)n);
                continue;
            }
            if (buf.magic == 0x53524C43 && r >= 24) {             // "CLRS" the survivor pressed X: respawn us at their feet
                const float *rf = (const float *)(buf.raw + 8);
                float ryaw = *(const float *)(buf.raw + 20);
                g_coop_peer_downed = 0;
                if (g_coop_downed) coop_respawn_at(rf, ryaw);
                continue;
            }
            if (r < (int)sizeof pkt) continue;
            memcpy(&pkt, buf.raw, sizeof pkt);
            if (pkt.magic != 0x434C4152) continue;   // v4 (feet + fwd), see the sender
            memcpy((void *)g_coop_host_pos, pkt.pos, 12);
            memcpy((void *)g_coop_host_pawnmat, pkt.pawnmat, 60);
            if (pkt.pawnmat[4] > 0.5f) g_coop_trig_latch = 1;   // a tap that lives in ONE packet must not be lost when two packets arrive in a frame
            g_coop_host_yaw = pkt.yaw;
            g_coop_host_pitch = pkt.pitch;
            g_coop_host_pawn_ok = 1;
            if (pkt.pawnmat[3] == 1.0f) coop_snap_push(pkt.tms, &pkt.pawnmat[12], pkt.yaw, pkt.pitch);
            g_coop_host_seq = ++pkts;
            g_coop_host_ms = GetTickCount64();
            g_coop_peer_downed = pkt.downed;   // the peer's downed flag (we can respawn them with X)
            if (g_cfg.coop_log && (g_coop_host_seq <= 3 || g_coop_host_seq % 180 == 0))
                logf("coop: recv #%u host pos %.2f %.2f %.2f pawn %.2f %.2f %.2f (seq track ok)",
                     g_coop_host_seq, pkt.pos[0], pkt.pos[1], pkt.pos[2],
                     pkt.pawnmat[12], pkt.pawnmat[13], pkt.pawnmat[14]);
        }
    }

    // DOWNED SPECTATOR (day 7 rework, owner: "a spectator cam on the remaining player"):
    // a CHASE CAM - our pawn (= the camera; its HUD + viewmodel are gated off while
    // downed, see coop_update_gates) floats 2.6 m behind and 0.6 m above the survivor,
    // facing their way and pitched down at their chest, so their model fills the
    // centre of the view. Live input is muted. Our stream keeps the spot where we went
    // DOWN (g_downed_pos), so the survivor sees our body there, not under the camera.
    if (!g_coop_downed) g_downed_pos_ok = 0;
    else if (!g_downed_pos_ok) {
        uint32_t mo0 = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
        if (mo0 >= 0x80000000 && mo0 < 0x84000000) {
            for (int k = 0; k < 3; k++) g_downed_pos[k] = *(volatile float *)(uintptr_t)(mo0 + 0xA0 + 4 * k);
            g_downed_yaw = *(volatile float *)(uintptr_t)(ctrl + 0x18);
            g_downed_pos_ok = 1;
        }
    }
    if (g_coop_downed && g_coop_host_pawn_ok && g_coop_host_ms != 0 &&
        GetTickCount64() - g_coop_host_ms <= 400) {
        uint32_t mo = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
        if (mo >= 0x80000000 && mo < 0x84000000) {
            float hf[3] = {g_coop_host_pawnmat[0], g_coop_host_pawnmat[1], g_coop_host_pawnmat[2]}; // survivor view fwd
            float hl = sqrtf(hf[0] * hf[0] + hf[2] * hf[2]);
            if (hl > 1e-3f) {
                float Fh[3] = {hf[0] / hl, 0.0f, hf[2] / hl};
                const float BACK = 2.6f, UP = 0.6f;
                float tx = g_coop_host_pawnmat[12] - BACK * Fh[0];
                float ty = g_coop_host_pawnmat[13] + UP;
                float tz = g_coop_host_pawnmat[14] - BACK * Fh[2];
                // our eye height above our feet (measured, before the move)
                float eyeh = *(volatile float *)(uintptr_t)(mo + 0x104) - *(volatile float *)(uintptr_t)(mo + 0xA4);
                if (!(eyeh > 0.5f && eyeh < 2.5f)) eyeh = 1.5f;
                // pitch (+ = down) from our eye to the survivor's chest (feet + 1.2)
                float drop = (ty + eyeh) - (g_coop_host_pawnmat[13] + 1.2f);
                float cam_pitch = 57.2957795f * atan2f(drop, BACK);
                volatile float *f = (volatile float *)(uintptr_t)(mo + 0xA0);
                volatile float *snap = (volatile float *)(uintptr_t)(mo + 0x190);
                float dx = tx - f[0], dy = ty - f[1], dz = tz - f[2];
                f[0] += dx; f[1] += dy; f[2] += dz;
                snap[0] += dx; snap[1] += dy; snap[2] += dz;
                *(volatile float *)(uintptr_t)(ctrl + 0x18) = g_coop_host_yaw;    // face the survivor's way
                *(volatile float *)(uintptr_t)(ctrl + 0x1C) = cam_pitch;          // looking down at them
                static int dl;
                if (dl++ < 3) logf("coop: downed spectator: chase cam at %.1f %.1f %.1f (2.6 m behind, 0.6 m up), yaw %.1f pitch %.1f; our body stays at %.1f %.1f %.1f",
                                   tx, ty, tz, g_coop_host_yaw, cam_pitch, g_downed_pos[0], g_downed_pos[1], g_downed_pos[2]);
            }
        }
    }

    // Pilot 10: the per-frame attempt runs before the tracker (gated internally:
    // it clones once on the first live enemy group slot, then returns instantly).
    coop_pilot10_try();
    coop_clone_disarm_early();
    coop_self_model_step(ctrl);

    // Pilot 11: the LAN drive. The guest's clone body = the remote player's PAWN
    // body block streamed in packet v3: copy the pawn's +0x70 rotation columns
    // and write its translation (+12..14 = the pawn's grounded feet position the
    // game itself maintains - always on the same plane the pawn stands on, and
    // facing where the remote body faces). No synthetic eye offsets anymore.
    // Milestone 2 (packet v4): the adopted factory body stands at the REMOTE
    // player's feet. (v3 memcpy'd a non-body block into +0x70 - removed.)
    if (g_p10_done && g_p10_gdst >= 0x80100000 && g_p10_gdst < 0x84000000 &&
        g_coop_listens != INVALID_SOCKET && g_coop_host_ms != 0 &&
        GetTickCount64() - g_coop_host_ms <= 400 && g_coop_host_pawn_ok &&
        g_coop_host_pawnmat[3] == 1.0f) {
        float rx = g_coop_host_pawnmat[12], ry = g_coop_host_pawnmat[13], rz = g_coop_host_pawnmat[14];
        float ryaw = g_coop_host_yaw, rpitch = g_coop_host_pitch;
        float ist[5], ivel[2];
        bool interp = coop_interp_remote(ist, ivel);   // smooth: the partner 80 ms in the past, between two packets
        if (interp) { rx = ist[0]; ry = ist[1]; rz = ist[2]; ryaw = ist[3]; rpitch = ist[4]; }
        float rf[3] = {g_coop_host_pawnmat[0], g_coop_host_pawnmat[1], g_coop_host_pawnmat[2]};
        (void)rf;   // body facing comes from movement now (coop_loco_body), not the view
        // m2c: both copies CRASHED on the first drive frame (AV 0x69955, read off the
        // stack top in the engine world/sector code) - the clone was placed EXACTLY on
        // the local player's feet (both spawn on one point). Minimum separation 1 m:
        // push out along local->remote (or the local player's left when coincident).
        // Locomotion still uses the true remote feet (rx, rz) below.
        float px = rx, pz = rz;
        {
            uint32_t mo = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
            if (mo >= 0x80000000 && mo < 0x84000000) {
                const volatile float *mf = (const volatile float *)(uintptr_t)(mo + 0xA0);
                const volatile float *ml = (const volatile float *)(uintptr_t)(mo + 0xD0);
                float ddx = rx - mf[0], ddz = rz - mf[2];
                float d = sqrtf(ddx * ddx + ddz * ddz);
                const float MIN_SEP = 1.0f;
                if (fabsf(ry - mf[1]) < 1.5f && d < MIN_SEP) {
                    if (d > 0.05f) { ddx /= d; ddz /= d; }
                    else {
                        float lx = ml[0], lz = ml[2], ll = sqrtf(lx * lx + lz * lz);
                        if (ll > 1e-3f) { ddx = lx / ll; ddz = lz / ll; } else { ddx = 1.0f; ddz = 0.0f; }
                    }
                    px = mf[0] + MIN_SEP * ddx; pz = mf[2] + MIN_SEP * ddz;
                    static int seplog;
                    if (seplog++ < 3)
                        logf("coop: m2 separation: remote feet %.2f m from mine -> clone pushed to %.1f %.1f", d, px, pz);
                }
            }
        }
        uint32_t donor = coop_place_body(g_p10_gdst, px, ry, pz, nullptr);
        {   // smoothness probe: the clone's per-frame step while the partner moves (mean, frames standing, the largest step)
            static float lx, lz; static int have; static double sum, sum2; static uint32_t nf, nzero; static float mx; static uint64_t t0;
            if (have) {
                float st = sqrtf((px - lx) * (px - lx) + (pz - lz) * (pz - lz));
                if (st < 2.0f) {
                    sum += st; sum2 += (double)st * st; nf++;
                    if (st < 0.001f) nzero++;
                    if (st > mx) mx = st;
                }
            }
            lx = px; lz = pz; have = 1;
            uint64_t now = GetTickCount64();
            if (!t0) t0 = now;
            if (now - t0 >= 3000 && nf) {
                double mean = sum / nf, sd = sqrt((std::max)(0.0, sum2 / nf - mean * mean));
                if (mean > 0.01) logf("coop: clone motion: %u frames, step mean %.3f m sd %.3f, standing %u, largest %.3f (interp %d)", nf, mean, sd, nzero, mx, interp ? 1 : 0);
                sum = sum2 = 0; nf = nzero = 0; mx = 0; t0 = now;
            }
        }
        coop_aim_body(g_p10_gdst, ryaw, rpitch);                         // upper-body aim = remote view (interpolated)
        static VelTrack rv;                                              // remote player's velocity
        vel_update(rv, rx, rz);
        if (interp) { rv.vx = ivel[0]; rv.vz = ivel[1]; }                // the snapshot pair's velocity: no per-frame noise
        coop_loco_body(g_p10_gdst, rv.vx, rv.vz);                        // idle / walk / run = remote speed
        // FIRE (fire96: link+0x41 = trigger -> the clone's own weapon pipeline fires,
        // muzzle flash + sound from its equipped weapon): drive the private link's
        // trigger from the remote; keep the clone's clip at the remote's count (>= 1)
        // so it never dry-fires early and never out-shoots the real player.
        coop_clone_weapon(g_p10_gdst, (int)g_coop_host_pawnmat[5]);      // weapon MODEL = the remote's
        coop_clone_team(g_p10_gdst, *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C));   // ally, not enemy
        coop_ex_apply(*(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C));   // the peer's explosions + our grenade count (both roles; no link needed)
        if (g_aim_link) {
            uint8_t trig = (g_coop_host_pawnmat[4] > 0.5f || g_coop_trig_latch) ? 1 : 0;
            g_coop_trig_latch = 0;
            // owner screenshot 18:04: the host window had closed with its trigger held -> the joiner's
            // clone fired forever (looping muzzle flash). No fresh state packet for 0.5 s = trigger released.
            if (!g_coop_host_ms || GetTickCount64() - g_coop_host_ms > 500) trig = 0;
            // owner: "the joiner sees the host clone playing muzzle flash occasionally" (idea: the idle
            // animation). Checked: the trigger flag never rises without a press (0 events in 215 s of
            // scripted runs), but 12 presses of play 20261112_165417 came with the remote clip at 0 -
            // a DRY pull. The clone's clip is held >= 1, so it fired a real shot + flash the player
            // never fired. An empty remote clip = no clone shot.
            if ((int)g_coop_host_pawnmat[6] == 0) trig = 0;
            // ONE FIRE PATH (owner, 3rd report: "muzzle flash still plays from the host clone on the
            // joiner side"; that session: 2 host shotgun shots = 18 rays -> 4 extra replay shots). A real
            // shot reached the clone twice: the trigger made the clone fire its OWN shot, and the catch-up
            // fired the peer's real rays when that own shot was > 60 ms late (always, for the shotgun) ->
            // 2-3 blasts per shot, plus trigger shots with no real bullet at all. Now the clone's weapon
            // fires ONLY from the peer's received rays (coop_shot_catchup, no delay): one blast per real
            // shot. The trigger still feeds the AI alert below. Env COOP_CLONE_TRIGGER=1 = the old dual path.
            static int old_trig = -1;
            if (old_trig < 0) { char e[8] = {0}; old_trig = GetEnvironmentVariableA("COOP_CLONE_TRIGGER", e, sizeof e) && e[0] == '1'; g_shot_rays_only = !old_trig; }
            *(volatile uint8_t *)(uintptr_t)(g_aim_link + 0x41) = old_trig ? trig : 0;
            // tg4-tg6 (0 of 388 host-ordered kills landed): both damage functions gate on the ATTACKER's
            // link byte +0x4D ("may hurt now"). The game set it on the clone while its trigger was down;
            // with the trigger path off it kept whatever the donor had at clone time (random per run), so
            // the clone's bullets - the peer's real shots AND the kill shots - did no damage. Every bullet
            // the clone fires is real: always 1.
            *(volatile uint8_t *)(uintptr_t)(g_aim_link + 0x4D) = 1;
            link_sync_voice(g_aim_link, g_aim_link_src);   // voice index (dn144), see link_sync_voice
            uint32_t W = *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x278 + 0x28);
            int rclip = (int)g_coop_host_pawnmat[6];
            if (W >= 0x80000000 && W < 0x84000000 && rclip >= 0) {
                uint32_t cp = *(volatile uint32_t *)(uintptr_t)(W + 0xF4);
                if (cp >= 0x80000000 && cp < 0x84000000)
                    *(volatile int16_t *)(uintptr_t)(cp + 0x18) = (int16_t)(rclip < 1 ? 1 : rclip);
            }
            coop_shot_catchup(g_p10_gdst);   // hit sync: fire the remote rays the clone missed
            coop_xd_test();                  // --xd-test (host)
            coop_aggro_log();                // host: who each enemy is targeting
            if (g_cfg.coop_role == 2) coop_enemy_tick(*(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C));   // ENEMY SYNC (joiner)
            if (g_cfg.coop_role == 1 && trig) coop_clone_gunshot();   // the joiner's shots ALERT the AI
            static uint8_t ltrig = 0xFF;
            if (trig != ltrig) {
                ltrig = trig;
                logf("coop: lan fire: remote trigger %u (remote weapon id %d clip %d) -> clone W %08x st %d",
                     trig, (int)g_coop_host_pawnmat[5], rclip, W,
                     (W >= 0x80000000 && W < 0x84000000) ? *(volatile int *)(uintptr_t)(W + 0xD8) : -9);
            }
        }
        static uint32_t lastd;
        if ((g_coop_host_seq - lastd) >= 180) {
            lastd = g_coop_host_seq;
            uint32_t pobj = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
            const volatile float *me = (const volatile float *)(uintptr_t)(pobj + 0xA0);
            logf("coop: m2 lan drive #%u: body %08x at REMOTE feet %.1f %.1f %.1f (donor %08x) | my feet %.1f %.1f %.1f | remote yaw %.1f pitch %.1f",
                 g_coop_host_seq, g_p10_gdst, rx, ry, rz, donor, me[0], me[1], me[2],
                 g_coop_host_yaw, g_coop_host_pitch);
        }
    }

    // Pilot 13f park (--p13-park, solo visual check). USER RULE: the clone stands
    // BESIDE THE PLAYER on the player's own floor (the deck plane the deck NPCs
    // share). Player feet = pobj+0xA0..+0xA8 (game-maintained; y 7.75 on mission 1).
    // Spot = feet + fwd*3.5 + left*1.0 (in view, off the gun side). y IS written:
    // the engine does not re-ground a moved body (run 41: y stayed 2.9 everywhere).
    // Rotation columns stay game-owned. Runs only without fresh LAN data.
    if (g_cfg.p13_park && g_p10_done && g_p10_gdst >= 0x80100000 && g_p10_gdst < 0x84000000 &&
        !(g_coop_host_ms != 0 && GetTickCount64() - g_coop_host_ms <= 400)) {
        uint32_t pobj = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
        if (pobj >= 0x80000000 && pobj < 0x84000000) {
            const volatile float *eye  = (const volatile float *)(uintptr_t)(pobj + 0x100);
            const volatile float *fwd  = (const volatile float *)(uintptr_t)(pobj + 0xF0);
            const volatile float *lrow = (const volatile float *)(uintptr_t)(pobj + 0xD0);
            const volatile float *feet = (const volatile float *)(uintptr_t)(pobj + 0xA0);
            float fx = fwd[0], fz = fwd[2];
            float fl = sqrtf(fx * fx + fz * fz);
            float lx = lrow[0], lz = lrow[2];
            float ll = sqrtf(lx * lx + lz * lz);
            if (fl > 1e-3f && ll > 1e-3f && !(feet[0] == 0.0f && feet[2] == 0.0f) &&
                feet[1] > eye[1] - 3.0f && feet[1] < eye[1]) {
                fx /= fl; fz /= fl; lx /= ll; lz /= ll;
                float bx = feet[0] + 3.5f * fx + 1.0f * lx;
                float by = feet[1];
                float bz = feet[2] + 3.5f * fz + 1.0f * lz;
                // solo aim check: the clone takes the LOCAL player's view forward
                // (pitched) -> facing + aim basis follow where this player looks
                float lf[3] = {fwd[0], fwd[1], fwd[2]};
                // owner report (aim58): turning the view made the clone DRIFT - the
                // spot was recomputed from the view every frame and orbited the
                // player. Latch the spot once; only facing + aim follow the view.
                // Locomotion test (owner request): keep a WORLD-FIXED offset from the
                // player (latched once) so the clone walks alongside when the player
                // walks, and still never orbits when the view turns.
                static int park_latched;
                static float park_off[3];
                if (!park_latched) {
                    park_latched = 1;
                    park_off[0] = bx - feet[0]; park_off[1] = 0.0f; park_off[2] = bz - feet[2];
                    logf("coop: p13f park: OFFSET latched %.1f %.1f from the player's feet", park_off[0], park_off[2]);
                }
                bx = feet[0] + park_off[0]; by = feet[1]; bz = feet[2] + park_off[2];
                static VelTrack lv;                       // the local player's velocity (solo test)
                vel_update(lv, feet[0], feet[2]);
                float pre[3]; rd3(g_p10_gdst + 0xF0, pre);   // did the game rewrite it since our last write?
                float ayaw = *(volatile float *)(uintptr_t)(ctrl + 0x18);
                float apitch = *(volatile float *)(uintptr_t)(ctrl + 0x1C);
                if (g_cfg.p13_face) {
                    // --p13-face (front-view aim check): face the camera (yaw + 180,
                    // horizontal forward reversed) but keep the SAME pitch, so the
                    // clone aims up/down toward us as the camera looks up/down
                    lf[0] = -lf[0]; lf[2] = -lf[2];
                    ayaw += 180.0f;
                    if (ayaw > 180.0f) ayaw -= 360.0f;
                    // owner: "have the clone look at the camera" - aim the look straight
                    // at the player's eye (pobj+0x100) from the clone's head (~1.6 m):
                    // yaw = atan2(dx, dz) (F = (sin yaw, 0, cos yaw)); pitch sign taken
                    // from the player's own pitch vs its forward Y (latched once seen)
                    float dx = eye[0] - bx, dy = eye[1] - (by + 1.6f), dz = eye[2] - bz;
                    float dh = sqrtf(dx * dx + dz * dz);
                    if (dh > 0.2f) {
                        static float psign = 0.0f;
                        if (psign == 0.0f && fabsf(apitch) > 3.0f && fabsf(fwd[1]) > 0.05f)
                            psign = ((apitch > 0.0f) == (fwd[1] > 0.0f)) ? 1.0f : -1.0f;
                        // look104: controller yaw == atan2(fwd x, z) -> same convention.
                        // (The gun103 "90 deg off" was the AI aim pose blading the rig;
                        // fixed by the yaw twist in coop_bend_array.)
                        float pyaw = *(volatile float *)(uintptr_t)(ctrl + 0x18);
                        float a = 0.0f;
                        ayaw = 57.2957795f * atan2f(dx, dz);
                        apitch = (psign != 0.0f ? psign : 1.0f) * 57.2957795f * atan2f(dy, dh);
                        static uint32_t lk;
                        if ((lk++ % 120) == 0)
                            logf("coop: p13 look-at-camera: yaw %.1f pitch %.1f (ctrl yaw %.1f, atan2 fwd %.1f, rel %.1f, sign %.0f, d %.2f m, dy %.2f)",
                                 ayaw, apitch, pyaw, 57.2957795f * atan2f(fx, fz), a, psign, dh, dy);
                        if ((lk % 120) == 1) {
                            uint32_t cb = g_p10_gdst, ol = g_aim_link;
                            auto ry = [&](uint32_t off) { return 57.2957795f * atan2f(*(volatile float *)(uintptr_t)(cb + off), *(volatile float *)(uintptr_t)(cb + off + 8)); };
                            logf("coop: p13 look-at-camera: CLONE rows yaw(+70) %.1f yaw(+90) %.1f look(+F0) %.1f | root(+110) %.1f (+120) %.1f (+130) %.1f | donor %08x rows(+90) %.1f root(+130) %.1f | link +10 %.1f +18 %.1f +1C %.1f",
                                 ry(0x70), ry(0x90), ry(0xF0), ry(0x110), ry(0x120), ry(0x130), g_last_donor,
                                 g_last_donor ? 57.2957795f * atan2f(*(volatile float *)(uintptr_t)(g_last_donor + 0x90), *(volatile float *)(uintptr_t)(g_last_donor + 0x98)) : -999.f,
                                 g_last_donor ? 57.2957795f * atan2f(*(volatile float *)(uintptr_t)(g_last_donor + 0x130), *(volatile float *)(uintptr_t)(g_last_donor + 0x138)) : -999.f,
                                 ol ? *(volatile float *)(uintptr_t)(ol + 0x10) : -999.f, ol ? *(volatile float *)(uintptr_t)(ol + 0x18) : -999.f,
                                 ol ? *(volatile float *)(uintptr_t)(ol + 0x1C) : -999.f);
                        }
                    }
                }
                uint32_t donor = coop_place_body(g_p10_gdst, bx, by, bz, nullptr);   // facing = movement (loco)
                // solo aim: the local controller's yaw/pitch (deg, +0x18/+0x1C) - the
                // same values packet v4 carries as yaw/pitch for the LAN drive
                coop_aim_body(g_p10_gdst, ayaw, apitch);
                coop_loco_body(g_p10_gdst, lv.vx, lv.vz); // idle at 0 velocity, walk/run = player speed
                // WEAPON step 1 (owner: "the clone need to shoot properly"): FUN_001f0860
                // feeds the weapon state machine (FUN_001eefd0) from the link object:
                // link+0x41 = trigger -> weapon (body+0x278) state +0xD8 = 1 FIRE ->
                // FUN_001e2b90 (bullet fire). Solo check: log the PLAYER's link bytes
                // +0x40..+0x4F and weapon (pobj+0x278: state +0xD8, id +0x44, def +0x28)
                // and MIRROR its trigger into the clone's private link.
                {
                    uint32_t pl = *(volatile uint32_t *)(uintptr_t)(pobj + 0x324);
                    uint32_t cl = g_aim_link;
                    if (pl >= 0x80000000 && pl < 0x84000000) {
                        uint8_t trig = *(volatile uint8_t *)(uintptr_t)(pl + 0x41);
                        if (cl) *(volatile uint8_t *)(uintptr_t)(cl + 0x41) = trig;
                        // solo: the clone's weapon model mirrors the local player's
                        coop_clone_weapon(g_p10_gdst, weapon_type(*(volatile uint32_t *)(uintptr_t)(pobj + 0x278 + 0x28)));
                        coop_clone_team(g_p10_gdst, pobj);
                        static uint8_t last[16]; static uint32_t wn;
                        uint8_t now[16];
                        for (int k = 0; k < 16; k++) now[k] = *(volatile uint8_t *)(uintptr_t)(pl + 0x40 + k);
                        // fire95: body+0x278 = the INVENTORY (its +0xD8 is a pointer); the
                        // live weapon object W = [inv+0x28]: state W+0xD8 (1 = fire), clip
                        // [W+0xF4]+0x18, fire type *[W+0xF4]; id = inv+0x44; armed gate =
                        // body+0x29C (FUN_001f0860 skips the weapon when 0)
                        auto wdesc = [](uint32_t body, char *o, size_t on) {
                            uint32_t inv = body + 0x278;
                            uint32_t W = *(volatile uint32_t *)(uintptr_t)(inv + 0x28);
                            int st = -9, clip = -9, ft = -9;
                            if (W >= 0x80000000 && W < 0x84000000) {
                                st = *(volatile int *)(uintptr_t)(W + 0xD8);
                                uint32_t cp = *(volatile uint32_t *)(uintptr_t)(W + 0xF4);
                                if (cp >= 0x80000000 && cp < 0x84000000) {
                                    clip = *(volatile int16_t *)(uintptr_t)(cp + 0x18);
                                    ft = *(volatile int *)(uintptr_t)cp;
                                }
                            }
                            // weapon swap RE: the two inventory slots (def ptrs; def byte 0 =
                            // weapon type id), current slot +0x43, switch state +0x38
                            uint32_t sl = *(volatile uint32_t *)(uintptr_t)(inv + 0x20);
                            uint32_t s0 = 0, s1 = 0; int i0 = -1, i1 = -1;
                            if (sl >= 0x80000000 && sl < 0x84000000) {
                                s0 = *(volatile uint32_t *)(uintptr_t)sl; s1 = *(volatile uint32_t *)(uintptr_t)(sl + 4);
                                if (s0 >= 0x80000000 && s0 < 0x84000000) i0 = *(volatile int8_t *)(uintptr_t)s0;
                                if (s1 >= 0x80000000 && s1 < 0x84000000) i1 = *(volatile int8_t *)(uintptr_t)s1;
                            }
                            snprintf(o, on, "W %08x st %d clip %d ftype %d id %02x armed %08x | slots %08x[%d] %08x[%d] cur %d sw %d",
                                     W, st, clip, ft, *(volatile uint8_t *)(uintptr_t)(inv + 0x44),
                                     *(volatile uint32_t *)(uintptr_t)(body + 0x29C), s0, i0, s1, i1,
                                     *(volatile int8_t *)(uintptr_t)(inv + 0x43), *(volatile int *)(uintptr_t)(inv + 0x38));
                        };
                        char pd[200], cd[200];
                        wdesc(pobj, pd, sizeof pd); wdesc(g_p10_gdst, cd, sizeof cd);
                        static char lastc[200];
                        if (memcmp(now, last, 16) != 0 || strcmp(cd, lastc) != 0 || (wn++ % 300) == 0) {
                            memcpy(last, now, 16); strcpy(lastc, cd);
                            logf("coop: weapon: player link+40 %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                                 now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7],
                                 now[8], now[9], now[10], now[11], now[12], now[13], now[14], now[15]);
                            logf("coop: weapon:   PLAYER %s", pd);
                            logf("coop: weapon:   CLONE  %s (clone link+41 %02x)", cd, cl ? *(volatile uint8_t *)(uintptr_t)(cl + 0x41) : 0xEE);
                        }
                    }
                }
                if (g_cfg.aim_test_deg != 0.0f) coop_aim_test_rec1(g_p10_gdst, g_cfg.aim_test_deg, g_cfg.aim_test_rec);
                {   // skeleton map: 3 snapshots (idle, look-up window, later)
                    static uint32_t skn;
                    skn++;
                    if (skn == 200) coop_log_skeleton(g_p10_gdst, "idle");
                    if (skn == 260) coop_log_skeleton(g_p10_gdst, "idle2");
                    if (skn == 900) coop_log_skeleton(g_p10_gdst, "late");
                }
                static uint32_t aimn;
                if ((aimn++ % 120) == 0)
                    logf("coop: aim: write F %.2f %.2f %.2f | found before write %.2f %.2f %.2f",
                         lf[0], lf[1], lf[2], pre[0], pre[1], pre[2]);
                static uint32_t dlog;
                if (dlog != donor) { dlog = donor; logf("coop: p13f park: mat2 donor %08x", donor); }
                static uint32_t besn;
                if ((besn++ % 120) == 0)
                    logf("coop: p13f park BESIDE player: body %08x at %.1f %.1f %.1f (feet %.1f %.1f %.1f)",
                         g_p10_gdst, bx, by, bz, feet[0], feet[1], feet[2]);
            }
        }
    }

    // Pilot 10: the differential tracker. Every 30th frame read the fresh-construct
    // clone's and the real soldier's key fields: if the game touches the clone the
    // way it touches real units, the values evolve identically; wherever they
    // diverge per frame = the render-chain difference.
    if (g_clone_rec >= 0x10000 && g_real_rec >= 0x10000) {
        static uint32_t trk;
        if ((++trk % 30) == 0) {
            uint16_t wc = *(volatile uint16_t *)(uintptr_t)(g_clone_rec + 0x13C);
            uint16_t wr = *(volatile uint16_t *)(uintptr_t)(g_real_rec + 0x13C);
            float cx = *(volatile float *)(uintptr_t)(g_clone_rec + 0x20);
            float cyy = *(volatile float *)(uintptr_t)(g_clone_rec + 0x24);
            float cz = *(volatile float *)(uintptr_t)(g_clone_rec + 0x28);
            float rx = *(volatile float *)(uintptr_t)(g_real_rec + 0x20);
            float ry2 = *(volatile float *)(uintptr_t)(g_real_rec + 0x24);
            float rz = *(volatile float *)(uintptr_t)(g_real_rec + 0x28);
            uint32_t mc = *(volatile uint32_t *)(uintptr_t)(g_clone_rec + 0x70);
            uint32_t mr = *(volatile uint32_t *)(uintptr_t)(g_real_rec + 0x70);
            uint8_t bc = *(volatile uint8_t *)(uintptr_t)(g_clone_rec + 0x13D);
            uint8_t br = *(volatile uint8_t *)(uintptr_t)(g_real_rec + 0x13D);
            uint32_t lbc = *(volatile uint32_t *)(uintptr_t)(g_clone_rec + 0x1BC);
            uint32_t lbr = *(volatile uint32_t *)(uintptr_t)(g_real_rec + 0x1BC);
            logf("coop: trk: clone w%04x pos %.1f %.1f %.1f m%08x 1BC %08x | real w%04x pos %.1f %.1f %.1f m%08x 1BC %08x",
                 wc, cx, cyy, cz, mc, lbc, wr, rx, ry2, rz, mr, lbr);
        }
    }

    // Pilot 11 observer: the registered clone's live state — if the engine's tick
    // took ownership, its +0xA0 position evolves and the vtable/list links hold.
    if (g_p10_done && g_p10_gdst >= 0x10000 && g_p10_gdst < 0x84000000) {
        static uint32_t obsn;
        if ((++obsn % 60) == 0) {
            float qx = *(volatile float *)(uintptr_t)(g_p10_gdst + 0xA0);
            float qy = *(volatile float *)(uintptr_t)(g_p10_gdst + 0xA4);
            float qz = *(volatile float *)(uintptr_t)(g_p10_gdst + 0xA8);
            uint32_t nxt = *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0xB0);
            uint8_t busy = *(volatile uint8_t *)(uintptr_t)(g_p10_gdst + 0x13B);
            uint32_t vt = *(volatile uint32_t *)(uintptr_t)g_p10_gdst;
            logf("coop: p10: obs: clone %08x vt %08x busy %u next %08x pos %.1f %.1f %.1f",
                 g_p10_gdst, vt, busy, nxt, qx, qy, qz);
            // second-matrix differential: our body vs every natural pawn slot
            {
                const volatile float *m = (const volatile float *)(uintptr_t)(g_p10_gdst + 0x110);
                logf("coop: p13f mat2 OURS %08x +A0 %.1f %.1f %.1f | +140 %.2f %.2f %.2f %.2f | r0 %.2f %.2f %.2f",
                     g_p10_gdst, qx, qy, qz, m[12], m[13], m[14], m[15], m[0], m[1], m[2]);
                uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
                uint32_t arr = (mgr >= 0x80000000 && mgr < 0x84000000)
                    ? *(volatile uint32_t *)(uintptr_t)(mgr + 0x79BC) : 0;
                if (arr >= 0x80000000 && arr < 0x84000000 && (obsn % 300) == 0) {
                    for (int i = 0; i < 16; i++) {
                        uint32_t b = *(volatile uint32_t *)(uintptr_t)(arr + i * 4);
                        if (b < 0x80000000 || b >= 0x84000000 || b == g_p10_gdst) continue;
                        const volatile float *a = (const volatile float *)(uintptr_t)(b + 0xA0);
                        const volatile float *n = (const volatile float *)(uintptr_t)(b + 0x110);
                        logf("coop: p13f mat2 NPC[%d] %08x +A0 %.1f %.1f %.1f | +140 %.2f %.2f %.2f %.2f | r0 %.2f %.2f %.2f",
                             i, b, a[0], a[1], a[2], n[12], n[13], n[14], n[15], n[0], n[1], n[2]);
                        logf("coop: p13f post NPC[%d] %08x +10=%08x +14=%02x +31C=%08x +2F0=%08x +3A7=%02x",
                             i, b, *(volatile uint32_t *)(uintptr_t)(b + 0x10),
                             *(volatile uint8_t *)(uintptr_t)(b + 0x14),
                             *(volatile uint32_t *)(uintptr_t)(b + 0x31C),
                             *(volatile uint32_t *)(uintptr_t)(b + 0x2F0),
                             *(volatile uint8_t *)(uintptr_t)(b + 0x3A7));
                    }
                    {
                        uint32_t b = g_p10_gdst;
                        logf("coop: p13f post OURS %08x +10=%08x +14=%02x +31C=%08x +2F0=%08x +3A7=%02x",
                             b, *(volatile uint32_t *)(uintptr_t)(b + 0x10),
                             *(volatile uint8_t *)(uintptr_t)(b + 0x14),
                             *(volatile uint32_t *)(uintptr_t)(b + 0x31C),
                             *(volatile uint32_t *)(uintptr_t)(b + 0x2F0),
                             *(volatile uint8_t *)(uintptr_t)(b + 0x3A7));
                    }
                }
            }
        }
    }

    float yaw = cpu_of(ctrl + 0x18), pitch = cpu_of(ctrl + 0x1C);
    float rp = cpu_of(ctrl + 0xA0), ry = cpu_of(ctrl + 0xA4);
    float sp = cpu_of(ctrl + 0xB8), sy = cpu_of(ctrl + 0xBC);
    uint32_t obj = *(const uint32_t *)(uintptr_t)(ctrl + 0x8C);
    float m[16] = {};
    if (obj >= 0x10000 && obj < 0x84000000) memcpy(m, (const void *)(uintptr_t)(obj + 0xD0), sizeof m);

    // Pilot 11 census: walk the world tick list (world+0x5CB4 chain, next at +0xB0)
    // and record each entry's identity/position/model-pointer fields. One throttled
    // pass per 600 frames = the map of who actually lives on the render list - this
    // tells apart the pawn/donor bodies (torso rigs) from any full-character
    // entries (the lobby model picker enumerates these).
    {
        static int cen_last = -1;
        int fc = (int)g_frame_count.load();
        if (fc % 1800 == 0 && fc != cen_last && g_cfg.coop_log) {
            cen_last = fc;
            uint32_t wrld = *(volatile uint32_t *)(uintptr_t)0x002D45D4;
            if (wrld >= 0x80000000 && wrld < 0x84000000) {
                uint32_t node = *(volatile uint32_t *)(uintptr_t)(wrld + 0x5CB4);
                int step = 0, logged = 0;
                while (node >= 0x80100000 && node < 0x84000000 && logged < 28 && step < 96) {
                    float nx = *(volatile float *)(uintptr_t)(node + 0xA0);
                    float ny = *(volatile float *)(uintptr_t)(node + 0xA4);
                    float nz = *(volatile float *)(uintptr_t)(node + 0xA8);
                    uint32_t vtn = *(volatile uint32_t *)(uintptr_t)node;
                    uint32_t p110 = *(volatile uint32_t *)(uintptr_t)(node + 0x110);
                    uint32_t p114 = *(volatile uint32_t *)(uintptr_t)(node + 0x114);
                    if (step % 3 == 0) {
                        logf("coop: p11 census [%02d] %08x vt %08x pos %.1f %.1f %.1f +110 %08x +114 %08x",
                             step, node, vtn, nx, ny, nz, p110, p114);
                        logged++;
                    }
                    uint32_t nxt = *(volatile uint32_t *)(uintptr_t)(node + 0xB0);
                    if (nxt == node) break;
                    node = nxt;
                    step++;
                }
            }
        }
    }

    coop_load_keepalive();                       // the partner may still wait at its loading screen for our CLLR
    coop_exit_step(obj);                         // the shared level exit countdown
    coop_real_cp_step();                         // the game's real checkpoint popup: the reacher pulls the partner (CLCK)
    coop_hm_step();                              // hit markers: a hit enemy at 0 health = a kill
    coop_part_step();                            // a level part the partner has and we do not: switch to it here
    {   // COOP_CP_TEST=<s> (test): at <s> s (from the co-op start) run the checkpoint rule as if the PARTNER set one off
        static float at = -2.0f; static int done;
        if (at < -1.0f) { char e[16] = {0}; at = GetEnvironmentVariableA("COOP_CP_TEST", e, sizeof e) ? (float)atof(e) : -1.0f; }
        if (at > 0 && !done && g_coop_t0 && (float)(GetTickCount64() - g_coop_t0) / 1000.0f >= at) {
            done = 1;
            if (g_cfg.coop_role == 2) {   // the JOINER starts a section alone (no synced trigger): the host must come to it
                g_cp_ms = 0;
                logf("coop: checkpoint: TEST - a section starts here alone");
                coop_checkpoint_section();
            }
        }
    }
    coop_quit_step();                            // the partner left / went silent: quit the level here
    if (g_coop_clone) coop_squad_donor_step();   // a level without the squad character loads it from a donor level
    if (!g_p10_done && coop_own_spawn_on()) {        // own spawn: no natural spawn came first -> build it here
        const float *pp = (const float *)(uintptr_t)(obj + 0xA0);
        float at[3] = {pp[0] + 2.0f, pp[1], pp[2] + 2.0f};
        coop_spawn_own(at, "first play tick");
    }
    coop_route_step(ctrl, obj);   // route recorder / ghost run (env COOP_ROUTE_REC / COOP_ROUTE)
    if (g_cfg.coop_role) coop_enid_update(obj);   // enemy identity table (both roles; before the send below)
    {   // LEVEL END probe (day 15): the game object *0x2D45F0 +0x21088 = the mode request (1 / 2 = leave the level)
        static uint32_t last_req = 0xFFFFFFFF, gobj_logged;
        uint32_t go = *(volatile uint32_t *)(uintptr_t)0x002D45F0;
        if (go >= 0x80000000 && go < 0x84000000) {
            if (go != gobj_logged) { gobj_logged = go; logf("coop: level end probe: game object %08x (mode request at %08x), end-action object %08x (code at +4)", go, go + 0x21088, *(volatile uint32_t *)(uintptr_t)0x002D19A4); }
            uint32_t rq = *(volatile uint32_t *)(uintptr_t)(go + 0x21088);
            if (rq != last_req) { logf("coop: level end probe: mode request %u -> %u (mode object %08x)", last_req, rq, *(volatile uint32_t *)(uintptr_t)(go + 0x21070)); last_req = rq; }
        }
    }
    coop_trig_trace(obj);                         // COOP_TRIG_LOG=1
    coop_trig_step(obj);                          // trigger sync (host: test for the joiner; joiner: replay)
    if (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000 && !g_coop_peer_downed) {   // clone watchdog
        static uint32_t last_life;
        uint32_t life = *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x384);
        if (life != last_life) logf("coop: clone guard: clone life state %u -> %u while the peer is up%s", last_life, life, life ? " - SOMETHING ELSE RETIRES IT" : "");
        last_life = life;
    }
    {   // COOP_CENSUS=<sec>[,<sec>..]: classified census of the world tick list ([world+0x5CB4], next +0xB0).
        // OBJECT KINDS (+0xC4; cen1, Naszran yard): 1 = soldier (vt 0028c1e8; enemy when its team +0x39C
        // differs from the player's; the clone is one too), 2 = player (vt 0028cfe0), 4 = DESTRUCTIBLE
        // PROP (vt 0028ed98, 0x200 B; +0x2F0 = damage taken so far; the yard's blasts are centred on
        // these), 3 = debris piece (vt 0028eca8, spawned by a blast), 7 = small static item (vt 0028c640).
        static float cs[4] = {-1, -1, -1, -1}; static int ci = -1;
        if (ci < 0) {
            ci = 0; char e[64] = {0};
            if (GetEnvironmentVariableA("COOP_CENSUS", e, sizeof e)) sscanf(e, "%f,%f,%f,%f", &cs[0], &cs[1], &cs[2], &cs[3]);
        }
        if (ci < 4 && cs[ci] > 0 && g_coop_t0 && (float)(GetTickCount64() - g_coop_t0) * 0.001f >= cs[ci]) {
            ci++;
            uint32_t wrld = *(volatile uint32_t *)(uintptr_t)0x002D45D4;
            uint32_t node = (wrld >= 0x80000000 && wrld < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(wrld + 0x5CB4) : 0;
            const volatile float *pf = (const volatile float *)(uintptr_t)(obj + 0xA0);
            static float cat[3]; static int cat_on = -1;   // COOP_CENSUS_AT=x,y,z: centre the 60 m list there
            if (cat_on < 0) { char e2[64] = {0}; cat_on = GetEnvironmentVariableA("COOP_CENSUS_AT", e2, sizeof e2) && sscanf(e2, "%f,%f,%f", &cat[0], &cat[1], &cat[2]) == 3; }
            if (cat_on) pf = cat;
            int step = 0, cnt[8] = {0};
            logf("coop: CENSUS %d at %.0f s - player %08x at %.1f %.1f %.1f (objects within 60 m listed)", ci, cs[ci - 1], obj, pf[0], pf[1], pf[2]);
            while (node >= 0x80000000 && node < 0x84000000 && step < 1500) {
                uint32_t vt = 0, k = 0; float q[3] = {0, 0, 0}, hp = 0;
                if (!coop_safe_copy(&vt, node, 4)) break;
                coop_safe_copy(q, node + 0xA0, 12); coop_safe_copy(&k, node + 0xC4, 4); coop_safe_copy(&hp, node + 0x2F0, 4);
                float dx = q[0] - pf[0], dy = q[1] - pf[1], dz = q[2] - pf[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
                const char *cls = "other";
                if (node == obj) cls = "PLAYER";
                else if (node == g_p10_gdst) cls = "CLONE";
                else if (k == 1 && vt == 0x0028C1E8) cls = is_enemy(node, obj) ? (hp > 0.0f ? "ENEMY" : "ENEMY(dead)") : "ALLY";
                else if (k == 4 && vt == 0x0028ED98) cls = "PROP";
                else if (k == 3) cls = "DEBRIS";
                else if (k == 7) cls = "ITEM";
                cnt[k < 8 ? k : 0]++;
                if (d < 60.0f && k == 4) {   // prop: its TYPE pointer (+0x118; props of one type behave alike) and part positions
                    uint32_t ty = 0; char pt[320] = {0}; int pl = 0; float pp[12][3];
                    coop_safe_copy(&ty, node + 0x118, 4);
                    int np = coop_prop_parts(node, pp, 12);
                    for (int i2 = 0; i2 < np; i2++) pl += snprintf(pt + pl, sizeof pt - pl, " (%.1f %.1f %.1f)", pp[i2][0], pp[i2][1], pp[i2][2]);
                    logf("coop: census %-11s %08x at %.1f %.1f %.1f (%.0f m) damage %.1f | type %08x, %d parts:%s", cls, node, q[0], q[1], q[2], d, hp, ty, np, pt);
                } else if ((d < 60.0f || (k == 1 && strcmp(cls, "ALLY") == 0)) && k != 7)   // allies at any distance (squad survey)
                    logf("coop: census %-11s %08x at %.1f %.1f %.1f (%.0f m) %s %.1f | vt %08x kind %u | team %u +2C0 %08x +2C4 %08x link %08x ctx %08x", cls, node, q[0], q[1], q[2], d, k == 4 ? "damage" : "hp", hp, vt, k,
                         k == 1 ? *(volatile uint32_t *)(uintptr_t)(node + 0x39C) : 0, k == 1 ? *(volatile uint32_t *)(uintptr_t)(node + 0x2C0) : 0,
                         k == 1 ? *(volatile uint32_t *)(uintptr_t)(node + 0x2C4) : 0, k == 1 ? *(volatile uint32_t *)(uintptr_t)(node + 0x324) : 0,
                         k == 1 ? *(volatile uint32_t *)(uintptr_t)(node + 0x31C) : 0);
                else if (d < 12.0f && k == 7)
                    logf("coop: census %-11s %08x at %.1f %.1f %.1f (%.0f m) | vt %08x kind %u", cls, node, q[0], q[1], q[2], d, vt, k);
                if (k == 1 && g_unit_arr) {   // ENEMY IDENTITY hunt: body fields that point into the level unit array
                    char ub[200] = {0}; int ul = 0;
                    for (int o = 0; o < 0x420 && ul < 170; o += 4) {
                        uint32_t v = 0; coop_safe_copy(&v, node + o, 4);
                        if (v >= g_unit_arr && v < g_unit_arr + g_unit_cnt * 0x1D0)
                            ul += snprintf(ub + ul, sizeof ub - ul, " +%03x->unit %u(+%x)", o, (v - g_unit_arr) / 0x1D0, (v - g_unit_arr) % 0x1D0);
                    }
                    logf("coop: census   soldier %08x unit links:%s", node, ul ? ub : " none");
                }
                uint32_t nxt = 0;
                if (!coop_safe_copy(&nxt, node + 0xB0, 4) || nxt == node) break;
                node = nxt; step++;
            }
            logf("coop: CENSUS %d end: %d objects - soldiers %d, players %d, props %d, debris %d, items %d", ci, step + 1, cnt[1], cnt[2], cnt[4], cnt[3], cnt[7]);
            {   // CHARACTER TABLE (squad research): the anim instance (body +0x328) keeps its character entry at +0x50,
                // taken from *0x2D12C8 + 0xF8 + index * 0x60 (FUN_0017E420 from the soldier constructor). Dump the table
                // and every soldier's entry index + type record (body +0x320).
                uint32_t cm = *(volatile uint32_t *)(uintptr_t)0x002D12C8;
                uint32_t tbl = (cm >= 0x80000000 && cm < 0x84000000) ? cm + 0xF8 : 0;
                logf("coop: chartable: manager %08x table %08x", cm, tbl);
                for (int i = 0; tbl && i < 24; i++) {
                    uint32_t w[24] = {0};
                    if (!coop_safe_copy(w, tbl + i * 0x60, 0x60)) break;
                    bool empty = true; for (int k = 0; k < 24; k++) if (w[k]) empty = false;
                    if (empty) { logf("coop: chartable: entry %d empty - stop", i); break; }
                    logf("coop: chartable: entry %d @%08x: %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x %08x",
                         i, tbl + i * 0x60, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19], w[20], w[21], w[22], w[23]);
                }
                uint32_t s16[16]; uint32_t ns = enemy_slots(s16);
                for (uint32_t k = 0; k < ns; k++) {
                    uint32_t b = s16[k], A = 0, ent = 0, ty = 0, tw[4] = {0}, team = 0; float hp = 0;
                    coop_safe_copy(&A, b + 0x328, 4); if (A >= 0x80000000 && A < 0x84000000) coop_safe_copy(&ent, A + 0x50, 4);
                    coop_safe_copy(&ty, b + 0x320, 4); if (ty >= 0x80000000 && ty < 0x84000000) coop_safe_copy(tw, ty, 16);
                    coop_safe_copy(&team, b + 0x39C, 4); coop_safe_copy(&hp, b + 0x2F0, 4);
                    logf("coop: chartable: soldier %08x team %u hp %g -> entry %08x (index %d) | type %08x: %08x %08x %08x %08x", b, team, hp, ent,
                         (tbl && ent >= tbl) ? (int)((ent - tbl) / 0x60) : -1, ty, tw[0], tw[1], tw[2], tw[3]);
                }
            }
        }
    }
    {   // COOP_AIM_AT=enemy|prop (+ COOP_AIM_FROM=<sec>, default 90): TEST PILOT AIM. Each frame the
        // player's look (ctrl +0x18 yaw, +0x1C pitch, + = down) is turned onto the nearest live enemy
        // (chest) or the nearest prop that has not blown up yet (within 45 m; a target is dropped
        // after 6 s). Scripted trigger presses (--press "..s:rt*N") then hit it.
        static int mode = -1; static float from = 90.0f;
        static uint32_t tgt = 0; static uint64_t tgt_ms = 0; static uint32_t done[96]; static int ndone = 0;
        static int tpart = 0;                 // prop mode: the part being shot (done[] holds prop + part)
        static uint32_t aim_type = 0;
        if (mode < 0) {
            char e[16] = {0}; mode = 0;
            static float apt[3];   // COOP_AIM_AT=x,y,z: aim at that POINT (side-objective targets live outside the world list)
            if (GetEnvironmentVariableA("COOP_AIM_AT", e, sizeof e)) {
                char e3[64] = {0}; GetEnvironmentVariableA("COOP_AIM_AT", e3, sizeof e3);
                mode = e[0] == 'e' ? 1 : (e[0] == 'p' ? 2 : (sscanf(e3, "%f,%f,%f", &apt[0], &apt[1], &apt[2]) == 3 ? 3 : 0));
            }
            g_aim_pt[0] = apt[0]; g_aim_pt[1] = apt[1]; g_aim_pt[2] = apt[2];
            char v[16] = {0};
            if (GetEnvironmentVariableA("COOP_AIM_FROM", v, sizeof v)) from = (float)atof(v);
            char ty[16] = {0};
            if (GetEnvironmentVariableA("COOP_AIM_TYPE", ty, sizeof ty)) aim_type = (uint32_t)strtoul(ty, nullptr, 16);   // props of this type only (+0x118)
            if (mode) logf("coop: aim-at: test pilot aims at the nearest %s from %.0f s (prop type filter %08x)", mode == 1 ? "ENEMY" : "PROP", from, aim_type);
        }
        if (mode == 3 && g_coop_t0 && (float)(GetTickCount64() - g_coop_t0) * 0.001f >= from && obj >= 0x80000000 && obj < 0x84000000) {
            const volatile float *eye = (const volatile float *)(uintptr_t)(obj + 0x100);
            float dx = g_aim_pt[0] - eye[0], dy = g_aim_pt[1] - eye[1], dz = g_aim_pt[2] - eye[2], h = sqrtf(dx * dx + dz * dz);
            if (h > 0.05f) {
                *(volatile float *)(uintptr_t)(ctrl + 0x18) = 57.2957795f * atan2f(dx, dz);
                *(volatile float *)(uintptr_t)(ctrl + 0x1C) = 57.2957795f * atan2f(-dy, h);
            }
            static int pl; if (!pl++) logf("coop: aim-at: test pilot aims at the POINT %.1f %.1f %.1f", g_aim_pt[0], g_aim_pt[1], g_aim_pt[2]);
        } else
        if (mode && g_coop_t0 && (float)(GetTickCount64() - g_coop_t0) * 0.001f >= from && obj >= 0x80000000 && obj < 0x84000000) {
            const volatile float *eye = (const volatile float *)(uintptr_t)(obj + 0x100);
            uint64_t now = GetTickCount64();
            auto blown = [&](const float q[3]) {
                uint32_t n = g_ex_all_n < 64 ? g_ex_all_n : 64;
                for (uint32_t i = 0; i < n; i++) {
                    float dx = g_ex_all[i][0] - q[0], dy = g_ex_all[i][1] - q[1], dz = g_ex_all[i][2] - q[2];
                    if (dx * dx + dy * dy + dz * dz < 2.25f) return true;
                }
                return false;
            };
            auto valid = [&](uint32_t n, int part) {
                uint32_t vt = 0, k = 0; float hp = 0;
                if (n < 0x80000000 || n >= 0x84000000 || !coop_safe_copy(&vt, n, 4)) return false;
                coop_safe_copy(&k, n + 0xC4, 4); coop_safe_copy(&hp, n + 0x2F0, 4);
                if (mode == 1) return k == 1 && vt == 0x0028C1E8 && is_enemy(n, obj) && hp > 0.0f;
                if (k != 4 || vt != 0x0028ED98) return false;
                if (aim_type) { uint32_t t2 = 0; coop_safe_copy(&t2, n + 0x118, 4); if (t2 != aim_type) return false; }
                float pp[12][3]; int np = coop_prop_parts(n, pp, 12);
                return part < np && !blown(pp[part]);
            };
            if (tgt && (!valid(tgt, tpart) || now - tgt_ms > 6000)) {
                if (ndone < 96) done[ndone++] = tgt + (uint32_t)tpart;       // props are 0x200 apart: + part keeps them distinct
                logf("coop: aim-at: target %08x%s finished (%s)", tgt, mode == 2 ? " part" : "", valid(tgt, tpart) ? "6 s timeout" : (mode == 1 ? "dead" : "destroyed"));
                tgt = 0;
            }
            if (!tgt) {
                uint32_t wrld = *(volatile uint32_t *)(uintptr_t)0x002D45D4;
                uint32_t node = (wrld >= 0x80000000 && wrld < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(wrld + 0x5CB4) : 0;
                float best = 45.0f; int step = 0;
                while (node >= 0x80000000 && node < 0x84000000 && step++ < 1500) {
                    float pp[12][3]; int np = 1;
                    uint32_t k4 = 0; coop_safe_copy(&k4, node + 0xC4, 4);
                    if (mode == 2) np = k4 == 4 ? coop_prop_parts(node, pp, 12) : 0;
                    for (int pi = 0; pi < np; pi++) {
                        bool skip = false;
                        for (int i = 0; i < ndone; i++) if (done[i] == node + (uint32_t)pi) skip = true;
                        if (skip || !valid(node, pi)) continue;
                        float q[3];
                        if (mode == 2) { q[0] = pp[pi][0]; q[1] = pp[pi][1]; q[2] = pp[pi][2]; } else coop_safe_copy(q, node + 0xA0, 12);
                        float dx = q[0] - eye[0], dy = q[1] - eye[1], dz = q[2] - eye[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (d < best && d > 1.0f) { best = d; tgt = node; tpart = pi; }
                    }
                    uint32_t nxt = 0;
                    if (!coop_safe_copy(&nxt, node + 0xB0, 4) || nxt == node) break;
                    node = nxt;
                }
                if (tgt) {
                    tgt_ms = now;
                    float q[3]; coop_safe_copy(q, tgt + 0xA0, 12);
                    float pp[12][3];
                    if (mode == 2 && tpart < coop_prop_parts(tgt, pp, 12)) { q[0] = pp[tpart][0]; q[1] = pp[tpart][1]; q[2] = pp[tpart][2]; }
                    uint32_t ty = 0; coop_safe_copy(&ty, tgt + 0x118, 4);
                    if (mode == 1) logf("coop: aim-at: new target ENEMY %08x at %.1f %.1f %.1f (%.1f m)", tgt, q[0], q[1], q[2], best);
                    else logf("coop: aim-at: new target PROP %08x part %d (type %08x) at %.1f %.1f %.1f (%.1f m)", tgt, tpart, ty, q[0], q[1], q[2], best);
                }
            }
            if (tgt) {
                float q[3]; coop_safe_copy(q, tgt + 0xA0, 12);
                float pp[12][3];
                if (mode == 2 && tpart < coop_prop_parts(tgt, pp, 12)) { q[0] = pp[tpart][0]; q[1] = pp[tpart][1]; q[2] = pp[tpart][2]; }
                float dx = q[0] - eye[0], dy = q[1] + (mode == 1 ? 1.2f : 0.0f) - eye[1], dz = q[2] - eye[2];
                float h = sqrtf(dx * dx + dz * dz);
                if (h > 0.05f) {
                    *(volatile float *)(uintptr_t)(ctrl + 0x18) = 57.2957795f * atan2f(dx, dz);
                    *(volatile float *)(uintptr_t)(ctrl + 0x1C) = 57.2957795f * atan2f(-dy, h);
                }
            }
        }
    }
    if (g_cfg.coop_log && (n % g_coop_log_every) == 0)
        logf("coop: call %u frame %u controller %08x | view yaw %+7.3f pitch %+7.3f deg | rates (p %+.4f, y %+.4f) speeds (p %+.4f, y %+.4f) | "
             "cam %08x eye %.3f %.3f %.3f | cam rows %08.4g %08.4g %08.4g | dir %08.4g %08.4g %08.4g",
             n, g_frame_count.load(), ctrl, yaw, pitch, rp, ry, sp, sy, obj, m[12], m[13], m[14],
             m[0], m[1], m[2], m[8], m[9], m[10]);

    if (g_coop_sock != INVALID_SOCKET) {
        // packet v3: eye ypr+pos (markers/remote view) + the sender's PAWN body
        // block (+0x70..+0xAC from mgr+0x79BC[0]) = the LAN drive target for the
        // other side's puppet (stands at this player's feet, facing as this body
        // faces - all game-maintained, no synth offsets).
        // packet v4 (magic 0x434C4152): pawnmat[0..2] = this player's view forward
        // (pobj+0xF0), [3] = 1.0 valid marker, [12..14] = this player's FEET
        // (pobj+0xA0, game-maintained, on the floor plane). v3 read *(mgr+0x79BC) =
        // the slot-ARRAY pointer (not a body) - never the player.
        struct { uint32_t magic, seq, ctrl, obj; float yaw, pitch, pos[3], pawnmat[15]; uint32_t downed; double tms; } pkt;   // tms: the sender's clock (ms, QPC) - interpolation
        memset(&pkt, 0, sizeof pkt);
        pkt = { 0x434C4152, n, ctrl, obj, yaw, pitch, {m[12], m[13], m[14]}, {}, g_coop_downed };
        pkt.tms = coop_qpc_ms();
        if (obj >= 0x80000000 && obj < 0x84000000) {
            const volatile float *feet = (const volatile float *)(uintptr_t)(obj + 0xA0);
            const volatile float *fwd = (const volatile float *)(uintptr_t)(obj + 0xF0);
            pkt.pawnmat[0] = fwd[0]; pkt.pawnmat[1] = fwd[1]; pkt.pawnmat[2] = fwd[2];
            pkt.pawnmat[3] = 1.0f;
            {   // [7] valid, [8..10] the crosshair point, [11] its distance
                float ap[3], ad;
                if (coop_crosshair_point(obj, ap, &ad)) {
                    pkt.pawnmat[7] = 1.0f; pkt.pawnmat[8] = ap[0]; pkt.pawnmat[9] = ap[1]; pkt.pawnmat[10] = ap[2]; pkt.pawnmat[11] = ad;
                    static uint32_t al;
                    if ((al++ % 600) == 0) logf("coop: aim point: crosshair at %.1f %.1f %.1f (%.1f m)", ap[0], ap[1], ap[2], ad);
                }
            }
            pkt.pawnmat[12] = feet[0]; pkt.pawnmat[13] = feet[1]; pkt.pawnmat[14] = feet[2];
            if (g_coop_downed && g_downed_pos_ok) {   // our body stays where we went down (the camera spot is ours alone)
                pkt.pawnmat[12] = g_downed_pos[0]; pkt.pawnmat[13] = g_downed_pos[1]; pkt.pawnmat[14] = g_downed_pos[2];
                // owner (day 14): "a downed clone head and body rotate when the other player is close looking at
                // him" - our view while downed is the CHASE CAM behind the survivor, so the yaw / look we sent
                // followed the survivor's view. A downed body keeps the yaw it went down with and aims nowhere.
                float yr = g_downed_yaw * 0.0174532925f;
                pkt.yaw = g_downed_yaw; pkt.pitch = 70.0f;
                pkt.pawnmat[0] = sinf(yr); pkt.pawnmat[1] = 0.0f; pkt.pawnmat[2] = cosf(yr);
                pkt.pawnmat[7] = 0.0f;   // no crosshair point
            }
            // WEAPON (fire96): [4] = trigger (player link +0x41), [5] = weapon id
            // (inventory obj+0x278 +0x44), [6] = clip ([[inv+0x28]+0xF4]+0x18)
            uint32_t pl = *(volatile uint32_t *)(uintptr_t)(obj + 0x324);
            if (pl >= 0x80000000 && pl < 0x84000000) pkt.pawnmat[4] = *(volatile uint8_t *)(uintptr_t)(pl + 0x41) ? 1.0f : 0.0f;
            uint32_t W = *(volatile uint32_t *)(uintptr_t)(obj + 0x278 + 0x28);
            // [5] = the CURRENT weapon's type (its first byte: SMG 8, pistol 13);
            // inv+0x44 reads 00 until the first switch (swap97)
            pkt.pawnmat[5] = (float)weapon_type(W);
            pkt.pawnmat[6] = -1.0f;
            if (W >= 0x80000000 && W < 0x84000000) {
                uint32_t cp = *(volatile uint32_t *)(uintptr_t)(W + 0xF4);
                if (cp >= 0x80000000 && cp < 0x84000000) pkt.pawnmat[6] = (float)*(volatile int16_t *)(uintptr_t)(cp + 0x18);
            }
        }
        sendto(g_coop_sock, (const char *)&pkt, sizeof pkt, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        // HIT SYNC: this frame's real bullet rays (origin + unit dir per pellet)
        for (int i = 0; i < g_shot_out_n; i += 16) {
            struct { uint32_t magic, seq, n; ShotRay r[16]; } sp;
            sp.magic = 0x48534C43; sp.seq = n; sp.n = (uint32_t)(g_shot_out_n - i < 16 ? g_shot_out_n - i : 16);
            memcpy(sp.r, g_shot_out + i, sp.n * sizeof(ShotRay));
            sendto(g_coop_sock, (const char *)&sp, (int)(12 + sp.n * sizeof(ShotRay)), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            g_shot_stats[0] += sp.n;
        }
        g_shot_out_n = 0;
        if (g_cfg.coop_role == 1) coop_enemy_send(obj, n);   // ENEMY SYNC: host streams enemy state
        for (int i = 0; i < g_enray_out_n; i += 32) {        // PHASE 3: this frame's enemy bullet rays
            struct { uint32_t magic, seq, n; EnemyRay e[32]; } ep;
            ep.magic = 0x52454C43; ep.seq = n; ep.n = (uint32_t)(g_enray_out_n - i < 32 ? g_enray_out_n - i : 32);
            memcpy(ep.e, g_enray_out + i, ep.n * sizeof(EnemyRay));
            sendto(g_coop_sock, (const char *)&ep, (int)(12 + ep.n * sizeof(EnemyRay)), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            g_enray_stats[0] += ep.n;
        }
        g_enray_out_n = 0;
        if (g_hit_out_n) {                                   // PHASE 3: host-decided hits on the joiner
            struct { uint32_t magic, seq, n; HitMsg h[16]; } hp;
            hp.magic = 0x54484C43; hp.seq = n; hp.n = (uint32_t)g_hit_out_n;
            memcpy(hp.h, g_hit_out, g_hit_out_n * sizeof(HitMsg));
            sendto(g_coop_sock, (const char *)&hp, (int)(12 + hp.n * sizeof(HitMsg)), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            g_hit_stats[0] += hp.n;
            g_hit_out_n = 0;
        }
        if (g_ex_out_n) {                                    // CLEX: this frame's explosions (both roles)
            struct { uint32_t magic, seq, n; ExMsg x[4]; } ep2;
            ep2.magic = 0x58454C43; ep2.seq = n; ep2.n = (uint32_t)g_ex_out_n;
            memcpy(ep2.x, g_ex_out, g_ex_out_n * sizeof(ExMsg));
            sendto(g_coop_sock, (const char *)&ep2, (int)(12 + ep2.n * sizeof(ExMsg)), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            g_ex_stats[0] += ep2.n;
            g_ex_out_n = 0;
        }
        if (g_cfg.coop_role == 2 && g_trig_req_n) {          // CLTQ: the joiner's trigger requests (sent 3 times: UDP)
            struct { uint32_t magic, seq, n; uint16_t id[256]; } rq;
            rq.magic = 0x51544C43; rq.seq = n; rq.n = (uint32_t)g_trig_req_n | ((g_cp_sec & 0xFFFF) << 16);
            memcpy(rq.id, g_trig_req, g_trig_req_n * 2);
            for (int r3 = 0; r3 < 3; r3++)
                sendto(g_coop_sock, (const char *)&rq, 12 + g_trig_req_n * 2, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            g_trig_req_n = 0;
        }
        if (g_cfg.coop_role == 1 && g_sp_sync == 1 && g_sp_n) {       // CLSP: the host's spawn counts per spawner (every 6th frame)
            static uint32_t sf;
            if ((sf++ % 6) == 3) {
                struct { uint32_t magic, seq, n; uint8_t cnt[256]; } sp2;
                sp2.magic = 0x50534C43; sp2.seq = n; sp2.n = (uint32_t)g_sp_n | ((g_cp_sec & 0xFFFF) << 16);
                memcpy(sp2.cnt, g_sp_cnt, g_sp_n);
                sendto(g_coop_sock, (const char *)&sp2, 12 + g_sp_n, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            }
        }
        if (g_cfg.coop_role == 1 && g_trig_sync == 1 && g_trig_n) {   // CLTG: the host's trigger activation counts (every 6th frame)
            static uint32_t tf;
            if ((tf++ % 6) == 0) {
                struct { uint32_t magic, seq, n; uint16_t cnt[512]; } tp;
                tp.magic = 0x47544C43; tp.seq = n; tp.n = (uint32_t)g_trig_n | ((g_cp_sec & 0xFFFF) << 16);
                memcpy(tp.cnt, g_trig_cnt, g_trig_n * 2);
                sendto(g_coop_sock, (const char *)&tp, 12 + g_trig_n * 2, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            }
        }
        if (g_xd_out_n) {                                    // explosion / melee hits on the joiner
            struct { uint32_t magic, seq, n; XdMsg x[4]; } xp;
            xp.magic = 0x44584C43; xp.seq = n; xp.n = (uint32_t)g_xd_out_n;   // "CLXD"
            memcpy(xp.x, g_xd_out, g_xd_out_n * sizeof(XdMsg));
            sendto(g_coop_sock, (const char *)&xp, (int)(12 + xp.n * sizeof(XdMsg)), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
            g_xd_out_n = 0;
        }
        static uint32_t erl;
        if ((erl++ % 300) == 0 && (g_enray_stats[0] | g_enray_stats[1]))
            logf("coop: enemy ray: sent %u received %u used by puppets %u | puppet pellets without a host ray %u",
                 g_enray_stats[0], g_enray_stats[1], g_enray_stats[2], g_enray_stats[3]);
        static uint32_t sl;
        if ((sl++ % 120) == 0 && (g_shot_stats[0] | g_shot_stats[1]))
            logf("coop: hit sync: rays sent %u received %u used by clone pellets %u (extra replay shots %u) dropped stale %u | joiner hits on enemies %u (damage %.0f)",
                 g_shot_stats[0], g_shot_stats[1], g_shot_stats[2], g_shot_stats[4], g_shot_stats[3], g_jhit_n, g_jhit_dmg);
    }
    g_shot_out_n = 0;   // solo / no socket: never accumulate
    g_enray_out_n = 0;
    g_hit_out_n = 0;
    g_xd_out_n = 0;
    g_ex_out_n = 0;
}

// CROSSHAIR POINT (owner: "aim at crosshair point"). The clone's gun sits ~0.5 m below its
// player's eye; aimed PARALLEL to the view it lands low at short range. Each sender now
// traces its own crosshair ray with the game's line test FUN_001DAFF0 (port NOTES: thiscall,
// ECX = end vec4, EAX = start vec4; stack: world *0x2D45D4, mask, entity to ignore, flag,
// out* (+0 hit point, +0x10 normal, +0x20 fraction); mask 0x21 = the game's own LOS/bullet
// tests) and sends the hit point; the clone converges its gun on it.
volatile uint32_t g_rc_fn = 0x001DAFF0, g_rc_end, g_rc_start, g_rc_world, g_rc_mask, g_rc_ign, g_rc_flag, g_rc_out, g_rc_esp;
volatile uint8_t g_rc_ret;
__declspec(naked) void coop_thunk_raycast()
{
    __asm {
        pushad
        mov dword ptr [g_rc_esp], esp
        push dword ptr [g_rc_out]
        push dword ptr [g_rc_flag]
        push dword ptr [g_rc_ign]
        push dword ptr [g_rc_mask]
        push dword ptr [g_rc_world]
        mov ecx, dword ptr [g_rc_end]
        mov eax, dword ptr [g_rc_start]
        call dword ptr [g_rc_fn]
        mov byte ptr [g_rc_ret], al
        mov esp, dword ptr [g_rc_esp]
        popad
        ret
    }
}
// trace from obj's eye along its look; out = the point the crosshair sits on (or 120 m out)
static bool coop_crosshair_point(uint32_t obj, float out[3], float *dist)
{
    if (obj < 0x80000000 || obj >= 0x84000000) return false;
    uint32_t world = *(volatile uint32_t *)(uintptr_t)0x2D45D4;
    if (world < 0x80000000 || world >= 0x84000000) return false;
    __declspec(align(16)) static float a[4], b[4], res[16];
    const volatile float *eye = (const volatile float *)(uintptr_t)(obj + 0x100), *F = (const volatile float *)(uintptr_t)(obj + 0xF0);
    const float R = 300.0f;
    for (int k = 0; k < 3; k++) { a[k] = eye[k]; b[k] = eye[k] + F[k] * R; }
    a[3] = b[3] = 1.0f;
    memset(res, 0, sizeof res); res[8] = 2.0f;
    g_rc_world = world; g_rc_mask = 0x21; g_rc_ign = obj; g_rc_flag = 1;
    g_rc_start = (uint32_t)(uintptr_t)a; g_rc_end = (uint32_t)(uintptr_t)b; g_rc_out = (uint32_t)(uintptr_t)res;
    g_rc_ret = 0;
    coop_thunk_raycast();
    float fr = res[8];
    if (g_rc_ret && fr >= 0.0f && fr <= 1.0f) {
        for (int k = 0; k < 3; k++) out[k] = res[k];
        *dist = fr * R;
    } else {
        for (int k = 0; k < 3; k++) out[k] = eye[k] + F[k] * 120.0f;
        *dist = 120.0f;
    }
    return true;
}
volatile float g_hand_world[3]; volatile int g_hand_ok; volatile uint64_t g_hand_ms;   // the clone's gun hand (world), set after each bend
// clone side: the remote crosshair point -> yaw/pitch from the clone's gun height
static bool coop_converged_aim(float *yaw, float *pitch)
{
    static int off = -1;
    if (off < 0) { char e[8] = {0}; off = GetEnvironmentVariableA("COOP_AIM_POINT", e, sizeof e) && e[0] == '0'; }
    if (off || g_coop_host_pawnmat[7] != 1.0f || !g_coop_host_ms || GetTickCount64() - g_coop_host_ms > 400) return false;
    uint32_t c = g_p10_gdst;
    if (c < 0x80000000 || c >= 0x84000000) return false;
    const volatile float *feet = (const volatile float *)(uintptr_t)(c + 0xA0), *F = (const volatile float *)(uintptr_t)(c + 0x90);
    // gun origin ~ chest front: feet + 1.35 m up + 0.3 m along the body facing
    float o[3] = {feet[0] + 0.3f * F[0], feet[1] + 1.35f, feet[2] + 0.3f * F[2]};
    // play 22:32 (owner: "the rifle aim is still off"): the rifle hand sits ~0.5 m to the
    // SIDE of that centre line (bone 24 model x -0.48) -> the barrel ran parallel to the
    // crosshair ray, half a metre beside it (2-4 deg at the 8-17 m seen in play). Use the
    // clone's real hand (last bend's bone 24, world) when it is fresh and near the body.
    if (g_hand_ok && GetTickCount64() - g_hand_ms < 200) {
        float hx = g_hand_world[0] - feet[0], hz = g_hand_world[2] - feet[2];
        if (hx * hx + hz * hz < 1.5f * 1.5f) for (int k = 0; k < 3; k++) o[k] = g_hand_world[k];
    }
    float dx = g_coop_host_pawnmat[8] - o[0], dy = g_coop_host_pawnmat[9] - o[1], dz = g_coop_host_pawnmat[10] - o[2];
    float h = sqrtf(dx * dx + dz * dz);
    if (h * h + dy * dy < 2.25f) return false;   // closer than 1.5 m: the plain look is better
    *yaw = 57.2957795f * atan2f(dx, dz);
    *pitch = 57.2957795f * atan2f(-dy, h);       // + = down (link convention)
    return true;
}
// The marker's position on the guest's screen, in fractions of the picture rect.
// false = stale packet, off-camera or too close. Runs on the game thread (Swap).
bool coop_marker_screen(float out[2])
{
    if (GetTickCount64() - g_coop_host_ms > 400) return false;
    return coop_project_point((const float *)g_coop_host_pos, out);
}

// Generic: project a world point through the guest's own camera (fractions of the picture
// rect); false when behind the camera or outside it.
bool coop_project_point(const float wpos[3], float out[2])
{
    float hx = wpos[0], hy = wpos[1], hz = wpos[2];
    uint32_t ctrl = g_coop_last_ctrl;
    if (ctrl < 0x10000 || ctrl >= 0x84000000) return false;
    uint32_t obj = *(const uint32_t *)(uintptr_t)(ctrl + 0x8C);
    if (obj < 0x10000 || obj >= 0x84000000) return false;
    const float *eye = (const float *)(uintptr_t)(obj + 0x100);
    const float *L = (const float *)(uintptr_t)(obj + 0xD0);
    const float *U = (const float *)(uintptr_t)(obj + 0xE0);
    const float *F = (const float *)(uintptr_t)(obj + 0xF0);
    float dx = hx - eye[0], dy = hy - eye[1], dz = hz - eye[2];
    float depth = dx * F[0] + dy * F[1] + dz * F[2];
    if (depth < 0.3f) return false;

    // The fov input is horizontal degrees; 70 = the game's 4:3 default, 86 = the game's
    // widescreen equivalent. Widescreen mode is on by default.
    float adjusted_fov = g_cfg.widescreen && g_cfg.fov <= 70.0f ? 86.0f : g_cfg.fov;
    float hTan = tanf(adjusted_fov * 3.14159265f / 360.0f);
    float vTan = hTan * 0.75f; // the 4:3 vertical extent (calibratable from screenshots)
    float sx = (dx * L[0] + dy * L[1] + dz * L[2]) / (depth * 2 * hTan);
    float sy = (dx * U[0] + dy * U[1] + dz * U[2]) / (depth * 2 * vTan);
    if (sx < -1.05f || sx > 1.05f || sy < -1.05f || sy > 1.05f) return false;
    out[0] = 0.5f + sx * 0.5f;
    out[1] = 0.5f - sy * 0.5f;
    return true;
}

// ---- Stage 2 capture A body: the spawn journal. ----
extern "C" void coop_log_spawn(uint32_t edi, const float *pos, uint32_t args_base)
{
    uint32_t n = ++g_spawn_calls;
    if (n > 40) return;
    if (!g_cfg.coop_log || !(n <= 10 || n % 10 == 0)) return;
    uint32_t args[7];
    for (int i = 0; i < 7; i++) args[i] = *(const uint32_t *)(uintptr_t)(args_base + 4 * i);
    logf("coop: spawn #%u mgr %08x pos %+.4g %+.4g %+.4g | args %08x %08x %08x %08x %08x %08x %08x",
         n, edi, pos[0], pos[1], pos[2], args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
    for (int i = 0; i < 7; i++) {
        if (args[i] >= 0x10000 && args[i] < 0x84000000) {
            uint32_t w[4];
            memcpy(w, (const void *)(uintptr_t)args[i], 16);
            logf("coop:   arg%d %08x: %08x %08x %08x %08x", i, args[i], w[0], w[1], w[2], w[3]);
        }
    }
}

// ---- Stage 2 capture B bodies: the level-load driver + the loop-tail injection (clone). ----
extern "C" void coop_level_prepare(uint32_t obj);
// Pilot 9: the model-spawn chain (verbatim from FUN_001ea0d0). Globals per call.
extern "C" volatile uint32_t g_s2_ret;   // the callee's return value
volatile uint32_t g_s2_ret;
extern "C" volatile float g_puppet_pos[3];
volatile float g_puppet_pos[3] = {0, 0, 0};
extern "C" volatile uint32_t g_puppet_life;
volatile uint32_t g_puppet_life = 0;      // GetTickCount64 of the last fresh position

// __fastcall: ECX = arg1 (the pool). Returns the free slot id.
__declspec(naked) void stage2_slotpick()
{
    __asm {
        pushad
        mov ecx, dword ptr [g_s2_a1]
        call dword ptr [g_s2_fn]
        mov dword ptr [g_s2_ret], eax
        popad
        retn
    }
}

// __thiscall-family: ECX = the level object (this), EAX = the source unit,
// stack (slot, 0, mgrCell) — replicating FUN_001ea0d0's exact call sequence.
__declspec(naked) void stage2_fxspawn()
{
    __asm {
        pushad
        mov eax, dword ptr [g_s2_a3]        // slot
        mov ecx, dword ptr [0x002d45d4]     // *world-cell
        add ecx, 05afch                     // the model-manager cell
        push eax                            // arg3 (pushed first = the slot)
        push 0                              // arg2
        push ecx                            // arg1 = mgrCell
        mov ecx, dword ptr [g_s2_a1]        // this = the level object
        mov eax, dword ptr [g_s2_a2]        // EAX = the source unit
        call dword ptr [g_s2_fn]            // FUN_001e4740 (callee cleans the 3 args)
        mov dword ptr [g_s2_ret], eax
        popad
        retn
    }
}

// Runs once at level tail (game thread, inside FUN_001eb440's own context): the three
// verbatim calls that produce a fresh rendered entity carrying unit0's model.
extern "C" void coop_level_tail_read(uint32_t obj)
{
    if (!g_coop_clone) return;
    static bool done;
    if (done) return;
    if (obj < 0x10000 || obj >= 0x84000000) return;
    uint32_t cnt = *(volatile uint32_t *)(uintptr_t)(obj + 0x30);
    uint32_t arrA = *(volatile uint32_t *)(uintptr_t)(obj + 0x18);
    if (cnt < 2 || arrA < 0x10000 || arrA >= 0x84000000) return;
    done = true;

    // The fresh-construct clone = the last slot (unit0's raw copy, appended by
    // coop_level_prepare before the loop). The tick (FUN_001e4050) requires:
    //   clone+0x114 -> model block with +0x40 == 0x1342cfb1 && +0x44 == 0x5b408352,
    //   and clone+0xB4 != 0. Diff the clone's model fields against unit0's, and
    //   apply the one-line fix (share unit0's model pointer) where they differ.
    uint32_t real0 = arrA;
    uint32_t clone = arrA + (cnt - 1) * 0x1D0;
    if (clone >= 0x84000000) return;

    logf("coop: diff: unit0 %08x clone(tail) %08x (cnt %u)", real0, clone, cnt);
    uint32_t mr = *(volatile uint32_t *)(uintptr_t)(real0 + 0x114);
    uint32_t mc = *(volatile uint32_t *)(uintptr_t)(clone + 0x114);
    uint32_t b4r = *(volatile uint32_t *)(uintptr_t)(real0 + 0xB4);
    uint32_t b4c = *(volatile uint32_t *)(uintptr_t)(clone + 0xB4);
    logf("coop: diff: unit0 +0x114 %08x +0xB4 %08x | clone +0x114 %08x +0xB4 %08x", mr, b4r, mc, b4c);

    if (mr >= 0x10000 && mr < 0x84000000) {
        uint32_t h1r = *(volatile uint32_t *)(uintptr_t)(mr + 0x40);
        uint32_t h2r = *(volatile uint32_t *)(uintptr_t)(mr + 0x44);
        logf("coop: diff: unit0 model hashes %08x %08x", h1r, h2r);
        if (mc >= 0x10000 && mc < 0x84000000) {
            uint32_t h1c = *(volatile uint32_t *)(uintptr_t)(mc + 0x40);
            uint32_t h2c = *(volatile uint32_t *)(uintptr_t)(mc + 0x44);
            logf("coop: diff: clone model hashes %08x %08x", h1c, h2c);
            if (h1c != h1r || h2c != h2r) {
                logf("coop: diff: clone model hash differs -> ensuring the raw load contract");
                // The fresh-construct loop already gave the clone its own pointer; the
                // definitive fields for the tick are carried through +0x110 and +0x114
                // (the game's own clone pattern): mirror unit0's chain explicitly.
                *(volatile uint32_t *)(uintptr_t)(clone + 0x110) = *(volatile uint32_t *)(uintptr_t)(real0 + 0x110);
                *(volatile uint32_t *)(uintptr_t)(clone + 0x114) = mr;
                *(volatile uint32_t *)(uintptr_t)(clone + 0x118) = *(volatile uint32_t *)(uintptr_t)(real0 + 0x118);
                logf("coop: diff: fix applied: clone model ptrs <- unit0");
            } else {
                logf("coop: diff: clone model hashes already match (no fix needed)");
            }
        } else {
            logf("coop: diff: clone model block invalid (%08x) -> fix applied", mc);
            *(volatile uint32_t *)(uintptr_t)(clone + 0x110) = *(volatile uint32_t *)(uintptr_t)(real0 + 0x110);
            *(volatile uint32_t *)(uintptr_t)(clone + 0x114) = mr;
            *(volatile uint32_t *)(uintptr_t)(clone + 0x118) = *(volatile uint32_t *)(uintptr_t)(real0 + 0x118);
        }
        if (b4c == 0 && b4r != 0) {
            *(volatile uint32_t *)(uintptr_t)(clone + 0xB4) = b4r;
            logf("coop: diff: clone +0xB4 was 0 -> mirrored unit0's %08x", b4r);
        }
        // The visible flag: the WORD at +0x13C = 0x0100 (the game's own pattern — set at
        // load by 0x001d931a on every real unit, re-kept per frame by FUN_001be850).
        // The fresh-construct clone copies 0 here, hence invisible; hence this write:
        *(volatile uint16_t *)(uintptr_t)(clone + 0x13C) = 0x0100;
        logf("coop: diff: visibility fix applied: clone +0x13C word = 0x0100");

        // PILOT 10 v3: the clone moved to a lazily-gated per-frame attempt
        // (coop_pilot10_try): at the level tail the external group slot array was
        // measured all-null this run, so the enemy bodies spawn later, mid-play.
        // Mildly throttled wait logs prove liveness; the first non-null live slot
        // triggers the one-shot clone + registration.
        coop_pilot10_try();

        // Re-offset the position AFTER construction (the ctor rewrote the raw +0x20):
        // step the clone 2.5 m along x and z inside unit0's own grid cell (the earlier
        // 8 m cross-cell write was the crash; in-cell offsets stay safe).
        float ux = *(volatile float *)(uintptr_t)(real0 + 0x20);
        float cy = *(volatile float *)(uintptr_t)(real0 + 0x24);
        float uz = *(volatile float *)(uintptr_t)(real0 + 0x28);
        // The +0x70..0xAC block = a column-major 4x4 whose translation column (floats
        // 12..14 = +0xA0!) equals the +0xA0 smoothed position. Copy unit0's rotation
        // intact, write only the translation, plus the record's +0x20 trio.
        memcpy((void *)(uintptr_t)(clone + 0x70), (const void *)(uintptr_t)(real0 + 0x70), 0x30);
        *(volatile float *)(uintptr_t)(clone + 0xA0) = ux + 2.5f;
        *(volatile float *)(uintptr_t)(clone + 0xA4) = cy;
        *(volatile float *)(uintptr_t)(clone + 0xA8) = uz + 2.5f;
        *(volatile float *)(uintptr_t)(clone + 0x20) = ux + 2.5f;
        *(volatile float *)(uintptr_t)(clone + 0x24) = cy;
        *(volatile float *)(uintptr_t)(clone + 0x28) = uz + 2.5f;
        logf("coop: diff: clone position re-offset to %.2f %.2f %.2f", ux + 2.5f, cy, uz + 2.5f);
    }
    g_coop_clone_pos[0] = (float)(clone + 0x20 ?
        *(volatile float *)(uintptr_t)(clone + 0x20) : 0);
    g_coop_clone_pos[1] = *(volatile float *)(uintptr_t)(clone + 0x24);
    g_coop_clone_pos[2] = *(volatile float *)(uintptr_t)(clone + 0x28);
    g_coop_clone_on = 1;
    logf("coop: diff: clone pos tracked at %.2f %.2f %.2f", g_coop_clone_pos[0],
         g_coop_clone_pos[1], g_coop_clone_pos[2]);
    g_clone_rec = clone;
    g_real_rec = real0;
}
extern "C" void coop_log_level_arg(uint32_t obj)
{
    if (!g_cfg.coop_log) return;
    g_level_obj = obj;
    g_level_frame = g_frame_count.load();
    uint32_t w[16] = {};
    if (obj >= 0x10000 && obj < 0x84000000) memcpy(w, (const void *)(uintptr_t)obj, 64);
    logf("coop: level part obj %08x: p0 %x p1 %08x p2 %08x p3 %08x p4 %08x p5 %08x p6 %08x p7 %08x",
         obj, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    logf("coop:   p8 %08x p9 %08x p10 %08x p11 %08x | unit count p12 %d p13 %d p14 %d p15 %d",
         w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
}

// Pilot 6: grows array A by one BEFORE the loader's original body runs (called from
// level_thunk) and appends a RAW clone of unit[0]. The game's own loop then constructs,
// ids, queues and registers it with its own fresh child nodes and full register
// context — no synthetic finisher needed.
// a new level: forget the previous level's clone (its body, private link and first-look state are
// stale addresses there); the chained factory builds a fresh one at this level's spawn #4
static void coop_clone_state_reset(void)
{
    g_own_brains_n = 0;   // a new level: our own spawn pops new brains
    g_p10_done = 0; g_p10_gdst = 0; g_p10_tries = 0;
    g_coop_self_body = 0; g_self_link = 0; g_self_link_src = 0;
    g_aim_link = 0; g_aim_link_src = 0; g_aim_shared_checked = 0; g_aim_shared_with = 0;
    g_coop_clone_on = 0;
    memset(g_clone_w, 0, sizeof g_clone_w);
}
// SQUAD DONOR (lobby step 4, part 3): the squad soldier is character slot 6 (the character manager *0x2D12C8, slots at
// +0xF8, 0x60 each; slot +0xC = the loaded character, 0 = not in this level). Its data comes with the level's
// Stg_0001\StLevel.bin, which levels 01 / 08 (LevelSelect slots 1 / 7) do not carry. A level without it gets a DONOR
// level's StLevel.bin: read into contiguous memory we own, relocated by the game's own FUN_000B6030 (EBX = the
// buffer; what the StLevel load callback LAB_001D3830 runs), then registered the way a streamed unit is:
// FUN_001C4C30(*0x2D1980, buffer +0xC, 0) - the ADDITIVE mode: the level package stays, only characters the level
// lacks fill their slots (FUN_0017BD00). The level's FUN_00144FC0 (speech / AI names of the MAIN package) is not run.
// Env COOP_SQUAD_DONOR=<level folder> (default 0 = Veblensk; -1 = off); only with COOP_CLONE_SLOT=6. Once per level load.
// COOP_SQUAD_DONOR_FORCE=1 (test): load it even where the level has slot 6.
// the clone's one look: the masked squad operative = squad type index 0x1E (Veblensk's male operative; owner, day 15).
// Env COOP_SQUAD_TYPE_INDEX=<n> picks another. Levels carry other squad types (Naszran town: only 0x1F) -> the
// donor supplies 0x1E where the level lacks it.
static int coop_squad_want()
{
    static int want = -2;
    if (want == -2) { char e[8] = {0}; want = GetEnvironmentVariableA("COOP_SQUAD_TYPE_INDEX", e, sizeof e) ? (int)strtoul(e, nullptr, 0) : 0x1E; }
    return want;
}
static bool g_donor_tried;
static uint8_t *g_donor_buf;
static uint32_t g_donor_cap;
static bool g_donor_used;   // this level registered the donor (the clone's type record may point into it)
static uint32_t coop_char_slot(int i)
{
    uint32_t cm = *(volatile uint32_t *)(uintptr_t)0x002D12C8;
    if (cm < 0x80000000 || cm >= 0x84000000) return 0;
    return cm + 0xF8 + i * 0x60;
}
static void coop_call_reloc(uint32_t buf)
{
    __asm {
        pushad
        mov ebx, buf
        mov eax, 0x000B6030
        call eax
        popad
    }
}
static void coop_call_register(uint32_t mgr, uint32_t pkg)
{
    __asm {
        pushad
        mov esi, esp
        push 0
        push pkg
        push mgr
        mov eax, 0x001C4C30
        call eax
        mov esp, esi
        popad
    }
}
static void coop_squad_donor_step()
{
    if (g_donor_tried) return;
    uint32_t sl = coop_char_slot(6);
    uint32_t lm = *(volatile uint32_t *)(uintptr_t)0x002D1980;
    if (!sl || lm < 0x80000000 || lm >= 0x84000000) return;
    uint32_t pkg = *(volatile uint32_t *)(uintptr_t)(lm + 0x8528);
    if (pkg < 0x80000000) return;   // the level package is not registered yet
    g_donor_tried = true;
    static int donor = -2;
    if (donor == -2) {
        char e[8] = {0};
        donor = GetEnvironmentVariableA("COOP_SQUAD_DONOR", e, sizeof e) ? atoi(e) : 0;
        char c[8] = {0};   // prototype: only with the squad look asked for (COOP_CLONE_SLOT=6) - normal play is untouched
        if (!coop_own_spawn_on() && !(GetEnvironmentVariableA("COOP_CLONE_SLOT", c, sizeof c) && atoi(c) == 6)) donor = -1;
    }
    uint32_t have = *(volatile uint32_t *)(uintptr_t)(sl + 0xC);
    static int force = -1;   // test: load the donor even here (its type resources then serve the clone - mission 1's case)
    if (force < 0) { char e[8] = {0}; force = GetEnvironmentVariableA("COOP_SQUAD_DONOR_FORCE", e, sizeof e) && e[0] == '1'; }
    uint32_t want = (uint32_t)coop_squad_want(), tw0 = 0;
    if (want < 0x2C) tw0 = *(volatile uint32_t *)(uintptr_t)(lm + 0x7A20 + want * 0x40);
    // STALE SQUAD (two-PC play 20261115 #2, "launching another mission doesn't draw clones"): mission 0 has the squad
    // natively; the game leaves its slot-6 character and its type-1E record in place when the next level lacks them.
    // Mission 0 -> mission 1: both looked present, the donor was skipped, and the type record's resource pointed into
    // the freed mission-0 memory (block fd36942a len 182859657) -> own spawn waited for ever, no clone on either copy.
    // A type whose resource block is unreadable is left over from an earlier mission: flush slot 6 and the type entry
    // so the donor's registration fills them again (as on a first mission-1 load, where both were empty).
    if (have && tw0 && !force) {
        uint32_t blk = 0, len = 0, probe = 0;
        bool live = tw0 >= 0x80000000 && tw0 < 0x84000000 && coop_safe_copy(&blk, tw0 + 0x38, 4) && coop_safe_copy(&len, tw0 + 0x3C, 4) &&
                    len < 0x10000 && (len == 0 || (blk >= 0x80000000 && blk < 0x84000000 && coop_safe_copy(&probe, blk, 4)));
        if (!live) {
            logf("coop: squad donor: slot 6 (%08x) and type %02x (resource %08x block %08x len %u) are LEFT OVER from an earlier mission - flushing them, loading the donor",
                 have, want, tw0, blk, len);
            *(volatile uint32_t *)(uintptr_t)(sl + 0xC) = 0;
            for (uint32_t i = 0; i < 0x2C; i++) {   // every squad (slot 6) type with a dead resource (0x1D too)
                uint32_t t = lm + 0x7A20 + i * 0x40, r = *(volatile uint32_t *)(uintptr_t)t, b2 = 0, l2 = 0, p2 = 0;
                if (!r || *(volatile uint32_t *)(uintptr_t)(t + 8) != 6) continue;
                bool ok2 = r >= 0x80000000 && r < 0x84000000 && coop_safe_copy(&b2, r + 0x38, 4) && coop_safe_copy(&l2, r + 0x3C, 4) &&
                           l2 < 0x10000 && (l2 == 0 || (b2 >= 0x80000000 && b2 < 0x84000000 && coop_safe_copy(&p2, b2, 4)));
                if (!ok2) { *(volatile uint32_t *)(uintptr_t)t = 0; logf("coop: squad donor: type %02x flushed", i); }
            }
            have = 0; tw0 = 0;
        }
    }
    if (have && tw0 && !force) { logf("coop: squad donor: this level has the squad character (slot 6 = %08x) and type %02x - nothing to load", have, want); return; }
    if (have && !tw0) logf("coop: squad donor: slot 6 is here (%08x) but squad type %02x is not - loading it from the donor", have, want);
    if (donor < 0) { logf("coop: squad donor: slot 6 empty, donor off (COOP_SQUAD_DONOR=-1)"); return; }
    wchar_t path[600];
    swprintf(path, 600, L"%s\\levels\\level_%02d\\stg_0001\\StLevel.bin", g_cfg.game_dir.c_str(), donor);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { logf("coop: squad donor: cannot open level_%02d StLevel.bin (%lu)", donor, GetLastError()); return; }
    DWORD sz = GetFileSize(f, nullptr);
    if (sz > g_donor_cap) {
        if (g_donor_buf) xk::MmFreeContiguousMemory(g_donor_buf);
        uint32_t want = (sz + 0xFFF) & ~0xFFFu;
        g_donor_buf = (uint8_t *)xk::MmAllocateContiguousMemoryEx(want, 0, 0xFFFFFFFF, 0x1000, PAGE_READWRITE);
        g_donor_cap = g_donor_buf ? want : 0;
    }
    DWORD got = 0;
    bool ok = g_donor_buf && ReadFile(f, g_donor_buf, sz, &got, nullptr) && got == sz;
    CloseHandle(f);
    if (!ok) { logf("coop: squad donor: allocation / read failed (size %lu, buffer %p)", sz, g_donor_buf); return; }
    uint32_t b = (uint32_t)(uintptr_t)g_donor_buf;
    logf("coop: squad donor: level_%02d StLevel.bin (%lu B) read at %08x - relocating", donor, sz, b);
    auto dump_dicts = [](const char *when) {
        uint32_t g = *(volatile uint32_t *)(uintptr_t)0x002D45D8, t[22] = {0};
        if (g >= 0x80000000 && g < 0x84000000) { coop_safe_copy(t, g + 0xC64, 44); coop_safe_copy(t + 11, g + 0xC90, 44); }
        logf("coop: squad donor: dictionaries %s: C64 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x | C90 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
             when, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15], t[16], t[17], t[18], t[19], t[20], t[21]);
    };
    dump_dicts("before");
    // TEXTURES: the relocation binds each donor material to its texture BY NAME (FUN_001FBCE0 over the dictionary
    // table *0x2D45D8 +0xC64, 11 entries) - but a dictionary is only searched when its address lies in the current
    // memory region (the region table at 0x2D12F4, 9 dwords per region; current = DAT_002D1654) or in region 1, or
    // when the current region is 0xD / 0xE. Our buffer is in no region -> its textures were never found and the
    // squad drew with other textures (stripes, mission 1). During the relocation: region 0xD (search everything)
    // and the table holds only the region-1 (global) dictionaries, so the donor's own textures win; afterwards the
    // level's table comes back with the donor's dictionaries appended.
    uint32_t gd = *(volatile uint32_t *)(uintptr_t)0x002D45D8;
    volatile uint32_t *tab = (volatile uint32_t *)(uintptr_t)(gd + 0xC64);
    uint32_t saved[11], r1lo = *(volatile uint32_t *)(uintptr_t)(0x002D12F4 + 0x24), r1hi = *(volatile uint32_t *)(uintptr_t)(0x002D12F8 + 0x24);
    for (int i = 0; i < 11; i++) { saved[i] = tab[i]; tab[i] = (saved[i] >= r1lo && saved[i] < r1hi) ? saved[i] : 0; }
    uint32_t region = *(volatile uint32_t *)(uintptr_t)0x002D1654;
    *(volatile uint32_t *)(uintptr_t)0x002D1654 = 0xD;
    coop_call_reloc(b);
    *(volatile uint32_t *)(uintptr_t)0x002D1654 = region;
    uint32_t added[11]; int na = 0;
    for (int i = 0; i < 11; i++) {
        uint32_t v = tab[i];
        bool old = false;
        for (int k = 0; k < 11; k++) if (saved[k] && saved[k] == v) old = true;
        if (v && !old) added[na++] = v;
    }
    for (int i = 0; i < 11; i++) tab[i] = saved[i];
    for (int k = 0; k < na; k++) {
        int i = 0; while (i < 11 && tab[i]) i++;
        if (i < 11) tab[i] = added[k]; else logf("coop: squad donor: the dictionary table is full - donor dictionary %08x left out", added[k]);
    }
    logf("coop: squad donor: textures bound with region 0xD (was %u; region 1 = %08x..%08x); %d donor dictionaries appended", region, r1lo, r1hi, na);
    dump_dicts("after");
    uint32_t dpkg = *(volatile uint32_t *)(uintptr_t)(b + 0xC);
    logf("coop: squad donor: relocated, package %08x - registering it (additive, the unit path)", dpkg);
    coop_call_register(lm, dpkg);
    g_donor_used = true;
    uint32_t now = *(volatile uint32_t *)(uintptr_t)(sl + 0xC);
    logf("coop: squad donor: slot 6 -> %08x%s (level package still %08x)", now, now ? " - the squad character is loaded" : " - STILL EMPTY",
         *(volatile uint32_t *)(uintptr_t)(lm + 0x8528));
}

// COOP_CLONE_SLOT=<0..6>: the clone (and the self model) is built with that CHARACTER SLOT (the type record word +8 the
// constructor passes to FUN_0017E420) - a private copy of the request's type record (def[0]). 6 = the squad. Only
// when that slot is loaded (+0xC), else the request's own type.
static void coop_clone_type_override(uint32_t dc)
{
    static int want = -2;
    if (want == -2) { char e[8] = {0}; want = GetEnvironmentVariableA("COOP_CLONE_SLOT", e, sizeof e) ? atoi(e) : -1; }
    if (want < 0 || want > 6) return;
    uint32_t ty = *(volatile uint32_t *)(uintptr_t)dc, sl = coop_char_slot(want);
    if (ty < 0x80000000 || ty >= 0x84000000 || !sl || !*(volatile uint32_t *)(uintptr_t)(sl + 0xC)) {
        logf("coop: clone slot: slot %d not loaded (or type %08x bad) - the request's own type", want, ty);
        return;
    }
    uint32_t cp = (uint32_t)(uintptr_t)_aligned_malloc(0x100, 16);
    if (!cp) return;
    memset((void *)(uintptr_t)cp, 0, 0x100);
    // a live soldier of that slot (the squad in a squad level): its WHOLE type record (word 0 = a per-type resource;
    // the slot alone gave the squad body without the squad's helmet)
    uint32_t src_ty = 0;
    {
        uint32_t s16[16]; uint32_t ns = enemy_slots(s16);
        for (uint32_t k = 0; k < ns && !src_ty; k++) {
            uint32_t tk = 0, w2 = 0xFFFFFFFF;
            if (!coop_safe_copy(&tk, s16[k] + 0x320, 4) || tk < 0x80000000 || tk >= 0x84000000) continue;
            if (coop_safe_copy(&w2, tk + 8, 4) && w2 == (uint32_t)want) src_ty = tk;
        }
    }
    uint32_t lvl = *(volatile uint32_t *)(uintptr_t)0x002D45D4, stbuf = 0;
    if (lvl >= 0x80000000 && lvl < 0x84000000) coop_safe_copy(&stbuf, lvl + 0x5B30, 4);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t t = pass ? src_ty : ty, w[16] = {0};
        if (!t) continue;
        coop_safe_copy(w, t, 0x40);
        logf("coop: clone slot: %s type %08x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x | word0 - StLevel buffer %08x = %08x",
             pass ? "squad" : "request", t, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15],
             stbuf, w[0] - stbuf);
    }
    coop_safe_copy((void *)(uintptr_t)cp, src_ty ? src_ty : ty, 0x40);
    if (!src_ty && g_donor_buf && g_donor_used) {
        // no squad soldier here: words 0 and 14 of the squad type record point into level_00's StLevel.bin (mission 0
        // run: +0xD8800 / +0x78E00 of the level's buffer) -> the same offsets in the donor's copy of that file.
        // Env COOP_SQUAD_TYPE_OFF=<w0 hex>,<w14 hex> for another donor.
        static uint32_t o0 = 0xFFFFFFFF, o14 = 0;
        if (o0 == 0xFFFFFFFF) {
            o0 = 0xD8800; o14 = 0x78E00;
            char e[32] = {0};
            if (GetEnvironmentVariableA("COOP_SQUAD_TYPE_OFF", e, sizeof e)) sscanf(e, "%x,%x", &o0, &o14);
        }
        uint32_t b = (uint32_t)(uintptr_t)g_donor_buf;
        *(volatile uint32_t *)(uintptr_t)cp = b + o0;
        *(volatile uint32_t *)(uintptr_t)(cp + 0x38) = b + o14;
        logf("coop: clone slot: no squad soldier here - type words 0 / 14 = donor %08x + %x / + %x", b, o0, o14);
    }
    uint32_t old = *(volatile uint32_t *)(uintptr_t)(cp + 8);
    *(volatile uint32_t *)(uintptr_t)(cp + 8) = (uint32_t)want;
    *(volatile uint32_t *)(uintptr_t)dc = cp;
    // the request's word 0xF is the TEAM (constructor -> body +0x39C); team 1 makes the factory take the enemy's
    // appearance record (def[0x12]: +2C0 / +2C4 = the enemy head). A squad request is team 0 -> the default look
    // (+2C0 -1, +2C4 0x020204). The friendly chain already builds the clone on team 0.
    uint32_t team = *(volatile uint32_t *)(uintptr_t)(dc + 0x3C);
    *(volatile uint32_t *)(uintptr_t)(dc + 0x3C) = 0;
    logf("coop: clone slot: type %08x (slot %u) -> private copy %08x with slot %d; request team %u -> 0", ty, old, cp, want, team);
}

// OWN SPAWN (owner, day 15: "drop the 4th soldier spawn logic and spawn the clone with his own entity with the squad
// model"; mission 1 never reached a 4th spawn). The natural spawner FUN_001AF880 only needs things we can make:
//   - the BRAIN (the factory's arg 1, body +0x324) = popped off the free list *(*0x2D1988 + 0x22CDC), next at +0x1F00;
//   - the TYPE RECORD = *0x2D1980 + 0x7A20 + index * 0x40 (filled by FUN_001C4C30 from the level / donor package:
//     word 1 = index, word 2 = character slot). The squad = the entries with slot 6 (0x1D / 0x1E in Veblensk);
//   - the REQUEST (0x60 B): [0] type, [4..7] position + yaw, [8..9] the soldier's name hash (body +8 / +0xC),
//     [10..11] weapon id, [12] group (-1), byte [13] group slot (0xFF), [14] spawn counter, [15] TEAM (0 = ours),
//     [16] type index (+0x40, FUN_001D21F0), [17] +0xCAC, [18] appearance record (team 1 only), [20..21] patrol (0 =
//     none);
//   - after the call: body +0x10 / +0x14 (0 here), +0x31C the spawn point (none), +0x2F0 health.
// Both copies build it at the same moment: the first natural spawn's entry, or the first play tick when the level
// has spawned nobody yet. The squad's type comes from the level or the squad donor (level_00's StLevel.bin).
// Env COOP_OWN_SPAWN=0 = the old chain at spawn #4.
static bool coop_own_spawn_on()
{
    static int on = -1;
    if (on < 0) { char e[8] = {0}; on = !(GetEnvironmentVariableA("COOP_OWN_SPAWN", e, sizeof e) && e[0] == '0'); }
    return on && g_coop_clone && g_p13_on;
}
static uint32_t coop_pop_brain()
{
    uint32_t pool = *(volatile uint32_t *)(uintptr_t)0x002D1988;
    if (pool < 0x80000000 || pool >= 0x84000000) return 0;
    uint32_t o = *(volatile uint32_t *)(uintptr_t)(pool + 0x22CDC);
    if (o < 0x80000000 || o >= 0x84000000) return 0;
    *(volatile uint32_t *)(uintptr_t)(pool + 0x22CDC) = *(volatile uint32_t *)(uintptr_t)(o + 0x1F00);
    return o;
}
static uint32_t coop_squad_type(uint32_t *idx_out)
{
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x002D1980;
    if (mgr < 0x80000000 || mgr >= 0x84000000) return 0;
    // Veblensk has TWO squad types (slot 6): index 0x1D = the female operative, 0x1E = the male one (owner: the clone
    // came out female with the first match). Prefer 0x1E; env COOP_SQUAD_TYPE_INDEX=<n> picks another.
    int want = coop_squad_want();
    uint32_t fallback = 0, fidx = 0, squad = 0, sidx = 0;
    char list[160]; int lo = 0; list[0] = 0;
    for (uint32_t i = 0; i < 0x2C; i++) {
        uint32_t t = mgr + 0x7A20 + i * 0x40;
        uint32_t w0 = *(volatile uint32_t *)(uintptr_t)t, w2 = *(volatile uint32_t *)(uintptr_t)(t + 8);
        if (!w0) continue;
        if (w2 == 6) {
            if (lo < 140) lo += snprintf(list + lo, sizeof list - lo, " %02x", i);
            if (!squad || (int)i == want) { squad = t; sidx = i; }
        }
        if (!fallback && w2 == 0) { fallback = t; fidx = i; }
    }
    if (squad) { logf("coop: own spawn: squad types (slot 6):%s -> index %02x (wanted %02x)", list, sidx, want); *idx_out = sidx; return squad; }
    *idx_out = fidx;
    return fallback;   // no squad type at all: the level's common soldier
}
static uint32_t coop_build_one(uint32_t type, uint32_t tidx, const float pos[3], float yawdeg, uint32_t tag)
{
    uint32_t brain = coop_pop_brain();
    if (brain && g_own_brains_n < 4) g_own_brains[g_own_brains_n++] = brain;
    if (!brain) { logf("coop: own spawn: no free brain"); return 0; }
    uint32_t d = (uint32_t)(uintptr_t)_aligned_malloc(0x400, 16);
    if (!d) return 0;
    memset((void *)(uintptr_t)d, 0, 0x400);
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)d;
    volatile float *f = (volatile float *)(uintptr_t)d;
    w[0] = type;
    f[4] = pos[0]; f[5] = pos[1]; f[6] = pos[2]; f[7] = yawdeg;
    w[8] = 0x504F4F43; w[9] = tag;               // "COOP" + tag: a name no script knows
    w[10] = 0x97C60000; w[11] = 0x54461272;       // the common soldier's rifle (every surveyed level spawns it)
    w[12] = 0xFFFFFFFF; w[13] = 0xFF;            // no group
    w[14] = 0; w[15] = 0; w[16] = tidx;
    g_fact_mgr = *(volatile uint32_t *)(uintptr_t)0x002D1980;
    g_fact_a1 = brain; g_fact_def = d; g_fact_ctx = 0; g_fact_out = 0;
    coop_thunk_factory();
    uint32_t b = g_fact_out;
    if (b < 0x80100000 || b >= 0x84000000) { logf("coop: own spawn: factory returned %08x (brain %08x)", b, brain); return 0; }
    *(volatile uint32_t *)(uintptr_t)(b + 0x10) = 0;
    *(volatile uint8_t *)(uintptr_t)(b + 0x14) = 0;
    *(volatile uint32_t *)(uintptr_t)(b + 0x31C) = 0;
    *(volatile uint32_t *)(uintptr_t)(b + 0x2F0) = *(volatile uint32_t *)(uintptr_t)0x295CEC;
    *(volatile uint8_t *)(uintptr_t)(b + 0x3A7) = 1;   // no fall physics: we drive its position
    uint32_t wp = 0, wpp = *(volatile uint32_t *)(uintptr_t)(b + 0x298);
    if (wpp >= 0x80000000 && wpp < 0x84000000) wp = *(volatile uint32_t *)(uintptr_t)wpp;
    logf("coop: own spawn: built %08x (brain %08x, type %08x index %u, team %u, weapon %08x)", b, brain, type, tidx,
         *(volatile uint32_t *)(uintptr_t)(b + 0x39C), wp);
    {   // level survey 20261114: missions 4 and 5 crashed on BOTH copies right after this build (AV 0x1A0086: FUN_001A01A0
        // -> FUN_001A0050(brain + 0x1EC0) read [[brain + 0x1ED0] + 0xC4] with +0x1ED0 = 0). FUN_001A01A0 takes that path
        // when the body's TEAM manager (*0x2D1988 + 0x22C40 + team * 4) is active (+0x20) and the brain has a GROUP
        // (+0x1EF0, the formation FUN_0018FC10 fills together with +0x1ED0). Mission 4 / 5 run a team-0 (squad)
        // formation; our hand-built brain carried a group without its +0x1ED0 link. A brain of ours is no formation
        // member: +0x1EF0 = 0 when +0x1ED0 is 0 (the path the game takes for an ungrouped soldier).
        uint32_t w[16] = {};
        coop_safe_copy(w, brain + 0x1EC0, 64);
        uint32_t tm = 0, tm20 = 0, team = *(volatile uint32_t *)(uintptr_t)(b + 0x39C);
        uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988;
        if (team < 8) coop_safe_copy(&tm, R + 0x22C40 + team * 4, 4);
        if (tm >= 0x80000000 && tm < 0x84000000) coop_safe_copy(&tm20, tm + 0x20, 4);
        logf("coop: own spawn: brain %08x +1EC0: %08x %08x %08x %08x | +1ED0 %08x +1EF0 %08x +1F00 %08x | team %u manager %08x +20 %08x",
             brain, w[0], w[1], w[2], w[3], w[4], w[12], *(volatile uint32_t *)(uintptr_t)(brain + 0x1F00), team, tm, tm20);
        {
            uint32_t m8c = 0, n8c = 0, nb = 0x8014F220; coop_safe_copy(&m8c, brain + 0x8C, 4); coop_safe_copy(&n8c, nb + 0x8C, 4);
            uint32_t sq[8] = {}; coop_safe_copy(sq, R + 0x22B40, 32);
            logf("coop: own spawn: brain %08x +8C %08x (body %08x) +CA8 %x +1F04 %u | brain %08x +8C %08x | squad formation +10 %08x +14 %u +20 %04x",
                 brain, m8c, b, *(volatile uint32_t *)(uintptr_t)(brain + 0xCA8), *(volatile uint32_t *)(uintptr_t)(brain + 0x1F04), nb, n8c, sq[4], sq[5], sq[0x20 / 4] & 0xFFFF);
        }
        {   // FUN_001A1820 (formation join): a brain of a SQUAD type (+0xCA8 0x1D / 0x1E / 0x1F) joins the SQUAD formation
            // (*0x2D1988 + 0x22B40), which in missions 4 / 5 follows the player (+0x10 = the player, kind 2): our two
            // extra members left a rank without a body to follow -> a null +0x1ED0 -> the crash. Our brains take a
            // non-squad type here (after the factory: the look is already built), so they join their team's own
            // formation (the manager's +0x30), led by our own clone. TRIED: still crashed (the team formation left the same gap) -
            // the FORMATION GUARD (FUN_001A01A0) is the fix; this stays as a test switch: COOP_BRAIN_TYPE=<hex> (default: keep).
            static int bt = -2;
            if (bt == -2) { char e[16] = {0}; bt = GetEnvironmentVariableA("COOP_BRAIN_TYPE", e, sizeof e) ? (strcmp(e, "keep") == 0 ? -1 : (int)strtol(e, nullptr, 16)) : -1; }   // default keep: the formation guard fixed it
            uint32_t ty = *(volatile uint32_t *)(uintptr_t)(brain + 0xCA8);
            if (bt >= 0 && (ty == 0x1D || ty == 0x1E || ty == 0x1F)) {
                *(volatile uint32_t *)(uintptr_t)(brain + 0xCA8) = (uint32_t)bt;
                logf("coop: own spawn: brain %08x type %x -> %x (no squad formation for our bodies)", brain, ty, bt);
            }
        }
        if (w[4] == 0 && w[12] != 0) {
            *(volatile uint32_t *)(uintptr_t)(brain + 0x1EF0) = 0;
            logf("coop: own spawn: brain %08x had a group (%08x) without its link - left out of the formation", brain, w[12]);
        }
    }
    return b;
}
static bool g_own_busy;   // our own factory calls re-enter the spawn hook (coop_log_pawn_def): no second build
static bool coop_spawn_own(const float *at, const char *where)
{
    if (g_own_busy || g_p10_done || !coop_own_spawn_on()) return false;
    struct Busy { Busy() { g_own_busy = true; } ~Busy() { g_own_busy = false; } } busy;
    static uint32_t fails;
    coop_squad_donor_step();   // a squad-less level gets the squad from the donor first
    uint32_t tidx = 0, type = coop_squad_type(&tidx);
    if (!type) { if (fails++ < 3) logf("coop: own spawn (%s): no type record yet", where); return false; }
    {   // two-PC play 20261115 (both copies crashed at 0x1C5C84, the 3rd level load of the session, the clone the level's
        // FIRST spawn): the body constructor (FUN_001DA4F0 -> FUN_001C5C60) copies [[type] + 0x3C] bytes from
        // [[type] + 0x38] - that pointer was garbage (fd36942a): the squad type record still held a previous level's
        // resource. Build only once the record's resource and its block are readable; until then wait (next tick).
        uint32_t res = 0, blk = 0, len = 0, probe = 0;
        bool ok = coop_safe_copy(&res, type, 4) && res >= 0x80000000 && res < 0x84000000 &&
                  coop_safe_copy(&blk, res + 0x38, 4) && coop_safe_copy(&len, res + 0x3C, 4) &&
                  len < 0x10000 && (len == 0 || (blk >= 0x80000000 && blk < 0x84000000 && coop_safe_copy(&probe, blk, 4) &&
                                                  coop_safe_copy(&probe, blk + len - 1 - ((len - 1) & 3), 4)));
        if (!ok) {
            static uint64_t lt; uint64_t nw = GetTickCount64();
            if (nw - lt > 2000) { lt = nw; logf("coop: own spawn (%s): the squad type record %08x is not ready (resource %08x block %08x len %u) - waiting", where, type, res, blk, len); }
            return false;
        }
    }
    float pos[3], yawdeg = 0.0f;
    const float *eye = (const float *)(uintptr_t)(0x801FD1B0 + 0x100), *fwd = (const float *)(uintptr_t)(0x801FD1B0 + 0xF0);
    if (at) { pos[0] = at[0]; pos[1] = at[1]; pos[2] = at[2]; }
    else if (!(eye[0] == 0.0f && eye[2] == 0.0f)) { pos[0] = eye[0] + 4.0f * fwd[0]; pos[1] = eye[1] - 1.5f; pos[2] = eye[2] + 4.0f * fwd[2]; }
    else { if (fails++ < 3) logf("coop: own spawn (%s): no position yet", where); return false; }
    if (fwd[0] * fwd[0] + fwd[2] * fwd[2] > 1e-6f) yawdeg = 57.2957795f * (float)atan2(fwd[0], fwd[2]);
    logf("coop: own spawn (%s): type %08x index %u slot %u at %.1f %.1f %.1f", where, type, tidx,
         *(volatile uint32_t *)(uintptr_t)(type + 8), pos[0], pos[1], pos[2]);
    uint32_t b = coop_build_one(type, tidx, pos, yawdeg, 0x454E4F31 + (uint32_t)g_cfg.coop_role);
    if (!b) return false;
    g_p10_gdst = b;
    g_p10_done = 1;
    g_coop_clone_on = 1;
    g_coop_clone_pos[0] = *(volatile float *)(uintptr_t)(b + 0xA0);
    g_coop_clone_pos[1] = *(volatile float *)(uintptr_t)(b + 0xA4);
    g_coop_clone_pos[2] = *(volatile float *)(uintptr_t)(b + 0xA8);
    logf("coop: own spawn: the CLONE is %08x at %.1f %.1f %.1f", b, g_coop_clone_pos[0], g_coop_clone_pos[1], g_coop_clone_pos[2]);
    static int self_on = -1;
    if (self_on < 0) { char e[8] = {0}; self_on = g_cfg.coop_downed && !(GetEnvironmentVariableA("COOP_SELF_MODEL", e, sizeof e) && e[0] == '0'); }
    if (self_on) {
        float pk[3] = {pos[0], pos[1] - 100.0f, pos[2]};
        uint32_t sb = coop_build_one(type, tidx, pk, yawdeg, 0x464C4553);   // "SELF"
        if (sb && sb != b) {
            *(volatile float *)(uintptr_t)(sb + 0x2F0) = 3.4028235e38f;
            g_coop_self_body = sb; g_self_link = 0; g_self_link_src = 0;
            g_self_park[0] = pk[0]; g_self_park[1] = pk[1]; g_self_park[2] = pk[2];
            logf("coop: self model: built %08x (own spawn, parked at %.1f %.1f %.1f)", sb, pk[0], pk[1], pk[2]);
        } else logf("coop: self model: own spawn failed - no self model this level");
    }
    return true;
}

// CHECKPOINTS (two-PC play 20261114, owner: "the checkpoint popup didn't sync: one player went ahead and triggered the
// checkpoint but the popup didn't trigger for the other"; then "when a player reaches a checkpoint, teleport the other
// player near him; if the other player is downed, respawn him"). A checkpoint = a trigger (mission 1: #37, no targets)
// whose level part sets up the next SECTION through coop_level_prepare a frame later - on both copies at once (trigger
// sync). The game's "checkpoint reached" popup shows only on the copy whose own player stands there. Here: when the
// section comes within 2 s of a trigger the PARTNER set off, our player moves 0.8 m behind the partner (only when more
// than 10 m away; a downed player is respawned there with full health) and gets a green note.
char g_coop_info_msg[96];
volatile unsigned long long g_coop_info_msg_until;
char g_coop_warn_msg[96];                    // fe/coop_menu.cpp sets it (the joiner's save warning); d3d/device.cpp draws it
volatile unsigned long long g_coop_warn_msg_until;
static uint64_t g_cp_moved_ms;              // when the checkpoint rule last moved our player
static void coop_cp_move(const float feet[3], float yaw, const char *why)
{
    uint32_t ctrl = g_coop_last_ctrl, po = 0;
    if (ctrl >= 0x80000000 && ctrl < 0x84000000) coop_safe_copy(&po, ctrl + 0x8C, 4);
    float d = 1e9f;
    if (po >= 0x80000000 && po < 0x84000000) {
        const volatile float *f = (const volatile float *)(uintptr_t)(po + 0xA0);
        float dx = f[0] - feet[0], dy = f[1] - feet[1], dz = f[2] - feet[2];
        d = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    bool downed = g_coop_downed != 0;
    static float lim = -1.0f;   // COOP_CP_MOVE_DIST=<m>: the distance from which the left-behind player is moved (10)
    if (lim < 0.0f) { char e[16] = {0}; lim = GetEnvironmentVariableA("COOP_CP_MOVE_DIST", e, sizeof e) ? (float)atof(e) : 5.0f; }   // 5 m (was 10: a 9.9-m case stayed put)
    if (downed || d > lim) {
        logf("coop: checkpoint: the PARTNER reached it (%s; %.1f m away%s) - %s next to them", why, d, downed ? ", we are downed" : "", downed ? "respawning" : "moving");
        coop_respawn_at(feet, yaw, downed);
        g_cp_moved_ms = GetTickCount64();
        strncpy(g_coop_info_msg, "CHECKPOINT REACHED - MOVED TO YOUR PARTNER", sizeof g_coop_info_msg - 1);
    } else {
        logf("coop: checkpoint: the PARTNER reached it (%s; %.1f m away) - close enough, no move", why, d);
        strncpy(g_coop_info_msg, "CHECKPOINT REACHED", sizeof g_coop_info_msg - 1);
    }
    g_coop_info_msg_until = GetTickCount64() + 4000;
}
// Two-PC play 20261115 #5 (owner: "revert teleportation back to the part-streamer trigger: not all checkpoints are
// close to a part streamer, so a lagging player was stuck in a void when the other player triggered a part streamer.
// Remove teleportation from the checkpoints, give it to the part-streamer trigger with a message"): a real checkpoint
// only shows a note on the partner's copy (the game's own popup shows on the reacher's) - and (owner: "checkpoints can
// revive a downed player") a DOWNED player respawns next to the partner who reached it, with full health.
static void coop_cp_note(const float feet[3], float yaw, const char *why)
{
    if (g_coop_downed) {
        logf("coop: checkpoint: the PARTNER reached it (%s) - we are downed: respawning next to them", why);
        coop_respawn_at(feet, yaw, true);
        g_cp_moved_ms = GetTickCount64();
        strncpy(g_coop_info_msg, "CHECKPOINT REACHED - BACK IN THE FIGHT", sizeof g_coop_info_msg - 1);
    } else {
        logf("coop: checkpoint: the PARTNER reached it (%s) - a note only (the moves happen at level-part switches)", why);
        strncpy(g_coop_info_msg, "YOUR PARTNER REACHED A CHECKPOINT", sizeof g_coop_info_msg - 1);
    }
    g_coop_info_msg_until = GetTickCount64() + 4000;
}
static void coop_cp_send(const char *why)
{
    uint32_t ctrl = g_coop_last_ctrl, po = 0;
    if (ctrl >= 0x80000000 && ctrl < 0x84000000) coop_safe_copy(&po, ctrl + 0x8C, 4);
    if (po < 0x80000000 || po >= 0x84000000 || g_coop_sock == INVALID_SOCKET) { logf("coop: checkpoint: no player object - CLCP not sent"); return; }
    struct { uint32_t magic; float feet[3]; float yaw; uint32_t sec; } m;
    m.magic = 0x50434C43;
    const volatile float *f = (const volatile float *)(uintptr_t)(po + 0xA0);
    m.feet[0] = f[0]; m.feet[1] = f[1]; m.feet[2] = f[2];
    m.yaw = *(volatile float *)(uintptr_t)(ctrl + 0x18);
    m.sec = g_cp_sec;
    for (int r = 0; r < 5; r++) sendto(g_coop_sock, (const char *)&m, (int)sizeof m, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
    logf("coop: level part: section %u here (%s) - told the partner (CLCP)", g_cp_sec, why);
}
static void coop_cp_recv(const uint8_t *raw, int len)
{
    if (!g_cfg.coop_role || len < 24) return;
    uint32_t sec; memcpy(&sec, raw + 20, 4);
    if (!sec || sec <= g_cp_peer_sec) return;   // the 5 copies count once
    g_cp_peer_sec = sec;
    float feet[3]; memcpy(feet, raw + 4, 12);
    float yaw; memcpy(&yaw, raw + 16, 4);
    (void)feet; (void)yaw;
    // playtest 20261114 #7 (owner: "the teleport rule is applied to some part-streamer event, not to the real
    // checkpoint"): a level-part section no longer moves anyone - only the game's real checkpoint does (coop_real_cp_step).
    logf("coop: level part: the partner is in section %u (ours %u)", sec, g_cp_sec);
}
// playtest 20261114 #3: the 3rd section of mission 1 started with NO synced trigger (each copy's own player started it:
// the joiner 8 s before the host) -> "section without a recent trigger - nothing to do", nobody moved, and the joiner
// fought the section's first enemies alone. Now SECTION NUMBERS: each copy counts its sections since the level load;
// the copy that starts section N first sends CLCP {feet, yaw, N}; the other copy moves next to it (or respawns there
// when downed) and its own section follows once its player stands there.
// LEVEL PARTS (playtest 20261114 #4, owner: "a level loading event isn't synced: the joiner went first into the tunnel,
// enemies spawned but the joiner couldn't hurt them; the host went ahead but nothing loaded for him - geometry and
// enemies missing, an empty void at the end of the tunnel"). Mission 1 streams its level in PARTS (world *0x2D45D4:
// two slots of 0x880 at +0x49D0, current part +0x5AF0, request +0x5AE8). The part switch is NOT a trigger activation:
// the area trigger's own test (FUN_001C9370 / FUN_001C8B40, trigger +0x124 == 1) calls FUN_001C7620, which - once the
// slot holding part (current + 1) is streamed in (slot +8 == current + 1, slot +0x38 == 0) - sets +0x5AE8 = 2; the
// level driver (FUN_001F1080 case 4) then loads it through FUN_001EB440 (our coop_level_prepare). (+0x124 == 2 ->
// FUN_001B73B0: +0x5AE8 = 1, the plain advance.) Area triggers are local per player, the trigger hook never sees the
// switch, and a player moved next to the partner by the checkpoint rule may stand PAST the area -> his copy never
// switched. Now the copy that is behind (the partner reported a higher section) does what FUN_001C7620 does itself,
// half a second after the move, as soon as the next part is streamed in; again for each section it is behind.
static void coop_part_step()
{
    {   // once per level load: how many level parts this level streams (1 = none: the part rules never act)
        static uint32_t logged = 0xFFFFFFFF;
        uint32_t W0 = *(volatile uint32_t *)(uintptr_t)0x002D45D4, pd = 0;
        if (g_cfg.coop_role && logged != g_load_seq && W0 >= 0x80000000 && W0 < 0x84000000 &&
            coop_safe_copy(&pd, W0 + 0x5AF4, 4) && pd >= 0x80000000 && pd < 0x84000000) {
            uint8_t cnt = 0; coop_safe_copy(&cnt, pd + 0x11, 1);
            logged = g_load_seq;
            logf("coop: level part: this level streams %u part(s) (current %d)", cnt, *(volatile int32_t *)(uintptr_t)(W0 + 0x5AF0));
        }
    }
    if (!g_cfg.coop_role || g_cp_peer_sec <= g_cp_sec) return;
    uint64_t now = GetTickCount64();
    if (now - g_cp_moved_ms < 500) return;
    // playtest #7 (owner: "one time the host was in the void"): switching parts drops the oldest part - a player still
    // standing in it floats in the void. Two-PC play 20261115 #5: the partner went ahead into the next part, so ours
    // follows; a player more than 40 m from the partner is first moved next to them (once the part is streamed in
    // here) with "GETTING TOO FAR - TELEPORTED TO YOUR PARTNER", the switch half a second later. A downed player is not
    // moved (spectating; back at the next main objective as before).
    bool is_near = false;
    float pfeet[3] = {g_coop_host_pawnmat[12], g_coop_host_pawnmat[13], g_coop_host_pawnmat[14]};
    {
        uint32_t ctrl = g_coop_last_ctrl, po = 0;
        if (ctrl >= 0x80000000 && ctrl < 0x84000000) coop_safe_copy(&po, ctrl + 0x8C, 4);
        if (po >= 0x80000000 && po < 0x84000000 && g_coop_host_pawn_ok) {
            const volatile float *f = (const volatile float *)(uintptr_t)(po + 0xA0);
            float dx = f[0] - pfeet[0], dy = f[1] - pfeet[1], dz = f[2] - pfeet[2];
            is_near = dx * dx + dy * dy + dz * dz < 40.0f * 40.0f;
        }
    }
    bool moved_recently = now - g_cp_moved_ms < 8000;
    static uint64_t t_req, t_log; static uint32_t req_for;
    if (req_for == g_cp_sec && now - t_req < 5000) return;   // a request is on its way (one per section, retried after 5 s)
    uint32_t W = *(volatile uint32_t *)(uintptr_t)0x002D45D4;
    if (W < 0x80000000 || W >= 0x84000000) return;
    int32_t cur = *(volatile int32_t *)(uintptr_t)(W + 0x5AF0);
    uint32_t state = *(volatile uint32_t *)(uintptr_t)(W + 0x5AE0), flag = *(volatile uint32_t *)(uintptr_t)(W + 0x5AE8);
    int ready = -1; uint32_t sd[2][3] = {};
    for (int k = 0; k < 2; k++) {
        uint32_t sl = W + 0x49D0 + k * 0x880;
        sd[k][0] = *(volatile uint32_t *)(uintptr_t)sl; sd[k][1] = *(volatile uint32_t *)(uintptr_t)(sl + 8);
        sd[k][2] = *(volatile uint8_t *)(uintptr_t)(sl + 0x38);
        if ((int32_t)sd[k][1] == cur + 1 && sd[k][2] == 0) ready = k;
    }
    if (state == 0x1C && flag == 0 && ready >= 0 && !is_near && !moved_recently && !g_coop_downed && g_coop_host_pawn_ok) {
        logf("coop: level part: the partner is at section %u, we at %u and far from them - teleported to the partner before the switch", g_cp_peer_sec, g_cp_sec);
        coop_respawn_at(pfeet, g_coop_host_yaw, false);
        g_cp_moved_ms = now;
        strncpy(g_coop_info_msg, "GETTING TOO FAR - TELEPORTED TO YOUR PARTNER", sizeof g_coop_info_msg - 1);
        g_coop_info_msg_until = now + 4000;
        return;   // the switch follows half a second later (moved_recently)
    }
    if (state == 0x1C && flag == 0 && ready >= 0) {
        *(volatile uint32_t *)(uintptr_t)(W + 0x5AE8) = 2;
        t_req = now; req_for = g_cp_sec;
        logf("coop: level part: the partner is at section %u, we at %u -> part %d streamed in (slot %d) - switching to it here (+0x5AE8 = 2)",
             g_cp_peer_sec, g_cp_sec, cur + 1, ready);
        return;
    }
    if (now - t_log > 2000) {
        t_log = now;
        logf("coop: level part: the partner is at section %u, we at %u - waiting (state %x, request %u, part %d, slots %x/%d/%u %x/%d/%u)",
             g_cp_peer_sec, g_cp_sec, state, flag, cur, sd[0][0], (int32_t)sd[0][1], sd[0][2], sd[1][0], (int32_t)sd[1][1], sd[1][2]);
    }
}
// REAL CHECKPOINTS (playtest 20261114 #7): the game shows "CHECKPOINT REACHED" (string C9ABBCB2, coop_loc_lookup) - on
// both copies at once when a synced trigger set it off. The player who reached it = the one closer to that trigger
// (g_cp_tpos, activation / replay within 4 s); with no recent synced trigger the popup came from our own player. That
// copy sends CLCK {feet, yaw}; the other copy moves its player 0.8 m behind (more than COOP_CP_MOVE_DIST m away) or
// respawns him there when downed, and its part follows (coop_part_step: just moved).
// HIT MARKER (owner 20261115: "a hit marker option in the co-op lobby that enables a local hit marker: each time the
// player hits a target the marker appears; each kill shows a red one"). The host owns enemy health, so it sees every
// hit: its own player's (attacker = our player) and the joiner's (attacker = the clone, firing the joiner's rays). A hit
// shows at once; a kill = that enemy's health at 0 within 0.5 s of the hit. The host's own -> its marker; the joiner's ->
// "CLHM" {kill} to the joiner, which shows its own. Drawn by d3d/device.cpp (coop_hitmarker) when g_cfg.coop_hitmarker.
volatile unsigned long long g_coop_hm_ms;
volatile int g_coop_hm_kill;
static struct { uint32_t enemy; int who; uint64_t ms; } g_hm_pend[16];
static void coop_hm_show(bool kill)
{
    if (!g_cfg.coop_hitmarker) return;
    uint64_t now = GetTickCount64();
    if (!kill && g_coop_hm_kill && now - g_coop_hm_ms < 450) return;   // a kill marker is not cut short by a hit
    g_coop_hm_kill = kill ? 1 : 0;
    g_coop_hm_ms = now;
}
static void coop_hm_send(bool kill)
{
    if (g_coop_sock == INVALID_SOCKET) return;
    struct { uint32_t magic, kill; } m = {0x4D484C43, kill ? 1u : 0u};   // "CLHM"
    sendto(g_coop_sock, (const char *)&m, (int)sizeof m, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
}
static uint32_t g_hm_dead[32]; static uint32_t g_hm_dead_w;   // enemies whose kill marker already showed (this level)
static void coop_hm_hit(uint32_t enemy, int who)
{
    // owner (20261115): "have the red hit marker only play once after the enemy dies" - shots into a body that is already
    // dead (health 0, or its kill already shown) show nothing and start no new kill check
    float hp0 = 1.0f;
    if (coop_safe_copy(&hp0, enemy + 0x2F0, 4) && hp0 <= 0.0f) return;
    for (int i = 0; i < 32; i++) if (g_hm_dead[i] == enemy) {
        if (hp0 > 0.0f && hp0 < 1e6f) { g_hm_dead[i] = 0; break; }   // alive again: the game reused the body for a new enemy
        return;
    }
    if (who == 1) coop_hm_show(false); else coop_hm_send(false);
    uint64_t now = GetTickCount64();
    int k = -1;
    for (int i = 0; i < 16; i++) if (g_hm_pend[i].enemy == enemy) k = i;
    if (k < 0) for (int i = 0; i < 16 && k < 0; i++) if (!g_hm_pend[i].enemy || now - g_hm_pend[i].ms > 500) k = i;
    if (k < 0) k = (int)(now % 16);
    g_hm_pend[k].enemy = enemy; g_hm_pend[k].who = who; g_hm_pend[k].ms = now;
}
static void coop_hm_step()   // host, play tick: a hit enemy whose health reached 0 -> a kill marker for whoever hit it last
{
    {   // COOP_HM_TEST=1 (test): the marker on, a hit and a kill alternately every 2 s (screenshots)
        // COOP_HM_TEST=2: the HOST sends a CLHM hit / kill every 2 s instead (the joiner's network path)
        static int t = -1;
        if (t < 0) { char e[8] = {0}; t = GetEnvironmentVariableA("COOP_HM_TEST", e, sizeof e) ? atoi(e) : 0; if (t == 1) g_cfg.coop_hitmarker = true; }
        static uint64_t last; static int n;
        if (t && GetTickCount64() - last > 2000) {
            last = GetTickCount64();
            bool k = (n++ & 1) != 0;
            if (t == 1) coop_hm_show(k);
            else if (t == 2 && g_cfg.coop_role == 1) coop_hm_send(k);
        }
    }
    if (g_cfg.coop_role != 1) return;
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 16; i++) {
        uint32_t e = g_hm_pend[i].enemy;
        if (!e) continue;
        float hp = 1.0f;
        if (!coop_safe_copy(&hp, e + 0x2F0, 4) || hp <= 0.0f) {
            static uint32_t kl;
            if (kl++ < 20) logf("coop: hit marker: enemy %08x killed by the %s", e, g_hm_pend[i].who == 1 ? "HOST" : "JOINER");
            if (g_hm_pend[i].who == 1) coop_hm_show(true); else coop_hm_send(true);
            g_hm_dead[g_hm_dead_w++ & 31] = e;   // its kill marker showed: once
            g_hm_pend[i].enemy = 0;
        } else if (now - g_hm_pend[i].ms > 500) g_hm_pend[i].enemy = 0;
    }
}
static void coop_hm_recv(const uint8_t *raw, int len)   // joiner: the host saw our hit / kill
{
    if (g_cfg.coop_role != 2 || len < 8) return;
    uint32_t kill; memcpy(&kill, raw + 4, 4);
    coop_hm_show(kill != 0);
}
static void coop_real_cp_step()
{
    static uint64_t done;
    if (!g_cfg.coop_role || !g_cp_popup_ms || g_cp_popup_ms == done) return;
    done = g_cp_popup_ms;
    uint32_t ctrl = g_coop_last_ctrl, po = 0;
    if (ctrl >= 0x80000000 && ctrl < 0x84000000) coop_safe_copy(&po, ctrl + 0x8C, 4);
    if (po < 0x80000000 || po >= 0x84000000) return;
    const volatile float *f = (const volatile float *)(uintptr_t)(po + 0xA0);
    // realcp test: the position of "the last synced trigger" was a stale one (section 1's #37, 110 s old) on both copies
    // -> the host was taken as the reacher and the joiner (at the checkpoint) was moved 137 m back. The HOST knows who set
    // the trigger off: an activation inside its joiner block (the joiner's request, or its trigger test at the joiner's
    // feet) is the joiner's (g_cp_who 2), its own player's otherwise. The host decides; the joiner acts on its own only
    // when no synced trigger came (its own local trigger showed the popup).
    bool recent = g_cp_ms && g_cp_popup_ms >= g_cp_ms && g_cp_popup_ms - g_cp_ms < 4000;
    bool ours;
    if (g_cfg.coop_role == 1) {
        ours = !recent || g_cp_who != 2;
        logf("coop: checkpoint: REAL checkpoint (host decides) - %s", !recent ? "no synced trigger: our own player" : g_cp_who == 2 ? "set off from the JOINER's side" : "set off by our own player");
        if (!ours) {   // the joiner reached it: the host goes there
            if (g_coop_host_pawn_ok) {
                float feet[3] = {g_coop_host_pawnmat[12], g_coop_host_pawnmat[13], g_coop_host_pawnmat[14]};
                coop_cp_note(feet, g_coop_host_yaw, "the JOINER reached a REAL checkpoint");
            }
            return;
        }
    } else {
        ours = !recent;
        logf("coop: checkpoint: REAL checkpoint (joiner) - %s", ours ? "no synced trigger: our own player reached it" : "a synced trigger: the host decides");
        if (!ours) return;
    }
    if (g_coop_sock == INVALID_SOCKET) return;
    struct { uint32_t magic; float feet[3]; float yaw; } m;
    m.magic = 0x4B434C43;   // "CLCK"
    m.feet[0] = f[0]; m.feet[1] = f[1]; m.feet[2] = f[2];
    m.yaw = *(volatile float *)(uintptr_t)(ctrl + 0x18);
    for (int r = 0; r < 5; r++) sendto(g_coop_sock, (const char *)&m, (int)sizeof m, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
    logf("coop: checkpoint: our position sent to the partner (CLCK)");
}
static void coop_real_cp_recv(const uint8_t *raw, int len)
{
    if (!g_cfg.coop_role || len < 20) return;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last < 3000) return;   // the 5 copies count once
    last = now;
    float feet[3]; memcpy(feet, raw + 4, 12);
    float yaw; memcpy(&yaw, raw + 16, 4);
    coop_cp_note(feet, yaw, "the partner reached a REAL checkpoint (CLCK)");
}
static void coop_checkpoint_section()
{
    if (!g_cfg.coop_role) return;
    g_cp_sec++;
    // playtest 20261114 #4: a level part loads into the slot of the part before the last one (mission 1: part 3 at
    // part 1's memory, 81A52800) - every trigger / spawner indexed from that part is now part 3's data at the same
    // address (the joiner logged "spawner #4 spawned bf800000" = floats; nothing synced after the tunnel). Index both
    // tables again (300 ms later: the part is being built now); counts start at 0 for the new objects on both copies,
    // and the CLTG / CLSP / CLTQ packets carry the section number, so a copy only uses the partner's counts while both
    // are in the same part.
    g_trig_T = 0; g_trig_n = 0; g_trig_host_n = 0; g_trig_req_n = 0; g_trig_req_in_n = 0;
    g_sp_T = 0; g_sp_n = 0; g_sp_host_n = 0;
    g_part_ms = GetTickCount64();
    {
        uint32_t W = *(volatile uint32_t *)(uintptr_t)0x002D45D4;
        if (W >= 0x80000000 && W < 0x84000000)
            logf("coop: level part: section %u here - world part %d, request %u", g_cp_sec + 0, *(volatile int32_t *)(uintptr_t)(W + 0x5AF0), *(volatile uint32_t *)(uintptr_t)(W + 0x5AE8));
    }
    coop_cp_send(g_cp_peer_sec >= g_cp_sec ? "the partner is there already" : "first");   // the partner learns our section
}

static uint32_t g_prep_load_seq = 0xFFFFFFFF;   // the level load the last prepare belonged to (a section: unchanged)
extern "C" void coop_level_prepare(uint32_t obj)
{
    g_coop_teardown = 0;   // a new level: the quit teardown is over
    if (!g_coop_clone) return;
    g_donor_tried = false;   // every load (and restart): the squad donor check runs again
    coop_exit_reset();
    g_donor_used = false;
    uint32_t cnt = *(volatile uint32_t *)(uintptr_t)(obj + 0x30);
    uint32_t arrA = *(volatile uint32_t *)(uintptr_t)(obj + 0x18);
    if (arrA < 0x10000 || arrA >= 0x84000000 || cnt < 1 || cnt > 0x1000) {
        // playtest 20261114 #5 (joiner crash, a call to 0 in a trigger replay): mission 1's PART 4 has NO unit array
        // (level part obj p6 = 0) -> this returned before the section rules: no section number, no table re-index;
        // the trigger table still held part 3's objects and the joiner's replay of #14 called through a dead one.
        // A part without units is still a section.
        bool sec = g_prep_load_seq == g_load_seq && g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000 &&
                   *(volatile uint32_t *)(uintptr_t)g_p10_gdst == 0x0028C1E8;
        if (sec) {
            coop_checkpoint_section();
            logf("coop: level prepare: a new SECTION without units (level part %u) - the section rules ran", *(volatile uint32_t *)(uintptr_t)(obj + 4));
        }
        return;
    }
    // play 20261112_180631 (joiner): a first level (level_06) was loaded, then Veblensk. This prepare
    // ran ONCE per process and the spawn counter kept counting -> in Veblensk no clone slot was added
    // and the chained factory fired at the wrong spawn: the clone was built at 801e39a0 from another
    // soldier's def (a crouched ally rig) and every natural pawn sat one address later than on the
    // host (pairing by address broken). Now EVERY level load prepares again: the spawn counter and
    // the clone state start fresh (a block we already swapped in is skipped).
    static uint32_t last_src, loads;
    if (arrA == last_src) {
        // play 20261114_005119: a mission RESTART reloads the same level - its unit array is still OUR block
        // (the clone slot already in it), so nothing was prepared again: the reload retired the old clone
        // body (life 4, link 0) and, with the spawn counter and the clone state untouched, no new clone was
        // built -> both players invisible to each other. A restart resets the clone state, the spawn counter
        // and the enemy ids, and the chained factory builds a fresh clone at this load's spawn #4.
        loads++;
        logf("coop: level prepare: RESTART of the same level (load #%u) - spawn counter %u -> 0, clone state reset (old clone %08x)",
             loads, (uint32_t)g_pawncap_n, g_p10_gdst);
        coop_clone_state_reset();
        g_pawncap_n = 0;
        coop_enid_reset();
        g_cp_sec = 0; g_cp_peer_sec = 0;   // checkpoint section numbers start again
        // rc1: the trigger / spawn sync tables stayed indexed (same table pointer 0x2D45D0) -> the joiner's
        // level-start triggers (the intro script) and its first spawns were held as requests to the host,
        // which was still in its intro: the intro message came 15 s late and spawn #3/#4 (the clone's chained
        // factory) never ran. Drop the tables: as on a first load the level start runs locally on both copies
        // and the tables are indexed again once play begins.
        g_trig_T = 0; g_trig_n = 0; g_sp_T = 0; g_sp_n = 0;
        return;
    }
    loads++;
    // TWO-PC PLAY 20261114 (mission 1; both copies crashed at 0x1A0086 after the 3rd "checkpoint"): mission 1 sets up
    // each new SECTION through this same routine (908 s, 1601 s - no level load in between). Each one reset the clone
    // state: a new clone + self model were built and the old pair stayed in the world with live brains (the crash: the
    // 908-s self model's brain 801353C0 ticking with a gone target, a null read at +0xC4). It also reset the enemy ids
    // while the two copies reach a section at different times (host 908 s, joiner 971 s) -> enemy desync. A prepare
    // with NO level load since the last one = a section: the clone, the self model, the spawn counter and the enemy
    // ids stay (only a real load / a restart resets them).
    bool section = loads > 1 && g_load_seq == g_prep_load_seq && g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000 &&
                   *(volatile uint32_t *)(uintptr_t)g_p10_gdst == 0x0028C1E8 && *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x384) == 0;
    g_prep_load_seq = g_load_seq;
    if (section) {
        coop_checkpoint_section();
        logf("coop: level prepare: a new SECTION of the same level (prepare #%u, load #%u) - the clone %08x, the self model, the spawn counter (%u) and the enemy ids stay",
             loads, g_load_seq, g_p10_gdst, (uint32_t)g_pawncap_n);
    } else {
        // two-PC play 20261115: the 2nd mission of a session quit 6.6 s in with "no partner state for 20 s" - the time of
        // the partner's last packet was from the PREVIOUS mission (the lobby sends none). A new level waits for a new one.
        g_coop_host_ms = 0;
        if (loads > 1) {
            logf("coop: level prepare: level load #%u - spawn counter %u -> 0, clone state reset (old clone %08x, done %u)",
                 loads, (uint32_t)g_pawncap_n, g_p10_gdst, g_p10_done);
            coop_clone_state_reset();
        }
        g_pawncap_n = 0;
        coop_enid_reset();          // enemy identity: spawn-point counters and ids start fresh in every level
        g_cp_sec = 0; g_cp_peer_sec = 0;   // checkpoint section numbers start again
    }

    uint32_t size = (cnt + 1) * 0x1D0;
    // The unit array is read as an aligned matrix table (movaps at slot+0xA0) by other
    // systems, so the block must be 16-byte aligned like the game's own arena blocks.
    uint32_t src = (uint32_t)_aligned_malloc(size, 16);
    if (!src) return;
    memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)arrA, cnt * 0x1D0);

    // The clone: a byte copy of unit 0 in its raw (pre-construct) state, appended as
    // the last slot. Position: unit0 + 2 m on both axes — the same world-grid cell.
    uint8_t *clone = (uint8_t *)(uintptr_t)(src + cnt * 0x1D0);
    memcpy(clone, (const void *)(uintptr_t)arrA, 0x1D0);
    float *pos = (float *)(uintptr_t)(clone + 0x20);
    pos[0] += 2.0f; pos[2] += 2.0f;
    memcpy(clone + 0xA0, clone + 0x20, 12);
    *(volatile uint32_t *)(uintptr_t)(clone + 0x54) = src;
    *(volatile uint32_t *)(uintptr_t)(clone + 0x9C) = 0;

    *(volatile uint32_t *)(uintptr_t)(obj + 0x18) = src;
    *(volatile uint32_t *)(uintptr_t)(obj + 0x30) = cnt + 1;
    last_src = src;
    g_unit_arr = src; g_unit_cnt = cnt + 1;

    logf("coop: level prepare: clone slot %u appended (unit0 %08x raw, cnt %u->%u, new array %08x)",
         cnt, arrA, cnt, cnt + 1, src);
    g_coop_clone_pos[0] = pos[0]; g_coop_clone_pos[1] = pos[1]; g_coop_clone_pos[2] = pos[2];
    g_coop_clone_on = 1;
}

// ---- Stage 2 capture C: the level stream interpreter journal (pilot 7 prep). ----
// FUN_001e1800 is the level entity-stream interpreter; its caller (FUN_001eb440) sets
// EDI to the record context and passes the stream pointer as the first stack arg
// (stream: [0]=record count, [1]=records base; 16-byte records: byte0 opcode + 3 dwords).
// The journal dumps the first records per level (cap-limited) so pilot 7 can replay one
// real case-0x0A spawn record through FUN_001df340.
extern "C" void coop_journal_stream(uint32_t edi, uint32_t args_base)
{
    static int jruns;
    if (jruns >= 4) return;
    if (!g_cfg.coop_log) return;
    jruns++;
    // args_base points at arg1's stack SLOT; the slot holds the stream struct pointer
    // (verified live: {count=983, records*} for mission 1's Vratska stream).
    uint32_t param = *(volatile uint32_t *)(uintptr_t)args_base;
    if (param < 0x10000 || param >= 0x84000000) {
        logf("coop: stream #%d: arg1 not a stream ptr (%08x)", jruns, param);
        return;
    }
    uint32_t cnt = *(volatile uint32_t *)(uintptr_t)param;
    uint32_t recs = *(volatile uint32_t *)(uintptr_t)(param + 4);
    logf("coop: stream #%d: struct %08x count %u records %08x (ctx EDI %08x)", jruns, param, cnt, recs, edi);
    if (recs < 0x10000 || recs >= 0x84000000) return;
    int show = cnt < 4096 ? (int)cnt : 4096;
    int n0a = 0;
    uint32_t hist[256] = {};
    for (int i = 0; i < show; i++) {
        uint32_t base = recs + (uint32_t)i * 0x10;
        uint8_t op = *(volatile uint8_t *)(uintptr_t)base;
        hist[op]++;
        if (op != 0x0A || n0a >= 8) continue;
        uint32_t a1 = *(volatile uint32_t *)(uintptr_t)(base + 4);
        uint32_t a2 = *(volatile uint32_t *)(uintptr_t)(base + 8);
        uint32_t a3 = *(volatile uint32_t *)(uintptr_t)(base + 12);
        float blk[8];
        memcpy(blk, (const void *)(uintptr_t)(a1 + 0x10), 32);
        logf("coop:   SPAWN rec %02d a1 %08x a2 %08x a3 %08x pos %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f",
             i, a1, a2, a3, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7]);
        n0a++;
    }
    char line[300];
    int w = 0;
    for (int op = 0; op < 256; op++) {
        if (!hist[op]) continue;
        int need = snprintf(line + w, sizeof line - w, " %02x:%u", op, hist[op]);
        if (need < 0 || w + need >= (int)sizeof line) break;
        w += need;
    }
    logf("coop:   opcode histogram (%d records):%s", show, line);
}

// ---- Naked thunks (byte-for-byte replacement of patched instruction pairs). ----

// Pilot 12: the registration logger body (reads the capture channels; runs on the
// game thread between pushad/popad).
extern "C" void coop_log_registration(void)
{
    uint32_t body = g_regcap_body, worldv = g_regcap_world, caller = g_regcap_ret;
    if (body < 0x10000 || body >= 0x84000000) return;
    if (!g_regc_quiet)
    logf("coop: regc: body %08x vt %08x caller %08x pos %.1f %.1f %.1f",
         body, *(volatile uint32_t *)(uintptr_t)body, caller,
         *(volatile float *)(uintptr_t)(body + 0xA0),
         *(volatile float *)(uintptr_t)(body + 0xA4),
         *(volatile float *)(uintptr_t)(body + 0xA8));
    (void)worldv;
    // pilot 13c: adopt the redirected pawn (the world-register return address sits
    // in FUN_001df440: 001df582/001df587) as the LAN-drive puppet for our pose
    if (g_regcap_on && g_coop_clone && caller >= 0x001df500 && caller <= 0x001df600 && !g_rematch_hit) {
        float bx = *(volatile float *)(uintptr_t)(body + 0xA0);
        float by = *(volatile float *)(uintptr_t)(body + 0xA4);
        float bz = *(volatile float *)(uintptr_t)(body + 0xA8);
        float dx = bx - g_rematch_pos[0], dz = bz - g_rematch_pos[2];
        if (dx * dx + dz * dz < 1.0f) {
            g_rematch_hit = 1;
            g_p10_gdst = body;
            g_p10_done = 1;
            g_coop_clone_on = 1;
            g_coop_clone_pos[0] = bx;
            g_coop_clone_pos[1] = by;
            g_coop_clone_pos[2] = bz;
            logf("coop: p13c: FACTORY PAWN ADOPTED: %08x at %.1f %.1f %.1f", body, bx, by, bz);
        }
    }
}

// Pilot 12b: the pawn-def logger (def = the spawn definition struct the army of
// factory calls pass; one dump per pawn spawn, ~4 per mission).
extern "C" void coop_log_pawn_def(void)
{
    uint32_t n = ++g_pawncap_n;
    {   // ar5 / ar6: a spawner re-spawns with the previous body's +0x324 as the new enemy's brain - when
        // that body was the clone or a puppet it is OUR private copy (the clone's 03963010 brained 14
        // new enemies; a puppet's crashed the renderer in ar3). Give the factory the game's class instead.
        uint32_t o = g_pawncap_owner, fix = 0;
        if (o && o == g_aim_link) {
            // sp3: the CLONE's private brain came off the game's free list while the clone lives; its class
            // is still the brain of the natural pawn it was copied with -> do not hand that out. The new enemy
            // keeps this brain; the clone gets a fresh private copy next frame.
            // sp5: resetting and waiting for coop_aim_body left the clone brainless for good (its +0x324 still
            // held the old native-memory copy, which coop_aim_body will not copy) -> no aim / fire / enemy tick.
            // Make the new copy now, from the original class.
            uint32_t nl = (g_aim_link_src >= 0x80000000 && g_aim_link_src < 0x84000000 && g_p10_gdst) ? coop_make_private_link(g_p10_gdst, g_aim_link_src) : 0;
            logf("coop: spawn #%u: the clone's private brain %08x was reused by the game - the clone got a new copy %08x", n, o, nl);
            if (nl) g_aim_link = nl;
            o = 0;
        }
        for (int i = 0; i < g_pl_n && !fix; i++) if (g_pl[i].link == o && g_pl[i].orig) fix = g_pl[i].orig;
        for (int i = 0; i < 128 && !fix; i++) if (g_pl_all[i].link == o && g_pl_all[i].orig) fix = g_pl_all[i].orig;
        if (fix) {   // is that class still the brain of a LIVE enemy (directly, or behind one of our private links)?
            uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980, arr = 0, user = 0;
            if (mgr >= 0x80000000 && mgr < 0x84000000) arr = *(volatile uint32_t *)(uintptr_t)(mgr + 0x79BC);
            for (int i = 0; arr >= 0x80000000 && arr < 0x84000000 && i < 16 && !user; i++) {
                uint32_t b = *(volatile uint32_t *)(uintptr_t)(arr + 4 * i);
                if (b < 0x80000000 || b >= 0x84000000 || !(*(volatile float *)(uintptr_t)(b + 0x2F0) > 0.0f)) continue;
                uint32_t l = *(volatile uint32_t *)(uintptr_t)(b + 0x324);
                if (l == fix) user = b;
                for (int q = 0; q < g_pl_n && !user; q++) if (g_pl[q].body == b && g_pl[q].link == l && g_pl[q].orig == fix) user = b;
            }
            // m1p4 test (joiner crash 0x19D493 on our clone's brain): the class behind one of our links was the brain of
            // OUR SELF MODEL (own spawn) - the swap gave it to a new enemy, two bodies on one brain. Our own bodies'
            // brains are never handed out.
            for (int q = 0; q < g_own_brains_n && !user; q++) if (g_own_brains[q] == fix) user = 0xFFFFFFFF;
            static int cl;
            if (user && cl++ < 60) logf("coop: spawn #%u: class %08x (for our link %08x) is still the brain of %s %08x - no swap, the copy stays", n, fix, o, user == 0xFFFFFFFF ? "OUR own body" : "live enemy", user);
            if (user) fix = 0;
        }
        if (fix && g_pawncap_owner_slot >= 0x10000) {
            *(volatile uint32_t *)(uintptr_t)g_pawncap_owner_slot = fix;
            g_pawncap_owner = fix;
            static int fl;
            if (fl++ < 300) logf("coop: spawn #%u: owner %08x is one of our private links - the factory gets its class %08x", n, o, fix);
        }
    }
    uint32_t owner = g_pawncap_owner, def = g_pawncap_def, mgrv = g_pawncap_mgr;
    uint32_t ctx = g_pawncap_ctx;
    {   // ENEMY IDENTITY hunt: which level unit does this spawn come from? (owner / def / ctx pointers into the unit array)
        const uint32_t v[3] = {owner, def, ctx}; const char *nm[3] = {"owner", "def", "ctx"};
        for (int i = 0; i < 3; i++)
            if (g_unit_arr && v[i] >= g_unit_arr && v[i] < g_unit_arr + g_unit_cnt * 0x1D0)
                logf("coop: spawn #%u: %s %08x = unit %u (+%x)", n, nm[i], v[i], (v[i] - g_unit_arr) / 0x1D0, (v[i] - g_unit_arr) % 0x1D0);
        if (n <= 40) {
            uint32_t dw[16] = {0};
            if (def >= 0x10000 && def < 0x84000000) coop_safe_copy(dw, def, 64);
            logf("coop: spawn #%u: owner %08x def %08x ctx %08x (unit array %08x, %u units) | model words d8 %08x d9 %08x d10 %08x d11 %08x d12 %08x d13 %08x",
                 n, owner, def, ctx, g_unit_arr, g_unit_cnt, dw[8], dw[9], dw[10], dw[11], dw[12], dw[13]);
        }
    }
    {   // TRIGGER HUNT (env COOP_SPAWN_STACK=1): the game return addresses on the stack at every natural
        // spawn after the level start - the script / trigger code that caused the spawn is among them
        static int ss = -1;
        if (ss < 0) { char e[8] = {0}; ss = GetEnvironmentVariableA("COOP_SPAWN_STACK", e, sizeof e) && e[0] == '1'; }
        if (ss && n <= 60) {
            const uint32_t *sp = (const uint32_t *)_AddressOfReturnAddress();
            char buf[700]; int o = 0, cnt = 0;
            for (int i = 0; i < 0x300 && cnt < 40 && o < 660; i++) {
                uint32_t v = 0;
                if (!coop_safe_copy(&v, (uint32_t)(uintptr_t)(sp + i), 4)) break;
                if (v >= 0x00011000 && v < 0x00230000) { o += snprintf(buf + o, sizeof buf - o, " %06x", v); cnt++; }
            }
            logf("coop: spawn #%u stack:%s", n, buf);
        }
    }
    if (owner >= 0x80000000 && owner < 0x84000000) {
        bool have = false;
        for (int i = 0; i < g_class_objs_n; i++) if (g_class_objs[i] == owner) have = true;
        if (!have && g_class_objs_n < 24) g_class_objs[g_class_objs_n++] = owner;
    }
    if (ctx >= 0x80000000 && ctx < 0x84000000) {   // enemy identity: the ordinal of this spawn at its spawn point
        int k = -1;
        for (int i = 0; i < g_spawn_ctx_n; i++) if (g_spawn_ctx[i].ctx == ctx) k = i;
        if (k < 0 && g_spawn_ctx_n < 128) { k = g_spawn_ctx_n++; g_spawn_ctx[k].ctx = ctx; g_spawn_ctx[k].count = 0; }
        if (k >= 0) {
            g_spawn_ctx[k].count++;
            if (g_spawn_pend_n >= 32) { memmove(g_spawn_pend, g_spawn_pend + 1, 31 * sizeof g_spawn_pend[0]); g_spawn_pend_n = 31; }
            g_spawn_pend[g_spawn_pend_n].ctx = ctx; g_spawn_pend[g_spawn_pend_n].ord = g_spawn_ctx[k].count; g_spawn_pend_n++;
        }
    }
    if (def < 0x10000 || def >= 0x84000000) return;
    // pilot 13e: cache the def words (the last real spawn's values)
    memcpy((void *)g_defcache, (const void *)(uintptr_t)def, 0x38);
    // pilot 13f: chain OUR factory call at the natural spawn #4 ENTRY - the one
    // instant the def's runtime links are freshest (the game reuses/refills its
    // requester objects per spawn; later = stale = the 13b crash). Full-size
    // copy (the factory's FUN_001d21f0(def) walks far past 0x38 - the 13e crash).
    // FRIENDLY LOOK (owner, day 15: "fixed friendly look" - the lobby step 4 choice): the clone (and the self model)
    // chain onto the level's SQUAD soldier when it has one. A spawn whose request (def) is the one the previous spawn
    // used and whose previous body came out on the player's side (team +0x39C 0 = ours; the immortal squad) is a
    // squad spawn: its links are as fresh as spawn #4's. Otherwise (no squad before #4) spawn #4 as before.
    // Both copies see the same spawn order, so they decide alike. Env COOP_FRIENDLY_LOOK=0 = always #4.
    static uint32_t prev_def, prev_ctx;
    static int friendly = -1;
    if (friendly < 0) { char e[8] = {0}; friendly = !(GetEnvironmentVariableA("COOP_FRIENDLY_LOOK", e, sizeof e) && e[0] == '0'); }
    bool chain_now = false;
    // level survey 20261114: missions 4 and 5 crashed ~1 s in on both copies (AV 0x1A0086, FUN_001A0050(brain + 0x1EC0)
    // with a null body at +0x1ED0): the brain was 8014F220 = this FIRST natural spawn's own, and our two own-spawn
    // builds ran NESTED inside its entry (the factory's per-spawn state reused) - that unit came out with no body link
    // and its AI action read [0 + 0xC4]. The own spawn is no longer built here: the first play tick builds it (the
    // mission 1 path, which never had a natural spawn before it), the same moment on both copies.
    // (was: if (!g_p10_done && coop_own_spawn_on() && owner >= 0x80000000) coop_spawn_own(nullptr, "first natural spawn's entry");)
    // own spawn on: the clone is built on the first play tick - the old chain (spawn #4 / the squad soldier's look) stays off
    if (g_p13_on && !g_p10_done && g_coop_clone && !coop_own_spawn_on()) {
        if (n == 1) { prev_def = 0; prev_ctx = 0; }
        if (friendly && n >= 2 && n <= 4 && def == prev_def && prev_ctx) {
            uint32_t s16[16]; uint32_t ns = enemy_slots(s16), ally = 0, team = 0xFF; float hp = 0;
            for (uint32_t k = 0; k < ns && !ally; k++) {
                uint32_t b = s16[k], cx = 0;
                if (b == g_p10_gdst || !coop_safe_copy(&cx, b + 0x31C, 4) || cx != prev_ctx) continue;
                coop_safe_copy(&team, b + 0x39C, 4); coop_safe_copy(&hp, b + 0x2F0, 4);
                ally = b;
            }
            logf("coop: friendly look: spawn #%u reuses spawn #%u's request; that body %08x team %u hp %g -> %s", n, n - 1, ally, team, hp,
                 ally && team == 0 ? "a SQUAD spawn: the clone chains here" : "not the squad");
            if (ally && team == 0) chain_now = true;
        }
        if (n == 4) chain_now = true;   // fallback: as before
        prev_def = def; prev_ctx = ctx;
    }
    if (chain_now && g_p13_on && !g_p10_done && g_coop_clone) coop_squad_donor_step();   // before the clone's type is chosen
    if (chain_now && g_p13_on && !g_p10_done && g_coop_clone) {
        // the cinematic camera (static, alive from boot) - the player bridge has
        // NOT fired yet at spawn time (52.7 s vs ~60 s), so no ctrl basis exists
        uint32_t cam = 0x801FD1B0;
        const float *eye = (const float *)(uintptr_t)(cam + 0x100);
        const float *fwd = (const float *)(uintptr_t)(cam + 0xF0);
        const float *lrow = (const float *)(uintptr_t)(cam + 0xD0);
        if (!(eye[0] == 0.0f && eye[2] == 0.0f)) {
            float tx = eye[0] + 4.0f * fwd[0] + 3.0f * lrow[0];
            float ty = eye[1] - 1.5f;
            float tz = eye[2] + 4.0f * fwd[2] + 3.0f * lrow[2];
            float yawdeg = 0.0f;
            float sy2 = fwd[0], cz2 = fwd[2];
            float len2 = sy2 * sy2 + cz2 * cz2;
            if (len2 > 1e-6f) yawdeg = 57.2957795f * (float)atan2(sy2, cz2);
            uint32_t defcopy = (uint32_t)_aligned_malloc(0x400, 16);
            if (defcopy) {
                // def lives on the caller's stack: copy only the fields the factory
                // reads (deepest = +0x54, FUN_001d21f0); 0x400 crossed the stack top
                // (run 38 AV). Zero the rest of the buffer.
                memset((void *)(uintptr_t)defcopy, 0, 0x400);
                memcpy((void *)(uintptr_t)defcopy, (const void *)(uintptr_t)def, 0x60);
                coop_clone_type_override(defcopy);
                *(volatile float *)(uintptr_t)(defcopy + 0x10) = tx;
                *(volatile float *)(uintptr_t)(defcopy + 0x14) = ty;
                *(volatile float *)(uintptr_t)(defcopy + 0x18) = tz;
                *(volatile float *)(uintptr_t)(defcopy + 0x1C) = yawdeg;
                g_fact_mgr = mgrv;          // live, this instant
                g_fact_a1 = owner;           // live class object
                g_fact_def = defcopy;        // full-size copy, our pose
                g_fact_ctx = ctx;            // live class ctx
                // snapshot the requester frame BEFORE the call: our chained factory
                // call re-enters coop_thunk_pawncap and overwrites g_pawncap_ebp with
                // OUR frame (run 49 read 00200216/00/1 instead of the real pointers;
                // run 50 watch: the requester writes 81b111c0/02/81afd2f0 at 1af982..98e)
                uint32_t rbp = g_pawncap_ebp;
                coop_thunk_factory();
                uint32_t newBody = g_fact_out;
                if (newBody >= 0x80100000 && newBody < 0x84000000) {
                    // replay the requester's post-call steps (0x1af96c) on OUR body,
                    // with the live requester frame (spawn #4's own args)
                    if (rbp >= 0x10000 && rbp < 0x84000000) {
                        uint32_t a5 = *(volatile uint32_t *)(uintptr_t)(rbp + 0x18);
                        uint32_t a9 = *(volatile uint32_t *)(uintptr_t)(rbp + 0x28);
                        uint8_t a10 = *(volatile uint8_t *)(uintptr_t)(rbp + 0x2C);
                        uint32_t k = *(volatile uint32_t *)(uintptr_t)0x295CEC;
                        *(volatile uint32_t *)(uintptr_t)(newBody + 0x10) = a9;
                        *(volatile uint8_t *)(uintptr_t)(newBody + 0x14) = a10;
                        *(volatile uint32_t *)(uintptr_t)(newBody + 0x31C) = a5;
                        *(volatile uint32_t *)(uintptr_t)(newBody + 0x2F0) = k;
                        logf("coop: p13f: post-steps replayed (ebp %08x): +10=%08x +14=%02x +31C=%08x +2F0=%08x",
                             rbp, a9, a10, a5, k);
                        // run 52 watch: health +0x2F0 zeroed 1 frame after spawn at
                        // 001df7f3 = FUN_001df630, the fall/landing physics (fatal
                        // landing: the ctor seeds the old pose from the pose globals =
                        // natural #4's spot ~9 m away -> one-frame "impact"). Its first
                        // test is `[esi+0x3a7] != 0 -> skip`: opt the LAN-driven puppet
                        // out of fall physics (we drive its position every frame).
                        *(volatile uint8_t *)(uintptr_t)(newBody + 0x3A7) = 1;
                    } else {
                        logf("coop: p13f: post-steps SKIPPED - bad requester ebp %08x", rbp);
                    }
                    g_p10_gdst = newBody;
                    g_p10_done = 1;
                    g_coop_clone_on = 1;
                    g_coop_clone_pos[0] = *(volatile float *)(uintptr_t)(newBody + 0xA0);
                    g_coop_clone_pos[1] = *(volatile float *)(uintptr_t)(newBody + 0xA4);
                    g_coop_clone_pos[2] = *(volatile float *)(uintptr_t)(newBody + 0xA8);
                    logf("coop: p13f: CHAINED factory built %08x at %.1f %.1f %.1f",
                         newBody, g_coop_clone_pos[0], g_coop_clone_pos[1], g_coop_clone_pos[2]);
                    // SELF MODEL: a second chained call, same instant, same live links (both copies build it, so
                    // the spawn counters stay equal). Built parked 100 m below the clone's spot.
                    static int self_on = -1;
                    if (self_on < 0) { char e[8] = {0}; self_on = g_cfg.coop_downed && !(GetEnvironmentVariableA("COOP_SELF_MODEL", e, sizeof e) && e[0] == '0'); }
                    uint32_t dc2 = self_on ? (uint32_t)_aligned_malloc(0x400, 16) : 0;
                    if (dc2) {
                        memset((void *)(uintptr_t)dc2, 0, 0x400);
                        memcpy((void *)(uintptr_t)dc2, (const void *)(uintptr_t)def, 0x60);
                        coop_clone_type_override(dc2);
                        *(volatile float *)(uintptr_t)(dc2 + 0x10) = tx;
                        *(volatile float *)(uintptr_t)(dc2 + 0x14) = ty - 100.0f;
                        *(volatile float *)(uintptr_t)(dc2 + 0x18) = tz;
                        *(volatile float *)(uintptr_t)(dc2 + 0x1C) = yawdeg;
                        g_fact_mgr = mgrv; g_fact_a1 = owner; g_fact_def = dc2; g_fact_ctx = ctx;
                        coop_thunk_factory();
                        uint32_t sb = g_fact_out;
                        if (sb >= 0x80100000 && sb < 0x84000000 && sb != newBody) {
                            if (rbp >= 0x10000 && rbp < 0x84000000) {
                                *(volatile uint32_t *)(uintptr_t)(sb + 0x10) = *(volatile uint32_t *)(uintptr_t)(rbp + 0x28);
                                *(volatile uint8_t *)(uintptr_t)(sb + 0x14) = *(volatile uint8_t *)(uintptr_t)(rbp + 0x2C);
                                *(volatile uint32_t *)(uintptr_t)(sb + 0x31C) = *(volatile uint32_t *)(uintptr_t)(rbp + 0x18);
                            }
                            *(volatile float *)(uintptr_t)(sb + 0x2F0) = 3.4028235e38f;
                            *(volatile uint8_t *)(uintptr_t)(sb + 0x3A7) = 1;     // no fall physics: we place it
                            g_coop_self_body = sb; g_self_link = 0; g_self_link_src = 0;
                            g_self_park[0] = tx; g_self_park[1] = ty - 100.0f; g_self_park[2] = tz;
                            logf("coop: self model: built %08x (parked at %.1f %.1f %.1f) - shown crouched where we go down",
                                 sb, tx, ty - 100.0f, tz);
                        } else logf("coop: self model: chained call returned %08x - no self model this level", sb);
                    }
                } else {
                    logf("coop: p13f: chained call returned %08x", newBody);
                }
            }
        }
    }
    if (g_regc_quiet) return;
    logf("coop: pawncap: spawn #%u mgr %08x a1 %08x def %08x ctx %08x", n, mgrv, owner, def, ctx);
    uint32_t d[14];
    memcpy(d, (const void *)g_defcache, 56);
    logf("coop: pawndef 0-7 : %08x %08x %08x %08x %08x %08x %08x %08x",
         d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    logf("coop: pawndef 8-13: %08x %08x %08x %08x %08x %08x",
         d[8], d[9], d[10], d[11], d[12], d[13]);
}

// two-PC play 20261114 (owner): "pausing the game doesn't remove in-game input for the player - can still move, fire,
// throw grenades". Co-op keeps the world running under the pause menu (install_no_pause), and this player update runs
// inside it; single player runs neither while paused. While co-op is on and our pause menu is open, the game's player
// update is skipped (our bridge above still runs: the partner keeps getting our state).
volatile uint32_t g_skip_player;
__declspec(naked) void coop_thunk()
{
    __asm {
        pushfd
        pushad
        push ecx
        call coop_on_player_update
        add esp, 4
        popad
        popfd
        cmp dword ptr [g_skip_player], 0
        je run_player
        ret
    run_player:
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        jmp dword ptr [g_coop_resume]
    }
}

// Pilot 12b: entry hook on FUN_001df440 - patch site 001df446 (EAX already holds
// the mgr param, [esp+4] = the def), displaced 6 bytes (sub esp,8 / push edi /
// mov edi,eax / pad). Resume 001df44c.
__declspec(naked) void coop_thunk_pawncap()
{
    __asm {
        mov dword ptr [g_pawncap_mgr], eax    // EAX = mgr param (still original here)
        mov dword ptr [g_pawncap_ebp], ebp    // EBP = the requester's (FUN_001af880) frame
        mov eax, [esp+0xC]                    // arg1: the class/def object
        mov dword ptr [g_pawncap_owner], eax
        lea eax, [esp+0xC]
        mov dword ptr [g_pawncap_owner_slot], eax   // where it lives: coop_log_pawn_def may swap it
        mov eax, [esp+0x10]                   // arg2: the spawn request (pos + hashes)
        mov dword ptr [g_pawncap_def], eax
        mov eax, [esp+0x14]                   // arg3: the class context (ESI in body)
        mov dword ptr [g_pawncap_ctx], eax
        pushfd
        pushad
        call coop_log_pawn_def
        popad
        popfd
        mov eax, dword ptr [g_pawncap_mgr]    // restore EAX = mgr for the displaced code
        push edi                              // displaced: push edi; mov edi,eax; xor eax,eax
        mov edi, eax
        xor eax, eax
        jmp dword ptr [g_pawncap_resume]
    }
}

// Pilot 13b: CALL FUN_001df440 (the pawn factory): EAX = mgr; TWO stack args
// (both exits are `pop edi; add esp,8; ret 8` - verified in the XBE bytes):
//   arg1 = the class/def object (captured)
//   arg2 = the spawn request (pos quad at +0x10..0x1c + model hashes)
// There is NO arg3: "ctx" ([esp+0x14] in the capture) is caller stack data, and
// ESI is saved/overwritten by the prologue (not an input). Pushing a third dword
// left the stack 4 B short after RET 8 -> popad shifted -> retn popped saved EAX
// (= a1 = 0x80149280) -> run 39 ILLEGAL_INSTRUCTION at 80149280.
__declspec(naked) void coop_thunk_factory()
{
    __asm {
        pushad
        mov eax, dword ptr [g_fact_def]
        push eax
        mov eax, dword ptr [g_fact_a1]
        push eax
        mov eax, dword ptr [g_fact_mgr]
        call dword ptr [g_fact_fn]
        mov dword ptr [g_fact_out], eax
        popad
        retn
    }
}

// SKELETON ARM AIM, round 4: entry hook on FUN_0016f420 (the skin-palette upload,
// EAX = skin record; prologue 55 8B EC 83 E4 F0 = push ebp / mov ebp,esp / and
// esp,-16, resume at +6). rec+0x08 = bone 4x4 array, rec+0x0C = int bone indices,
// (char)rec+0x18 = bones used. Step 1 = identify the clone's record (log each
// distinct record once with its bones' translations).
volatile uint32_t g_skin_rec;
volatile void *g_skin_resume;
volatile uint32_t g_coop_skin_mark;   // read by d3d/draw.cpp (--helmet-scan); declared in coop.h
volatile float g_aim_pitch_deg;   // last look pitch written to the clone (coop_aim_body)
volatile float g_aim_yaw_deg;     // last look yaw (deg) written to the clone (coop_aim_body)

// skin83 rig map (MODEL space, feet at origin, Y up, faces +Z; 25-bone soldier rig,
// index table 81e3a380): spine 0 root, 1 pelvis .97, 5, 8 (1.18 = PIVOT), 11 chest
// 1.30, 14 neck 1.46, 15/18 head; R arm 17 > 20 > 22 > 24 (hand fwd); L arm 16 > 19 >
// 21 > 23; legs 4-7-10-13 / 3-6-9-12. Bones are ABSOLUTE model-space 4x4s -> each
// upper-body bone rotates rigidly about the pivot; no hierarchy pass needed.
static const int kUpperBones[] = {11, 14, 15, 18, 16, 17, 19, 20, 21, 22, 23, 24};
static void coop_bend_upper(uint32_t bones, float pitch_deg)
{
    static uint32_t s_bones;
    static float saved[25][16], bent[25][16];
    float cur[25][16];
    memcpy(cur, (const void *)(uintptr_t)bones, sizeof cur);           // bones 0..24
    if (s_bones != bones || memcmp(cur, bent, sizeof cur) != 0)
        memcpy(saved, cur, sizeof saved);                               // fresh anim pose
    s_bones = bones;
    memcpy(bent, saved, sizeof bent);
    const float *pv = &saved[8][12];                                    // pivot = bone 8 joint
    float a = pitch_deg * 0.0174532925f, c = cosf(a), s = sinf(a);
    const float K[3] = {1.0f, 0.0f, 0.0f};                              // model X = pitch axis
    for (int bi : kUpperBones) {
        float *m = bent[bi];
        for (int r = 0; r < 3; r++) rot_about(m + 4 * r, K, c, s);     // axis rows
        float t[3] = {m[12] - pv[0], m[13] - pv[1], m[14] - pv[2]};
        rot_about(t, K, c, s);
        m[12] = pv[0] + t[0]; m[13] = pv[1] + t[1]; m[14] = pv[2] + t[2];
    }
    memcpy((void *)(uintptr_t)bones, bent, sizeof bent);
}

// Per-array bend with a saved/bent cache: a buffer that still holds our last bent
// output is re-bent from the saved anim pose (never compounds); a fresh anim pose is
// saved first. Every USED bone whose model-space joint is above 1.0 m (spine/chest/
// neck/head/arms/hands; not pelvis/legs) rotates rigidly about model X through (0,1,0).
static volatile float g_gun_k1;          // barrel = bone-24 r0 + k1 r1 + k2 r2 (per weapon)
static volatile float g_gun_k2;
static volatile float g_arm_yaw_model;   // diagnostics: the anim pose's arm direction (model yaw)
static volatile float g_gun_pitch_anim;  // diagnostics: the anim pose's gun pitch (deg, +down; 1000 = not measured)
volatile float g_need_twist;            // last spine twist the aim needed (deg, unclamped)
volatile float g_blade_ema;             // slow average of the aim pose's gun yaw relative to the root (deg)
volatile int g_blade_ok;
volatile int g_clone_idle;              // coop_loco_body: the clone stands (the walk cycle swings the gun -> no blade samples)
volatile int g_hold_ok;                 // the clone's upper body is the captured hold pose
volatile float g_hold_blade;            // its gun yaw in model space (constant)
volatile uint64_t g_need_twist_ms;      // when it was measured
static bool coop_bend_array(uint32_t rb, uint32_t ix, int nb, float p, float aim_model_yaw = 1000.0f)
{
    if (rb < 0x80000000 || rb >= 0x84000000 || ix < 0x80000000 || ix >= 0x84000000 || nb <= 0 || nb > 64) return false;
    struct Cache { uint32_t arr; int maxb; float saved[65][16], bent[65][16]; };
    static Cache cache[6]; static int ncache;
    Cache *cc = nullptr;
    for (int i = 0; i < ncache; i++) if (cache[i].arr == rb) cc = &cache[i];
    if (!cc && ncache < 6) { cc = &cache[ncache++]; cc->arr = rb; cc->maxb = -1; memset(cc->bent, 0, sizeof cc->bent); }
    if (!cc) {   // a 7th array (the clone rebuilt / re-rigged): recycle the cache
        static int victim; cc = &cache[victim]; victim = (victim + 1) % 6;
        cc->arr = rb; cc->maxb = -1; memset(cc->bent, 0, sizeof cc->bent);
    }
    int maxb = -1;
    for (int i = 0; i < nb; i++) { int b = *(volatile int *)(uintptr_t)(ix + 4 * i); if (b > maxb && b < 64) maxb = b; }
    if (maxb < 0) return false;
    size_t sz = (size_t)(maxb + 1) * 64;
    float cur[65][16];
    memcpy(cur, (const void *)(uintptr_t)rb, sz);
    bool fresh = cc->maxb != maxb || memcmp(cur, cc->bent, sz) != 0;
    if (fresh) memcpy(cc->saved, cur, sz);   // fresh anim pose
    cc->maxb = maxb;
    memcpy(cc->bent, cc->saved, sz);
    // HOLD POSE (owner: "the clone should not have any idle animation at all and just point
    // the gun where the player aims at all times"). Once, capture a steady AIM pose from the
    // clone's own animation (standing, weapon raised + level + extended); from then on every
    // upper-body bone (joint above 1.0 m: spine, chest, neck, head, arms, hands) comes from
    // that capture, re-attached at the CURRENT pelvis (the legs keep their walk/idle cycle).
    // No idle sway is left above the hips, and the gun axis measured below is a constant.
    // Env COOP_CLONE_HOLD=0 = off (the old anim-driven upper body).
    // two-PC play 20261114 (owner screenshot: the clone aiming up, the rifle off the right hand): bones were picked by
    // HEIGHT (joints above 1.0 m = upper body; the pitch pivots 1.00 / 1.18 / 1.30 m by height too). With a rifle held
    // low the right hand / forearm sit near those lines - part of the arm took the hold pose / the bend and part did
    // not, and the hand came off the gun. On the 25-bone soldier rig (skin83 map: 14 neck, 15 / 18 head, arms 16 > 19 >
    // 21 > 23 and 17 > 20 > 22 > 24) bones 14-24 always count as upper body ABOVE every pivot: the arms and the gun move
    // with the chest as one piece.
    auto bend_h = [&](int b, float h) { return (maxb == 24 && b >= 14 && b <= 24 && h < 1.5f) ? 1.5f : h; };
    static float cap[65][16]; static int cap_ok = 0, cap_maxb = -1, hold_env = -1;
    static uint64_t first_ms;
    if (hold_env < 0) { char e[8] = {0}; hold_env = !(GetEnvironmentVariableA("COOP_CLONE_HOLD", e, sizeof e) && e[0] == '0'); }
    if (!first_ms) first_ms = GetTickCount64();
    extern volatile int g_clone_idle; extern volatile int g_hold_ok; extern volatile float g_hold_blade;
    // hp223: the first passing pose held the gun at the hip (hand 1.15 m); the joiner's
    // relaxed host clone never passed at all. Now: SCORE every standing candidate with a
    // forward barrel (higher hand, longer reach, level barrel = better), keep the best,
    // and capture it 6 s after the first candidate - at once if it is already shouldered
    // (hand >= 1.35 m, |pitch| < 8). The pitch bend levels whatever pitch remains.
    {   // COOP_HOLD_DUMP_AT=<sec>: write the clone's CURRENT anim pose (fresh, unheld) at that time
        // as a C initializer, tagged with the equipped weapon type (pistol pose recording)
        static float dump_at = -2.0f;
        if (dump_at < -1.0f) { char e[16] = {0}; dump_at = GetEnvironmentVariableA("COOP_HOLD_DUMP_AT", e, sizeof e) ? (float)atof(e) : -1.0f; }
        static bool dumped = false;
        if (dump_at > 0 && !dumped && g_coop_t0 && (float)(GetTickCount64() - g_coop_t0) / 1000.0f >= dump_at && maxb >= 24) {
            dumped = true;
            uint32_t Wd = *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x278 + 0x28);
            char fn[64]; snprintf(fn, sizeof fn, "clone_pose_type%d.inc", weapon_type(Wd));
            FILE *df = fopen(fn, "w");
            if (df) {
                fprintf(df, "// clone anim pose at %.1f s, weapon type %d, %d bones\n", dump_at, weapon_type(Wd), maxb + 1);
                for (int b = 0; b <= maxb; b++) {
                    fprintf(df, "{");
                    for (int k = 0; k < 16; k++) fprintf(df, "%.7ff%s", cc->saved[b][k], k < 15 ? "," : "");
                    fprintf(df, "},\n");
                }
                fclose(df);
                logf("coop: pose dump: %s written", fn);
            }
        }
    }
    // nz226 (Naszran): soldiers there never hold an AIMED pose while standing -> the capture
    // fell back to the gun across the chest (reach 0.01, yaw -108) = "not aiming". The rig is
    // the same 25-bone soldier skeleton in every mission, so an aimed pose recorded once
    // (hd227, mission 1: pitch 3.8, hand 1.12, reach 0.53, gun yaw -16.6) is BUILT IN and
    // used from the first frame in any level; the live capture below is only for another
    // rig (or env COOP_HOLD_CAPTURE=1).
    static const float kHoldPose[25][16] = {
#include "clone_holdpose.inc"
    };
    // owner: "does the game have a handgun rig for models? if yes the clone should transfer to
    // it when the player switches to a handgun". pdry/pax (Naszran, weapon type 0): the soldier
    // anim set has a two-handed HANDGUN aim (arms extended, side-on) -> a second built-in
    // pose (pax 78 s, src/coop/clone_holdpose_pistol.inc), picked by the equipped weapon type:
    // handguns (type 0 in Naszran, 13 = the mission-1 pistol) use it, everything else the
    // rifle pose. Switches the moment the clone's weapon type changes.
    static const float kHoldPosePistol[25][16] = {
#include "clone_holdpose_pistol.inc"
    };
    // the Naszran rifle (type 4) gets its OWN native aim pose (rc_70 dry, 74 s) like the handgun
    static const float kHoldPoseRifle4[25][16] = {
#include "clone_holdpose_rifle4.inc"
    };
    // Veblensk weapons (owner play 20261112: "weapon alignment for most weapons"): with the SMG pose
    // the big handgun (type 1) and the AK (type 5) hung at odd angles (wt1_0 / wt5_0) - the hand grip
    // is weapon-specific. Each got its NATIVE aim pose, dumped with the hold off while the clone's own
    // aim layer pointed it at the camera (wn1 / wn5 / wn6, COOP_CLONE_WTYPE + COOP_HOLD_DUMP_AT=74).
    static const float kHoldPoseT1[25][16] = {
#include "clone_holdpose_t1.inc"
    };
    static const float kHoldPoseT5[25][16] = {
#include "clone_holdpose_t5.inc"
    };
    static const float kHoldPoseT6[25][16] = {
#include "clone_holdpose_t6.inc"
    };
    // the Naszran SMG (type 3; owner play 20261114_150609: "his right hand doesn't align with his player aim" - it
    // used the mission-1 SMG pose): its NATIVE aim pose, dumped like the others (wn3_nasz: solo Naszran,
    // COOP_CLONE_WTYPE=3 + COOP_CLONE_HOLD=0 + COOP_HOLD_DUMP_AT=74, both hands on the gun, pointing at the camera)
    static const float kHoldPoseT3[25][16] = {
#include "clone_holdpose_t3.inc"
    };
    static int cap_env = -1;
    if (cap_env < 0) { char e[8] = {0}; cap_env = GetEnvironmentVariableA("COOP_HOLD_CAPTURE", e, sizeof e) && e[0] == '1'; }
    if (hold_env && !cap_env && maxb == 24) {
        uint32_t Wp = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x278 + 0x28) : 0;
        int wtp = weapon_type(Wp);
        bool handgun = wtp == 0 || wtp == 13 || wtp == 14;   // 14 = the mission-1 Glock (two-PC play 20261114: "the Glock doesn't align" - it took the rifle pose)
        const float (*want)[16] = handgun ? kHoldPosePistol : (wtp == 4 ? kHoldPoseRifle4 : kHoldPose);
        if (wtp == 1) want = kHoldPoseT1; else if (wtp == 5) want = kHoldPoseT5; else if (wtp == 6) want = kHoldPoseT6;
        else if (wtp == 3) want = kHoldPoseT3;
        static const void *cur_pose;
        if (cur_pose != (const void *)want) {
            cur_pose = want;
            memcpy(cap, want, sizeof kHoldPose); cap_maxb = 24; cap_ok = 1;
            // owner (20261115): "strafing to the left still doesn't rotate the legs" - the HANDGUN pose's gun points +54 deg
            // off the body: the twist room let the legs turn only 31 deg on a left strafe (the right side reached 90).
            // De-blade it the same way.
            if (want == kHoldPoseT3 || (handgun && fabsf(57.2957795f * atan2f(want[24][0], want[24][2])) > 25.0f)) {
                // owner (play 20261114_151810): "strafing to the right rotates the upper body of the clone while it should
                // stay on his player aim". This native pose is BLADED: its gun points -78 deg off the body (the others
                // ~-20). The spine twist = (aim vs legs) - blade, capped at 90: a strafe (legs on the move, aim 90 off)
                // asked up to 168 one way and the upper body stopped short of the aim. Turn the pose's upper body once,
                // the way the runtime twist does (joints above 1.0 m about the vertical line through the pelvis), so its
                // gun points straight ahead (0): a full sideways strafe then needs exactly the 90 cap either way.
                float b0 = 57.2957795f * atan2f(cap[24][0], cap[24][2]);
                float d = 0.0f - b0, a = d * 0.0174532925f, c = cosf(a), sn = sinf(a), pz = cap[1][14];
                const float Y[3] = {0.0f, 1.0f, 0.0f};
                for (int bb = 0; bb <= 24; bb++) {
                    float *m = cap[bb];
                    if (bend_h(bb, want[bb][13]) <= 1.0f || fabsf(want[bb][12]) > 1.5f || fabsf(want[bb][14]) > 1.5f) continue;
                    for (int r = 0; r < 3; r++) rot_about(m + 4 * r, Y, c, sn);
                    float t[3] = {m[12], m[13], m[14] - pz};
                    rot_about(t, Y, c, sn);
                    m[12] = t[0]; m[13] = t[1]; m[14] = pz + t[2];
                }
                logf("coop: hold pose: type %d pose de-bladed %.1f -> %.1f (upper body turned %.1f)", wtp, b0, 57.2957795f * atan2f(cap[24][0], cap[24][2]), d);
            }
            g_hold_blade = 57.2957795f * atan2f(cap[24][0], cap[24][2]); g_hold_ok = 1;
            logf("coop: hold pose = BUILT-IN %s pose for weapon type %d (gun yaw (model) %.1f)", handgun ? "HANDGUN" : ((wtp == 4 || wtp == 1 || wtp == 3 || wtp == 5 || wtp == 6) ? "native" : "rifle"), wtp, g_hold_blade);
        }
    }
    static float best[65][16]; static float best_score = -1e9f, best_gp, best_h, best_reach, best_yaw;
    static float fb[65][16]; static float fb_score = -1e9f, fb_gp, fb_h, fb_reach, fb_yaw;   // fallback tier
    static uint64_t cand_ms, idle_ms;
    if (hold_env && !cap_ok && maxb >= 24 && g_clone_idle && GetTickCount64() - first_ms > 3000) {
        const float *hr = cc->saved[24], *ch = cc->saved[11];
        float gh = sqrtf(hr[0] * hr[0] + hr[2] * hr[2]);
        float gp = gh > 0.2f ? 57.2957795f * atan2f(-hr[1], gh) : 90.0f;          // barrel pitch, + = down
        float rx = hr[12] - ch[12], rz = hr[14] - ch[14], reach = sqrtf(rx * rx + rz * rz);
        float gy_model = 57.2957795f * atan2f(hr[0], hr[2]);
        // hp224: hand at chest height with NO reach (0.01) and a sideways barrel (-109) =
        // the gun held across the chest. Tier A = an AIMED gun (reach >= 0.25, barrel within
        // 95 deg of the front, |pitch| < 30); tier B (fallback after 12 s idle without any
        // tier A) = any forward barrel.
        if (!idle_ms) idle_ms = GetTickCount64();
        float score = 2.0f * hr[13] + 2.0f * reach - fabsf(gp) / 30.0f;
        if (reach >= 0.25f && fabsf(gy_model) < 95.0f && fabsf(gp) < 30.0f) {
            if (!cand_ms) cand_ms = GetTickCount64();
            if (score > best_score) {
                memcpy(best, cc->saved, sz); best_score = score; best_gp = gp; best_h = hr[13]; best_reach = reach; best_yaw = gy_model;
            }
        } else if (fabsf(gy_model) < 110.0f && fabsf(gp) < 45.0f && score > fb_score) {
            memcpy(fb, cc->saved, sz); fb_score = score; fb_gp = gp; fb_h = hr[13]; fb_reach = reach; fb_yaw = gy_model;
        }
        bool shouldered = cand_ms && best_h >= 1.35f && fabsf(best_gp) < 8.0f;
        bool take_a = cand_ms && (shouldered || GetTickCount64() - cand_ms > 6000);
        bool take_b = !cand_ms && fb_score > -1e8f && GetTickCount64() - idle_ms > 12000;
        if (take_a || take_b) {
            memcpy(cap, take_a ? best : fb, sz); cap_maxb = maxb; cap_ok = 1;
            if (take_a) {   // COOP_HOLD_DUMP=1: write the aimed pose as a C initializer (to embed in the DLL)
                char e[8] = {0};
                if (GetEnvironmentVariableA("COOP_HOLD_DUMP", e, sizeof e) && e[0] == '1') {
                    FILE *df = fopen("clone_holdpose.inc", "w");
                    if (df) {
                        fprintf(df, "// captured %s: gun pitch %.2f hand %.3f reach %.3f yaw %.2f, %d bones\n", "tier A", best_gp, best_h, best_reach, best_yaw, maxb + 1);
                        for (int b = 0; b <= maxb; b++) {
                            fprintf(df, "{");
                            for (int k = 0; k < 16; k++) fprintf(df, "%.7ff%s", best[b][k], k < 15 ? "," : "");
                            fprintf(df, "},\n");
                        }
                        fclose(df);
                        logf("coop: hold pose written to clone_holdpose.inc");
                    }
                }
            }
            g_hold_blade = take_a ? best_yaw : fb_yaw; g_hold_ok = 1;
            logf("coop: hold pose CAPTURED (%s): gun pitch %.1f, hand height %.2f, reach %.2f, gun yaw (model) %.1f",
                 take_a ? (shouldered ? "aimed, shouldered" : "aimed, best of 6 s") : "fallback, no aimed pose in 12 s",
                 take_a ? best_gp : fb_gp, take_a ? best_h : fb_h, take_a ? best_reach : fb_reach, g_hold_blade);
        }
    }
    if (hold_env && cap_ok && cap_maxb == maxb && maxb >= 1) {
        float dx = cc->saved[1][12] - cap[1][12], dy = cc->saved[1][13] - cap[1][13], dz = cc->saved[1][14] - cap[1][14];
        for (int b = 0; b <= maxb; b++) {
            const float *o = cap[b];
            if (bend_h(b, o[13]) <= 1.0f || fabsf(o[12]) > 1.5f || fabsf(o[14]) > 1.5f) continue;   // upper body only
            memcpy(cc->bent[b], cap[b], 64);
            cc->bent[b][12] += dx; cc->bent[b][13] += dy; cc->bent[b][14] += dz;
        }
    }
    const bool held = hold_env && cap_ok && cap_maxb == maxb;
    float (*P)[16] = held ? cc->bent : cc->saved;   // the pose the measurements see
    // SPINE-DISTRIBUTED bend (owner: proceed with polish): the angle is split over three
    // spine joints - 40% lower back (1.00 m), 35% mid-spine (1.18 m = bone 8), 25% chest
    // (1.30 m = bone 11). Bones are classified by their ORIGINAL (saved) height; the
    // rotations run TOP-DOWN so each lower joint carries everything above it (a rigid
    // chain: the higher pivots are themselves moved by the lower rotations).
    const float piv_h[3] = {1.30f, 1.18f, 1.00f};   // top-down
    const float piv_w[3] = {0.25f, 0.35f, 0.40f};
    int ub[64]; int nu = 0;                        // unique used bones (dup idx entries)
    for (int i = 0; i < nb && nu < 64; i++) {
        int b = *(volatile int *)(uintptr_t)(ix + 4 * i);
        if (b < 0 || b > maxb) continue;
        bool dup = false;
        for (int j = 0; j < nu; j++) if (ub[j] == b) { dup = true; break; }
        if (!dup) ub[nu++] = b;
    }
    float pz = 0.0f;                               // pivots on the spine line (pelvis z)
    if (maxb >= 1) pz = P[1][14];
    // YAW TWIST (owner: "have the clone look at the camera"; look107: the AI's combat
    // aim layer blades the whole rig ~58 deg off the body facing, so the gun points
    // where the AI aims). Rotate every upper-body bone (joint above 1.0 m) about the
    // VERTICAL spine line (x 0, z pz) by yaw_twist FIRST; the pitch then bends about the
    // twisted model X axis (R_y(twist) X = (cos, 0, -sin)) so it stays in the aim plane.
    // The pivots lie on the twist axis, so they do not move.
    float K[3] = {1.0f, 0.0f, 0.0f};
    // look108: twisting to the ROOT facing was not enough - the aim pose is bladed in
    // model space too (arms point ~60-90 deg off model +Z). Measure the arms in the
    // fresh pose: chest (bone 11) -> midpoint of the hands (23, 24), horizontal, and
    // twist by (wanted model yaw - arm yaw).
    float yaw_twist = 0.0f;
    if (maxb >= 24) {   // GUN AXIS PROBE (clone aim round 2): the hand bones' rows vs the hand->hand line
        static uint32_t gp;
        if ((gp++ % 1800) == 0) {
            const float *R = cc->saved[24], *L = cc->saved[23], *ch = cc->saved[11];
            float hx = L[12] - R[12], hy = L[13] - R[13], hz = L[14] - R[14];
            logf("coop: gunaxis: R24 t %.2f %.2f %.2f r0 %.2f %.2f %.2f r1 %.2f %.2f %.2f r2 %.2f %.2f %.2f | L23 t %.2f %.2f %.2f r0 %.2f %.2f %.2f r1 %.2f %.2f %.2f r2 %.2f %.2f %.2f | R->L %.2f %.2f %.2f | chest t %.2f %.2f %.2f",
                 R[12], R[13], R[14], R[0], R[1], R[2], R[4], R[5], R[6], R[8], R[9], R[10],
                 L[12], L[13], L[14], L[0], L[1], L[2], L[4], L[5], L[6], L[8], L[9], L[10], hx, hy, hz, ch[12], ch[13], ch[14]);
        }
    }
    // CLONE AIM round 2 (owner: "still plays idle animation and doesn't align with the
    // player's aim pitch/yaw"): the chest->hands line is a poor gun proxy - it swings
    // -20..-80 deg with the idle anim (font166 log) and sits ~40 deg off the barrel when
    // the weapon is held across the body. The gun is RIGID on the right hand (bone 24), so
    // measure the barrel from that bone's rotation: gun = 0.98 r0 - 0.19 r1 - 0.05 r2
    // (calibrated in gax170 frame 3780: muzzle on the camera). Both the yaw twist AND the
    // pitch are then CORRECTIONS (wanted - anim's own gun direction), so the gun holds the
    // player's aim whatever the idle/combat animation does with the arms.
    float gun_pitch_anim = 1000.0f;
    if (aim_model_yaw < 999.0f && maxb >= 24) {
        const float *ch = P[11], *hl = P[23], *hr = P[24];
        // axes190 (exact on-screen probes, front view): the drawn barrel runs along r0 of
        // bone 24; the earlier gax170 mix (-0.19 r1) tilted it ~10 deg down
        // play (Naszran Foundry, 2 sessions): pitch AND yaw still off - the barrel axis in the
        // hand depends on the WEAPON model. nzax230/_70 (Naszran rifle, type 4, exact probes):
        // barrel = r0 + 0.485 r1 (~26 deg from r0 toward r1); the mission-1 SMG (type 8,
        // axes190): r0. Per-type table, keyed on the clone's equipped weapon (first byte of
        // W = body+0x278+0x28); env COOP_GUN_K1=<k> overrides the r1 weight (tuning).
        float k1 = 0.0f;
        {
            uint32_t Wc = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x278 + 0x28) : 0;
            int wt = weapon_type(Wc);
            // nb_70/_m40/_20 (owner: "the rifle is still off"): three views of the SAME held
            // native rifle pose (no bend), barrel lines read off the frames, triangulated in model
            // space (pair solutions within 1 deg): barrel = r0 - 0.263 r1 + 0.050 r2 for type 4.
            // The old +0.485 (one side view, eyeballed) had the wrong sign; plain r0 was 15 deg off.
            if (wt == 4) k1 = -0.263f;
            g_gun_k1 = k1;
            static float k1_env = -1000.0f;
            if (k1_env < -999.0f) { char e[16] = {0}; k1_env = GetEnvironmentVariableA("COOP_GUN_K1", e, sizeof e) ? (float)atof(e) : -500.0f; }
            if (k1_env > -499.0f) k1 = k1_env;
            static int lwt = -1;
            if (wt != lwt) { lwt = wt; logf("coop: gun axis: clone weapon type %d -> barrel = r0 + %.3f r1", wt, k1); }
        }
        static float k2_env = -1000.0f;
        if (k2_env < -999.0f) { char e[16] = {0}; k2_env = GetEnvironmentVariableA("COOP_GUN_K2", e, sizeof e) ? (float)atof(e) : 0.0f; }
        float k2 = 0.0f;
        { uint32_t Wk = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x278 + 0x28) : 0;
          if (weapon_type(Wk) == 4) k2 = 0.050f; }
        if (GetEnvironmentVariableA("COOP_GUN_K2", nullptr, 0)) k2 = k2_env;
        g_gun_k2 = k2;
        float gx = hr[0] + k1 * hr[4] + k2 * hr[8], gy = hr[1] + k1 * hr[5] + k2 * hr[9], gz = hr[2] + k1 * hr[6] + k2 * hr[10];
        float gh = sqrtf(gx * gx + gz * gz);
        float ax = 0.5f * (hl[12] + hr[12]) - ch[12], az = 0.5f * (hl[14] + hr[14]) - ch[14];
        if (gh > 0.2f) { ax = gx; az = gz; gun_pitch_anim = 57.2957795f * atan2f(-gy, gh); }   // +pitch = down
        if (gh > 0.2f) {   // per-weapon barrel PITCH vs the hand's r0 (native poses recorded aiming at the camera, wanted pitch -2.4):
            uint32_t Wq = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x278 + 0x28) : 0;
            int wq = weapon_type(Wq);
            (void)wq;   // wv1/wv5/wv6: offsets derived from the dumped poses were WRONG (the dump is the pose BEFORE our bend) - none by default
            static float gpo = 1000.0f;   // COOP_GUN_POFS=<deg> adds to it (calibration)
            if (gpo > 999.0f) { char e[16] = {0}; gpo = GetEnvironmentVariableA("COOP_GUN_POFS", e, sizeof e) ? (float)atof(e) : 0.0f; }
            gun_pitch_anim += gpo;
        }
        if (ax * ax + az * az > 0.01f) {
            float arm = 57.2957795f * atan2f(ax, az);
            {   // FACE-ON yaw correction (owner play 22:44 screenshot: "still off" - the clone facing the
                // viewer, rifle ~45 deg to its side). Side views hide yaw (the old triangulation and the
                // gt_y+-60 check); the face-on sweep yo_* (solo Naszran, --p13-face, aim at the camera)
                // put the rifle straight at the camera at +16..+24, off at 0 / +32 -> +20 for type 4.
                // COOP_GUN_YOFS=<deg> overrides (calibration sweeps).
                static float gyo = 1000.0f;
                if (gyo > 999.0f) { char e[16] = {0}; gyo = GetEnvironmentVariableA("COOP_GUN_YOFS", e, sizeof e) ? (float)atof(e) : -1000.0f; }
                uint32_t Wy = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x278 + 0x28) : 0;
                // types 1 / 5 / 6 (native poses): r0 is the barrel, like the SMG (wv runs: +20.1 / +10.0 pointed them off-target)
                int wy = weapon_type(Wy);
                float yo = wy == 4 ? 20.0f : 0.0f;
                if (gyo > -999.0f) yo = gyo;
                arm += yo;
            }
            g_arm_yaw_model = arm;
            if (held) { extern volatile float g_hold_blade; g_hold_blade = arm; }   // per-weapon axis -> the legs' blade too
            yaw_twist = aim_model_yaw - arm;
            while (yaw_twist > 180.0f) yaw_twist -= 360.0f;
            while (yaw_twist < -180.0f) yaw_twist += 360.0f;
            g_need_twist = yaw_twist;      // unclamped (diagnostics)
            g_need_twist_ms = GetTickCount64();
            {   // play213: the closed loop on g_need_twist oscillated (the anim root lags the
                // commanded legs) -> keep a SLOW average of the pose's blade (gun yaw vs root,
                // independent of where the legs point) and aim the legs at aim - blade
                extern volatile float g_blade_ema; extern volatile int g_blade_ok; extern volatile int g_clone_idle;
                float d = g_clone_idle ? arm - g_blade_ema : 0.0f;
                while (d > 180.0f) d -= 360.0f;
                while (d < -180.0f) d += 360.0f;
                if (!g_blade_ok) { g_blade_ema = arm; g_blade_ok = 1; } else g_blade_ema += 0.01f * d;
                while (g_blade_ema > 180.0f) g_blade_ema -= 360.0f;
                while (g_blade_ema < -180.0f) g_blade_ema += 360.0f;
            }
            // a spine can only twist so far; past ~60 deg the legs turn in place
            // (coop_loco_body), so the twist stays small in play
            float tcap = held ? 90.0f : 80.0f;
            if (yaw_twist > tcap) yaw_twist = tcap;
            if (yaw_twist < -tcap) yaw_twist = -tcap;
        }
    }
    {   // COOP_HOLD_NOBEND=1 (calibration): the held pose with NO twist / pitch (a fixed model-space pose)
        static int nb = -1;
        if (nb < 0) { char e[8] = {0}; nb = GetEnvironmentVariableA("COOP_HOLD_NOBEND", e, sizeof e) && e[0] == '1'; }
        if (nb && held) { yaw_twist = 0.0f; aim_model_yaw = 1000.0f; gun_pitch_anim = 1000.0f; p = 0.0f; }
    }
    static float cls[65][16];          // heights for the classification (before any rotation)
    memcpy(cls, P, sz);
    if (yaw_twist != 0.0f) {
        float a = yaw_twist * 0.0174532925f, c = cosf(a), s = sinf(a);
        const float Y[3] = {0.0f, 1.0f, 0.0f};
        for (int j = 0; j < nu; j++) {
            int b = ub[j];
            const float *o = cls[b];
            if (bend_h(b, o[13]) <= 1.0f || fabsf(o[12]) > 1.5f || fabsf(o[14]) > 1.5f) continue;
            float *m = cc->bent[b];
            for (int r = 0; r < 3; r++) rot_about(m + 4 * r, Y, c, s);
            float t[3] = {m[12], m[13], m[14] - pz};
            rot_about(t, Y, c, s);
            m[12] = t[0]; m[13] = t[1]; m[14] = pz + t[2];
        }
    }
    if (aim_model_yaw < 999.0f) {
        // pitch axis = horizontal, perpendicular to where the gun now points (model yaw
        // aim_model_yaw): R_y(m) X = (cos m, 0, -sin m); m = 0 keeps the verified K = X.
        // (Set even when the twist is 0 - it used to stay X then.)
        float m = aim_model_yaw * 0.0174532925f;
        K[0] = cosf(m); K[2] = -sinf(m);
    }
    if (gun_pitch_anim < 999.0f) {
        // pitch CORRECTION: the anim's own gun pitch is removed, so the barrel ends at the
        // player's pitch (the idle pose's lowered/raised weapon no longer adds to it)
        // side175/176: a large correction leans the whole torso (sideways when the gun is
        // held across the chest); the game's own aim layer already pitches the gun with
        // the look pitch (dry174: anim gun -19 at look -17, +6 at +11.5), so the
        // correction only trims its lag/error -> capped at +-35
        p -= gun_pitch_anim;
        float cap_p = held ? 70.0f : 35.0f;   // a held AIM pose leans naturally; the anim pose did not
        if (p > cap_p) p = cap_p;
        if (p < -cap_p) p = -cap_p;
    }
    g_gun_pitch_anim = gun_pitch_anim;
    for (int k = 0; k < 3; k++) {
        float a = p * piv_w[k] * 0.0174532925f, c = cosf(a), s = sinf(a);
        const float pv[3] = {0.0f, piv_h[k], pz};
        for (int j = 0; j < nu; j++) {
            int b = ub[j];
            const float *o = cls[b];
            if (bend_h(b, o[13]) <= piv_h[k] || fabsf(o[12]) > 1.5f || fabsf(o[14]) > 1.5f) continue;  // above this joint
            float *m = cc->bent[b];
            for (int r = 0; r < 3; r++) rot_about(m + 4 * r, K, c, s);
            float t[3] = {m[12] - pv[0], m[13] - pv[1], m[14] - pv[2]};
            rot_about(t, K, c, s);
            m[12] = pv[0] + t[0]; m[13] = pv[1] + t[1]; m[14] = pv[2] + t[2];
        }
    }
    // diagnostics: COOP_BEND_DRY=1 measures + logs but leaves the anim pose untouched
    static int dry = -1;
    if (dry < 0) { char e[8] = {0}; dry = GetEnvironmentVariableA("COOP_BEND_DRY", e, sizeof e) && e[0] == '1'; if (dry) logf("coop: bend: DRY (measure only)"); }
    if (dry) { memcpy(cc->bent, cur, sz); return fresh; }   // bent == what the buffer holds -> next anim write re-saves
    memcpy((void *)(uintptr_t)rb, cc->bent, sz);
    return fresh;
}
// clone health (owner: "the clone in the Naszran foundry does not follow his player's aim")
static volatile uint32_t g_bend_bp_calls, g_bend_draw_fresh, g_bend_draw_calls;
static volatile float g_bend_p, g_bend_tw = 1000.0f;
static volatile uint32_t g_clone_ix;   // the clone rig's bone index table (learned in the skin hook)
static volatile int g_clone_nb;        // its used-bone count (25 for the soldier rig)
static volatile float g_conv_pitch; static volatile bool g_conv_ok;   // crosshair-point aim (coop_converged_aim)
static float coop_bend_pitch(void) { return g_cfg.aim_test_deg != 0.0f ? g_cfg.aim_test_deg : (g_conv_ok ? g_conv_pitch : g_aim_pitch_deg); }
static uint32_t coop_clone_anim_block(void)
{
    uint32_t A = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x328) : 0;
    if (A < 0x80000000 || A >= 0x84000000) return 0;
    uint32_t blk = *(volatile uint32_t *)(uintptr_t)(A + 0x54);
    return (blk >= 0x80000000 && blk < 0x84000000) ? blk : 0;
}

// EARLY BEND (helmet fix): the owner saw the helmet float - it is placed from the head
// bone during the anim update, before the draw-time bend. FUN_00097410 = the character
// bone-hierarchy pass (hw91 read-watch on bone 18: 0x97796/0x9789a + its 4x4 helper
// 0x9c940), called from 0x91ed5 / 0x91ee7. Those two calls are re-pointed to a wrapper:
// run the original, then bend BOTH of the clone's double-buffered arrays ([blk+0x50],
// [blk+0x54]) - the cache makes an unchanged buffer a no-op. Everything downstream
// (attachments, skin upload) then sees the bent pose.
extern "C" void coop_after_bonepass(void)
{
    // --rec4-test (drawn-gun hunt): recprobe showed the gun is a separate RIGID object
    // (the body floated, the gun stayed). rec4 (A+0x40; hand height, yawed 90 deg) is the
    // candidate gun frame: lift its WORLD translation (+0x70..+0x78) by 0.5 m right after
    // the bone pass; a saved/bent guard keeps it from accumulating.
    if (g_cfg.rec4_test && g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) {
        uint32_t A = *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x328);
        uint32_t R4 = (A >= 0x80000000 && A < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(A + 0x40) : 0;
        if (R4 >= 0x80000000 && R4 < 0x84000000) {
            static float saved_y, bent_y; static int have;
            float cur = *(volatile float *)(uintptr_t)(R4 + 0x74);
            if (!have || cur != bent_y) { saved_y = cur; have = 1; }
            bent_y = saved_y + 0.5f;
            *(volatile float *)(uintptr_t)(R4 + 0x74) = bent_y;
            static uint32_t rn;
            if ((rn++ % 600) == 0) logf("coop: rec4-test: R4 %08x world y %.2f -> %.2f", R4, saved_y, bent_y);
        }
    }
    if (!(g_cfg.skin_bones == 1 && !g_cfg.bend_at_draw)) return;
    uint32_t blk = coop_clone_anim_block();
    if (!blk || !g_clone_ix || g_clone_nb <= 0) return;
    g_bend_bp_calls++;
    float p = coop_bend_pitch();
    // yaw twist = aim yaw - the render root's facing (+0x110 row 2 = model +Z in world;
    // yaw = atan2(x, z), the same convention as the link yaw). Positive = toward model
    // +X (= root row 0, yaw + 90), matching Rodrigues about +Y.
    float tw = 1000.0f, ryaw = 0.0f;                 // 1000 = no twist (aim not live yet)
    if (g_aim_link) {
        // nzfix231 (Naszran): +0x130 is the ROOT BONE's forward (= body x pelvis bone); the
        // Naszran soldier's pelvis is turned 51 deg and tilted 24 deg off the body, so the
        // twist was off by that much. The bones live in the BODY's frame (pose rows
        // +0x70/+0x80/+0x90, the draw's model matrix) -> take the yaw from the body's F row.
        const volatile float *F = (const volatile float *)(uintptr_t)(g_p10_gdst + 0x90);
        if (fabsf(F[0]) + fabsf(F[2]) > 1e-3f) {
            ryaw = 57.2957795f * atan2f(F[0], F[2]);
            static float yofs = 1000.0f;             // diagnostics: COOP_AIM_YAW_OFS=<deg> turns the aim (side-on checks)
            if (yofs > 999.0f) { char e[16] = {0}; yofs = GetEnvironmentVariableA("COOP_AIM_YAW_OFS", e, sizeof e) ? (float)atof(e) : 0.0f; }
            float cy, cp;
            bool conv = coop_converged_aim(&cy, &cp);
            if (conv) g_conv_pitch = cp;
            g_conv_ok = conv;
            tw = (conv ? cy : g_aim_yaw_deg) + yofs - ryaw;        // wanted aim, in MODEL yaw
            while (tw > 180.0f) tw -= 360.0f;
            while (tw < -180.0f) tw += 360.0f;
        }
    }
    g_bend_p = p; g_bend_tw = tw;   // the draw-time safety net re-applies exactly this
    coop_bend_array(*(volatile uint32_t *)(uintptr_t)(blk + 0x50), g_clone_ix, g_clone_nb, p, tw);
    coop_bend_array(*(volatile uint32_t *)(uintptr_t)(blk + 0x54), g_clone_ix, g_clone_nb, p, tw);
    {   // the gun hand in world space (bone 24 through the logic transform: pose rows +0x70, feet +0xA0)
        // -> the origin of the crosshair-point convergence next frame
        uint32_t rbh = *(volatile uint32_t *)(uintptr_t)(blk + 0x50);
        if (rbh >= 0x80000000 && rbh < 0x84000000 && g_clone_nb > 24) {
            const volatile float *h24 = (const volatile float *)(uintptr_t)(rbh + 24 * 0x40);
            const volatile float *P = (const volatile float *)(uintptr_t)(g_p10_gdst + 0x70);
            const volatile float *Fe = (const volatile float *)(uintptr_t)(g_p10_gdst + 0xA0);
            for (int k = 0; k < 3; k++) g_hand_world[k] = h24[12] * P[k] + h24[13] * P[4 + k] + h24[14] * P[8 + k] + Fe[k];
            g_hand_ok = 1; g_hand_ms = GetTickCount64();
        }
    }
    {   // COOP_GUN_MARK=1: world points for on-screen markers (ground-truth check of the gun axis)
        static int gm = -1;
        if (gm < 0) { char e[8] = {0}; gm = GetEnvironmentVariableA("COOP_GUN_MARK", e, sizeof e) && e[0] == '1'; }
        uint32_t rb = *(volatile uint32_t *)(uintptr_t)(blk + 0x50);
        if (gm && rb >= 0x80000000 && rb < 0x84000000) {
            const volatile float *R = (const volatile float *)(uintptr_t)(g_p10_gdst + 0x110);   // root rows L U F T
            const volatile float *b24 = (const volatile float *)(uintptr_t)(rb + 24 * 0x40);
            const volatile float *b11 = (const volatile float *)(uintptr_t)(rb + 11 * 0x40);
            // mark179: +0x110 = the ROOT bone's world matrix (origin at the hips, 0.94 m up);
            // model bones have the feet at the origin -> origin = T - 0.94 * U row
            auto w = [&](float x, float y, float z, volatile float *o) {
                for (int k = 0; k < 3; k++) o[k] = x * R[k] + (y - 0.94f) * R[4 + k] + z * R[8 + k] + R[12 + k];
            };
            float gx = b24[0] + g_gun_k1 * b24[4] + g_gun_k2 * b24[8], gy = b24[1] + g_gun_k1 * b24[5] + g_gun_k2 * b24[9],
                  gz = b24[2] + g_gun_k1 * b24[6] + g_gun_k2 * b24[10];   // same axis as the bend
            w(b24[12], b24[13], b24[14], &g_gunmark[0]);
            w(b24[12] + 0.7f * gx, b24[13] + 0.7f * gy, b24[14] + 0.7f * gz, &g_gunmark[3]);
            {   // blue = the same hand through the LOGIC transform (pose rows +0x70 + feet +0xA0)
                const volatile float *P = (const volatile float *)(uintptr_t)(g_p10_gdst + 0x70);
                const volatile float *Fe = (const volatile float *)(uintptr_t)(g_p10_gdst + 0xA0);
                for (int k = 0; k < 3; k++) g_gunmark[9 + k] = b24[12] * P[k] + b24[13] * P[4 + k] + b24[14] * P[8 + k] + Fe[k];
            }
            static float yofs2 = 1000.0f;
            if (yofs2 > 999.0f) { char e[16] = {0}; yofs2 = GetEnvironmentVariableA("COOP_AIM_YAW_OFS", e, sizeof e) ? (float)atof(e) : 0.0f; }
            float ay = (tw < 999.0f ? tw + ryaw : g_aim_yaw_deg + yofs2) * 0.0174532925f, ap = p * 0.0174532925f;   // the bend's own target (converged aim in co-op)
            g_gunmark[6] = g_gunmark[0] + 0.7f * sinf(ay) * cosf(ap);
            g_gunmark[7] = g_gunmark[1] - 0.7f * sinf(ap);
            g_gunmark[8] = g_gunmark[2] + 0.7f * cosf(ay) * cosf(ap);
            g_gunmark_on = 1;
            {   // COOP_GUN_CAL=1: log the clone draw's model->clip rows + bone 24 (model space) once a
                // second - with the drawn muzzle/grip pixels read off a frame, solve the barrel axis
                static int cal = -1; static uint64_t lc;
                if (cal < 0) { char e[8] = {0}; cal = GetEnvironmentVariableA("COOP_GUN_CAL", e, sizeof e) && e[0] == '1'; }
                static uint32_t lfr = 0xFFFFFFFF;
                uint32_t fr = g_frame_count.load();
                (void)lc;
                if (cal && g_clone_mvp_ok && (fr % 60) == 59 && fr != lfr) {   // the bone pass of the frame that is captured next
                    lfr = fr;
                    logf("coop: guncal: frame %u mvp %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f | b24 %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f",
                         g_frame_count.load(),
                         g_clone_mvp[0], g_clone_mvp[1], g_clone_mvp[2], g_clone_mvp[3], g_clone_mvp[4], g_clone_mvp[5], g_clone_mvp[6], g_clone_mvp[7],
                         g_clone_mvp[8], g_clone_mvp[9], g_clone_mvp[10], g_clone_mvp[11], g_clone_mvp[12], g_clone_mvp[13], g_clone_mvp[14], g_clone_mvp[15],
                         b24[0], b24[1], b24[2], b24[4], b24[5], b24[6], b24[8], b24[9], b24[10], b24[12], b24[13], b24[14]);
                }
            }
            {   // model-space copies (projected with the clone draw's own model->clip matrix)
                float my = (tw < 999.0f ? tw : 0.0f) * 0.0174532925f, mp = p * 0.0174532925f;
                float hm[3] = {b24[12], b24[13], b24[14]};
                for (int k = 0; k < 3; k++) g_gunmodel[k] = hm[k];
                g_gunmodel[3] = hm[0] + 0.7f * gx; g_gunmodel[4] = hm[1] + 0.7f * gy; g_gunmodel[5] = hm[2] + 0.7f * gz;
                g_gunmodel[6] = hm[0] + 0.7f * sinf(my) * cosf(mp); g_gunmodel[7] = hm[1] - 0.7f * sinf(mp);
                g_gunmodel[8] = hm[2] + 0.7f * cosf(my) * cosf(mp);
                g_gunmodel[9] = b11[12]; g_gunmodel[10] = b11[13]; g_gunmodel[11] = b11[14];
                for (int ax = 0; ax < 3; ax++)       // axis probes: +r0 -r0 +r1 -r1 +r2 -r2
                    for (int sg = 0; sg < 2; sg++)
                        for (int k = 0; k < 3; k++)
                            g_gunmodel[12 + 3 * (2 * ax + sg) + k] = hm[k] + (sg ? -0.5f : 0.5f) * b24[4 * ax + k];
            }
            static uint32_t gml;
            if ((gml++ % 120) == 0) {
                const volatile float *Fe = (const volatile float *)(uintptr_t)(g_p10_gdst + 0xA0);
                const volatile float *P = (const volatile float *)(uintptr_t)(g_p10_gdst + 0x70);
                const volatile float *B0 = (const volatile float *)(uintptr_t)rb;
                logf("coop: gunmark: R110 %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f",
                     R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[11], R[12], R[13], R[14], R[15]);
                logf("coop: gunmark: pose70 %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | feet %.2f %.2f %.2f | b24 %.2f %.2f %.2f b0 %.2f %.2f %.2f -> hand %.2f %.2f %.2f",
                     P[0], P[1], P[2], P[4], P[5], P[6], P[8], P[9], P[10], Fe[0], Fe[1], Fe[2], b24[12], b24[13], b24[14],
                     B0[12], B0[13], B0[14], g_gunmark[0], g_gunmark[1], g_gunmark[2]);
            }
        }
    }
    static uint32_t n;
    if ((n++ % 120) == 0) logf("coop: bend EARLY (after bone pass): arrays %08x %08x pitch %.1f | aim yaw %.1f root yaw %.1f -> model aim %.1f, gun (model) %.1f, gun pitch anim %.1f",
                               *(volatile uint32_t *)(uintptr_t)(blk + 0x50), *(volatile uint32_t *)(uintptr_t)(blk + 0x54), p,
                               g_aim_yaw_deg, ryaw, tw, g_arm_yaw_model, g_gun_pitch_anim);
}
volatile uint32_t g_bonepass_fn = 0x00097410, g_bonepass_ret;
void coop_thunk_bonepass();
__declspec(naked) void coop_thunk_bonepass()
{
    __asm {
        pop dword ptr [g_bonepass_ret]      // caller's return address; the stack now = its args
        call dword ptr [g_bonepass_fn]      // the original pass, args/regs untouched
        pushfd
        pushad
        call coop_after_bonepass
        popad                               // EAX = the original return value
        popfd
        push dword ptr [g_bonepass_ret]
        ret
    }
}
static void install_bonepass_hook(void)
{
    const uint32_t sites[2] = {0x00091ED5, 0x00091EE7};
    g_bonepass_fn = xbe_va(0x00097410);
    for (uint32_t s : sites) {
        uint32_t a = xbe_va(s);
        auto *code = (uint8_t *)(uintptr_t)a;
        int32_t rel; memcpy(&rel, code + 1, 4);
        if (code[0] != 0xE8 || a + 5 + rel != g_bonepass_fn) { logf("coop: bone-pass call %08x mismatch; skipped", s); continue; }
        int32_t nrel = (int32_t)((uintptr_t)coop_thunk_bonepass - (a + 5));
        memcpy(code + 1, &nrel, 4);
        logf("coop: bone-pass call %08x re-pointed to the early-bend wrapper", s);
    }
}

extern "C" void coop_on_skin(void)
{
    uint32_t rec = g_skin_rec;
    if (rec < 0x10000 || rec >= 0x84000000) return;
    // Skin records are a per-FRAME draw-list pool; bone arrays are per character and the
    // clone's are DOUBLE-BUFFERED ([blk+0x50] / [blk+0x54]). The skin hook now learns the
    // clone rig's index table + count (for the early bend) and bends here only in the
    // old draw-time mode (--bend-at-draw) or for the all-soldier test (--skin-bones ffffffff).
    uint32_t rb = *(volatile uint32_t *)(uintptr_t)(rec + 8);
    int nb = *(volatile int8_t *)(uintptr_t)(rec + 0x18);
    uint32_t ix = *(volatile uint32_t *)(uintptr_t)(rec + 0xC);
    uint32_t blk = coop_clone_anim_block();
    bool is_clone = blk && (rb == *(volatile uint32_t *)(uintptr_t)(blk + 0x50) || rb == *(volatile uint32_t *)(uintptr_t)(blk + 0x54));
    // --rec-probe <0|1> (drawn-gun hunt): each soldier draws TWO skin records on one
    // skeleton (+0x1C zero / non-zero). For the clone's record of the chosen kind, point
    // rec+8 at a private copy of the bones shifted +0.8 m up (records are rebuilt every
    // frame; FUN_0016f420 reads rec+8 right after this hook) -> that mesh floats.
    if (is_clone && g_cfg.rec_probe >= 0) {
        uint32_t k1c = *(volatile uint32_t *)(uintptr_t)(rec + 0x1C);
        if ((g_cfg.rec_probe == 0) == (k1c == 0)) {
            static float shifted[66][16];                   // [0] = the idx -1 matrix
            memcpy(shifted, (const void *)(uintptr_t)(rb - 0x40), sizeof shifted);
            for (int b = 0; b < 66; b++) shifted[b][13] += 0.8f;
            *(volatile uint32_t *)(uintptr_t)(rec + 8) = (uint32_t)(uintptr_t)&shifted[1];
            static uint32_t pn;
            if ((pn++ % 300) == 0) logf("coop: rec-probe: clone rec %08x (+1C %08x) bones shifted +0.8 m", rec, k1c);
        }
    }
    if (is_clone && ix >= 0x80000000 && ix < 0x84000000 && nb > 0 && (ix != g_clone_ix || nb != g_clone_nb)) {
        logf("coop: bend: clone rig %s: idx %08x n %d (was %08x n %d; arrays %08x / %08x)", g_clone_ix ? "CHANGED" : "learned",
             ix, nb, g_clone_ix, g_clone_nb,
             *(volatile uint32_t *)(uintptr_t)(blk + 0x50), *(volatile uint32_t *)(uintptr_t)(blk + 0x54));
        g_clone_ix = ix; g_clone_nb = nb;
    }
    // SAFETY NET: bend the clone's array at draw time as well. A no-op when the bone pass
    // already bent it (same inputs); when the clone was animated through another path
    // (no bone-pass call) the pose arrives fresh here and gets the aim bend anyway.
    if (is_clone && g_cfg.skin_bones == 1 && !g_cfg.bend_at_draw && g_clone_ix && ix == g_clone_ix) {
        g_bend_draw_calls++;
        if (coop_bend_array(rb, ix, nb, coop_bend_pitch(), g_bend_tw)) g_bend_draw_fresh++;
    }
    if ((g_cfg.skin_bones == 0xFFFFFFFFu) || (g_cfg.skin_bones == 1 && g_cfg.bend_at_draw && is_clone) ||
        (g_cfg.skin_bones > 1 && g_cfg.skin_bones != 0xFFFFFFFFu && rb == g_cfg.skin_bones))
        coop_bend_array(rb, ix, nb, coop_bend_pitch());
    // --helmet-scan: mark the clone's body draw for draw.cpp (~60 s, live park)
    if (is_clone) { static uint32_t mk; if (++mk > 900) g_coop_skin_mark = 1; }
    if (is_clone && g_gunmark_on) g_coop_skin_mark = 2;   // COOP_GUN_MARK: capture this draw's model->clip rows
    (void)coop_bend_upper;
    static uint32_t seen[8]; static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == rec) return;
    if (nseen >= 8) return;
    seen[nseen++] = rec;
    uint32_t bones = *(volatile uint32_t *)(uintptr_t)(rec + 8);
    uint32_t idx = *(volatile uint32_t *)(uintptr_t)(rec + 0xC);
    int n = *(volatile int8_t *)(uintptr_t)(rec + 0x18);
    uint32_t cloneA = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x328) : 0;
    logf("coop: skin rec %08x: w0 %08x w1 %08x bones %08x idx %08x n %d +10 %08x +14 %08x +1C %08x | clone body %08x A %08x pos %.1f %.1f %.1f",
         rec, *(volatile uint32_t *)(uintptr_t)rec, *(volatile uint32_t *)(uintptr_t)(rec + 4), bones, idx, n,
         *(volatile uint32_t *)(uintptr_t)(rec + 0x10), *(volatile uint32_t *)(uintptr_t)(rec + 0x14),
         *(volatile uint32_t *)(uintptr_t)(rec + 0x1C), g_p10_gdst, cloneA,
         g_coop_clone_pos[0], g_coop_clone_pos[1], g_coop_clone_pos[2]);
    if (bones < 0x10000 || bones >= 0x84000000 || idx < 0x10000 || idx >= 0x84000000 || n <= 0 || n > 64) return;
    for (int i = 0; i < n; i++) {
        int bi = *(volatile int *)(uintptr_t)(idx + 4 * i);
        // skin82: soldier rig index tables start with -1 = the matrix just BEFORE
        // [rec+8] (FUN_0016f420 multiplies by 0x40 unchecked) - valid
        if (bi < -1 || bi > 128) { logf("coop: skin rec %08x: idx[%d] = %d (odd) - stop", rec, i, bi); break; }
        const volatile float *m = (const volatile float *)(uintptr_t)(bones + bi * 0x40);
        logf("coop: skin rec %08x: [%2d] bone %2d t %+8.2f %+8.2f %+8.2f  X %+.2f %+.2f %+.2f  Y %+.2f %+.2f %+.2f",
             rec, i, bi, m[12], m[13], m[14], m[0], m[1], m[2], m[4], m[5], m[6]);
    }
}
void coop_thunk_skin();
__declspec(naked) void coop_thunk_skin()
{
    __asm {
        mov dword ptr [g_skin_rec], eax
        pushfd
        pushad
        call coop_on_skin
        popad
        popfd
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        jmp dword ptr [g_skin_resume]
    }
}

// ---- HIT SYNC (owner: "the joiner shares his hits to the host": walls, enemies,
// destructibles). hit114: FUN_001e2b90 never runs for these guns (fire type 0); the
// real shot = FUN_001dbd50(shot ctx) (probe: 23 calls in a 1.5 s burst, only while
// firing; ctx[1] = W, ctx[3] = stats: [0] pellets, [5] range). Per pellet: origin =
// the shooter transform's position, then 0x1DC008 CALL FUN_001cef40(&origin) (EDI =
// &dir, ESI = ctx, DL = player flag, RET 4) adds the spread and writes the unit dir;
// the raycast + damage + impacts follow (player branch FUN_001ca170 / 001db7b0, NPC
// branch FUN_001daff0 / 001d8d50). That CALL is re-pointed to a wrapper: after the
// original, PLAYER pellets are queued for the network (origin + dir) and CLONE pellets
// are overwritten with the next ray received from the other machine, so the clone's
// bullets on this side fly exactly where the remote player's real bullets flew.
volatile uint32_t g_pel_org, g_pel_dir, g_pel_ctx, g_pel_fn;
extern "C" void coop_on_pellet(void);
__declspec(naked) void coop_thunk_pellet()
{
    __asm {
        push dword ptr [esp + 4]            // re-push &origin
        call dword ptr [g_pel_fn]           // FUN_001cef40 (RET 4 pops the copy)
        pushad
        mov eax, dword ptr [esp + 0x20 + 4] // &origin (our caller's arg)
        mov dword ptr [g_pel_org], eax
        mov dword ptr [g_pel_dir], edi
        mov dword ptr [g_pel_ctx], esi
        call coop_on_pellet
        popad
        ret 4
    }
}
extern "C" void coop_on_pellet(void)
{
    uint32_t ctx = g_pel_ctx;
    if (ctx < 0x80000000 || ctx >= 0x84000000) return;
    uint32_t W = *(volatile uint32_t *)(uintptr_t)(ctx + 4);
    if (W < 0x80000000 || W >= 0x84000000) return;
    uint32_t owner = *(volatile uint32_t *)(uintptr_t)(W + 0xF0);
    volatile float *o = (volatile float *)(uintptr_t)g_pel_org;
    volatile float *d = (volatile float *)(uintptr_t)g_pel_dir;
    uint32_t ctrl = g_coop_last_ctrl;
    uint32_t pobj = (ctrl >= 0x10000 && ctrl < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C) : 0;
    static uint32_t ln;
    // PHASE 3: enemy bullets. Host: record every enemy pellet (sent as CLER). Joiner: a
    // puppet's pellet takes the next host ray for that body (fresh < 400 ms), so a host
    // bullet that would hit the joiner's clone hits the joiner's real player here.
    if ((g_cfg.coop_role || g_cfg.enemy_test_clone) && owner && is_enemy(owner, pobj)) {
        if (g_cfg.coop_role == 0 && g_cfg.enemy_test_clone) {
            // solo variant: aim at the LOCAL player's chest - do enemy bullets damage a player at all?
            const volatile float *pf = (const volatile float *)(uintptr_t)(pobj + 0xA0);
            float t[3] = {pf[0] - o[0], pf[1] + 1.3f - o[1], pf[2] - o[2]};
            float l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (l > 0.1f) {
                // en128: from the dock the deck blocks the line (and 4 m back along it is still
                // under the deck floor) -> a HORIZONTAL pellet at chest height from 4 m away
                float h = sqrtf(t[0] * t[0] + t[2] * t[2]);
                if (h < 0.1f) h = 1.0f;
                d[0] = t[0] / h; d[1] = 0.0f; d[2] = t[2] / h;
                o[0] = pf[0] - 4.0f * d[0]; o[1] = pf[1] + 1.3f; o[2] = pf[2] - 4.0f * d[2];
            }
        }
        if (g_cfg.coop_role == 1 && g_cfg.enemy_test_clone && g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) {
            // test: aim this enemy pellet at the joiner's clone chest (the clone is immortal here)
            const volatile float *cf = (const volatile float *)(uintptr_t)(g_p10_gdst + 0xA0);
            float t[3] = {cf[0] - o[0], cf[1] + 1.3f - o[1], cf[2] - o[2]};
            float l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (l > 0.1f) { d[0] = t[0] / l; d[1] = t[1] / l; d[2] = t[2] / l; }
        }
        if (g_cfg.coop_role == 1 && g_enray_out_n < 64) {
            EnemyRay &e = g_enray_out[g_enray_out_n++];
            e.body = owner;
            for (int k = 0; k < 3; k++) { e.r.o[k] = o[k]; e.r.d[k] = d[k]; }
        } else if (g_cfg.coop_role == 2) {
            uint64_t now = GetTickCount64();
            int found = -1;
            for (int i = g_enray_r; i != g_enray_w; i++) {
                if (now - g_enray_ms[i & 127] > 400) { if (i == g_enray_r) g_enray_r++; continue; }
                if (g_enray_in[i & 127].body == owner) { found = i; break; }
            }
            if (found >= 0) {
                const ShotRay &r = g_enray_in[found & 127].r;
                static uint32_t el;
                if (el++ < 30) logf("coop: enemy ray: puppet %08x pellet o %.2f %.2f %.2f d %.3f %.3f %.3f -> HOST o %.2f %.2f %.2f d %.3f %.3f %.3f",
                                    owner, o[0], o[1], o[2], d[0], d[1], d[2], r.o[0], r.o[1], r.o[2], r.d[0], r.d[1], r.d[2]);
                for (int k = 0; k < 3; k++) { o[k] = r.o[k]; d[k] = r.d[k]; }
                g_enray_in[found & 127].body = 0;            // consumed
                while (g_enray_r != g_enray_w && g_enray_in[g_enray_r & 127].body == 0) g_enray_r++;
                g_enray_stats[2]++;
            } else g_enray_stats[3]++;
        }
    }
    if (owner && owner != pobj && owner != g_p10_gdst && pobj >= 0x80000000 && pobj < 0x84000000) {
        // phase 3 diagnostics: an ENEMY pellet - how close does its ray pass the player's chest?
        static uint32_t en;
        if (en++ < 80) {
            const volatile float *pf = (const volatile float *)(uintptr_t)(pobj + 0xA0);
            float c[3] = {pf[0] - o[0], pf[1] + 1.3f - o[1], pf[2] - o[2]};
            float t = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
            float q[3] = {c[0] - t * d[0], c[1] - t * d[1], c[2] - t * d[2]};
            logf("coop: enemy pellet %08x (team %u) o %.1f %.1f %.1f d %.3f %.3f %.3f | passes the player chest at %.2f m (along %.1f m)",
                 owner, *(volatile uint32_t *)(uintptr_t)(owner + 0x39C), o[0], o[1], o[2], d[0], d[1], d[2],
                 sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), t);
        }
    }
    if (owner && owner == pobj) {
        if (g_shot_out_n < 64) {
            ShotRay &r = g_shot_out[g_shot_out_n++];
            for (int k = 0; k < 3; k++) { r.o[k] = o[k]; r.d[k] = d[k]; }
        }
        if (ln++ < 40) logf("coop: hit sync: PLAYER pellet o %.2f %.2f %.2f d %.3f %.3f %.3f", o[0], o[1], o[2], d[0], d[1], d[2]);
    } else if (owner && (owner == g_p10_gdst || coop_is_self(owner)) && g_kill_ray_on) {
        for (int k = 0; k < 3; k++) { o[k] = g_kill_ray.o[k]; d[k] = g_kill_ray.d[k]; }
        g_kill_ray_on = 0;
    } else if (owner && owner == g_p10_gdst) {
        uint64_t now = GetTickCount64();
        while (g_shot_in_r != g_shot_in_w && now - g_shot_in_ms[g_shot_in_r & 63] > 400) { g_shot_in_r++; g_shot_stats[3]++; }  // stale
        if (g_shot_in_r != g_shot_in_w && !g_cfg.shot_test_npc) {
            const ShotRay &r = g_shot_in[g_shot_in_r & 63]; g_shot_in_r++;
            if (ln++ < 40) logf("coop: hit sync: CLONE pellet o %.2f %.2f %.2f d %.3f %.3f %.3f -> REMOTE o %.2f %.2f %.2f d %.3f %.3f %.3f",
                                o[0], o[1], o[2], d[0], d[1], d[2], r.o[0], r.o[1], r.o[2], r.d[0], r.d[1], r.d[2]);
            for (int k = 0; k < 3; k++) { o[k] = r.o[k]; d[k] = r.d[k]; }
            g_shot_stats[2]++;
        } else if (g_cfg.shot_test_npc) {
            if (g_shot_in_r != g_shot_in_w) { g_shot_in_r++; g_shot_stats[2]++; }   // test owns the aim; still drain the queue
            // solo test: aim this pellet at the nearest live ENEMY (team != the clone's,
            // health > 0) chest; log its health so the damage is visible in the log
            uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
            uint32_t arr = (mgr >= 0x80000000 && mgr < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(mgr + 0x79BC) : 0;
            uint32_t best = 0; float bd = 1e30f;
            for (int i = 0; arr >= 0x80000000 && arr < 0x84000000 && i < 16; i++) {
                uint32_t b = *(volatile uint32_t *)(uintptr_t)(arr + 4 * i);
                if (b < 0x80000000 || b >= 0x84000000 || b == owner) continue;
                if (*(volatile uint32_t *)(uintptr_t)(b + 0x39C) == *(volatile uint32_t *)(uintptr_t)(owner + 0x39C)) continue;
                float hp = *(volatile float *)(uintptr_t)(b + 0x2F0);
                if (!(hp > 0.0f) || hp > 1e30f) continue;
                const volatile float *f = (const volatile float *)(uintptr_t)(b + 0xA0);
                float dx = f[0] - o[0], dz = f[2] - o[2], dd = dx * dx + dz * dz;
                if (dd < bd) { bd = dd; best = b; }
            }
            if (best) {
                const volatile float *f = (const volatile float *)(uintptr_t)(best + 0xA0);
                float t[3] = {f[0] - o[0], f[1] + 1.2f - o[1], f[2] - o[2]};
                float l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                if (l > 0.1f) { d[0] = t[0] / l; d[1] = t[1] / l; d[2] = t[2] / l; }
                static uint32_t tn;
                if (tn++ < 60) logf("coop: shot-test: clone pellet -> enemy %08x at %.1f m, its health %.1f", best, l,
                                    *(volatile float *)(uintptr_t)(best + 0x2F0));
            }
        } else {
            if (ln++ < 40) logf("coop: hit sync: CLONE pellet with no remote ray queued (own aim) o %.2f %.2f %.2f d %.3f %.3f %.3f",
                                o[0], o[1], o[2], d[0], d[1], d[2]);
        }
    }
}
static void coop_shot_recv(const ShotRay *rays, int n)
{
    for (int i = 0; i < n; i++) {
        if (g_shot_in_w - g_shot_in_r >= 64) g_shot_in_r++;   // full: drop the oldest
        g_shot_in_ms[g_shot_in_w & 63] = GetTickCount64();
        g_shot_in[g_shot_in_w & 63] = rays[i]; g_shot_in_w++;
        g_shot_stats[1]++;
    }
}
// CATCH-UP: the host runs slower than the joiner in the two-copy setup (hit115: the
// clone fired ~8 pellets/s vs the joiner's 15), so rays the clone's own fire did not
// use within 60 ms are fired through the game's shot routine FUN_001dbd50(ammo ctx)
// (stdcall RET 4, no register inputs) with the clone's CURRENT ammo object - the
// pellet hook then swaps in the queued ray (raycast, damage, impacts, sound included).
volatile uint32_t g_shot_fn, g_shot_ctx_arg;
__declspec(naked) void coop_thunk_shot()
{
    __asm {
        pushad
        push dword ptr [g_shot_ctx_arg]
        call dword ptr [g_shot_fn]          // RET 4
        popad
        ret
    }
}
// ENEMY RAY CATCH-UP (play 20261113_213151: "enemies inside a building didn't sync their firing, standing
// there doing nothing on the joiner"): puppets fire through their OWN weapon state machine (link +0x41 = the
// host's trigger) and each pellet takes the host's ray; 1145 of 2865 host rays were never taken (no local
// shot - e.g. a puppet whose weapon logic did not run). A host ray still waiting after 100 ms makes its puppet
// fire once through FUN_001dbd50 (the clone catch-up's call): the pellet hook then uses that ray (muzzle
// flash, sound, tracer, the host's direction). 3 shots per frame, live puppets only.
static void coop_enray_catchup(void)
{
    if (g_cfg.coop_role != 2) return;
    if (!g_shot_fn) g_shot_fn = xbe_va(0x001DBD50);
    uint64_t now = GetTickCount64();
    static uint32_t nforced;
    int budget = 3;
    for (int i = g_enray_r; i != g_enray_w && budget > 0; i++) {
        EnemyRay &e = g_enray_in[i & 127];
        uint32_t b = e.body;
        if (!b) continue;
        uint64_t age = now - g_enray_ms[i & 127];
        if (age < 100 || age > 400) continue;
        bool pup = false;
        for (int k = 0; k < g_pl_n; k++) if (g_pl[k].body == b && g_pl[k].link) pup = true;
        float hp = 0; uint32_t life = 1;
        if (!pup || !coop_safe_copy(&hp, b + 0x2F0, 4) || !(hp > 0.0f) || !coop_safe_copy(&life, b + 0x384, 4) || life != 0) continue;
        uint32_t W = 0, ctx = 0;
        if (!coop_safe_copy(&W, b + 0x278 + 0x28, 4) || W < 0x80000000 || W >= 0x84000000) continue;
        if (!coop_safe_copy(&ctx, W + 0xF4, 4) || ctx < 0x80000000 || ctx >= 0x84000000) continue;
        int16_t clip = *(volatile int16_t *)(uintptr_t)(ctx + 0x18);
        if (clip < 2) *(volatile int16_t *)(uintptr_t)(ctx + 0x18) = 2;   // never a dry pull
        g_shot_ctx_arg = ctx;
        coop_thunk_shot();
        budget--; nforced++;
        if (nforced <= 30 || (nforced % 200) == 0)
            logf("coop: enemy ray: puppet %08x had not fired the host's shot after %llu ms -> forced shot (%u forced; ray %s)",
                 b, (unsigned long long)age, nforced, e.body == 0 ? "used" : "still waiting");
    }
}
static void coop_shot_catchup(uint32_t body)
{
    if (body < 0x80000000 || body >= 0x84000000) return;
    uint32_t W = *(volatile uint32_t *)(uintptr_t)(body + 0x278 + 0x28);
    if (W < 0x80000000 || W >= 0x84000000) return;
    uint32_t ctx = *(volatile uint32_t *)(uintptr_t)(W + 0xF4);
    if (ctx < 0x80000000 || ctx >= 0x84000000) return;
    if (!g_shot_fn) g_shot_fn = xbe_va(0x001DBD50);
    uint64_t now = GetTickCount64();
    for (int k = 0; k < 4 && g_shot_in_r != g_shot_in_w; k++) {
        while (g_shot_in_r != g_shot_in_w && now - g_shot_in_ms[g_shot_in_r & 63] > 400) { g_shot_in_r++; g_shot_stats[3]++; }
        if (g_shot_in_r == g_shot_in_w || (!g_shot_rays_only && now - g_shot_in_ms[g_shot_in_r & 63] < 60)) break;
        g_shot_ctx_arg = ctx;
        int before = g_shot_in_r;
        // rays-only: this call IS the shot - make sure the clip cannot block it (the fire block puts the
        // peer's clip count back right after)
        if (g_shot_rays_only && *(volatile int16_t *)(uintptr_t)(ctx + 0x18) < 2) *(volatile int16_t *)(uintptr_t)(ctx + 0x18) = 2;
        coop_thunk_shot();
        g_shot_stats[4]++;
        if (g_shot_in_r == before) {   // the pellet did not take the ray (en121: test mode fired 1128 replays) -> drop it, stop
            g_shot_in_r++; g_shot_stats[3]++;
            static int warned;
            if (!warned++) logf("coop: hit sync: catch-up shot used no ray - dropped, catch-up stopped this frame");
            break;
        }
    }
}
// ---- ENEMY SYNC, phase 1 (owner: host-authoritative enemies). The HOST owns enemy
// health and death; the JOINER's enemies take no damage of their own and die when the
// host says so. Pairing: both copies allocate pawn bodies in the same order (same
// level, same spawn order), so the body ADDRESS is the shared id - checked per entry
// (the joiner logs any id whose local body is not a live enemy).
//  - host: every 6 frames sends "CLEN" (magic 0x4E454C43, seq, n, n x {body, hp,
//    feet[3]}) for every pawn slot (mgr+0x79BC, 16) that is an enemy (team != the
//    player's, not the clone, not immortal).
//  - joiner: local enemies get body+0x3B0 = 1 (the game's own no-damage byte: tested by
//    FUN_001e74b0 / FUN_001dfa90, hit reactions still play). A host death (hp <= 0) for a
//    locally live body -> +0x3B0 = 0, hp = 0.01, and one pellet from the clone through
//    FUN_001dbd50 with the ray forced to the enemy's chest (0.6 m in front, from the
//    clone's side): the game's own bullet path kills it (death reaction, AI notify,
//    ragdoll). Retried every 250 ms (max 8) until the local hp reads 0.
static uint32_t enemy_slots(uint32_t out[16])
{
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
    uint32_t arr = (mgr >= 0x80000000 && mgr < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(mgr + 0x79BC) : 0;
    uint32_t n = 0;
    for (int i = 0; arr >= 0x80000000 && arr < 0x84000000 && i < 16; i++) {
        uint32_t b = *(volatile uint32_t *)(uintptr_t)(arr + 4 * i);
        if (b >= 0x80000000 && b < 0x84000000 && !coop_is_self(b)) out[n++] = b;   // the self model is no enemy
    }
    return n;
}
static bool is_enemy(uint32_t b, uint32_t pobj)
{
    if (b == g_p10_gdst || coop_is_self(b) || b == pobj || pobj < 0x80000000 || pobj >= 0x84000000) return false;
    if (*(volatile uint32_t *)(uintptr_t)b != 0x0028C1E8) return false;                       // pawn vtable
    if (*(volatile uint32_t *)(uintptr_t)(b + 0x39C) == *(volatile uint32_t *)(uintptr_t)(pobj + 0x39C)) return false;
    float hp = *(volatile float *)(uintptr_t)(b + 0x2F0);
    return hp < 1e30f;                                                                         // not an immortal squadmate
}
// JOINER PERCEPTION (day 9): the AI notices a shooter through the GUNSHOT BROADCAST in
// the weapon fire code FUN_001dbd50 (0x1dbe06), done only for a PLAYER-kind owner
// (+0xC4 == 2):  ECX = *0x2D1988 (AI registry; roster of 16 agents at +0x2B8C),
// EDI = the shooter's position (root matrix row 3, 16-byte aligned - MOVAPS),
// EAX = noise class (only 0xB = skip matters), stack arg = the shooter body (RET 4).
// FUN_001a22b0 -> FUN_0019fea0 per active agent (distance weight, same team weaker) ->
// FUN_00195450 -> FUN_0018c620 records the sighting in the agent's awareness list
// (class object +0x700, entities at +0x788 + k*0x20) -> the threat candidates -> the
// target. The joiner's clone is a soldier-kind body, so its shots never broadcast and
// the yard never noticed the joiner (ag197/200). Host: broadcast for the clone while the
// joiner's trigger is held (every 300 ms), exactly like the player's own shots.
volatile uint32_t g_bc_fn = 0x001A22B0, g_bc_reg, g_bc_src, g_bc_esp;
static uint64_t g_jshot_ms;   // the joiner's last gunshot broadcast (the aggro manager: nearby enemies engage the joiner)
__declspec(align(16)) static float g_bc_pos[4];
__declspec(naked) void coop_thunk_broadcast()
{
    __asm {
        pushad
        mov dword ptr [g_bc_esp], esp
        push dword ptr [g_bc_src]          // the shooter (FUN_001a22b0's stack arg, RET 4)
        mov ecx, dword ptr [g_bc_reg]
        lea edi, [g_bc_pos]
        xor eax, eax                       // noise class (anything but 0xB)
        call dword ptr [g_bc_fn]
        mov esp, dword ptr [g_bc_esp]
        popad
        ret
    }
}
static void coop_clone_gunshot(void)
{
    static int off = -1;
    if (off < 0) { char e[8] = {0}; off = GetEnvironmentVariableA("COOP_JOINER_NOISE", e, sizeof e) && e[0] == '0'; }
    if (off) return;
    uint32_t c = g_p10_gdst;
    if (c < 0x80000000 || c >= 0x84000000 || *(volatile uint32_t *)(uintptr_t)(c + 0x384) != 0) return;
    uint32_t reg = *(volatile uint32_t *)(uintptr_t)0x2D1988;
    if (reg < 0x80000000 || reg >= 0x84000000) return;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last < 70) return;   // ~ the SMG fire rate: the player broadcasts once per shot (pc207: 300 ms let the host outweigh the joiner)
    last = now;
    const volatile float *rt = (const volatile float *)(uintptr_t)(c + 0x140);   // root bone world translation
    g_bc_pos[0] = rt[0]; g_bc_pos[1] = rt[1]; g_bc_pos[2] = rt[2]; g_bc_pos[3] = 1.0f;
    g_bc_reg = reg; g_bc_src = c;
    coop_thunk_broadcast();
    g_jshot_ms = now;
    static int ln;
    if (ln++ < 10) logf("coop: perception: joiner gunshot broadcast at %.1f %.1f %.1f (clone %08x)", g_bc_pos[0], g_bc_pos[1], g_bc_pos[2], c);
}
// DOWNED = NOT A TARGET (day 9, owner: "fix enemies targeting downed player").
// The AI drops an entity whose life state +0x384 is non-zero: FUN_001954e0 (per-frame
// awareness) clears its tracked entity when [ent+0x384] != 0, and the target/threat code
// (FUN_001a0ba0, FUN_001a7bd0, FUN_001ab2f0) only takes entities with +0x384 == 0. Every
// player damage function gates on +0x384 == 0 too (FUN_001ed1e0/2b0/700/990), so a
// flagged player is also invulnerable. The game itself only writes 0 (spawn) and 4
// (removal, FUN_001d2dd0) there; nothing tests for 1 -> 1 = "downed".
// Flagged while downed: our own player (both roles) and, on the host, the joiner's
// clone while the joiner is downed (the AI runs on the host only). Only objects we
// flagged are restored to 0, and only if they still read 1.
// pc206: with the joiner's shots broadcast (coop_clone_gunshot) the game's OWN selection
// notices the joiner (first noticed = whoever it heard/saw first). pc207/208: but with
// both players firing it keeps every enemy on the PLAYER-kind body (AI code special-cases
// +0xC4 == 2), so "most threatening" needs the manager: ON by default, env COOP_AGGRO=0 off.
static int g_aggro_off = -1;
static void coop_downed_lifestate(void)
{
    if (g_aggro_off < 0) { char e[8] = {0}; g_aggro_off = GetEnvironmentVariableA("COOP_AGGRO", e, sizeof e) && e[0] == '0'; }
    uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    if (me < 0x80000000 || me >= 0x84000000) me = 0;
    static uint32_t me_set, clone_set;
    auto flag = [](uint32_t obj, bool want, uint32_t &set, const char *who) {
        volatile uint32_t *ls = obj ? (volatile uint32_t *)(uintptr_t)(obj + 0x384) : nullptr;
        if (want && ls) {
            if (*ls == 0) {
                *ls = 1; set = obj;
                logf("coop: downed: %s %08x life state 0 -> 1 (enemies drop it, no damage)", who, obj);
            }
        } else if (set) {
            volatile uint32_t *ps = (volatile uint32_t *)(uintptr_t)(set + 0x384);
            if (*ps == 1) *ps = 0;
            logf("coop: downed: %s %08x life state -> 0 (a target again)", who, set);
            set = 0;
        }
    };
    flag(me, g_cfg.coop_downed && g_coop_downed != 0, me_set, "our player");
    bool peer = g_cfg.coop_downed && g_cfg.coop_role == 1 && g_coop_peer_downed && g_coop_host_ms &&
                GetTickCount64() - g_coop_host_ms <= 1000;
    uint32_t c = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) ? g_p10_gdst : 0;
    flag(c, peer && c, clone_set, "the joiner's clone");
    // CLONE HEALTH: log on any change of the clone's identity, every 5 s otherwise
    if (c) {
        static uint32_t lvt, llink, lA, lblk, lix; static uint64_t lms;
        uint32_t vt = *(volatile uint32_t *)(uintptr_t)c, link = *(volatile uint32_t *)(uintptr_t)(c + 0x324);
        uint32_t A = *(volatile uint32_t *)(uintptr_t)(c + 0x328), blk = coop_clone_anim_block();
        uint64_t now = GetTickCount64();
        bool changed = vt != lvt || link != llink || A != lA || blk != lblk || g_clone_ix != lix;
        if (changed || now - lms >= 5000) {
            const volatile float *f = (const volatile float *)(uintptr_t)(c + 0xA0);
            logf("coop: clone health%s: body %08x vt %08x life %u link %08x (ours %08x) anim %08x blk %08x rig %08x n %d | bends: bone pass %u, draw %u (fresh %u) | aim yaw %.0f pitch %.0f tw %.0f | at %.1f %.1f %.1f",
                 changed ? " CHANGED" : "", c, vt, *(volatile uint32_t *)(uintptr_t)(c + 0x384), link, g_aim_link, A, blk,
                 g_clone_ix, g_clone_nb, g_bend_bp_calls, g_bend_draw_calls, g_bend_draw_fresh,
                 g_aim_yaw_deg, g_aim_pitch_deg, g_bend_tw, f[0], f[1], f[2]);
            lvt = vt; llink = link; lA = A; lblk = blk; lix = g_clone_ix; lms = now;
        }
    }
}
// AGGRO LOG (host; owner: "an enemy attacks the first player he sees and his aggro
// changes depending on the most threatening player" - check what the game does).
// Each enemy's AI agent = its link object (body+0x324); its current target is a
// registry index at +0x2A8 -> [*0x2D1988 + 0xFB4 + idx*0xC] (kind at +0xFB0, 0 =
// entity). FUN_001ab4a0 keeps 3 scored threat candidates and picks the highest.
// THREAT table (host): damage each player dealt to each enemy, decaying (~6 s time constant)
struct Threat { uint32_t enemy; float host, clone; uint64_t ms; uint32_t chosen; uint64_t chosen_ms; uint32_t tested; };
static Threat g_threat[16]; static int g_threat_n;
static Threat *coop_threat_get(uint32_t enemy, bool make)
{
    for (int i = 0; i < g_threat_n; i++) if (g_threat[i].enemy == enemy) return &g_threat[i];
    if (!make) return nullptr;
    if (g_threat_n >= 16) {                       // recycle the oldest record
        int o = 0;
        for (int i = 1; i < 16; i++) if (g_threat[i].ms < g_threat[o].ms) o = i;
        memset(&g_threat[o], 0, sizeof g_threat[o]); g_threat[o].enemy = enemy; g_threat[o].ms = GetTickCount64();
        return &g_threat[o];
    }
    Threat *t = &g_threat[g_threat_n++];
    memset(t, 0, sizeof *t); t->enemy = enemy; t->ms = GetTickCount64();
    return t;
}
static void coop_threat_decay(Threat *t)
{
    uint64_t now = GetTickCount64();
    float dt = (float)(now - t->ms) / 1000.0f;
    if (dt > 0) { float k = expf(-dt / 6.0f); t->host *= k; t->clone *= k; }
    t->ms = now;
}
static void coop_threat_add(uint32_t enemy, uint32_t attacker, float dmg)
{
    if (!(dmg > 0.0f) || dmg > 1e6f) return;
    uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    if (me && is_enemy(enemy, me)) {
        static int dl;
        if (dl++ < 30) logf("coop: aggro: enemy %08x damaged %.1f by %08x (host %08x, joiner clone %08x)", enemy, dmg, attacker, me, g_p10_gdst);
    }
    if (attacker != me && (attacker != g_p10_gdst || !g_p10_gdst)) return;
    if (!me || !is_enemy(enemy, me)) return;
    Threat *t = coop_threat_get(enemy, true);
    coop_threat_decay(t);
    if (attacker == me) t->host += dmg; else { t->clone += dmg; g_jhit_n++; g_jhit_dmg += dmg; }
    if (g_cfg.coop_role == 1) coop_hm_hit(enemy, attacker == me ? 1 : 2);   // the host owns enemy health: it sees every hit
    static int ln;
    if (ln++ < 40 || (ln % 20) == 0) logf("coop: aggro: %s hit enemy %08x for %.1f -> threat host %.0f joiner %.0f",
                        attacker == me ? "HOST" : "JOINER", enemy, dmg, t->host, t->clone);
}
// release the enemy's current combat behaviour (+0x648) like FUN_001ab4a0 does on a switch
volatile uint32_t g_bh_obj, g_bh_fn, g_bh_esp;
__declspec(naked) void coop_thunk_behaviour_release()
{
    __asm {
        pushad
        mov dword ptr [g_bh_esp], esp
        mov ecx, dword ptr [g_bh_obj]
        call dword ptr [g_bh_fn]
        mov esp, dword ptr [g_bh_esp]
        popad
        ret
    }
}
static void coop_set_enemy_target(uint32_t link, int idx)
{
    uint32_t beh = *(volatile uint32_t *)(uintptr_t)(link + 0x648);
    if (beh >= 0x80000000 && beh < 0x84000000) {
        uint32_t vt = *(volatile uint32_t *)(uintptr_t)beh;
        if (vt >= 0x10000 && vt < 0x84000000) {
            g_bh_obj = beh; g_bh_fn = *(volatile uint32_t *)(uintptr_t)(vt + 0x10);
            coop_thunk_behaviour_release();
        }
        *(volatile uint32_t *)(uintptr_t)(link + 0x648) = 0;
    }
    *(volatile int *)(uintptr_t)(link + 0x2A8) = idx;
    *(volatile uint8_t *)(uintptr_t)(link + 0x2A1) = 1;   // FUN_001ab4a0's "plan again" flag
}
static void coop_aggro_log(void)
{
    if (g_cfg.coop_role != 1) return;
    uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    if (me < 0x80000000 || me >= 0x84000000) return;
    uint32_t reg = *(volatile uint32_t *)(uintptr_t)0x2D1988;
    if (reg < 0x80000000 || reg >= 0x84000000) return;
    struct Track { uint32_t body, ent; };
    static Track tr[16]; static int ntr;
    static uint32_t frames;
    frames++;
    if (frames == 600 || frames == 3000) {   // where are the two players in the AI entity registry?
        int ih = -1, ic = -1, nent = 0, last = -1;
        for (int i = 0; i < 4096; i++) {
            if (*(volatile uint32_t *)(uintptr_t)(reg + 0xFB0 + i * 0xC) != 0) continue;
            uint32_t e = *(volatile uint32_t *)(uintptr_t)(reg + 0xFB4 + i * 0xC);
            if (e < 0x80000000 || e >= 0x84000000) continue;
            nent++; last = i;
            if (e == me && ih < 0) ih = i;
            if (e == g_p10_gdst && ic < 0) ic = i;
        }
        logf("coop: aggro: registry %08x: %d entity records (last idx %d); host player idx %d (+0x2A8 field %d), joiner clone idx %d (body +0x2A8 %d)",
             reg, nent, last, ih, *(volatile int *)(uintptr_t)(me + 0x2A8), ic,
             g_p10_gdst ? *(volatile int *)(uintptr_t)(g_p10_gdst + 0x2A8) : -9);
    }
    uint32_t s[16]; uint32_t ns = enemy_slots(s);
    int on_host = 0, on_clone = 0, other = 0, none = 0;
    {   // PERCEPTION HUNT: where does an engaged enemy's AI object hold the HOST body pointer?
        static int scans;
        if (scans < 3 && (frames % 120) == 0) {
            for (uint32_t i = 0; i < ns && scans < 3; i++) {
                uint32_t b = s[i];
                if (!is_enemy(b, me)) continue;
                uint32_t link = *(volatile uint32_t *)(uintptr_t)(b + 0x324);
                if (link < 0x80000000 || link >= 0x84000000 || *(volatile int *)(uintptr_t)(link + 0x2A8) != 0) continue;
                char buf[900]; int o = snprintf(buf, sizeof buf, "coop: perception hunt: enemy %08x link %08x holds host %08x at", b, link, me);
                for (uint32_t off = 0; off < 0x1FE0 && o < 860; off += 4)
                    if (*(volatile uint32_t *)(uintptr_t)(link + off) == me) o += snprintf(buf + o, sizeof buf - o, " +%X", off);
                logf("%s", buf);
                scans++;
            }
        }
    }
    auto label = [&](uint32_t e, char *buf, size_t n) {
        if (e == me) snprintf(buf, n, "HOST");
        else if (e && e == g_p10_gdst) snprintf(buf, n, "JOINER-CLONE");
        else if (!e) snprintf(buf, n, "none");
        else snprintf(buf, n, "%08x", e);
    };
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t b = s[i];
        if (!is_enemy(b, me)) continue;
        uint32_t link = *(volatile uint32_t *)(uintptr_t)(b + 0x324);
        if (link < 0x80000000 || link >= 0x84000000) continue;
        int idx = *(volatile int *)(uintptr_t)(link + 0x2A8);
        uint32_t ent = 0;
        if (idx >= 0 && idx < 4096 && *(volatile uint32_t *)(uintptr_t)(reg + 0xFB0 + idx * 0xC) == 0)
            ent = *(volatile uint32_t *)(uintptr_t)(reg + 0xFB4 + idx * 0xC);
        if (ent == me) on_host++; else if (ent && ent == g_p10_gdst) on_clone++; else if (ent) other++; else none++;
        Track *t = nullptr;
        for (int k = 0; k < ntr; k++) if (tr[k].body == b) t = &tr[k];
        if (!t && ntr < 16) { t = &tr[ntr++]; t->body = b; t->ent = 0xFFFFFFFF; }
        // ---- AGGRO MANAGER: first seen, then the most threatening player ----
        static int idx_host = -1, idx_clone = -1;
        if (frames % 300 == 1 || idx_host < 0 || idx_clone < 0) {
            idx_host = idx_clone = -1;
            for (int r = 0; r < 512; r++) {
                if (*(volatile uint32_t *)(uintptr_t)(reg + 0xFB0 + r * 0xC) != 0) continue;
                uint32_t e = *(volatile uint32_t *)(uintptr_t)(reg + 0xFB4 + r * 0xC);
                if (e == me && idx_host < 0) idx_host = r;
                if (e && e == g_p10_gdst && idx_clone < 0) idx_clone = r;
            }
        }
        // COOP_AGGRO_TEST=<sec> (diagnostics): from that time on, every engaged enemy is
        // credited 400 joiner threat once (deterministic switch test; the scripted
        // Naszran scene's shots go into the ground)
        static int aggro_test = -2;
        if (aggro_test == -2) { char e[16] = {0}; aggro_test = GetEnvironmentVariableA("COOP_AGGRO_TEST", e, sizeof e) ? atoi(e) : -1; }
        if (aggro_test > 0 && ent && (int64_t)(GetTickCount64() - g_coop_t0) >= (int64_t)aggro_test * 1000) {
            Threat *tt = coop_threat_get(b, true);
            if (!tt->tested) { coop_threat_decay(tt); tt->clone += 400.0f; tt->tested = 1;
                logf("coop: aggro: TEST +400 joiner threat on enemy %08x", b); }
        }
        // an IDLE enemy the joiner has hurt engages the joiner (the game's own perception
        // only knows the host player; the clone is outside the AI agent roster)
        Threat *idle_th = (!ent && g_p10_gdst) ? coop_threat_get(b, false) : nullptr;
        bool idle_hit = idle_th && idle_th->clone > 0.5f && *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x384) == 0;
        // owner (day 15): "enemies don't detect the joiner with the sound he makes - they never turn". The gunshot
        // broadcast reaches them, but an enemy that is IDLE or fighting a SQUADMATE stayed with the game's pick (the
        // manager below only touched enemies already on a player). The joiner fired in the last 0.6 s within 30 m
        // of this enemy and it is not on a player -> it engages the joiner (the threat rules then decide as before).
        bool heard = false;
        if (!g_aggro_off && g_p10_gdst && ent != me && ent != g_p10_gdst && g_jshot_ms && GetTickCount64() - g_jshot_ms < 600 &&
            *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x384) == 0) {
            const volatile float *eb = (const volatile float *)(uintptr_t)(b + 0xA0), *cb = (const volatile float *)(uintptr_t)(g_p10_gdst + 0xA0);
            float hx = eb[0] - cb[0], hy = eb[1] - cb[1], hz = eb[2] - cb[2];
            heard = hx * hx + hy * hy + hz * hz < 30.0f * 30.0f;
        }
        if (!g_aggro_off && (idle_hit || heard || (ent && (ent == me || ent == g_p10_gdst))) && idx_host >= 0 && idx_clone >= 0) {
            bool host_ok = *(volatile uint32_t *)(uintptr_t)(me + 0x384) == 0;
            bool clone_ok = *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x384) == 0;
            Threat *th = coop_threat_get(b, true);
            coop_threat_decay(th);
            if (idle_hit && !th->chosen) { th->chosen = g_p10_gdst; th->chosen_ms = GetTickCount64();
                logf("coop: aggro: idle enemy %08x was shot by the JOINER -> engages the joiner", b); }
            if (heard) {
                if (th->chosen != g_p10_gdst) {
                    static uint32_t hl;
                    if (hl++ < 60) logf("coop: aggro: enemy %08x (on %s) HEARD the joiner's shots -> engages the joiner",
                                        b, ent ? "a squadmate" : "nobody");
                }
                th->chosen = g_p10_gdst; th->chosen_ms = GetTickCount64();
            }
            uint64_t now = GetTickCount64();
            uint32_t want = th->chosen;
            if (!want) {   // FIRST NOTICED = the game's own first pick (it heard / saw that player first)
                want = (ent == me || ent == g_p10_gdst) ? ent : g_p10_gdst;
                th->chosen = want; th->chosen_ms = now;
                logf("coop: aggro: enemy %08x FIRST NOTICES the %s", b, want == me ? "HOST" : "JOINER");
            } else if ((ent == me || ent == g_p10_gdst) && ent != want && th->host < 10.0f && th->clone < 10.0f) {
                // the game switched on its own and nobody has a clear threat lead: follow it
                logf("coop: aggro: enemy %08x game switch %s -> %s accepted (threat host %.0f joiner %.0f)", b,
                     want == me ? "HOST" : "JOINER", ent == me ? "HOST" : "JOINER", th->host, th->clone);
                want = ent; th->chosen = want; th->chosen_ms = now;
            } else {
                float cur = want == me ? th->host : th->clone, oth = want == me ? th->clone : th->host;
                uint32_t other = want == me ? g_p10_gdst : me;
                bool cur_ok = want == me ? host_ok : clone_ok, oth_ok = want == me ? clone_ok : host_ok;
                if ((!cur_ok && oth_ok) ||
                    (oth_ok && now - th->chosen_ms >= 3000 && oth > cur * 1.5f + 10.0f)) {
                    logf("coop: aggro: enemy %08x switches %s -> %s (threat host %.0f joiner %.0f%s)", b,
                         want == me ? "HOST" : "JOINER", other == me ? "HOST" : "JOINER", th->host, th->clone,
                         cur_ok ? "" : ", target downed");
                    want = other; th->chosen = want; th->chosen_ms = now;
                }
            }
            bool want_ok = want == me ? host_ok : clone_ok;
            if (want_ok && ent != want) {
                coop_set_enemy_target(link, want == me ? idx_host : idx_clone);
                ent = want;
            }
        }
        if (t && t->ent != ent) {
            char a1[16], a2[16];
            label(t->ent == 0xFFFFFFFF ? 0 : t->ent, a1, sizeof a1); label(ent, a2, sizeof a2);
            const float *eb = (const float *)(uintptr_t)(b + 0xA0), *hp = (const float *)(uintptr_t)(me + 0xA0);
            float dh = sqrtf((eb[0] - hp[0]) * (eb[0] - hp[0]) + (eb[2] - hp[2]) * (eb[2] - hp[2])), dc = -1.0f;
            if (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) {
                const float *cp = (const float *)(uintptr_t)(g_p10_gdst + 0xA0);
                dc = sqrtf((eb[0] - cp[0]) * (eb[0] - cp[0]) + (eb[2] - cp[2]) * (eb[2] - cp[2]));
            }
            static int ln;
            if (ln++ < 120)
                logf("coop: aggro: enemy %08x target %s -> %s (dist host %.1f m, joiner clone %.1f m; host life %u clone life %u)",
                     b, a1, a2, dh, dc, *(volatile uint32_t *)(uintptr_t)(me + 0x384),
                     g_p10_gdst ? *(volatile uint32_t *)(uintptr_t)(g_p10_gdst + 0x384) : 0);
            t->ent = ent;
        }
    }
    if ((frames % 600) == 0 && (on_host | on_clone | other))
        logf("coop: aggro: %d enemies on the HOST, %d on the JOINER clone, %d on others, %d idle | host hp %.0f, joiner clone hits so far %u",
             on_host, on_clone, other, none, *(volatile float *)(uintptr_t)(me + 0x2F0), g_hit_stats[0]);
}
static EnemyRec g_en_host[16]; static int g_en_host_n; static uint32_t g_en_host_seq;
static void coop_enemy_recv(const EnemyRec *e, int n)
{
    // en122: a body the host killed LEAVES the host's pawn slots (it never reports hp 0)
    // -> an id present in the previous update and missing now = dead (kept as hp 0)
    // ENEMY IDENTITY: each host record -> OUR body with the same id (the address the host sent is
    // only remembered for translating its other packets). No local enemy with that id yet = it has
    // not spawned here (our player has not reached that trigger): the record is skipped for now.
    EnemyRec tr[16]; int tn = 0;
    for (int i = 0; i < n && tn < 16; i++) {
        uint32_t local = coop_enid_local(e[i].id);
        if (e[i].id) {
            int ik = -1;
            for (int k = 0; k < 48; k++) if (g_idhost[k].id == e[i].id) ik = k;
            if (ik < 0) ik = (int)(g_idhost_w++ % 48);
            g_idhost[ik].id = e[i].id; g_idhost[ik].host = e[i].body;
        }
        int hk = -1;
        for (int k = 0; k < g_hmap_n; k++) if (g_hmap[k].host == e[i].body) hk = k;
        if (hk < 0) { hk = g_hmap_n < 48 ? g_hmap_n++ : (int)(g_hmap_w++ % 48); g_hmap[hk].host = e[i].body; }
        g_hmap[hk].local = local;
        if (!local) {
            static uint32_t ml;
            if ((ml++ % 600) == 0) logf("coop: enemy sync: host enemy %08x (id %08x) is not spawned here yet - skipped", e[i].body, e[i].id);
            continue;
        }
        tr[tn] = e[i]; tr[tn].body = local; tn++;
    }
    e = tr; n = tn;
    EnemyRec next[16]; int nn = 0;
    for (int i = 0; i < n && nn < 16; i++) next[nn++] = e[i];
    for (int i = 0; i < g_en_host_n && nn < 16; i++) {
        bool still = false;
        for (int k = 0; k < n; k++) if (e[k].body == g_en_host[i].body) still = true;
        if (still) continue;
        EnemyRec gone = g_en_host[i];
        if (gone.hp > 0.0f) logf("coop: enemy sync: %08x left the host's enemy list -> dead", gone.body);
        gone.hp = 0.0f;
        static uint64_t gone_ms[16]; (void)gone_ms;
        next[nn++] = gone;   // carried while it stays missing (cheap; at most 16)
    }
    memcpy(g_en_host, next, nn * sizeof(EnemyRec));
    g_en_host_n = nn;
    g_en_host_seq++;
}
// COOP_ANIMDUMP=1 (diagnostics, both roles): once a second from 65 s to 110 s, append a
// record per enemy to port\animdump_<role>.bin: {u32 magic 'ADMP', u32 t_ms, u32 body,
// u32 link, u32 A, u32 blk, link[0x1FE0], A[0x100], blk[0x9D0]} -> offline diff of what the
// anim player holds on the host vs the joiner's puppet.
static void coop_anim_dump(uint32_t pobj)
{
    static int on = -1;
    if (on < 0) { char e[8] = {0}; on = GetEnvironmentVariableA("COOP_ANIMDUMP", e, sizeof e) && e[0] == '1'; }
    if (!on || !g_coop_t0) return;
    uint64_t t = GetTickCount64() - g_coop_t0;
    static uint64_t next = 65000;
    if (t < next || t > 110000) return;
    next = t - (t % 1000) + 1000;
    static FILE *f;
    if (!f) f = fopen(g_cfg.coop_role == 1 ? "animdump_host.bin" : "animdump_joiner.bin", "wb");
    if (!f) return;
    uint32_t s[16]; uint32_t ns = enemy_slots(s);
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t b = s[i];
        if (!is_enemy(b, pobj)) continue;
        uint32_t link = *(volatile uint32_t *)(uintptr_t)(b + 0x324), A = *(volatile uint32_t *)(uintptr_t)(b + 0x328);
        uint32_t blk = (A >= 0x80000000 && A < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(A + 0x54) : 0;
        if (link < 0x10000 || A < 0x80000000 || blk < 0x80000000 || blk >= 0x84000000) continue;
        uint32_t hdr[6] = {0x504D4441, (uint32_t)t, b, link, A, blk};
        fwrite(hdr, 4, 6, f);
        fwrite((const void *)(uintptr_t)link, 1, 0x1FE0, f);   // the whole AI object
        fwrite((const void *)(uintptr_t)A, 1, 0x100, f);
        fwrite((const void *)(uintptr_t)blk, 1, 0x9D0, f);
    }
    fflush(f);
}
// ENEMY AI-STATE MIRROR (day 9; owner: "joiner enemy animation node and behaviour tree
// doesn't sync completely"). ad211: during a Naszran firefight the host's AI changed 351
// VALUE fields of each enemy's AI object (link, 0x1FE0 B) that the joiner's puppet never
// got (its private link is a frozen spawn-time copy): +0x40 crouch (-> body+0x3A1), the
// +0x48..+0x58 action/aim fields, combat timers/states (+0x850, +0xC94..+0xCA0, ...).
// The puppet body reads ONLY the private link (the joiner's own AI runs on the original
// class object), so: host sends every changed non-pointer dword of each enemy's link
// (u16 dword index + u32 value), a full refresh every 2 s; the joiner writes them into
// the puppet's private link, skipping pointer-like values on either side and the fields
// the puppet drive owns. "CLAI" (0x49414C43): {magic, seq, n_enemies} then per enemy
// {u32 body, u16 count, u16 pad, count x {u16 idx, u16 pad, u32 value}}.
static bool ai_ptrlike(uint32_t v) { return (v >= 0x80000000 && v < 0x84000000) || (v >= 0x00100000 && v < 0x10000000); }
static bool ai_owned(uint32_t off)   // fields the puppet drive (or the clone/private-link setup) owns
{
    return off == 0x10 || off == 0x14 || off == 0x18 || off == 0x1C || off == 0x20 || off == 0x4C ||   // facing, look, speed, +0x4D may-hurt
           (off >= 0x60 && off <= 0x78) || off == 0x84 || off == 0x8C || off == 0xD8;      // move dir, speed, body, voice
}
// play213 (owner run): the BROAD mirror crashed the joiner at 176 s - AV 0x1904ad reading
// [[[arg+4]+0x14]+0x20]+0x324 = the TEAM record's player body (null for the enemy team):
// mirrored brain state (indices / behaviour states meaningful only on the host) sends the
// puppet's per-tick AI into paths it never takes locally. Default is now a WHITELIST of
// body-facing fields; COOP_AI_MIRROR=all = the broad mirror (test only), =0 = off.
static int g_ai_mode = -1;   // 0 off, 1 whitelist, 2 all
static bool ai_whitelisted(uint32_t off)
{
    return off == 0x40 || off == 0x48 || off == 0x50 || off == 0x54 || off == 0x58 || off == 0x80 ||
           off == 0x114 || off == 0x120 || off == 0x140;
}
static void coop_ai_mode_init(void)
{
    if (g_ai_mode >= 0) return;
    char e[8] = {0};
    if (!GetEnvironmentVariableA("COOP_AI_MIRROR", e, sizeof e)) g_ai_mode = 1;
    else g_ai_mode = e[0] == '0' ? 0 : (e[0] == 'a' ? 2 : 1);
    logf("coop: ai mirror mode %s", g_ai_mode == 0 ? "OFF" : g_ai_mode == 1 ? "whitelist" : "ALL (test)");
}
static void coop_ai_send(uint32_t pobj, uint32_t seq)
{
    coop_ai_mode_init();
    if (!g_ai_mode) return;
    struct Shadow { uint32_t body; uint32_t v[0x7F8]; uint64_t full_ms; };
    static Shadow *sh; static int nsh;
    if (!sh) sh = (Shadow *)calloc(16, sizeof(Shadow));
    static uint8_t buf[60000];
    uint32_t *hdr = (uint32_t *)buf; hdr[0] = 0x49414C43; hdr[1] = seq; hdr[2] = 0;
    size_t o = 12;
    uint64_t now = GetTickCount64();
    uint32_t s[16]; uint32_t ns = enemy_slots(s);
    static uint32_t stat_fields, stat_pk;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t b = s[i];
        if (!is_enemy(b, pobj)) continue;
        uint32_t link = *(volatile uint32_t *)(uintptr_t)(b + 0x324);
        if (link < 0x80000000 || link >= 0x84000000) continue;
        Shadow *w = nullptr;
        for (int k = 0; k < nsh; k++) if (sh[k].body == b) w = &sh[k];
        // full refreshes are staggered per enemy (a full record is ~12 KB; 7 at once overflow a datagram)
        if (!w) { if (nsh >= 16) continue; w = &sh[nsh]; memset(w, 0, sizeof *w); w->body = b; w->full_ms = now - 2000 + 300 * nsh; nsh++; }
        if (o + 8 + 0x7F8 * 8 > sizeof buf) break;
        bool full = now - w->full_ms >= 2000;
        if (full) w->full_ms = now;
        uint32_t *eh = (uint32_t *)(buf + o); eh[0] = b; size_t cnt_at = o + 4; o += 8;
        uint32_t cnt = 0;
        const volatile uint32_t *L = (const volatile uint32_t *)(uintptr_t)link;
        for (uint32_t k = 0; k < 0x7F8; k++) {
            uint32_t v = L[k];
            if (ai_ptrlike(v) || ai_owned(k * 4)) continue;
            if (g_ai_mode == 1 && !ai_whitelisted(k * 4)) continue;
            if (!full && v == w->v[k]) continue;
            w->v[k] = v;
            uint32_t *e = (uint32_t *)(buf + o); e[0] = k; e[1] = v; o += 8; cnt++;
        }
        *(uint32_t *)(buf + cnt_at) = cnt;
        hdr[2]++;
        stat_fields += cnt;
    }
    if (hdr[2]) { sendto(g_coop_sock, (const char *)buf, (int)o, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target); stat_pk++; }
    static uint64_t lastlog;
    if (now - lastlog >= 10000) { lastlog = now; logf("coop: ai mirror HOST: %u packets, %u fields sent (last packet %u B)", stat_pk, stat_fields, (unsigned)o); }
}
// joiner: latest field updates per body, applied in coop_enemy_tick to the puppet links
struct AiUpd { uint32_t body; uint32_t n; uint32_t idx[0x7F8]; uint32_t val[0x7F8]; };
static AiUpd *g_ai_in; static int g_ai_in_n;
static void coop_ai_recv(const uint8_t *p, int len)
{
    if (!g_ai_in) g_ai_in = (AiUpd *)calloc(16, sizeof(AiUpd));
    uint32_t ne = *(const uint32_t *)(p + 8);
    int o = 12;
    for (uint32_t i = 0; i < ne && o + 8 <= len; i++) {
        uint32_t b = *(const uint32_t *)(p + o), cnt = *(const uint32_t *)(p + o + 4); o += 8;
        if (cnt > 0x7F8 || o + (int)cnt * 8 > len) return;
        b = coop_host_to_local(b);                     // enemy identity: the host's body -> ours
        if (!b) { o += cnt * 8; continue; }            // not spawned here: nothing to mirror
        AiUpd *u = nullptr;
        for (int k = 0; k < g_ai_in_n; k++) if (g_ai_in[k].body == b) u = &g_ai_in[k];
        if (!u) { if (g_ai_in_n >= 16) { o += cnt * 8; continue; } u = &g_ai_in[g_ai_in_n++]; u->body = b; u->n = 0; }
        for (uint32_t k = 0; k < cnt && u->n < 0x7F8; k++) {
            uint32_t idx = *(const uint32_t *)(p + o), val = *(const uint32_t *)(p + o + 4); o += 8;
            u->idx[u->n] = idx; u->val[u->n] = val; u->n++;
        }
    }
}
static void coop_ai_apply(uint32_t body, uint32_t link)
{
    if (!g_ai_in || link < 0x10000) return;
    for (int k = 0; k < g_ai_in_n; k++) {
        AiUpd &u = g_ai_in[k];
        if (u.body != body || !u.n) continue;
        volatile uint32_t *L = (volatile uint32_t *)(uintptr_t)link;
        static uint32_t applied, skipped;
        for (uint32_t j = 0; j < u.n; j++) {
            uint32_t i = u.idx[j];
            if (i >= 0x7F8 || ai_owned(i * 4)) continue;
            coop_ai_mode_init();
            if (g_ai_mode == 0 || (g_ai_mode == 1 && !ai_whitelisted(i * 4))) continue;   // the joiner enforces its own mode too
            if (ai_ptrlike(L[i])) { skipped++; continue; }   // a pointer here on the joiner: never overwrite
            L[i] = u.val[j]; applied++;
        }
        u.n = 0;
        static uint64_t lastlog;
        uint64_t now = GetTickCount64();
        if (now - lastlog >= 10000) { lastlog = now; logf("coop: ai mirror JOINER: %u fields applied, %u skipped (pointer here)", applied, skipped); }
    }
}
// keep the identity table current: every live pawn in the manager's 16 slots has an id
static void coop_enid_update(uint32_t pobj)
{
    uint32_t s[16]; uint32_t ns = enemy_slots(s);
    for (int i = 0; i < g_enid_n; i++) {
        EnId &e = g_enid[i];
        if (!e.live) continue;
        bool ok = false;
        for (uint32_t k = 0; k < ns; k++) if (s[k] == e.body) ok = true;
        if (ok) {
            uint32_t vt = 0, cx = 0; float hp = 0;
            ok = coop_safe_copy(&vt, e.body, 4) && vt == 0x0028C1E8 && coop_safe_copy(&cx, e.body + 0x31C, 4) && cx == e.ctx &&
                 coop_safe_copy(&hp, e.body + 0x2F0, 4) && hp > 0.0f;
            if (ok && e.killing && hp > 1.0f) ok = false;     // tg4: the game already put a NEW enemy into this body slot
        }
        if (!ok) {
            e.live = 0;
            if (g_cfg.coop_role == 1) {
                g_dead_ids[g_dead_w++ & 63] = e.id;     // the host's word: this enemy is gone
                for (int q = 0; q < 32; q++) if (g_lasthit[q].body == e.body) {
                    KillDir &kd = g_killdir[g_killdir_w++ & 15];
                    kd.id = e.id; memcpy(kd.dir, g_lasthit[q].dir, 12); memcpy(kd.pt, g_lasthit[q].pt, 12);
                    kd.cause = g_lasthit[q].cause; memcpy(kd.bv, g_lasthit[q].bv, sizeof kd.bv); memcpy(kd.braw, g_lasthit[q].braw, sizeof kd.braw);
                    g_lasthit[q].body = 0;
                    break;
                }
            }
            else coop_pup_release(e.body, "its enemy ended");
        }
    }
    for (uint32_t k = 0; k < ns; k++) {
        uint32_t b = s[k], vt = 0, cx = 0; float hp = 0;
        if (b == g_p10_gdst || b == pobj) continue;
        if (!coop_safe_copy(&vt, b, 4) || vt != 0x0028C1E8 || !coop_safe_copy(&hp, b + 0x2F0, 4) || !(hp > 0.0f)) continue;
        if (coop_enid_of(b)) continue;
        coop_safe_copy(&cx, b + 0x31C, 4);
        uint32_t ord = 0; const char *how = "spawn order";
        for (int i = 0; i < g_spawn_pend_n; i++)
            if (g_spawn_pend[i].ctx == cx) {
                ord = g_spawn_pend[i].ord;
                memmove(g_spawn_pend + i, g_spawn_pend + i + 1, (g_spawn_pend_n - i - 1) * sizeof g_spawn_pend[0]); g_spawn_pend_n--;
                break;
            }
        if (!ord) {   // no spawn seen for it (alive before the hook, or an unknown ctx): the spawn count so far, at least 1
            how = "fallback";
            for (int i = 0; i < g_spawn_ctx_n; i++) if (g_spawn_ctx[i].ctx == cx) ord = g_spawn_ctx[i].count;
            if (!ord) ord = 1;
        }
        int slot = -1;
        for (int i = 0; i < g_enid_n; i++) if (!g_enid[i].live) { slot = i; break; }
        if (slot < 0 && g_enid_n < 48) slot = g_enid_n++;
        if (slot < 0) continue;
        EnId &e = g_enid[slot];
        e.body = b; e.ctx = cx; e.id = cx * 2654435761u + ord * 0x10001u + 1u; e.live = 1; e.killing = 0; e.ms = GetTickCount64();
        static int il;
        if (il++ < 80) logf("coop: enemy id: body %08x = spawn point %08x #%u -> id %08x (%s)", b, cx, ord, e.id, how);
    }
}
static void coop_enemy_send(uint32_t pobj, uint32_t seq)
{
    coop_anim_dump(pobj);
    static uint32_t fr;
    if ((fr++ % 3) != 0) return;   // 20 Hz
    uint32_t s[16]; uint32_t ns = enemy_slots(s);
    struct { uint32_t magic, seq, n; EnemyRec e[16]; uint32_t dead[64]; } pk;
    pk.magic = 0x4E454C43; pk.seq = seq; pk.n = 0;
    for (uint32_t i = 0; i < ns; i++) {
        if (!is_enemy(s[i], pobj)) continue;
        EnemyRec &r = pk.e[pk.n++];
        r.body = s[i];
        r.hp = *(volatile float *)(uintptr_t)(s[i] + 0x2F0);
        for (int k = 0; k < 3; k++) r.feet[k] = *(volatile float *)(uintptr_t)(s[i] + 0xA0 + 4 * k);
        // phase 2: the enemy's OWN body state (links can be shared per character class)
        const volatile float *F = (const volatile float *)(uintptr_t)(s[i] + 0x90);    // body pose fwd row
        const volatile float *L = (const volatile float *)(uintptr_t)(s[i] + 0xF0);    // look fwd (pitched)
        r.byaw = 57.2957795f * atan2f(F[0], F[2]);
        r.lyaw = 57.2957795f * atan2f(L[0], L[2]);
        float ly = L[1] < -1.0f ? -1.0f : (L[1] > 1.0f ? 1.0f : L[1]);
        r.lpitch = -57.2957795f * asinf(ly);        // link pitch: + = down (look104 sign)
        {   // play 20261113_182730 ("enemies' pitch and yaw when aiming don't sync"): an AI soldier's look
            // vector +0xF0 carries no pitch (always ~0 in the logs). The AI's own aim angles are its brain's
            // +0x18 (yaw) / +0x1C (pitch) - the same fields the puppet's private link is driven through.
            uint32_t br = *(volatile uint32_t *)(uintptr_t)(s[i] + 0x324), self = 0;
            float ay = 0, ap = 0;
            if (br >= 0x80000000 && br < 0x84000000 && coop_safe_copy(&self, br + 0x8C, 4) && self == s[i] &&
                coop_safe_copy(&ay, br + 0x18, 4) && coop_safe_copy(&ap, br + 0x1C, 4) &&
                ay > -720.0f && ay < 720.0f && ap > -90.0f && ap < 90.0f) { r.lyaw = ay; r.lpitch = ap; }
            // the anim input "aim_bearing" (FUN_0017c180) = brain +0x10 (facing) - brain +0x14 (aim yaw);
            // "aim_pitch" (FUN_0017a3b0) = brain +0x1C. The puppet's +0x14 was never written (frozen at spawn).
            float f10 = 0, f14 = 0;
            r.bear = 0.0f;
            if (self == s[i] && coop_safe_copy(&f10, br + 0x10, 4) && coop_safe_copy(&f14, br + 0x14, 4) && f10 > -720.0f && f10 < 720.0f && f14 > -720.0f && f14 < 720.0f) {
                float d = f10 - f14; while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f;
                r.bear = d;
            }
        }
        uint32_t W = *(volatile uint32_t *)(uintptr_t)(s[i] + 0x278 + 0x28);
        r.fire = (W >= 0x80000000 && W < 0x84000000 && *(volatile int *)(uintptr_t)(W + 0xD8) == 1) ? 1u : 0u;
        r.id = coop_enid_of(s[i]);
        r.tgt = 0;
        {   // the enemy's chosen target (brain +0x2A8 = registry index) -> who it is
            uint32_t br = *(volatile uint32_t *)(uintptr_t)(s[i] + 0x324), ti = 0xFFFFFFFF, ci = 0xFFFFFFFE, pi = 0xFFFFFFFD;
            if (br >= 0x80000000 && br < 0x84000000) coop_safe_copy(&ti, br + 0x2A8, 4);
            if (g_p10_gdst) coop_safe_copy(&ci, g_p10_gdst + 0x378, 4);
            coop_safe_copy(&pi, pobj + 0x378, 4);
            if (ti == ci) r.tgt = 2; else if (ti == pi) r.tgt = 1;
        }
        if (r.tgt) {   // diagnostics: the brain's aim vs the geometric angle to its target
            uint32_t tb = r.tgt == 2 ? g_p10_gdst : pobj;
            static uint32_t dl;
            if (tb && (dl++ % 400) == 0) {
                const volatile float *pf = (const volatile float *)(uintptr_t)(s[i] + 0xA0), *tp = (const volatile float *)(uintptr_t)(tb + 0xA0);
                float dx = tp[0] - pf[0], dy = (tp[1] + 1.3f) - (pf[1] + 1.6f), dz = tp[2] - pf[2], hl = sqrtf(dx * dx + dz * dz);
                logf("coop: enemy aim HOST: %08x on %s: brain yaw %.1f pitch %.1f bearing %.1f | geometric yaw %.1f pitch(+down) %.1f (%.1f m) | fire %u",
                     s[i], r.tgt == 2 ? "the JOINER" : "the host", r.lyaw, r.lpitch, r.bear, 57.2957795f * atan2f(dx, dz), -57.2957795f * atan2f(dy, hl), hl, r.fire);
            }
        }
    }
    // the dead-id ring rides right after the n records
    memcpy((char *)&pk + 12 + pk.n * sizeof(EnemyRec), g_dead_ids, sizeof g_dead_ids);
    sendto(g_coop_sock, (const char *)&pk, (int)(12 + pk.n * sizeof(EnemyRec) + sizeof g_dead_ids), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
    {   // CLKD: the recent deaths' last hits (16 x {id, dir, point}), with every enemy packet
        struct { uint32_t magic, n; KillDir k[16]; } kp;
        kp.magic = 0x444B4C43; kp.n = 16; memcpy(kp.k, g_killdir, sizeof g_killdir);
        sendto(g_coop_sock, (const char *)&kp, sizeof kp, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
    }
    coop_ai_send(pobj, seq);
    static uint32_t ln;
    if ((ln++ % 20) == 0) {
        char buf[512]; int o = 0;
        for (uint32_t i = 0; i < pk.n && o < 440; i++)
            o += snprintf(buf + o, sizeof buf - o, " %08x hp %.0f (%.1f %.1f %.1f)", pk.e[i].body, pk.e[i].hp, pk.e[i].feet[0], pk.e[i].feet[1], pk.e[i].feet[2]);
        logf("coop: enemy sync HOST: %u enemies:%s", pk.n, o ? buf : " -");
    }
}
static void coop_enemy_tick(uint32_t pobj)
{
    coop_anim_dump(pobj);
    static struct { uint32_t body; int tries; uint64_t next; uint32_t id; } pend[16];   // id: the enemy the kill is FOR (tg4: the address alone hit its successor)
    static int npend;
    struct Pup { uint32_t body, link, orig; VelTrack v; uint32_t seq; uint64_t ms; };
    static Pup pup[16]; static int npup;
    uint64_t now = GetTickCount64();
    uint32_t s[16]; uint32_t ns = enemy_slots(s);
    // 1) local enemies are immune unless a kill is pending for them
    for (uint32_t i = 0; i < ns; i++) {
        if (!is_enemy(s[i], pobj)) continue;
        bool pending = false;
        for (int k = 0; k < npend; k++) if (pend[k].body == s[i]) pending = true;
        if (!pending) *(volatile uint8_t *)(uintptr_t)(s[i] + 0x3B0) = 1;
    }
    // 1b) enemy identity: a local enemy whose id is in the host's DEAD ring died there (possibly before
    //     it even spawned here) -> pending kill
    for (uint32_t i = 0; i < ns; i++) {
        if (!is_enemy(s[i], pobj) || !(*(volatile float *)(uintptr_t)(s[i] + 0x2F0) > 0.0f)) continue;
        uint32_t id = coop_enid_of(s[i]);
        if (!id) continue;
        bool dead = false, have = false;
        for (int k = 0; k < 64; k++) if (g_dead_ids[k] == id) dead = true;
        if (!dead) continue;
        for (int k = 0; k < npend; k++) if (pend[k].body == s[i]) have = true;
        if (!have && npend < 16) {
            pend[npend++] = {s[i], 0, now, id};
            coop_enid_mark_killing(s[i]);
            logf("coop: enemy sync: %08x (id %08x) is DEAD on the host -> kill", s[i], id);
        }
    }
    // 2) host deaths -> pending kills; pairing check once per host packet
    static uint32_t seen_seq;
    if (g_en_host_seq != seen_seq) {
        seen_seq = g_en_host_seq;
        for (int i = 0; i < g_en_host_n; i++) {
            const EnemyRec &r = g_en_host[i];
            bool local_enemy = false;
            for (uint32_t k = 0; k < ns; k++) if (s[k] == r.body && is_enemy(r.body, pobj)) local_enemy = true;
            float lhp = local_enemy ? *(volatile float *)(uintptr_t)(r.body + 0x2F0) : -1.0f;
            static uint32_t badlog;
            if (!local_enemy && r.hp > 0.0f && badlog++ < 10)
                logf("coop: enemy sync: host enemy %08x has NO live local enemy at that address (pairing miss)", r.body);
            // playtest 20261114 #3 (mission 1, the last section): the joiner got there 8 s before the host; the host's
            // DEAD records of its OLD enemies matched the joiner's NEW section enemies by body ADDRESS (bodies are
            // reused) -> 20 fresh enemies killed at once, so they never fought the joiner and later showed as "not
            // spawned here". An address match counts only when the enemy ids agree (both known).
            uint32_t lid = local_enemy ? coop_enid_of(r.body) : 0;
            bool other = r.id && r.id != lid;   // playtest #5: a fresh body with no id yet (lid 0) was killed by address - it waits for its id
            static uint32_t olog;
            if (local_enemy && r.hp <= 0.0f && lhp > 0.0f && other && olog++ < 40)
                logf("coop: enemy sync: host says %08x DEAD but that is another enemy here (host id %08x, ours %08x) - kept", r.body, r.id, lid);
            if (local_enemy && r.hp <= 0.0f && lhp > 0.0f && !other) {
                bool have = false;
                for (int k = 0; k < npend; k++) if (pend[k].body == r.body) have = true;
                if (!have && npend < 16) {
                    pend[npend++] = {r.body, 0, now, coop_enid_of(r.body)};
                    coop_enid_mark_killing(r.body);
                    logf("coop: enemy sync: host says %08x DEAD (local hp %.1f) -> kill", r.body, lhp);
                }
            }
            static uint32_t plog;
            if (local_enemy && (plog++ % 400) == 0) {
                const volatile float *f = (const volatile float *)(uintptr_t)(r.body + 0xA0);
                float dx = f[0] - r.feet[0], dz = f[2] - r.feet[2];
                logf("coop: enemy sync: %08x host hp %.0f local hp %.0f | position gap %.2f m", r.body, r.hp, lhp, sqrtf(dx * dx + dz * dz));
            }
        }
    }
    // 2b) PHASE 2 puppets: every live paired enemy gets a PRIVATE link (the joiner's AI
    //     keeps writing the class object; the body reads ours) driven from the host:
    //     facing +0x10, look +0x18/+0x1C, speed/dir from the host feet stream, trigger
    //     +0x41; feet pulled to the host's (extrapolated) feet, +0x190 shifted along.
    for (int i = 0; i < g_en_host_n; i++) {
        const EnemyRec &r = g_en_host[i];
        if (!(r.hp > 0.0f)) continue;
        bool local_enemy = false, pending = false;
        for (uint32_t k = 0; k < ns; k++) if (s[k] == r.body && is_enemy(r.body, pobj)) local_enemy = true;
        for (int k = 0; k < npend; k++) if (pend[k].body == r.body) pending = true;
        if (!local_enemy || pending || !(*(volatile float *)(uintptr_t)(r.body + 0x2F0) > 0.0f)) {
            static uint32_t sk1;
            if ((sk1++ % 900) == 0) logf("coop: enemy sync: %08x not driven: local_enemy %d pending %d hp %.1f", r.body, (int)local_enemy, (int)pending, *(volatile float *)(uintptr_t)(r.body + 0x2F0));
            continue;
        }
        Pup *u = nullptr;
        for (int k = 0; k < npup; k++) if (pup[k].body == r.body) u = &pup[k];
        if (!u && npup >= 16) {
            // id3 (Veblensk): the table filled with enemies long dead (16 distinct addresses, never
            // freed) and every later enemy was paired but NEVER driven (7 puppets for 46 ids, gaps up
            // to 67 m). Reuse an entry whose body is not a live enemy any more.
            for (int k = 0; k < npup && !u; k++) {
                bool live = false;
                for (uint32_t q = 0; q < ns; q++)
                    if (s[q] == pup[k].body && is_enemy(s[q], pobj) && *(volatile float *)(uintptr_t)(s[q] + 0x2F0) > 0.0f) live = true;
                if (!live || !pup[k].link) { u = &pup[k]; memset(u, 0, sizeof *u); u->body = r.body; }
            }
            if (!u) { static uint32_t sk2; if ((sk2++ % 900) == 0) logf("coop: enemy sync: %08x not driven: puppet table full of live enemies", r.body); continue; }
        }
        if (!u) { u = &pup[npup++]; memset(u, 0, sizeof *u); u->body = r.body; }
        uint32_t cur = *(volatile uint32_t *)(uintptr_t)(r.body + 0x324);
        if (cur != u->link) {          // first time, or the body was re-spawned with a new class link
            if (cur < 0x80000000 || cur >= 0x84000000) {
                // tg3: the body already carries one of OUR private links (from an earlier enemy in this
                // body slot whose table entry was reused) - 14 paired enemies were never driven. Adopt it:
                // it is a private link for this body; the class object to restore before a kill is the
                // spawn owner whose +0x8C points at this body.
                uint32_t back = 0, orig = 0;
                if (cur >= 0x10000 && coop_safe_copy(&back, cur + 0x8C, 4) && back == r.body) {
                    for (int q = 0; q < g_class_objs_n; q++) {
                        uint32_t b8 = 0;
                        if (coop_safe_copy(&b8, g_class_objs[q] + 0x8C, 4) && b8 == r.body) orig = g_class_objs[q];
                    }
                    u->link = cur; u->orig = orig;
                    coop_pup_note(r.body, cur, orig);
                    memset(&u->v, 0, sizeof u->v);
                    logf("coop: enemy sync: %08x PUPPET (private link %08x ADOPTED, class %08x)", r.body, cur, orig);
                } else {
                    static uint32_t sk3;
                    if ((sk3++ % 900) == 0) logf("coop: enemy sync: %08x not driven: its link %08x is not a class object (ours was %08x)", r.body, cur, u->link);
                    continue;
                }
            } else {
            u->orig = cur;
            u->link = coop_make_private_link(r.body, cur);
            if (!u->link) continue;
            coop_pup_note(r.body, u->link, cur);
            memset(&u->v, 0, sizeof u->v);
            logf("coop: enemy sync: %08x PUPPET (private link %08x <- %08x)", r.body, u->link, cur);
            }
        }
        uint32_t o = u->link;
        {   // play219: a local-only event (a destructible explosion) left this puppet unarmed
            // (body+0x29C = 0) while the host's enemy is alive -> the joiner's brain for it
            // crashed. Keep the puppet's last weapon.
            auto &arm = g_pup_arm; int &narm = g_pup_narm;
            int ai = -1;
            for (int k = 0; k < narm; k++) if (arm[k].body == r.body) ai = k;
            if (ai < 0 && narm < 16) { ai = narm++; arm[ai].body = r.body; arm[ai].w = 0; }
            else if (ai < 0) {   // full: take over the entry of a body that is not a live enemy any more
                for (int k = 0; k < narm && ai < 0; k++) {
                    bool live = false;
                    for (uint32_t q = 0; q < ns; q++) if (s[q] == arm[k].body && *(volatile float *)(uintptr_t)(s[q] + 0x2F0) > 0.0f) live = true;
                    if (!live) { ai = k; arm[ai].body = r.body; arm[ai].w = 0; }
                }
            }
            volatile uint32_t *wp = (volatile uint32_t *)(uintptr_t)(r.body + 0x29C);
            if (ai >= 0) {
                if (*wp >= 0x80000000 && *wp < 0x84000000) arm[ai].w = *wp;
                else if (arm[ai].w) {
                    static int rl;
                    if (rl++ < 10) logf("coop: enemy sync: puppet %08x lost its weapon locally (+29C %08x) - restored %08x", r.body, *wp, arm[ai].w);
                    *wp = arm[ai].w;
                }
            }
        }
        coop_ai_apply(r.body, o);      // the host AI's value fields first; the drive below owns its own
        if (u->seq != g_en_host_seq) { u->seq = g_en_host_seq; u->ms = now; vel_update(u->v, r.feet[0], r.feet[2]); }
        float age = (float)(now - u->ms) / 1000.0f; if (age > 0.25f) age = 0.25f;
        float sp = sqrtf(u->v.vx * u->v.vx + u->v.vz * u->v.vz), dx = 0, dz = 0;
        if (sp < 0.15f) sp = 0.0f; else { dx = u->v.vx / sp; dz = u->v.vz / sp; }
        *(volatile float *)(uintptr_t)(o + 0x10) = r.byaw;
        float lyaw = r.lyaw, lpitch = r.lpitch, aimyaw = r.byaw - r.bear;   // +0x14 so that the bearing equals the host's
        if (r.tgt == 2 && pobj >= 0x80000000 && pobj < 0x84000000) {
            // the host's enemy is on US: aim from this puppet's eyes at our own chest, measured here (the
            // copies' positions differ by a few cm..m; the host's angle would miss us). Only while the host's
            // look already points our way (within 35 deg), so a turning / moving enemy keeps the host's look.
            const volatile float *pf = (const volatile float *)(uintptr_t)(r.body + 0xA0), *me = (const volatile float *)(uintptr_t)(pobj + 0xA0);
            float dx = me[0] - pf[0], dy = (me[1] + 1.3f) - (pf[1] + 1.6f), dz = me[2] - pf[2];
            float hl = sqrtf(dx * dx + dz * dz);
            if (hl > 0.5f) {
                float ay = 57.2957795f * atan2f(dx, dz), ap = -57.2957795f * atan2f(dy, hl);
                float dyaw = ay - r.lyaw; while (dyaw > 180.0f) dyaw -= 360.0f; while (dyaw < -180.0f) dyaw += 360.0f;
                if (fabsf(dyaw) < 35.0f) {
                    lyaw = ay; lpitch = ap; aimyaw = ay;
                    static uint32_t al;
                    if ((al++ % 600) == 0) logf("coop: enemy aim: puppet %08x aims at US: host look %.0f/%.0f -> local %.0f/%.0f (%.1f m)", r.body, r.lyaw, r.lpitch, ay, ap, hl);
                }
            }
        }
        *(volatile float *)(uintptr_t)(o + 0x14) = aimyaw;
        *(volatile float *)(uintptr_t)(o + 0x18) = lyaw;
        *(volatile float *)(uintptr_t)(o + 0x1C) = lpitch;
        *(volatile float *)(uintptr_t)(o + 0x20) = sp;  *(volatile float *)(uintptr_t)(o + 0x84) = sp;
        *(volatile float *)(uintptr_t)(o + 0x60) = dx;  *(volatile float *)(uintptr_t)(o + 0x64) = 0.0f; *(volatile float *)(uintptr_t)(o + 0x68) = dz;
        *(volatile float *)(uintptr_t)(o + 0x70) = dx;  *(volatile float *)(uintptr_t)(o + 0x74) = 0.0f; *(volatile float *)(uintptr_t)(o + 0x78) = dz;
        *(volatile uint8_t *)(uintptr_t)(o + 0x41) = (uint8_t)(r.fire ? 1 : 0);
        *(volatile uint8_t *)(uintptr_t)(o + 0x4D) = 0;   // phase 3: local puppet bullets never hurt us (the host decides)
        link_sync_voice(o, u->orig);   // puppets take hit reactions (pain barks) too
        // position: target = host feet + velocity * packet age; snap > 2 m, else blend 25%
        float tx = r.feet[0] + u->v.vx * age, ty = r.feet[1], tz = r.feet[2] + u->v.vz * age;
        volatile float *f = (volatile float *)(uintptr_t)(r.body + 0xA0);
        float ex = tx - f[0], ey = ty - f[1], ez = tz - f[2], gap = sqrtf(ex * ex + ez * ez);
        float a = gap > 2.0f ? 1.0f : 0.25f;
        float mx = ex * a, my = ey * a, mz = ez * a;
        f[0] += mx; f[1] += my; f[2] += mz;
        volatile float *pv = (volatile float *)(uintptr_t)(r.body + 0x190);   // never read as speed
        pv[0] += mx; pv[1] += my; pv[2] += mz;
        static uint32_t pl;
        if ((pl++ % 600) == 0)
            logf("coop: enemy sync: puppet %08x gap %.2f m speed %.2f yaw %.0f look %.0f/%.0f aim yaw %.0f (bearing %.0f) pitch %.0f fire %u tgt %u (%d puppets)",
                 r.body, gap, sp, r.byaw, r.lyaw, r.lpitch, aimyaw, r.bear, lpitch, r.fire, r.tgt, npup);
    }
    // --downed-at N (joiner): one synthetic 26-dmg "host hit" N seconds after launch
    // so the downed sequence (pin -> guard clamp -> hp 1 -> spectator) fires
    // deterministically, without waiting for the yard's stochastic fire.
    if (g_cfg.downed_at > 0 && g_cfg.coop_role == 2 && !g_coop_downed && g_p10_gdst) {
        static int fired = 0;
        if (!fired && (int64_t)(GetTickCount64() - g_coop_t0) >= (int64_t)g_cfg.downed_at * 1000) {
            fired = 1;
            coop_downed_test_fire(pobj);
        }
    }
    coop_enray_catchup();   // host enemy shots our puppets did not fire themselves
    coop_hit_apply(pobj);   // phase 3: the host's hits on our clone hurt our player
    coop_xd_apply(pobj);    // explosion / melee hits on our clone (host) hurt our player
    // 3) run pending kills
    for (int k = 0; k < npend; k++) {
        uint32_t b = pend[k].body;
        float hp = *(volatile float *)(uintptr_t)(b + 0x2F0);
        // tg4 (312 failed kills): the game refilled the body slot with a NEW enemy before the kill was
        // seen as done, and the kill (tracked by address) kept shooting the newcomer. The kill belongs
        // to an enemy ID: once this body no longer carries it, the target is gone.
        if (pend[k].id && coop_enid_of(b) != pend[k].id) {
            static int gl;
            if (gl++ < 60) logf("coop: enemy sync: %08x kill done - its enemy (id %08x) is gone, the slot now holds id %08x", b, pend[k].id, coop_enid_of(b));
            coop_pup_release(b, "kill target gone");
            pend[k] = pend[--npend]; k--;
            continue;
        }
        if (!(hp > 0.0f) || pend[k].tries >= 8) {
            logf("coop: enemy sync: %08x %s after %d forced shot(s)", b, hp > 0.0f ? "STILL ALIVE (gave up)" : "killed", pend[k].tries);
            pend[k] = pend[--npend]; k--;
            continue;
        }
        if (now < pend[k].next || g_p10_gdst < 0x80000000) continue;
        {   // sp1 / sp3 / ar3: every renderer crash (0x69955) followed a kill shot on a body spawned 0-40 ms
            // before - its render setup was not done. A host-ordered kill waits until the enemy is 1.5 s old.
            uint64_t born = 0;
            for (int q = 0; q < g_enid_n; q++) if (g_enid[q].live && g_enid[q].body == b) born = g_enid[q].ms;
            if (born && now - born < 1500) continue;
        }
        const volatile float *f = (const volatile float *)(uintptr_t)(b + 0xA0);
        const volatile float *c = (const volatile float *)(uintptr_t)(g_p10_gdst + 0xA0);
        float chest[3] = {f[0], f[1] + 1.2f, f[2]};
        float d[3] = {chest[0] - c[0], chest[1] - (c[1] + 1.4f), chest[2] - c[2]};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 0.01f) { d[0] = 0; d[1] = 0; d[2] = 1; l = 1; }
        for (int i = 0; i < 3; i++) { d[i] /= l; g_kill_ray.d[i] = d[i]; g_kill_ray.o[i] = chest[i] - 0.6f * d[i]; }
        bool blasted = false;
        for (int q = 0; q < 16 && pend[k].id && pend[k].tries == 0; q++) if (g_killdir[q].id == pend[k].id && g_killdir[q].cause == 2) {
            // the host's enemy died in a BLAST: the same blast call kills our puppet (full damage, no falloff)
            const KillDir &kd = g_killdir[q];
            for (int qq = 0; qq < npup; qq++)
                if (pup[qq].body == b && pup[qq].orig && *(volatile uint32_t *)(uintptr_t)(b + 0x324) == pup[qq].link) {
                    *(volatile uint32_t *)(uintptr_t)(b + 0x324) = pup[qq].orig; pup[qq].link = 0;
                }
            *(volatile uint8_t *)(uintptr_t)(b + 0x3B0) = 0;
            __declspec(align(16)) static float v0[4], v1[4], v3[4];
            memcpy(v0, kd.bv[0], 16); memcpy(v1, kd.bv[1], 16); memcpy(v3, kd.bv[2], 16);
            float big = 5000.0f;
            uint32_t vt = *(volatile uint32_t *)(uintptr_t)b;
            g_bk_fn = *(volatile uint32_t *)(uintptr_t)(vt + 0x2C); g_bk_this = b;
            g_bk_a[0] = (uint32_t)(uintptr_t)v0; g_bk_a[1] = (uint32_t)(uintptr_t)v1; memcpy((void *)&g_bk_a[2], &big, 4);
            g_bk_a[3] = (uint32_t)(uintptr_t)v3; g_bk_a[4] = kd.braw[4]; g_bk_a[5] = kd.braw[5]; g_bk_a[6] = 0;
            coop_thunk_blast_kill();
            blasted = true;
            static int bk;
            if (bk++ < 30) logf("coop: kill dir: %08x (id %08x) killed by the host's BLAST (kind %u) at %.1f %.1f %.1f -> hp %.2f",
                                b, pend[k].id, kd.braw[5], v0[0], v0[1], v0[2], *(volatile float *)(uintptr_t)(b + 0x2F0));
            break;
        }
        for (int q = 0; q < 16 && !blasted && pend[k].id && pend[k].tries == 0; q++) if (g_killdir[q].id == pend[k].id && g_killdir[q].cause == 3 && g_p10_gdst) {
            // the host's enemy died from a MELEE hit: the same directional hit kills our puppet
            const KillDir &kd = g_killdir[q];
            for (int qq = 0; qq < npup; qq++)
                if (pup[qq].body == b && pup[qq].orig && *(volatile uint32_t *)(uintptr_t)(b + 0x324) == pup[qq].link) {
                    *(volatile uint32_t *)(uintptr_t)(b + 0x324) = pup[qq].orig; pup[qq].link = 0;
                }
            *(volatile uint8_t *)(uintptr_t)(b + 0x3B0) = 0;
            __declspec(align(16)) static float mv[4], md[4];
            memcpy(mv, kd.bv[0], 16); memcpy(md, kd.bv[1], 16);
            float big = 5000.0f;
            uint32_t vt = *(volatile uint32_t *)(uintptr_t)b;
            g_bk_fn = *(volatile uint32_t *)(uintptr_t)(vt + 0x48); g_bk_this = b;
            g_bk_a[0] = (uint32_t)(uintptr_t)mv; g_bk_a[1] = (uint32_t)(uintptr_t)md; memcpy((void *)&g_bk_a[2], &big, 4); g_bk_a[3] = g_p10_gdst;
            g_bk_a[4] = g_bk_a[5] = g_bk_a[6] = 0;
            coop_thunk_blast_kill();
            blasted = true;
            static int mk;
            if (mk++ < 30) logf("coop: kill dir: %08x (id %08x) killed by the host's MELEE hit -> hp %.2f", b, pend[k].id, *(volatile float *)(uintptr_t)(b + 0x2F0));
        }
        if (blasted) { pend[k].tries++; pend[k].next = now + 250; continue; }
        for (int q = 0; q < 16 && pend[k].id; q++) if (g_killdir[q].id == pend[k].id) {   // the host's real killing hit
            const KillDir &kd = g_killdir[q];
            float hl = sqrtf(kd.dir[0] * kd.dir[0] + kd.dir[1] * kd.dir[1] + kd.dir[2] * kd.dir[2]);
            if (hl < 0.1f) break;
            float tp[3] = {chest[0], chest[1], chest[2]};
            float gx = kd.pt[0] - chest[0], gy = kd.pt[1] - chest[1], gz = kd.pt[2] - chest[2];
            // pj1: the host's exact hit point can miss this copy's pose (4-5 tries) -> only the FIRST try goes
            // through that point; later tries keep the host's direction through the chest
            if (pend[k].tries == 0 && gx * gx + gy * gy + gz * gz < 1.5f * 1.5f) { tp[0] = kd.pt[0]; tp[1] = kd.pt[1]; tp[2] = kd.pt[2]; }
            for (int i = 0; i < 3; i++) { g_kill_ray.d[i] = kd.dir[i] / hl; g_kill_ray.o[i] = tp[i] - 0.6f * g_kill_ray.d[i]; }
            static int kl;
            if (kl++ < 30) logf("coop: kill dir: %08x (id %08x) killed along the host's last hit %.2f %.2f %.2f at %.1f %.1f %.1f", b, pend[k].id,
                                g_kill_ray.d[0], g_kill_ray.d[1], g_kill_ray.d[2], tp[0], tp[1], tp[2]);
            break;
        }
        // mf1: a kill gave up after 8 forced shots - every ray along the host's direction was taken but never
        // reached the body (something on this copy sits in that line). From the 3rd try on: straight DOWN onto
        // the chest from 0.6 m above it (overhead cover is rare); the knockback then points down.
        if (pend[k].tries >= 2) {
            g_kill_ray.d[0] = 0.0f; g_kill_ray.d[1] = -1.0f; g_kill_ray.d[2] = 0.0f;
            g_kill_ray.o[0] = chest[0]; g_kill_ray.o[1] = chest[1] + 0.6f; g_kill_ray.o[2] = chest[2];
            static int dl; if (dl++ < 20) logf("coop: kill dir: %08x try %d - the host's line missed, firing down onto the chest", b, pend[k].tries);
        }
        // en124: the joiner crashed 2.2 s after a puppet's death at 0x19202f - its AI agent
        // still drives the ORIGINAL class link (ECX 8014b260), which never saw the death
        // (it ran on our private copy) -> restore the original link before the kill shot
        for (int q = 0; q < npup; q++)
            if (pup[q].body == b && pup[q].orig && *(volatile uint32_t *)(uintptr_t)(b + 0x324) == pup[q].link) {
                *(volatile uint32_t *)(uintptr_t)(b + 0x324) = pup[q].orig;
                logf("coop: enemy sync: %08x link restored %08x -> %08x before the kill", b, pup[q].link, pup[q].orig);
                pup[q].link = 0;   // dead bodies are skipped; a re-spawn at this address is re-privatized
            }
        *(volatile uint8_t *)(uintptr_t)(b + 0x3B0) = 0;
        *(volatile float *)(uintptr_t)(b + 0x2F0) = 0.01f;
        // play 20261114_100655: the kill shot is the CLONE's pellet - its muzzle flash and fire sound played at
        // the host's clone for every host-ordered kill (squadmates', grenades', ...) with the host not firing.
        // The ray is forced (0.6 m in front of the victim), so the shooter's place does not matter: the PARKED
        // self model (100 m below the level) fires it - flash and sound out of sight and hearing. Fallback: the clone.
        uint32_t shooter = g_p10_gdst;
        if (g_coop_self_body && g_self_link && !g_coop_downed) {
            uint32_t Ws = *(volatile uint32_t *)(uintptr_t)(g_coop_self_body + 0x278 + 0x28), cs = 0;
            if (Ws >= 0x80000000 && Ws < 0x84000000) cs = *(volatile uint32_t *)(uintptr_t)(Ws + 0xF4);
            if (cs >= 0x80000000 && cs < 0x84000000) {
                shooter = g_coop_self_body;
                *(volatile uint8_t *)(uintptr_t)(g_self_link + 0x4D) = 1;   // "may hurt now"
            }
        }
        { static int sl; if (sl++ < 5) logf("coop: kill shot: fired by the %s %08x", shooter == g_p10_gdst ? "CLONE (no parked self model)" : "parked SELF MODEL (no flash / sound at the clone)", shooter); }
        uint32_t W = *(volatile uint32_t *)(uintptr_t)(shooter + 0x278 + 0x28);
        uint32_t ctx = (W >= 0x80000000 && W < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(W + 0xF4) : 0;
        if (ctx >= 0x80000000 && ctx < 0x84000000) {
            if (!g_shot_fn) g_shot_fn = xbe_va(0x001DBD50);
            g_kill_ray_on = 1;
            g_shot_ctx_arg = ctx;
            g_kill_dbg_victim = b; g_kill_dbg_dmg = 0;
            int16_t clip0 = *(volatile int16_t *)(uintptr_t)(ctx + 0x18);
            if (clip0 < 2) *(volatile int16_t *)(uintptr_t)(ctx + 0x18) = 2;     // the kill shot must not be a dry pull
            coop_thunk_shot();
            static int kd;
            if (kd++ < 40) logf("coop: kill shot: victim %08x try %d | ray taken %d | clip %d -> %d | W %08x type %d | hp now %.3f +3B0 %u | clone link +4D %u | dist %.1f m",
                                b, pend[k].tries, g_kill_ray_on ? 0 : 1, (int)clip0, (int)*(volatile int16_t *)(uintptr_t)(ctx + 0x18), W, weapon_type(W),
                                *(volatile float *)(uintptr_t)(b + 0x2F0), *(volatile uint8_t *)(uintptr_t)(b + 0x3B0),
                                g_aim_link ? *(volatile uint8_t *)(uintptr_t)(g_aim_link + 0x4D) : 9, l);
            g_kill_ray_on = 0;
        }
        pend[k].tries++;
        pend[k].next = now + 250;
    }
}

// ---- PHASE 3: enemy damage to the joiner, decided on the HOST.
// Naszran trace (nas135-137): enemy bullet hits are queued per bullet by FUN_001bac00 and
// applied later through FUN_001d89e0 -> the victim's vtable+0x20: FUN_001ed990 for the
// player (writes +0x2F0 at 0x1edaf8, 26 per hit from 41 m), FUN_001e74b0 for pawns -
// the SAME thiscall arguments: (hitvec*, part, damage, p5, p6, attacker [, flags]); the
// damage comes from the shot (weapon stats + distance), not from the victim type. Both
// gate on the attacker's link byte +0x4D ("may hurt now"; 0 for the dock enemy that was
// fighting the squad -> the en128-129 forced shots never hurt the player).
//  - host: entry hook on FUN_001e74b0 (55 8B EC 83 E4 F0, resume +6); victim == the
//    joiner's clone and attacker an enemy with link+0x4D != 0 -> queue a HitMsg; sent
//    as "CLHT" (magic 0x54484C43) after the state packet.
//  - joiner: each HitMsg -> FUN_001ed990 on our player (ECX), attacker = the paired
//    puppet with its link +0x4D forced to 1 for the call. Puppet links keep +0x4D = 0
//    the rest of the time, so the joiner's local copies of enemy bullets never hurt it.
volatile uint32_t g_dmg_victim, g_dmg_args, g_dmg_resume;
// CLONE SHIELD (day 6, dn144/145): re-asserting hp FLT_MAX cannot stop the death
// path - a class-kill style damage fn zeroes hp AND latches the death state in one
// frame, and the game's death processing then crashes on the private link's
// uninitialized fields (dn144: FUN_0017d130 voice table[link+0xD8]; dn145: AV at
// 0019fd97 deeper in the same chain). The clone is a PUPPET - its real health
// lives on the other machine - so ALL local damage is skipped: entry hooks on the
// three pawn damage virtuals return without running the body when the victim is
// our clone. The FUN_001e74b0 hook still relays the hit to the joiner (the relay
// reads the args before the skip). Epilogues verified from the XBE:
// 001e74b0/001df830 = ret 0x1C (7 stack args), 001dfa90 = ret 0x10 (4 args).
volatile uint32_t g_dmg_skip = 0;
// DESTRUCTIBLE SYNC part 1 (day 9; 8 of 9 owner-session joiner crashes): a destructible blew
// up on the JOINER first (its DESTRUCT.bks load), the local blast killed the joiner's copy of
// an enemy the host still had alive at 100 hp (22:44: "host enemy 801e4120 has NO live local
// enemy"), and the dead copy's AI then read its null weapon. The host owns enemy health and
// deaths, so on the joiner NO local damage reaches a puppet: every damage virtual skips a
// victim whose immune byte +0x3B0 is set (coop_enemy_tick sets it on every paired enemy and
// clears it only for the host-ordered kill shot, which therefore still lands).
static volatile uint32_t g_pi_hits[4];
static bool coop_puppet_immune(uint32_t v, int which, uint32_t args = 0)
{
    if (g_cfg.coop_role != 2 || v < 0x80000000 || v >= 0x84000000 || v == g_p10_gdst) return false;
    if (*(volatile uint32_t *)(uintptr_t)v != 0x0028C1E8) return false;           // pawns only
    if (*(volatile uint8_t *)(uintptr_t)(v + 0x3B0) != 1) return false;           // our immune marker
    if (g_pi_hits[which & 3]++ < 8) {
        static const char *nm[4] = {"bullet +0x20", "debris +0x24", "explosion +0x2C", "directional +0x48"};
        const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)args;
        if (args >= 0x10000)
            logf("coop: puppet shield: local %s damage on puppet %08x skipped (host owns its health) | ret %08x args %08x %08x %08x %08x %08x %08x %08x",
                 nm[which & 3], v, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        else logf("coop: puppet shield: local %s damage on puppet %08x skipped (host owns its health)", nm[which & 3], v);
    }
    return true;
}
static void coop_threat_add(uint32_t enemy, uint32_t attacker, float dmg);
extern "C" void coop_on_pawn_damage(void)
{
    if (coop_is_self(g_dmg_victim)) { g_dmg_skip = 1; return; }   // the self model takes no damage
    g_dmg_skip = (g_p10_gdst && g_dmg_victim == g_p10_gdst) ? 1 : 0;
    // tg8: the game clears the clone link's +0x4D ("may hurt now") in the clone's own update, after our
    // per-frame write - by the time a queued bullet is applied it read 0 and the hit was rejected (kill
    // shots: 1 landed, 15 gave up). Every bullet the clone fires is a real one (the peer's shot or a
    // host-ordered kill): set the flag right here, at the damage call, when the attacker is the clone.
    if (g_dmg_args >= 0x10000 && g_p10_gdst) {
        const volatile uint32_t *da = (const volatile uint32_t *)(uintptr_t)g_dmg_args;
        if (da[6] == g_p10_gdst || (coop_is_self(da[6]) && g_kill_ray_on)) {
            uint32_t cl = *(volatile uint32_t *)(uintptr_t)(da[6] + 0x324);
            if (cl >= 0x10000) *(volatile uint8_t *)(uintptr_t)(cl + 0x4D) = 1;
        }
    }
    if (g_dmg_victim == g_kill_dbg_victim && g_dmg_args >= 0x10000) {
        const volatile uint32_t *ka = (const volatile uint32_t *)(uintptr_t)g_dmg_args;
        float kd; uint32_t kv = ka[3]; memcpy(&kd, &kv, 4);
        uint32_t al = (ka[6] >= 0x80000000 && ka[6] < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(ka[6] + 0x324) : 0;
        static int kl;
        if (kl++ < 40) logf("coop: kill shot: BULLET DAMAGE reached %08x: dmg %.1f attacker %08x (link %08x +4D %d) | victim hp %.3f +3B0 %u",
                            g_dmg_victim, kd, ka[6], al, al >= 0x10000 ? (int)*(volatile uint8_t *)(uintptr_t)(al + 0x4D) : -1,
                            *(volatile float *)(uintptr_t)(g_dmg_victim + 0x2F0), *(volatile uint8_t *)(uintptr_t)(g_dmg_victim + 0x3B0));
    }
    if (!g_dmg_skip && coop_puppet_immune(g_dmg_victim, 0, g_dmg_args)) { g_dmg_skip = 1; return; }
    if (g_cfg.coop_role == 1 && g_dmg_victim != g_p10_gdst && g_dmg_args >= 0x10000) {   // aggro: threat bookkeeping
        const volatile uint32_t *ta = (const volatile uint32_t *)(uintptr_t)g_dmg_args;
        float td; uint32_t tv = ta[3]; memcpy(&td, &tv, 4);
        coop_threat_add(g_dmg_victim, ta[6], td);
        float v4[4], p4[4];
        if (coop_safe_copy(v4, ta[1], 16) && coop_safe_copy(p4, ta[2], 16)) {
            int slot = -1;
            for (int q = 0; q < 32; q++) if (g_lasthit[q].body == g_dmg_victim) slot = q;
            if (slot < 0) slot = (int)(g_lasthit_w++ & 31);
            LastHit &lh = g_lasthit[slot];
            // play 20261113_214731: the host's MELEE kills reached the joiner as bullet deaths - a real melee
            // strike also runs the bullet damage right after vt+0x48. A melee / blast cause from the last
            // 0.5 s is kept.
            if (lh.body == g_dmg_victim && lh.cause >= 2 && GetTickCount64() - lh.ms < 500) goto lasthit_done;
            lh.body = g_dmg_victim; lh.ms = GetTickCount64();
            // kd1: arg 1 (vec) is the HIT POINT (world; victim feet + ~1 m), arg 2 (part) the SHOT DIRECTION
            // (unit; matches attacker -> victim: 0.60 0.80 vs 0.56 0.83)
            memcpy(lh.pt, v4, 12); memcpy(lh.dir, p4, 12);
            lh.cause = 1;
        lasthit_done:;
            static int vl;
            uint32_t att = ta[6];
            if (vl++ < 12 && att >= 0x80000000 && att < 0x84000000) {   // which way does vec point? (vs attacker -> victim)
                const volatile float *A = (const volatile float *)(uintptr_t)(att + 0xA0), *V = (const volatile float *)(uintptr_t)(g_dmg_victim + 0xA0);
                float ex = V[0] - A[0], ez = V[2] - A[2], el = sqrtf(ex * ex + ez * ez), vl2 = sqrtf(v4[0] * v4[0] + v4[2] * v4[2]);
                logf("coop: kill dir: hit on %08x vec %.2f %.2f %.2f | attacker->victim %.2f %.2f | dot %.2f | point %.1f %.1f %.1f (victim feet %.1f %.1f %.1f)",
                     g_dmg_victim, v4[0], v4[1], v4[2], el > 0 ? ex / el : 0, el > 0 ? ez / el : 0, (el > 0 && vl2 > 0) ? (v4[0] * ex + v4[2] * ez) / (el * vl2) : 0,
                     p4[0], p4[1], p4[2], V[0], V[1], V[2]);
            }
        }
    }
    if (g_cfg.coop_role != 1 || g_dmg_victim != g_p10_gdst || !g_p10_gdst) return;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_dmg_args;   // [0] = return address
    uint32_t vecp = a[1], part = a[2], p5 = a[4], p6 = a[5], att = a[6];
    float dmg = *(const volatile float *)&a[3];
    if (att < 0x80000000 || att >= 0x84000000 || vecp < 0x10000) return;
    uint32_t al = *(volatile uint32_t *)(uintptr_t)(att + 0x324);
    bool may = al >= 0x80000000 && al < 0x84000000 && *(volatile uint8_t *)(uintptr_t)(al + 0x4D) != 0;
    static uint32_t ln;
    if (ln++ < 30) logf("coop: hit relay: clone hit by %08x dmg %.1f part %08x p5 %u p6 %u (may hurt %d)", att, dmg, part, p5, p6, (int)may);
    {   // play 20261113_190252: a SQUADMATE (team 0, the clone's own team) shot the clone and the joiner took
        // the hit (11 of 30 relayed hits came from 801e39a0). Allies never hurt the player in the game:
        // a hit from the clone's own team is never relayed.
        // play 20261113_191753: the clone's own +0x39C was 1 until 75 s (coop "ally" sets it to 0 later) -> the
        // team of OUR PLAYER is the reference: the squad roster slot 3 (registry + 0x22980) = the local player.
        uint32_t at = 0xFFFFFFFF, ct = 0xFFFFFFFE, R = *(volatile uint32_t *)(uintptr_t)0x002D1988, me = 0;
        coop_safe_copy(&at, att + 0x39C, 4);
        if (coop_safe_copy(&me, R + 0x22980, 4) && me) coop_safe_copy(&ct, me + 0x39C, 4);
        else coop_safe_copy(&ct, g_p10_gdst + 0x39C, 4);
        if (at == ct) {
            static uint32_t fl;
            if (fl++ < 20) logf("coop: hit relay: clone hit by %08x of the clone's own team %u (an ally) - not relayed", att, at);
            return;
        }
    }
    if (!may) {
        // play 20261114_165540 (owner: "enemies don't damage the joiner"): the clone is a SQUAD soldier now (own spawn,
        // type 0x1E) and the game never sets "may hurt" (+0x4D) for an enemy fighting a squadmate (they are meant to
        // survive) - hitsA: 30+ hits on the clone, all "may hurt 0", none relayed (the enemy-built clone: 58). The clone
        // stands for a PLAYER: a hit from an enemy whose current TARGET is the clone counts (the target the aggro code
        // reads: brain +0x2A8 -> registry *0x2D1988 +0xFB0 + idx * 0xC, [0] == 0, [4] = the body). Stray shots aimed at
        // the real squadmates still never hurt the joiner.
        uint32_t reg = *(volatile uint32_t *)(uintptr_t)0x2D1988, tgt = 0; int idx = -1;
        if (reg >= 0x80000000 && reg < 0x84000000 && coop_safe_copy(&idx, al + 0x2A8, 4) && idx >= 0 && idx < 4096) {
            uint32_t rec[2] = {1, 0};
            if (coop_safe_copy(rec, reg + 0xFB0 + idx * 0xC, 8) && rec[0] == 0) tgt = rec[1];
        }
        static uint32_t tl;
        if (tl++ < 20) logf("coop: hit relay: may-hurt 0 - the attacker's target is %08x (%s)", tgt, tgt == g_p10_gdst ? "the clone: relayed" : "not the clone: dropped");
        if (tgt != g_p10_gdst) return;
    }
    if (g_hit_out_n >= 16) return;
    HitMsg &h = g_hit_out[g_hit_out_n++];
    for (int k = 0; k < 4; k++) h.vec[k] = *(const volatile float *)(uintptr_t)(vecp + 4 * k);
    h.part = part; h.p5 = p5; h.p6 = p6; h.attacker = att; h.dmg = dmg;
    h.pvec_ok = coop_safe_copy(h.pvec, part, 16) ? 1u : 0u;
    if (!h.pvec_ok) for (int k = 0; k < 4; k++) h.pvec[k] = h.vec[k];
}
__declspec(naked) void coop_thunk_pawn_damage()
{
    __asm {
        pushad
        mov dword ptr [g_dmg_victim], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_dmg_args], eax
        call coop_on_pawn_damage
        popad
        cmp dword ptr [g_dmg_skip], 0
        jne skipdmg
        push ebp                           // displaced: 55
        mov ebp, esp                       // displaced: 8B EC
        and esp, 0xFFFFFFF0                // displaced: 83 E4 F0
        jmp dword ptr [g_dmg_resume]
    skipdmg:
        ret 0x1C                           // thiscall callee-clean: ret + 7 stack args
    }
}
// clone shield entry hooks on the other two damage virtuals (no relay needed yet;
// explosions/stealth relay joins the grenade round).
volatile uint32_t g_xd_victim, g_xd_resume, g_xd_args, g_sd_args;
// review step: log the REAL arguments of non-bullet hits on the clone (the explosion /
// melee relay needs to know which args are pointers before translating them).
static void log_shield_hit(const char *which, uint32_t args, int nargs)
{
    static int n;
    if (n++ >= 24 || args < 0x10000) return;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)args;   // [0] = return address
    char buf[600]; int o = snprintf(buf, sizeof buf, "coop: clone shield: %s hit on the clone, ret %08x args", which, a[0]);
    for (int i = 1; i <= nargs && o < 560; i++) {
        uint32_t v = a[i];
        o += snprintf(buf + o, sizeof buf - o, " [%d]=%08x", i + 1, v);
        if ((v >= 0x80000000 && v < 0x84000000) || (v >= 0x00100000 && v < 0x10000000)) {
            const volatile float *f = (const volatile float *)(uintptr_t)v;
            o += snprintf(buf + o, sizeof buf - o, "{%.2f %.2f %.2f %.2f}", f[0], f[1], f[2], f[3]);
        } else {
            o += snprintf(buf + o, sizeof buf - o, "(%.2f)", *(const volatile float *)&a[i]);
        }
    }
    logf("%s", buf);
}
// SEH-guarded copy (no C++ objects in this frame): false when the source is unreadable
static bool coop_safe_copy(void *dst, uint32_t src, size_t n)
{
    __try { memcpy(dst, (const void *)(uintptr_t)src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool coop_is_game_object(uint32_t v)
{
    uint32_t vt = 0;
    if (v < 0x80000000 || v >= 0x84000000 || !coop_safe_copy(&vt, v, 4)) return false;
    return vt == 0x0028C1E8 || vt == 0x0028CFE0;   // pawn / player damage vtables
}
// host: queue a non-bullet hit on the joiner's clone (called BEFORE the shield skip)
static uint64_t g_last_rocket_ms;   // host: the last local rocket blast (kind 2) - the joiner takes rocket blasts itself
static void coop_xd_capture(uint32_t slot, uint32_t args, int nargs, const char *which)
{
    if (g_cfg.coop_role != 1 || args < 0x10000 || g_xd_out_n >= 4) return;
    if (slot == 0x2C && g_last_rocket_ms && GetTickCount64() - g_last_rocket_ms < 1500) {
        static int rl;
        if (rl++ < 20) logf("coop: xd relay: blast on the clone right after a rocket - not relayed (the joiner takes rocket blasts from the replay)");
        return;
    }
    XdMsg &m = g_xd_out[g_xd_out_n];
    memset(&m, 0, sizeof m);
    m.slot = slot; m.nargs = (uint32_t)nargs;
    m.host_clone = g_p10_gdst;
    m.host_player = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)args;   // [0] = return address
    // xd192: guessing pointers from values crashed the joiner (a pointer outside the
    // guessed ranges travelled raw). The layouts are known from the decompiles:
    //  +0x2C (FUN_001df830 / player FUN_001ed2b0): hitvec*, dir*, dmg, vec*, -, kind, falloff
    //  +0x48 (FUN_001dfa90 / player FUN_001ed5d0): hitvec*, dir* (4 floats), dmg, attacker
    // VECTOR args are always sent as copies (zeros if unreadable - never a raw host
    // address); every other arg is checked for a game object (remapped on the joiner).
    const uint32_t vec_mask = slot == 0x2C ? 0x0Bu : 0x03u;
    m.ptrmask = vec_mask;
    for (int i = 0; i < nargs; i++) {
        uint32_t v = a[1 + i];
        m.raw[i] = v;
        if (vec_mask & (1u << i)) { if (!coop_safe_copy(m.data[i], v, sizeof m.data[i])) memset(m.data[i], 0, sizeof m.data[i]); }
        else if (coop_is_game_object(v)) m.objmask |= 1u << i;
    }
    g_xd_out_n++;
    g_xd_stats[0]++;
    static int ln;
    if (ln++ < 20) {
        float dmg = 0; memcpy(&dmg, &m.raw[2], 4);
        logf("coop: xd relay: %s hit on the joiner's clone queued (slot +0x%X dmg %.1f, ptr mask %x obj mask %x, ret %08x)",
             which, slot, dmg, m.ptrmask, m.objmask, a[0]);
    }
}
// GRENADE / BLAST DEATHS (owner: "enemies' death reaction from grenades"): the host remembers an enemy's
// last BLAST (vt+0x2C FUN_001df830 thiscall: hitvec*, dir*, dmg, vec*, p6, kind, falloff; RET 0x1C) and
// sends it with the death (CLKD cause 2); the joiner then kills its puppet with that same call (full damage,
// no falloff) instead of a bullet - the game's own blast death and ragdoll.
static void coop_note_blast(uint32_t victim, uint32_t args)
{
    if (g_cfg.coop_role != 1 || victim == g_p10_gdst || args < 0x10000) return;
    uint32_t vt = 0; if (!coop_safe_copy(&vt, victim, 4) || vt != 0x0028C1E8) return;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)args;   // [0] ret, [1..7] args
    // play 20261113_212058: blasts reach vt+0x2C with args 7/8 = 0/1, 3/2 ... (never "falloff 3" as assumed -
    // 0 blasts were noted). A blast is the queued-blast apply FUN_001ce790 -> vt+0x2C, return address 0x1ce812.
    if (a[0] != 0x001CE812) return;
    int slot = -1;
    for (int q = 0; q < 32; q++) if (g_lasthit[q].body == victim) slot = q;
    if (slot < 0) slot = (int)(g_lasthit_w++ & 31);
    LastHit &lh = g_lasthit[slot]; lh.body = victim; lh.cause = 2; lh.ms = GetTickCount64();
    const int vi[3] = {1, 2, 4};
    for (int k = 0; k < 3; k++) if (!coop_safe_copy(lh.bv[k], a[vi[k]], 16)) memset(lh.bv[k], 0, 16);
    for (int k = 0; k < 7; k++) lh.braw[k] = a[1 + k];
    memcpy(lh.pt, lh.bv[0], 12);
    float l = sqrtf(lh.bv[1][0] * lh.bv[1][0] + lh.bv[1][1] * lh.bv[1][1] + lh.bv[1][2] * lh.bv[1][2]);
    for (int k = 0; k < 3; k++) lh.dir[k] = l > 1e-3f ? lh.bv[1][k] / l : 0.0f;
    static int bl;
    float dmg; memcpy(&dmg, &lh.braw[2], 4);
    if (bl++ < 20) logf("coop: kill dir: BLAST on %08x dmg %.1f kind %u falloff %u at %.1f %.1f %.1f", victim, dmg, lh.braw[5], lh.braw[6], lh.pt[0], lh.pt[1], lh.pt[2]);
}
extern "C" void coop_on_generic_damage(void)
{
    if (coop_is_self(g_xd_victim)) { g_dmg_skip = 1; return; }
    coop_note_blast(g_xd_victim, g_xd_args);
    g_dmg_skip = (g_p10_gdst && g_xd_victim == g_p10_gdst) ? 1 : 0;
    if (!g_dmg_skip && coop_puppet_immune(g_xd_victim, 2, g_xd_args)) { g_dmg_skip = 1; return; }
    if (g_dmg_skip) { log_shield_hit("generic FUN_001df830", g_xd_args, 7); coop_xd_capture(0x2C, g_xd_args, 7, "explosion/generic"); }
}
__declspec(naked) void coop_thunk_generic_damage()   // FUN_001df830 (vt+0x2C): bullets/generic/explosions
{
    __asm {
        pushad
        mov dword ptr [g_xd_victim], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_xd_args], eax
        call coop_on_generic_damage
        popad
        cmp dword ptr [g_dmg_skip], 0
        jne skipx
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_xd_resume]
    skipx:
        ret 0x1C
    }
}
volatile uint32_t g_sd_victim, g_sd_resume;   // (g_sd_args declared above)
// MELEE SYNC (owner: "check if melee attacks are synced"). vt+0x48 FUN_001dfa90 (thiscall: hitvec*, dir*,
// dmg, attacker; RET 0x10) = the players' melee strikes / takedowns on enemies (it ignores team-0 victims,
// so enemy melee never hurts a player - vanilla; enemies' close hits are bullets, already synced).
//  - JOINER: our player's melee hit on a PUPPET (skipped here by the puppet shield) -> "CLML" to the host
//    {host body, hitvec, dir, dmg}; the host applies it to its real enemy through vt+0x48 with the clone as
//    the attacker.
//  - HOST: a melee hit is an enemy's last hit (cause 3); on its death the joiner's puppet gets the same
//    vt+0x48 hit (the game's melee death) instead of a bullet.
static void coop_melee_note(uint32_t victim, uint32_t args)
{
    if (args < 0x10000) return;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)args;   // [0] ret, [1] hitvec*, [2] dir*, [3] dmg, [4] attacker
    uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    if (g_cfg.coop_role == 2) {
        if (!me || a[4] != me || g_ml_out_n >= 4) return;
        // play 20261113_214731: g_hmap kept STALE pairs for a re-used body (801e57a0 -> 801e71e0, then
        // 801e53e0, already dead on the host) -> 3 of 6 melee hits were lost. The host body comes from the
        // enemy ID (the host's current enemy record with our puppet's id).
        uint32_t hb = 0, id = coop_enid_of(victim);
        for (int i = 0; i < 48 && id; i++) if (g_idhost[i].id == id) hb = g_idhost[i].host;
        if (!hb) { g_ml_stats[2]++; return; }
        MeleeMsg &m = g_ml_out[g_ml_out_n++];
        m.host_body = hb;
        if (!coop_safe_copy(m.hv, a[1], 16)) memset(m.hv, 0, 16);
        if (!coop_safe_copy(m.dir, a[2], 16)) { memset(m.dir, 0, 16); m.dir[2] = 1.0f; }
        memcpy(&m.dmg, (const void *)&a[3], 4);
        g_ml_stats[0]++;
        static int ml;
        if (ml++ < 30) logf("coop: melee sync: OUR melee hit on puppet %08x (host %08x) dmg %.1f -> host", victim, hb, m.dmg);
    } else if (g_cfg.coop_role == 1 && victim != g_p10_gdst) {
        uint32_t vt = 0; if (!coop_safe_copy(&vt, victim, 4) || vt != 0x0028C1E8) return;
        int slot = -1;
        for (int q = 0; q < 32; q++) if (g_lasthit[q].body == victim) slot = q;
        if (slot < 0) slot = (int)(g_lasthit_w++ & 31);
        LastHit &lh = g_lasthit[slot]; lh.body = victim; lh.cause = 3; lh.ms = GetTickCount64();
        if (!coop_safe_copy(lh.bv[0], a[1], 16)) memset(lh.bv[0], 0, 16);
        if (!coop_safe_copy(lh.bv[1], a[2], 16)) memset(lh.bv[1], 0, 16);
        memset(lh.bv[2], 0, 16);
        for (int k = 0; k < 4; k++) lh.braw[k] = a[1 + k];
        memcpy(lh.pt, lh.bv[0], 12);
        float l = sqrtf(lh.bv[1][0] * lh.bv[1][0] + lh.bv[1][1] * lh.bv[1][1] + lh.bv[1][2] * lh.bv[1][2]);
        for (int k = 0; k < 3; k++) lh.dir[k] = l > 1e-3f ? lh.bv[1][k] / l : 0.0f;
        static int hl;
        float dmg; memcpy(&dmg, (const void *)&a[3], 4);
        if (hl++ < 20) logf("coop: melee sync: melee hit on enemy %08x by %08x dmg %.1f (noted as its last hit)", victim, a[4], dmg);
    }
}
extern "C" void coop_on_stealth_damage(void)
{
    if (coop_is_self(g_sd_victim)) { g_dmg_skip = 1; return; }
    coop_melee_note(g_sd_victim, g_sd_args);
    g_dmg_skip = (g_p10_gdst && g_sd_victim == g_p10_gdst) ? 1 : 0;
    if (!g_dmg_skip && coop_puppet_immune(g_sd_victim, 3, g_sd_args)) { g_dmg_skip = 1; return; }
    if (g_dmg_skip) { log_shield_hit("stealth FUN_001dfa90", g_sd_args, 4); coop_xd_capture(0x48, g_sd_args, 4, "melee/directional"); }
}
__declspec(naked) void coop_thunk_stealth_damage()   // FUN_001dfa90 (vt+0x48): directional/stealth kills
{
    __asm {
        pushad
        mov dword ptr [g_sd_victim], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_sd_args], eax
        call coop_on_stealth_damage
        popad
        cmp dword ptr [g_dmg_skip], 0
        jne skips
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_sd_resume]
    skips:
        ret 0x10
    }
}
// debris/physics impact damage FUN_001dfc30 (pawn vt+0x24, called by the physics contact pass
// FUN_000e0aa0 with (contact, dir, dmg, other body); RET 0x10): puppets skip it on the joiner.
// DAY 11 FIX (playtest 20261112_205905, joiner invisible to the host): the debris thunk used to
// check ONLY coop_puppet_immune - which returns false for the clone (v == g_p10_gdst) and only
// applies on role == 2 - so on the HOST the clone took debris/physics-impact damage. The LAN
// drive hard-places the clone across a height drop (the Veblensk yard descent, ~13 s into the
// ghost route); the physics contact pass dealt a fatal impact hit, the death chain ran
// (watch2 stack: FUN_000e0aa0 -> FUN_001dfc30 -> FUN_002059a0 -> FUN_002055f0 -> life 1 ->
// FUN_001e68b0 -> life 2, brain freed), and the despawned body stopped being drawn.
volatile uint32_t g_pd24_victim, g_pd24_resume, g_pd24_skip;
extern "C" void coop_on_debris_damage(void)
{
    if (coop_is_self(g_pd24_victim)) { g_pd24_skip = 1; return; }
    if (g_p10_gdst && g_pd24_victim == g_p10_gdst) {
        static int ln;
        if (ln++ < 12) logf("coop: clone shield: debris/physics-impact hit on the clone skipped (puppet: real health is on the other machine)");
        g_pd24_skip = 1;
        return;
    }
    g_pd24_skip = coop_puppet_immune(g_pd24_victim, 1) ? 1 : 0;
}
__declspec(naked) void coop_thunk_debris_damage()
{
    __asm {
        pushad
        mov dword ptr [g_pd24_victim], ecx
        call coop_on_debris_damage
        popad
        cmp dword ptr [g_pd24_skip], 0
        jne skipd
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_pd24_resume]
    skipd:
        ret 0x10
    }
}
// (The day-11 fall / void death guards on FUN_00205b20 / FUN_002056a0 were removed on day 12: the
// CLONE DEATH GUARD (install_death_guard) covers those two and the other three death variants, and
// installed first, so these two only logged "bytes mismatch - not installed".)
// ===== SIGHT-RECORD TRACE (day 12, COOP_SIGHT_TRACE=1) =====
// How enemies PERCEIVE the player, the RE state: FUN_00195450 (143 bytes, VA 0x195450) is the
// single funnel for ALL awareness records - its only live caller is FUN_0019fea0 (distance
// weight) <- FUN_001a22b0 (noise broadcast; callers 1A811E = AI shout inside FUN_001a80a0,
// 1DBE06 = weapon fire). The XBE also holds a direct-apply wrapper FUN_001cc900 (thiscall:
// ECX = AI object, EDI = stimulus class, EBX = entity, stack = [weight(float), pos*];
// body: lea esi,[ecx+0x700]; push eax/edx; call FUN_00195450; ret 0x10) - but it has ZERO
// references anywhere in the image (no E8/E9, no stored pointer, not in any jump table):
// dead/outlined code, likely the outline of an inlined caller. FUN_00191590 (the awareness
// consumer) shows the slot layout: list base = link+0x700, per slot stride 0x20: +0x84 =
// position-cell id from FUN_0018c560(pawn pos +0xa0), +0x88 = entity; team check at
// entity+0x39C; it returns the entity's LIVE pos +0xa0..+0xac as the "seen" position.
// This trace logs every record that flows through the funnel with its caller chain, so a
// run where an enemy spots a silent player shows exactly which code produced the record
// (noise ret 1A231E vs anything else). Prologue 51 0F 57 C0 0F 2F 44 24 08 55
// (push ecx; xorps; comiss xmm0,[esp+8] = the weight>0 check; push ebp) - re-executed in
// full, resume site+0xA (E9 rel32 + 5 NOPs; the comiss cannot be split).
volatile uint32_t g_st_resume;
static int g_st_logn;
extern "C" void coop_sight_trace(uint32_t esi, uint32_t edi, uint32_t ebx, uint32_t retaddr,
                                 uint32_t wbits, uint32_t posptr, const uint32_t *espptr)
{
    float weight;
    memcpy(&weight, &wbits, 4);
    if (!(weight > 0.0f) || g_st_logn >= 800) return;
    g_st_logn++;
    // Entry state (from FUN_0019fea0's call site, raw-decoded): ESI = the AI agent (pawn at
    // [agent], body at [pawn+0x8C], awareness list at agent+0x700), EBX = the entity body,
    // EDI = the broadcast distance class (1/2/3), [esp+4] = weight, [esp+8] = the entity body
    // again (its +0xA0 IS the position - the game passes the body as the position carrier).
    uint32_t pawn = 0, pawnbody = 0, ateam = 0x7FFFFFFF, kind = 0x7FFFFFFF, eteam = 0x7FFFFFFF;
    if (esi >= 0x80000000u && esi < 0x84000000u) {
        pawn = *(volatile uint32_t *)(uintptr_t)esi;
        if (pawn >= 0x80000000u && pawn < 0x84000000u) {
            pawnbody = *(volatile uint32_t *)(uintptr_t)(pawn + 0x8C);
            if (pawnbody >= 0x80000000u && pawnbody < 0x84000000u)
                ateam = *(volatile uint32_t *)(uintptr_t)(pawnbody + 0x39C);
        }
    }
    // sp10: EBX is the CALLER's source-position buffer (a stack address), not the entity; the entity
    // body is the stack arg [esp+8] = posptr
    float px = 0, py = 0, pz = 0, sx = 0, sy = 0, sz = 0;
    uint32_t ent = posptr;
    if (ent >= 0x80000000u && ent < 0x84000000u) {
        coop_safe_copy(&kind, ent + 0xC4, 4);
        coop_safe_copy(&eteam, ent + 0x39C, 4);
        float p3[3] = {0, 0, 0}; coop_safe_copy(p3, ent + 0xA0, 12); px = p3[0]; py = p3[1]; pz = p3[2];
    }
    { float s3[3] = {0, 0, 0}; if (ebx >= 0x10000) coop_safe_copy(s3, ebx, 12); sx = s3[0]; sy = s3[1]; sz = s3[2]; }
    uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    const char *who = ent == me ? " = OUR PLAYER" : (ent == g_p10_gdst ? " = THE CLONE" : "");
    logf("coop: sight: agent %08x pawn %08x <- ent %08x%s k %u te E%u/A%u dcls %u w %.2f pos %.1f %.1f %.1f srcpos %.1f %.1f %.1f (ret %08x)",
         esi, pawn, ent, who, kind, eteam, ateam, edi, weight, px, py, pz, sx, sy, sz, retaddr);
    if (g_st_logn <= 80 && espptr) log_stack_trace(espptr);   // full chain for the first records
}
__declspec(naked) void coop_thunk_sight_trace()
{
    __asm {
        pushad
        // pushad slot map (EAX pushed FIRST = highest address): [B+0x00]=EDI [B+0x04]=ESI
        // [B+0x0C]=origESP [B+0x10]=EBX [B+0x1C]=EAX; above: [B+0x20]=ret [B+0x24]=weight [B+0x28]=entity
        lea eax, [esp + 0x20]        // espptr = &retaddr (cdecl arg 7, pushed first)
        push eax
        push dword ptr [esp + 0x2C]  // entity (arg 6)
        push dword ptr [esp + 0x2C]  // weight (arg 5)
        push dword ptr [esp + 0x2C]  // retaddr (arg 4)
        push dword ptr [esp + 0x20]  // ebx = entity body (arg 3)
        push dword ptr [esp + 0x14]  // edi = distance class (arg 2)
        push dword ptr [esp + 0x1C]  // esi = the AI agent (arg 1)
        call coop_sight_trace
        add esp, 0x1C
        popad
        push ecx                     // re-execute FUN_00195450's 10-byte prologue
        xorps xmm0, xmm0
        comiss xmm0, dword ptr [esp + 8]
        push ebp
        jmp dword ptr [g_st_resume]
    }
}
static void install_sight_trace(void)
{
    char e[8] = {0};
    if (!(GetEnvironmentVariableA("COOP_SIGHT_TRACE", e, sizeof e) && e[0] == '1')) return;
    uint32_t site = xbe_va(0x00195450);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[10] = {0x51, 0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x44, 0x24, 0x08, 0x55};
    if (memcmp(code, kExpect, 10) != 0) { logf("coop: sight trace: 00195450 bytes mismatch - not installed"); return; }
    g_st_resume = site + 10;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_sight_trace - (site + 5));
    memcpy(code + 1, &rel, 4);
    for (int i = 5; i < 10; i++) code[i] = 0x90;
    logf("coop: sight trace installed at FUN_00195450 (COOP_SIGHT_TRACE: every awareness record + caller chain)");
}
// play219 (owner, Naszran Foundry): the JOINER crashed at 0x1af1a3 right as a destructible
// exploded - FUN_001af190 (AI agent update, EDI = agent) reads the agent body's current
// weapon [[[EDI+0x3AC]+0x8C]+0x29C] and its ammo object [+0xF4] without a null check; the
// joiner-side puppet 801e4120 had body+0x29C = 0. Guard: return when either is null (the
// function only refreshes a clip counter). Entry 8B 87 AC 03 00 00 is re-executed.
volatile uint32_t g_wg_resume, g_wg_skip, g_wg_edi, g_wg_hits;
extern "C" void coop_weapon_guard_check(void)
{
    g_wg_skip = 0;
    if (g_coop_teardown) { g_wg_skip = 1; return; }   // the level is being quit: no re-arm
    uint32_t edi = g_wg_edi;
    if (edi < 0x80000000 || edi >= 0x84000000) return;
    uint32_t ag = *(volatile uint32_t *)(uintptr_t)(edi + 0x3AC);
    uint32_t body = (ag >= 0x80000000 && ag < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(ag + 0x8C) : 0;
    uint32_t w = (body >= 0x80000000 && body < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(body + 0x29C) : 0;
    uint32_t am = (w >= 0x80000000 && w < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(w + 0xF4) : 0;
    if ((w < 0x80000000 || w >= 0x84000000) && body >= 0x80000000 && body < 0x84000000) {
        // play 23:04: skipping only this reader was not enough - FUN_00194be0 read the same
        // null weapon in the same AI pass (AV 0x194bfb). Re-arm the puppet HERE, before any
        // other reader runs, then let the function go on.
        for (int k = 0; k < g_pup_narm; k++)
            if (g_pup_arm[k].body == body && g_pup_arm[k].w) {
                *(volatile uint32_t *)(uintptr_t)(body + 0x29C) = g_pup_arm[k].w;
                w = g_pup_arm[k].w;
                am = *(volatile uint32_t *)(uintptr_t)(w + 0xF4);
                static int rl;
                if (rl++ < 10) logf("coop: weapon guard: puppet %08x lost its weapon - re-armed %08x at the AI update", body, w);
                break;
            }
    }
    if (am < 0x80000000 || am >= 0x84000000) {
        g_wg_skip = 1;
        if (g_wg_hits++ < 10) logf("coop: weapon guard: agent %08x body %08x has no weapon/ammo (w %08x ammo %08x) - clip refresh skipped", ag, body, w, am);
    }
}
__declspec(naked) void coop_thunk_weapon_guard()
{
    __asm {
        pushad
        mov dword ptr [g_wg_edi], edi
        call coop_weapon_guard_check
        popad
        cmp dword ptr [g_wg_skip], 0
        jne skipw
        mov eax, dword ptr [edi + 0x3AC]   // displaced: 8B 87 AC 03 00 00
        jmp dword ptr [g_wg_resume]
    skipw:
        ret
    }
}
// EXPLOSION LOG (destructible sync RE): FUN_001e0970 = the game's core explosion (fastcall,
// EAX = kind, [esp+8] = centre vec4*, XMM2 = radius; camera shake, sound slots, then a world
// query whose callback 0x1cda10 -> FUN_001cd7a0 queues a blast on every object in range).
// Logging only - the body runs unchanged. Env COOP_EXPLOG=0 off.
volatile uint32_t g_ex_resume, g_ex_eax, g_ex_ecx, g_ex_edx, g_ex_esp, g_ex_ebp; volatile float g_ex_r;
static int g_pr_on = -1;   // prop sync (defined with it below)
static bool coop_pj_is_replayed(uint32_t obj);
volatile uint32_t g_ex_skip, g_ex_replaying;
static volatile int g_my_gren; static uint64_t g_my_gren_ms;   // joiner: our own grenades in flight
static struct { uint32_t kind; float c[3]; uint64_t ms; uint8_t replay; } g_ex_recent[128]; static uint32_t g_ex_rw;
// a recent explosion of this kind: within 2 m / 3 s (the same destructible going off on both copies),
// or at the SAME SPOT (< 0.3 m) within 180 s - play 20261112: level-script set pieces fire on each
// copy when ITS player reaches the trigger (7.5 s and 13 s apart), and the short window let the
// relay play them a second time on the other copy.
static int coop_ex_find(uint32_t kind, const float c[3], uint8_t replay)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 128; i++) {
        const auto &e = g_ex_recent[i];
        if (!e.ms || e.replay != replay || e.kind != kind || now - e.ms > 180000) continue;
        float dx = e.c[0] - c[0], dy = e.c[1] - c[1], dz = e.c[2] - c[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < 0.09f || (d2 < 4.0f && now - e.ms <= 3000)) return i;
    }
    return -1;
}
static void coop_ex_note(uint32_t kind, const float c[3], uint8_t replay)
{
    auto &e = g_ex_recent[g_ex_rw++ & 127];
    e.kind = kind; e.c[0] = c[0]; e.c[1] = c[1]; e.c[2] = c[2]; e.ms = GetTickCount64(); e.replay = replay;
}
extern "C" void coop_on_explosion(void)
{
    static uint32_t n;
    g_ex_skip = 0;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_ex_esp;   // [0] ret, [1] p3 owner, [2] centre*, [3] dir*, [4..9] p6..p11
    uint32_t cp = a[2];
    float c[4] = {0, 0, 0, 0};
    if (cp >= 0x10000) coop_safe_copy(c, cp, 16);
    n++;
    { float *ea = g_ex_all[g_ex_all_n++ & 63]; ea[0] = c[0]; ea[1] = c[1]; ea[2] = c[2]; }
    if (n <= 600) logf("coop: EXPLOSION #%u kind %u at %.1f %.1f %.1f radius %.2f%s | ret %08x ecx %08x edx %08x p3 %08x p5 %08x p6 %08x p7 %08x",
                      n, g_ex_eax, c[0], c[1], c[2], g_ex_r, g_ex_replaying ? " (REPLAY from the peer)" : "",
                      a[0], g_ex_ecx, g_ex_edx, a[1], a[3], a[4], a[5]);
    if (n <= 600) {   // stack arg 7 = the object that exploded (props / flying pieces pass themselves, grenades 0)
        uint32_t so = a[7], vt = 0, k = 0, ty = 0; float q[3] = {0, 0, 0};
        if (so >= 0x80000000 && so < 0x84000000 && coop_safe_copy(&vt, so, 4)) {
            coop_safe_copy(&k, so + 0xC4, 4); coop_safe_copy(&ty, so + 0x118, 4); coop_safe_copy(q, so + 0xA0, 12);
            logf("coop: EXPLOSION #%u source %s %08x (vt %08x kind %u type %08x) at %.1f %.1f %.1f", n,
                 (k == 4 && vt == 0x0028ED98) ? "PROP" : (k == 3 ? "FLYING PIECE" : "object"), so, vt, k, ty, q[0], q[1], q[2]);
        } else logf("coop: EXPLOSION #%u source: no world object (a grenade, projectile or chained blast; arg %08x)", n, so);
    }
    if (g_ex_eax == 2 || g_ex_eax == 3) g_last_rocket_ms = GetTickCount64();   // rockets and grenades (local and replayed) - see coop_xd_capture
    // PROP EXPLOSIONS (pr2 joiner crash 0x1766e2): a destructible's own explosion (ret 0x1e41e4) after its
    // break. With prop sync both copies break the prop through the same damage, so each runs its own blast:
    // never relayed, never deduped (skipping it left the prop's debris queue with a null piece). Damage:
    // the joiner takes it here; the host does not CLXD-relay the clone's hit from it (same window as rockets).
    // (pr4: NOT 0x1e5b19 - that blast comes from a destructible of ANOTHER class (not vt 0028ED98, so not in
    // prop sync); its neighbours only break on the joiner through the relayed blast. Kept on the relay.)
    // pr6/pr7: with the player-attacker fix the joiner's prop now breaks from the replayed damage itself, so the
    // prop's own blast (ret 0x1e5b19) is local on both copies too (it went through the risky dedupe skip)
    if ((a[0] == 0x001E41E4 || a[0] == 0x001E5B19) && g_pr_on == 1) { g_last_rocket_ms = GetTickCount64(); return; }
    if (g_ex_replaying || !g_cfg.coop_role) return;
    // a ROCKET we only replayed for the eyes (projectile sync): its real explosion comes from the shooter's
    // copy through this relay -> no local blast (no double damage, no second fireball)
    if ((a[0] == 0x001E35B6 || a[0] == 0x001E3763) && coop_pj_is_replayed(g_ex_ebp)) {
        g_ex_skip = 1; g_ex_stats[3]++;
        static int pl;
        if (pl++ < 40) logf("coop: projectile sync: replayed %s %08x went off at %.1f %.1f %.1f - local blast skipped (the shooter's blast is relayed)", a[0] == 0x001E3763 ? "grenade" : "rocket", g_ex_ebp, c[0], c[1], c[2]);
        return;
    }
    // kind 13 (0xD) = a local effect only: FUN_001e0970 / FUN_001cd7a0 skip all damage for it. The
    // joiner makes one ~0.8 s after each host-ordered kill shot (co-op prop test: relayed to the host
    // as a blast the host never had) -> never relayed, never deduped.
    if (g_ex_eax == 0xD) return;
    // JOINER GRENADES: the explosion call carries no thrower (the grenade object 0x120 B has none
    // we could identify). The joiner's local enemy COPIES can throw grenades the host's enemies never
    // threw -> on the joiner a grenade explosion (FUN_001e3680, ret 0x1e3763) is real only when OUR
    // grenade count (player +0x286, md1: 3 -> 2 on a throw) dropped recently; the rest are skipped
    // (the host's real enemy grenades arrive through this relay).
    if (g_cfg.coop_role == 2 && a[0] == 0x001E3763) {
        if (g_my_gren > 0) g_my_gren--;
        else {
            g_ex_skip = 1; g_ex_stats[3]++;
            static int gl;
            if (gl++ < 10) logf("coop: explosion relay: grenade at %.1f %.1f %.1f not thrown by our player (a local enemy copy) - skipped", c[0], c[1], c[2]);
            return;
        }
    }
    // a local explosion the peer already sent us (the same destructible broke on both copies) -> skip ours
    if (coop_ex_find(g_ex_eax, c, 1) >= 0) {
        g_ex_skip = 1; g_ex_stats[3]++;
        logf("coop: explosion relay: local kind %u at %.1f %.1f %.1f duplicates a replayed one - skipped", g_ex_eax, c[0], c[1], c[2]);
        return;
    }
    coop_ex_note(g_ex_eax, c, 0);
    if (g_ex_out_n >= 4) return;
    ExMsg &m = g_ex_out[g_ex_out_n++];
    memset(&m, 0, sizeof m);
    m.kind = g_ex_eax; m.ecx = g_ex_ecx; m.edx = g_ex_edx; m.radius = g_ex_r;
    for (int k = 0; k < 4; k++) m.centre[k] = c[k];
    m.dir_ok = (a[3] >= 0x10000 && coop_safe_copy(m.dir, a[3], 16)) ? 1u : 0u;
    for (int k = 0; k < 9; k++) m.raw[k] = a[1 + k];
    uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    m.owner_kind = (a[1] && a[1] == me) ? 1u : ((a[1] && a[1] == g_p10_gdst) ? 2u : 0u);
}
// replay call: FUN_001e0970 with the sender's registers and 9 stack args (callee cleans 0x24)
volatile uint32_t g_exr_fn, g_exr_eax, g_exr_ecx, g_exr_edx, g_exr_esp; volatile float g_exr_r;
volatile uint32_t g_exr_args[9];
__declspec(naked) void coop_thunk_ex_replay()
{
    __asm {
        pushad
        mov dword ptr [g_exr_esp], esp
        push dword ptr [g_exr_args + 32]
        push dword ptr [g_exr_args + 28]
        push dword ptr [g_exr_args + 24]
        push dword ptr [g_exr_args + 20]
        push dword ptr [g_exr_args + 16]
        push dword ptr [g_exr_args + 12]
        push dword ptr [g_exr_args + 8]
        push dword ptr [g_exr_args + 4]
        push dword ptr [g_exr_args]
        movss xmm2, dword ptr [g_exr_r]
        mov eax, dword ptr [g_exr_eax]
        mov ecx, dword ptr [g_exr_ecx]
        mov edx, dword ptr [g_exr_edx]
        call dword ptr [g_exr_fn]
        mov esp, dword ptr [g_exr_esp]
        popad
        ret
    }
}
static void coop_ex_apply(uint32_t pobj)
{
    if (pobj >= 0x80000000 && pobj < 0x84000000) {   // our grenade count (player +0x286): a drop = our throw
        static int lastc = -1;
        int c = *(volatile uint8_t *)(uintptr_t)(pobj + 0x286);
        uint64_t now = GetTickCount64();
        if (lastc >= 0 && c < lastc && lastc - c <= 3) { g_my_gren += lastc - c; g_my_gren_ms = now; }
        lastc = c;
        if (g_my_gren && now - g_my_gren_ms > 10000) g_my_gren = 0;
    }
    if (!g_ex_in_n) return;
    if (pobj < 0x80000000 || pobj >= 0x84000000) { g_ex_in_n = 0; return; }
    if (!g_exr_fn) g_exr_fn = xbe_va(0x001E0970);
    for (int i = 0; i < g_ex_in_n; i++) {
        const ExMsg &m = g_ex_in[i];
        if (coop_ex_find(m.kind, m.centre, 0) >= 0) {   // it already went off here (the same destructible)
            g_ex_stats[3]++;
            logf("coop: explosion relay: peer kind %u at %.1f %.1f %.1f already happened here - dropped", m.kind, m.centre[0], m.centre[1], m.centre[2]);
            continue;
        }
        // ex1 replay crash (AV 0xcc22a): stack arg 1 is NOT an owner - FUN_001e0970 reads its +0xD0
        // (camera position, shake distance) and runs the world query on [arg+0x4960]: it is the
        // global world/camera object (801fd180 in both copies), passed through unchanged.
        uint32_t own = m.raw[0];
        uint32_t probe = 0;
        if (own < 0x80000000 || own >= 0x84000000 || !coop_safe_copy(&probe, own + 0x4960, 4)) {
            g_ex_stats[3]++; logf("coop: explosion relay: peer kind %u - world object %08x not readable here, dropped", m.kind, own); continue;
        }
        __declspec(align(16)) static float cen[4], dir[4];
        for (int k = 0; k < 4; k++) { cen[k] = m.centre[k]; dir[k] = m.dir[k]; }
        g_exr_args[0] = own;
        g_exr_args[1] = (uint32_t)(uintptr_t)cen;
        g_exr_args[2] = m.dir_ok ? (uint32_t)(uintptr_t)dir : m.raw[2];
        for (int k = 3; k < 9; k++) g_exr_args[k] = m.raw[k];
        // id4 (joiner AV 0x169a4c inside a replay): stack arg 7 = the exploding object. Props pass a
        // world object (same address on both copies), but chained blasts pass a pointer into the
        // SENDER's own process memory (03b425c0) - never dereference that here. Not a readable world
        // object on this copy -> 0, which is what grenades pass.
        {
            uint32_t so = g_exr_args[6], vt = 0;
            if (so && (so < 0x80000000 || so >= 0x84000000 || !coop_safe_copy(&vt, so, 4) || vt < 0x10000 || vt >= 0x00400000)) g_exr_args[6] = 0;
        }
        g_exr_eax = m.kind; g_exr_ecx = m.ecx; g_exr_edx = m.edx; g_exr_r = m.radius;
        // damage authority: the host relays its blast on our clone to the joiner (CLXD); the joiner
        // hurt itself locally with its own blast -> the replay must not queue a blast on the
        // victim whose damage already comes another way (joiner: our player; host: the clone)
        volatile uint8_t *pf = (volatile uint8_t *)(uintptr_t)(pobj + 0x201);   // player blast record +0x1E0, pending flag +0x21
        volatile uint8_t *cf = (g_p10_gdst >= 0x80000000 && g_p10_gdst < 0x84000000) ? (volatile uint8_t *)(uintptr_t)(g_p10_gdst + 0x101) : nullptr;   // pawn record +0xE0
        uint8_t pf0 = *pf, cf0 = cf ? *cf : 1;
        coop_ex_note(m.kind, m.centre, 1);
        g_ex_replaying = 1;
        coop_thunk_ex_replay();
        g_ex_replaying = 0;
        g_ex_stats[2]++;
        const char *cleared = "";
        // play 20261113_202414 ("the joiner couldn't be hurt by enemy rocket explosions"): rockets now fly on
        // both copies and the replayed one has no local blast, so THIS replay is the joiner's only rocket
        // blast; the host never relayed one (6 rocket blasts cleared, 1 CLXD all session). A rocket (kind 2)
        // replay keeps the blast it queued on our player; the host does not relay rocket hits on the clone.
        if (g_cfg.coop_role == 2 && !pf0 && *pf && m.kind != 2 && m.kind != 3) { *pf = 0; cleared = " (our player's queued blast cleared: the host relays it)"; }
        else if (g_cfg.coop_role == 2 && !pf0 && *pf) cleared = m.kind == 2 ? " (ROCKET: our player takes this blast here)" : " (GRENADE: our player takes this blast here)";
        if (g_cfg.coop_role == 1 && cf && !cf0 && *cf) { *cf = 0; cleared = " (the clone's queued blast cleared: the joiner took it locally)"; }
        static int al;
        if (al++ < 300) logf("coop: explosion relay: replayed peer kind %u at %.1f %.1f %.1f radius %.2f owner %08x (kind %u)%s",
                            m.kind, m.centre[0], m.centre[1], m.centre[2], m.radius, own, m.owner_kind, cleared);
    }
    g_ex_in_n = 0;
    static uint32_t sl;
    if ((sl++ % 600) == 0) logf("coop: explosion relay: sent %u received %u replayed %u duplicates %u", g_ex_stats[0], g_ex_stats[1], g_ex_stats[2], g_ex_stats[3]);
}
__declspec(naked) void coop_thunk_explosion()
{
    __asm {
        pushad
        mov dword ptr [g_ex_eax], eax
        mov dword ptr [g_ex_ecx], ecx
        mov dword ptr [g_ex_edx], edx
        mov dword ptr [g_ex_ebp], ebp
        movss dword ptr [g_ex_r], xmm2
        lea eax, [esp + 0x20]
        mov dword ptr [g_ex_esp], eax
        sub esp, 16
        movups [esp], xmm2
        call coop_on_explosion
        movups xmm2, [esp]
        add esp, 16
        popad
        cmp dword ptr [g_ex_skip], 0
        jne skipex
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_ex_resume]
    skipex:
        ret 0x24
    }
}
// PROJECTILE SYNC (play 20261113_191753: "rockets / missiles are not synced"). Weapon fire type 3 ->
// FUN_001e8540 -> FUN_001e35f0 (thiscall ECX = the projectile pool: 16 slots, used flags +0x3720,
// kind +0x3680 + i*4 = EDI, object 0x120 B at pool + 0x2400 + i*0x120; stack: matrix* (start pose),
// velocity*, two type words; RET 0x10). Calls from FUN_001e8540 return to 0x1e8794 (player, EDI 3) or
// 0x1e8802 (AI, EDI 0/1) with EBX = the weapon context (owner = [[EBX+4]+0xF0]). The rocket flies in
// FUN_001e34c0 and explodes there (FUN_001e0970 kind 2, ret 0x1e35b6, EBP = the projectile).
// The clone fires rays only, so the peer never saw a rocket; the joiner's puppets fired their own.
//  - sender: a rocket of OUR player (both roles) or of a host enemy (host) -> "CLPJ" {matrix, velocity,
//    type words, kind}, sent 2x;
//  - suppressed: on the joiner every rocket whose owner is not our player (puppets: the host's real
//    one arrives); on the host a rocket of the clone;
//  - receiver: FUN_001e35f0 with the same values on our pool -> the same rocket flies here; the slot
//    it took is remembered as REPLAYED and its local explosion is skipped (the real blast arrives
//    through the explosion relay, whose damage rules already hold). Env COOP_PROJ_SYNC=0 = off.
struct PjMsg { uint32_t seq, kind, w1, w2; float mat[16], vel[4]; uint32_t fn; };   // fn 0 = rocket (FUN_001e35f0), 1 = grenade (FUN_001dd000: w1 = kind, w2 = flag)
static int g_pj_on = -1;
volatile uint32_t g_pj_resume, g_pj_ecx, g_pj_edi, g_pj_ebx, g_pj_esp, g_pj_skip, g_pj_replaying;
volatile uint32_t g_gr_a, g_gr_resume;     // FUN_001e7b60 (grenade throw): EAX at entry = the thrower's anim instance ([EAX] = body)
static uint32_t g_pj_pool;                                   // the last pool seen in a local call
static PjMsg g_pj_out[8]; static int g_pj_out_n; static PjMsg g_pj_prev[8]; static int g_pj_prev_n;
static PjMsg g_pj_in[16]; static int g_pj_in_n; static uint32_t g_pj_seq, g_pj_last_in;
static struct { uint32_t obj; uint64_t ms; } g_pj_rep[16];
static uint32_t g_pj_stats[4];                               // sent, suppressed, replayed, local blasts skipped
static bool coop_pj_is_replayed(uint32_t obj)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 16; i++) if (g_pj_rep[i].obj == obj && now - g_pj_rep[i].ms < 20000) { g_pj_rep[i].obj = 0; g_pj_stats[3]++; return true; }
    return false;
}
extern "C" void coop_pj_check(void)
{
    g_pj_skip = 0;
    if (g_pj_on != 1 || !g_cfg.coop_role || g_pj_replaying) return;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_pj_esp;   // [0] ret, [1] matrix*, [2] velocity*, [3] w1, [4] w2
    uint32_t ret = a[0];
    bool gren = ret == 0x001E7BBC;
    // THROWN GRENADES (tt1 trace): FUN_001e32a0 (EBX = the grenade weapon, thrower = [EBX+0xF0]) ->
    // FUN_001dd000(matrix*, velocity*, kind, flag) with EDI = the pool [0x2D1998], RET 0x10; kind 3 = a player
    // (call 0x1e3464), 1 = an ally (0x1e348b), 0 = an enemy (0x1e34a8). Objects: pool + i*0x120, used flags +0x3700 (32).
    bool thrown = ret == 0x001E3469 || ret == 0x001E3490 || ret == 0x001E34AD;
    if (ret != 0x001E8794 && ret != 0x001E8802 && !gren && !thrown) return;      // not a weapon rocket / grenade throw
    g_pj_pool = g_pj_ecx;
    uint32_t wctx = g_pj_ebx, w = 0, owner = 0;
    if (gren) {
        uint32_t ob = 0, vt = 0;
        if (coop_safe_copy(&ob, g_gr_a, 4) && coop_safe_copy(&vt, ob, 4) && (vt == 0x0028C1E8 || vt == 0x0028CFE0)) owner = ob;
        static int gl;
        if (gl++ < 12) logf("coop: projectile sync: GRENADE throw, EAX %08x -> thrower %08x", g_gr_a, owner);
    } else if (thrown) { coop_safe_copy(&owner, wctx + 0xF0, 4); gren = true; }
    else { coop_safe_copy(&w, wctx + 4, 4); if (w) coop_safe_copy(&owner, w + 0xF0, 4); }
    uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    bool ours = owner && owner == me;
    if (!ours && (g_cfg.coop_role == 2 || owner == g_p10_gdst)) {
        g_pj_skip = 1; g_pj_stats[1]++;
        static int sl;
        if (sl++ < 30) logf("coop: projectile sync: local %s of %08x suppressed (%s)", gren ? "grenade" : "rocket", owner, g_cfg.coop_role == 2 ? "the host's real one is replayed here" : "the clone - the joiner's real one is replayed here");
        return;
    }
    if (g_pj_out_n >= 8) return;
    PjMsg &m = g_pj_out[g_pj_out_n++];
    m.seq = ++g_pj_seq; m.kind = g_pj_edi; m.w1 = a[3]; m.w2 = a[4]; m.fn = thrown ? 1u : 0u;
    if (!coop_safe_copy(m.mat, a[1], 64) || !coop_safe_copy(m.vel, a[2], 16)) { g_pj_out_n--; return; }
    g_pj_stats[0]++;
    static int ol;
    if (ol++ < 40) logf("coop: projectile sync: %s %s of %08x (kind %u) at %.1f %.1f %.1f vel %.1f %.1f %.1f -> peer",
                        ours ? "OUR" : "an enemy", gren ? "grenade" : "rocket", owner, m.kind, m.mat[12], m.mat[13], m.mat[14], m.vel[0], m.vel[1], m.vel[2]);
}
// play 20261113_202414 HOST CRASH 0x1e35f3: an entry hook at FUN_001e35f0 overwrote 0x1e35f3, which is
// the target of the function's own slot loop (jl 0x1e35f3 at 0x1e3605) -> with slot 0 busy the loop jumped
// into our JMP. The two weapon CALL sites (0x1e878f, 0x1e87fd: E8 rel32) now call this thunk instead;
// FUN_001e35f0 itself stays untouched (g_pj_resume = its entry).
__declspec(naked) void coop_thunk_grenade_throw()   // FUN_001e7b60 entry: 55 8B EC 83 E4 F0 re-executed (EAX kept)
{
    __asm {
        mov dword ptr [g_gr_a], eax
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_gr_resume]
    }
}
volatile uint32_t g_pj_resume_gr;
__declspec(naked) void coop_thunk_throw()      // the thrown-grenade calls -> FUN_001dd000 (EDI = pool)
{
    __asm {
        pushad
        mov dword ptr [g_pj_ecx], edi
        mov dword ptr [g_pj_edi], 0
        mov dword ptr [g_pj_ebx], ebx
        lea eax, [esp + 0x20]
        mov dword ptr [g_pj_esp], eax
        call coop_pj_check
        popad
        cmp dword ptr [g_pj_skip], 0
        jne skipth
        jmp dword ptr [g_pj_resume_gr]
    skipth:
        ret 0x10
    }
}
__declspec(naked) void coop_thunk_projectile()
{
    __asm {
        pushad
        mov dword ptr [g_pj_ecx], ecx
        mov dword ptr [g_pj_edi], edi
        mov dword ptr [g_pj_ebx], ebx
        lea eax, [esp + 0x20]
        mov dword ptr [g_pj_esp], eax
        call coop_pj_check
        popad
        cmp dword ptr [g_pj_skip], 0
        jne skippj
        jmp dword ptr [g_pj_resume]
    skippj:
        ret 0x10
    }
}
volatile uint32_t g_pjr_fn, g_pjr_ecx, g_pjr_edi, g_pjr_a[4], g_pjr_esp;
__declspec(naked) void coop_thunk_pj_replay()
{
    __asm {
        pushad
        mov dword ptr [g_pjr_esp], esp
        push dword ptr [g_pjr_a + 12]
        push dword ptr [g_pjr_a + 8]
        push dword ptr [g_pjr_a + 4]
        push dword ptr [g_pjr_a]
        mov ecx, dword ptr [g_pjr_ecx]
        mov edi, dword ptr [g_pjr_edi]
        call dword ptr [g_pjr_fn]
        mov esp, dword ptr [g_pjr_esp]
        popad
        ret
    }
}
static void coop_pj_recv(const uint8_t *raw, int len)
{
    if (g_pj_on != 1 || len < 12) return;
    uint32_t n = *(const uint32_t *)(raw + 8);
    if (n > 16 || len < (int)(12 + n * sizeof(PjMsg))) return;
    const PjMsg *m = (const PjMsg *)(raw + 12);
    for (uint32_t i = 0; i < n && g_pj_in_n < 16; i++) {
        if (m[i].seq <= g_pj_last_in && g_pj_last_in - m[i].seq < 100000) continue;
        g_pj_last_in = m[i].seq;
        g_pj_in[g_pj_in_n++] = m[i];
    }
}
static void coop_pj_step(void)   // every frame: send ours, replay the peer's
{
    if (g_pj_on != 1 || !g_cfg.coop_role) return;
    if (g_pj_out_n || g_pj_prev_n) {
        struct { uint32_t magic, seq, n; PjMsg m[16]; } pk;
        pk.magic = 0x4A504C43; pk.seq = g_pj_seq; pk.n = 0;
        for (int i = 0; i < g_pj_prev_n; i++) pk.m[pk.n++] = g_pj_prev[i];
        for (int i = 0; i < g_pj_out_n; i++) pk.m[pk.n++] = g_pj_out[i];
        sendto(g_coop_sock, (const char *)&pk, 12 + pk.n * (int)sizeof(PjMsg), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        memcpy(g_pj_prev, g_pj_out, g_pj_out_n * sizeof(PjMsg)); g_pj_prev_n = g_pj_out_n; g_pj_out_n = 0;
    }
    if (!g_pj_in_n) return;
    uint32_t pool = g_pj_pool;
    if (!pool) pool = *(volatile uint32_t *)(uintptr_t)0x002D1998;   // not seen yet: the global projectile pool (FUN_001e8540 passes [0x2D1998])
    uint8_t used0[16] = {0};
    for (int i = 0; i < g_pj_in_n; i++) {
        const PjMsg &m = g_pj_in[i];
        bool gr = m.fn == 1;
        uint8_t g0[32] = {0};
        if (!coop_safe_copy(used0, pool + 0x3720, 16) || !coop_safe_copy(g0, pool + 0x3700, 32)) { logf("coop: projectile sync: pool %08x not readable - dropped", pool); break; }
        __declspec(align(16)) static float mat[16], vel[4];
        memcpy(mat, m.mat, 64); memcpy(vel, m.vel, 16);
        g_pjr_fn = xbe_va(gr ? 0x001DD000 : 0x001E35F0); g_pjr_ecx = pool; g_pjr_edi = gr ? pool : m.kind;
        g_pjr_a[0] = (uint32_t)(uintptr_t)mat; g_pjr_a[1] = (uint32_t)(uintptr_t)vel; g_pjr_a[2] = m.w1; g_pjr_a[3] = m.w2;
        g_pj_replaying = 1;
        coop_thunk_pj_replay();
        g_pj_replaying = 0;
        uint8_t used1[16] = {0}, g1[32] = {0}; coop_safe_copy(used1, pool + 0x3720, 16); coop_safe_copy(g1, pool + 0x3700, 32);
        uint32_t obj = 0;
        if (gr) { for (int k = 0; k < 32; k++) if (!g0[k] && g1[k]) { obj = pool + k * 0x120; break; } }
        else for (int k = 0; k < 16; k++) if (!used0[k] && used1[k]) { obj = pool + 0x2400 + k * 0x120; break; }
        if (obj) {
            int slot = 0; uint64_t oldest = ~0ull;
            for (int k = 0; k < 16; k++) { if (!g_pj_rep[k].obj) { slot = k; break; } if (g_pj_rep[k].ms < oldest) { oldest = g_pj_rep[k].ms; slot = k; } }
            g_pj_rep[slot].obj = obj; g_pj_rep[slot].ms = GetTickCount64();
        }
        g_pj_stats[2]++;
        static int rl;
        if (rl++ < 40) logf("coop: projectile sync: replayed the peer's %s (kind %u) at %.1f %.1f %.1f -> %s %08x",
                            gr ? "grenade" : "rocket", gr ? m.w1 : m.kind, m.mat[12], m.mat[13], m.mat[14], obj ? "flying as" : "NO free slot", obj);
    }
    g_pj_in_n = 0;
    static uint32_t fl;
    if ((fl++ % 1800) == 0 && (g_pj_stats[0] | g_pj_stats[2]))
        logf("coop: projectile sync: sent %u suppressed %u replayed %u local blasts skipped %u", g_pj_stats[0], g_pj_stats[1], g_pj_stats[2], g_pj_stats[3]);
}
static void install_projectile_sync(void)
{
    char e[8] = {0};
    g_pj_on = (GetEnvironmentVariableA("COOP_PROJ_SYNC", e, sizeof e) && e[0] == '0') ? 0 : 1;
    if (!g_pj_on) { logf("coop: projectile sync: OFF (COOP_PROJ_SYNC=0)"); return; }
    uint32_t fn = xbe_va(0x001E35F0);
    uint32_t calls[6] = {xbe_va(0x001E878F), xbe_va(0x001E87FD), xbe_va(0x001E7BB7),            // rockets (player, AI), FUN_001e7b60
                         xbe_va(0x001E3464), xbe_va(0x001E348B), xbe_va(0x001E34A8)};           // thrown grenades (player, ally, enemy)
    uint32_t want[6] = {fn, fn, fn, xbe_va(0x001DD000), xbe_va(0x001DD000), xbe_va(0x001DD000)};
    for (int i = 0; i < 6; i++) {
        auto *c = (uint8_t *)(uintptr_t)calls[i];
        int32_t cur = 0; memcpy(&cur, c + 1, 4);
        if (c[0] != 0xE8 || calls[i] + 5 + cur != want[i]) { logf("coop: projectile sync: call site %08x is not a call to FUN_001e35f0 - not installed", calls[i]); g_pj_on = 0; return; }
    }
    {
        uint32_t g = xbe_va(0x001E7B60);
        auto *gc = (uint8_t *)(uintptr_t)g;
        static const uint8_t kG[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
        if (memcmp(gc, kG, 6) != 0) { logf("coop: projectile sync: 001e7b60 bytes mismatch - not installed"); g_pj_on = 0; return; }
        g_gr_resume = g + 6;
        gc[0] = 0xE9;
        int32_t grel = (int32_t)((uintptr_t)coop_thunk_grenade_throw - (g + 5));
        memcpy(gc + 1, &grel, 4);
        gc[5] = 0x90;
    }
    g_pj_resume = fn; g_pj_resume_gr = xbe_va(0x001DD000);
    for (int i = 0; i < 6; i++) {
        auto *c = (uint8_t *)(uintptr_t)calls[i];
        int32_t rel = (int32_t)((uintptr_t)(i < 3 ? (void *)coop_thunk_projectile : (void *)coop_thunk_throw) - (calls[i] + 5));
        memcpy(c + 1, &rel, 4);
    }
    logf("coop: projectile sync installed at the weapon calls 0x1e878f / 0x1e87fd and the grenade throw 0x1e7bb7 (rockets and grenades fly on both copies; the thrower's blast is the real one)");
}
// THROWABLE TRACE (grenade sync RE): FUN_001dce60 / FUN_001dd000 put a 0x120-byte object into the list at
// EDI (+0x3700 used flags x32, +0x3600 + i*4 = arg 3); grenades explode from that list (FUN_001e3680, EBP =
// the object). Log-only entry hooks (55 8B EC 83 E4 F0 re-executed; their loops jump back to +0x10).
volatile uint32_t g_tt_resume[2], g_tt_edi, g_tt_esp, g_tt_which;
extern "C" void coop_throw_trace(void)
{
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_tt_esp;   // [0] ret, [1] m*, [2] v*, [3] kind, [4]
    static int n;
    if (n++ < 60) {
        float m[16] = {0}, v[4] = {0};
        coop_safe_copy(m, a[1], 64); coop_safe_copy(v, a[2], 16);
        logf("coop: throw trace: %s ret %08x pool %08x kind %u a4 %u | at %.1f %.1f %.1f vel %.1f %.1f %.1f",
             g_tt_which ? "FUN_001dd000" : "FUN_001dce60", a[0], g_tt_edi, a[3], a[4], m[12], m[13], m[14], v[0], v[1], v[2]);
    }
}
#define COOP_TT_THUNK(NAME, IDX) __declspec(naked) void NAME() {     __asm pushad     __asm mov dword ptr [g_tt_edi], edi     __asm lea eax, [esp + 0x20]     __asm mov dword ptr [g_tt_esp], eax     __asm mov dword ptr [g_tt_which], IDX     __asm call coop_throw_trace     __asm popad     __asm push ebp     __asm mov ebp, esp     __asm and esp, 0xFFFFFFF0     __asm jmp dword ptr [g_tt_resume + IDX * 4] }
COOP_TT_THUNK(coop_thunk_tt0, 0)
COOP_TT_THUNK(coop_thunk_tt1, 1)
static void install_throw_trace(void)
{
    char e[8] = {0};
    if (!(GetEnvironmentVariableA("COOP_THROW_TRACE", e, sizeof e) && e[0] == '1')) return;
    uint32_t va[2] = {0x001DCE60, 0x001DD000}; void *th[2] = {(void *)coop_thunk_tt0, (void *)coop_thunk_tt1};
    static const uint8_t k[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    for (int i = 0; i < 2; i++) {
        uint32_t s2 = xbe_va(va[i]); auto *c = (uint8_t *)(uintptr_t)s2;
        if (memcmp(c, k, 6) != 0) { logf("coop: throw trace: %08x mismatch", va[i]); continue; }
        g_tt_resume[i] = s2 + 6; c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)th[i] - (s2 + 5)); memcpy(c + 1, &rel, 4); c[5] = 0x90;
    }
    logf("coop: throw trace installed (COOP_THROW_TRACE=1)");
}
static void install_explosion_log(void)
{
    char e[8] = {0};
    if (GetEnvironmentVariableA("COOP_EXPLOG", e, sizeof e) && e[0] == '0') return;
    uint32_t site = xbe_va(0x001E0970);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    if (memcmp(code, kExpect, 6) != 0) { logf("coop: explosion log: 001e0970 bytes mismatch - not installed"); return; }
    g_ex_resume = site + 6;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_explosion - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("coop: explosion log installed at FUN_001e0970");
}
// vb1 (Veblensk, two copies, first run with the joiner's enemy sync alive there): JOINER AV at
// 0x5bb17 = FUN_0005bb10 entry (thiscall, RET 0x28) reading [ECX+4] with ECX = 0. Caller chain
// FUN_001aed40 (agent's 3 target slots) -> FUN_0019fcd0(registry object) -> here: a target-registry
// entry (*0x2D1988 +0xFB0 + idx*0xC, type 0) whose object pointer is NULL on the joiner (the
// registries of the two copies differ once trigger-spawned enemies diverge). The function's own
// invalid-input exit is "xor eax,eax; ret 0x28" - the guard takes it for a null this.
// Displaced: 83 EC 3C (sub esp,0x3C) 53 (push ebx) 57 (push edi) = 5 bytes, re-executed.
volatile uint32_t g_ng_resume, g_ng_hits;
extern "C" void coop_null_this_log(void)
{
    if (g_ng_hits++ < 10) logf("coop: null guard: FUN_0005bb10 called with a null object (stale target-registry entry) - returned 0");
}
__declspec(naked) void coop_thunk_null_this()
{
    __asm {
        test ecx, ecx
        jz nullthis
        sub esp, 0x3C
        push ebx
        push edi
        jmp dword ptr [g_ng_resume]
    nullthis:
        pushad
        call coop_null_this_log
        popad
        xor eax, eax
        ret 0x28
    }
}
static void install_null_guard(void)
{
    uint32_t site = xbe_va(0x0005BB10);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[5] = {0x83, 0xEC, 0x3C, 0x53, 0x57};
    if (memcmp(code, kExpect, 5) != 0) { logf("coop: null guard: 0005bb10 bytes mismatch - not installed"); return; }
    g_ng_resume = site + 5;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_null_this - (site + 5));
    memcpy(code + 1, &rel, 4);
    logf("coop: null guard installed at FUN_0005bb10 (null object -> return 0)");
}
// PUPPET BRAIN OFF (joiner; vb1/vb2 + every earlier joiner AI crash: 0x1af1a3, 0x1a7f58, 0x194bfb,
// 0x19d700/0x19d747, 0x5bb17 all sit under FUN_001e5ef0 = the per-enemy AI update, EAX = the brain
// (class object), body = [EAX+0x8C]). A puppet's body reads OUR private link ([body+0x324], native
// memory), not this brain, so the brain's output is unused - it only left side effects (local
// grenades, stale target-registry reads, null weapons). On the joiner the update is skipped while
// the body's link is a private one; coop_enemy_tick restores the class link before a host-ordered
// kill, so the death still runs through the brain. Env COOP_PUPPET_BRAIN=1 = keep the brain running.
// Displaced (10 bytes): 53 57 8B F8 8B 87 8C 00 00 00 (push ebx; push edi; mov edi,eax; mov eax,[edi+0x8C]).
volatile uint32_t g_pb_resume, g_pb_eax, g_pb_skip, g_pb_count;
static int g_pb_keep = -1;
extern "C" void coop_puppet_brain_check(void)
{
    g_pb_skip = 0;
    // playtest #6 / cpprobe (joiner AV 0x1A7F5E right after "the partner left ... quitting it"): the session end turns
    // co-op off (this returned at the role check below) and the quit tears the level down while enemies still ran
    // their AI - one re-armed by the weapon guard with a weapon already gone. Until the next level load no AI update runs.
    if (g_coop_teardown) { g_pb_skip = 1; return; }
    uint32_t br = g_pb_eax;
    if (!g_cfg.coop_role || br < 0x80000000 || br >= 0x84000000) return;
    uint32_t body = *(volatile uint32_t *)(uintptr_t)(br + 0x8C);
    if (body < 0x80000000 || body >= 0x84000000 || body == g_p10_gdst) return;
    if (coop_is_self(body)) { g_pb_skip = 1; return; }                                // the self model has no AI
    // realcp test (joiner AV 0x19D493 in the think step of a dying puppet): the checkpoint move took the joiner 137 m
    // from its enemies, the host despawned them, the joiner killed its puppets - the kill puts each puppet's real class
    // link back, so it no longer looked like a puppet and its AI ran on a brain that never thought. A body being
    // killed here does not run the AI (the kill shot and the death need none).
    if (g_cfg.coop_role == 2 && coop_enid_is_killing(body)) { g_pb_skip = 1; return; }
    if (*(volatile uint32_t *)(uintptr_t)body != 0x0028C1E8) return;                  // pawns only
    // vb2 (AV 0x19d700) / vb3 (AV 0x18d5de): the brain reads [[brain+0x8C]+0x344]+0x60 with no null
    // check and the joiner's puppet body had +0x344 = 0 -> skip the update on such frames (both roles)
    if (*(volatile uint32_t *)(uintptr_t)(body + 0x344) == 0) {
        g_pb_skip = 1;
        static uint32_t nl;
        if ((nl++ % 3600) == 0) logf("coop: brain guard: body %08x (brain %08x) has no +0x344 component - AI update skipped (%u so far)", body, br, nl);
        return;
    }
    // playtest 20261114 #6 (joiner crash 0x1A7F5E in a puppet's AI update right after "the partner LEFT"): the session end
    // switches co-op off in the same frame it quits the level, and the puppets (private brain links) ran the game's AI
    // during the teardown. The JOINER's puppets stay skipped until the level is gone (the last co-op role counts).
    static int last_role;
    if (g_cfg.coop_role) last_role = g_cfg.coop_role;
    if ((g_cfg.coop_role ? g_cfg.coop_role : last_role) != 2 || g_pb_keep) return;
    uint32_t link = *(volatile uint32_t *)(uintptr_t)(body + 0x324);
    if (link == br || (link >= 0x80000000 && link < 0x84000000) || link < 0x10000) return;   // not privatized = not a puppet (now)
    g_pb_skip = 1;
    if ((g_pb_count++ % 36000) == 0) logf("coop: puppet brain: AI update skipped for puppet %08x (brain %08x, private link %08x) - %u skips so far", body, br, link, g_pb_count);
}
__declspec(naked) void coop_thunk_puppet_brain()
{
    __asm {
        pushad
        mov dword ptr [g_pb_eax], eax
        call coop_puppet_brain_check
        popad
        cmp dword ptr [g_pb_skip], 0
        jne skipbrain
        push ebx
        push edi
        mov edi, eax
        mov eax, dword ptr [edi + 0x8C]
        jmp dword ptr [g_pb_resume]
    skipbrain:
        ret
    }
}
static void install_puppet_brain(void)
{
    char e[8] = {0};
    // vb3: with every puppet brain skipped the joiner crashed EARLIER (55 s) - the full skip is
    // EXPERIMENTAL and OFF by default; COOP_PUPPET_BRAIN=0 turns it on.
    g_pb_keep = (GetEnvironmentVariableA("COOP_PUPPET_BRAIN", e, sizeof e) && e[0] == '0') ? 0 : 1;
    uint32_t site = xbe_va(0x001E5EF0);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[10] = {0x53, 0x57, 0x8B, 0xF8, 0x8B, 0x87, 0x8C, 0x00, 0x00, 0x00};
    if (memcmp(code, kExpect, 10) != 0) { logf("coop: puppet brain: 001e5ef0 bytes mismatch - not installed"); return; }
    g_pb_resume = site + 10;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_puppet_brain - (site + 5));
    memcpy(code + 1, &rel, 4);
    memset(code + 5, 0x90, 5);
    logf("coop: puppet brain hook installed at FUN_001e5ef0 (null-component guard on; full puppet-brain skip %s)", g_pb_keep ? "off" : "ON (COOP_PUPPET_BRAIN=0)");
}
// Two-PC play 20261115 #4 (0.4.0 merge, mission 1: "the joiner crashed when he teleported to the checkpoint the host
// triggered"): 3.6 s after the CLCK move the joiner was in the next level part (section 1, new spawns, a burst of host
// kills) and an AI behaviour test crashed at 0x18BF3E (read of 0x54, edi = 0x20). The caller at 0x1930AE looks an
// object up by id in the level table *0x2D45D0 (FUN_001B7EF0) and passes result + 0x20 in EAX to FUN_0018BF30
// (stdcall, 3 stack args, ret 0xC, bool in AL; also called at 0x18BFE0 and 0x19470F) with no null check: the object
// belonged to the part the AI came from. Guard: EAX below 0x10000 -> false. Displaced: 53 8B 5C 24 08.
volatile uint32_t g_lg_resume, g_lg_hits;
extern "C" void coop_lookup_guard_note(void)
{
    if ((g_lg_hits++ % 600) == 0) logf("coop: lookup guard: a behaviour test got no object (the level part it refers to is gone) - false (%u so far)", g_lg_hits);
}
__declspec(naked) void coop_thunk_lookup_guard()
{
    __asm {
        cmp eax, 0x10000
        jb nobj
        push ebx
        mov ebx, dword ptr [esp + 8]
        jmp dword ptr [g_lg_resume]
    nobj:
        pushad
        call coop_lookup_guard_note
        popad
        xor eax, eax
        ret 0xC
    }
}
static void install_lookup_guard(void)
{
    uint32_t site = xbe_va(0x0018BF30);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[5] = {0x53, 0x8B, 0x5C, 0x24, 0x08};
    if (memcmp(code, kExpect, 5) != 0) { logf("coop: lookup guard: 0018bf30 bytes mismatch - not installed"); return; }
    g_lg_resume = site + 5;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_lookup_guard - (site + 5));
    memcpy(code + 1, &rel, 4);
    logf("coop: lookup guard installed at FUN_0018bf30 (a missing level object -> false)");
}
// play 20261112_183200 + id2 (joiner AV 0x1904ad, both right after a host-ordered kill): FUN_00190470
// (a behaviour test, cdecl-style plain RET, bool in AL; arg = node) reads
// [[[node+4]+0x14]+0x20] = the enemy BODY and then body+0x324 with no null check; the body pointer
// is already cleared while the dying enemy's behaviour still runs. Guard: a null link in that chain
// -> return false. Displaced: 55 8B EC 83 E4 F0.
volatile uint32_t g_bg_resume, g_bg_arg, g_bg_skip, g_bg_hits;
extern "C" void coop_body_guard_check(void)
{
    g_bg_skip = 0;
    uint32_t n = g_bg_arg, a = 0, b = 0, c = 0;
    if (n < 0x10000 || !coop_safe_copy(&a, n + 4, 4) || a < 0x10000 || !coop_safe_copy(&b, a + 0x14, 4) || b < 0x10000 ||
        !coop_safe_copy(&c, b + 0x20, 4) || c < 0x10000) {
        g_bg_skip = 1;
        if (g_bg_hits++ < 10) logf("coop: body guard: FUN_00190470 node %08x has no body (%08x -> %08x -> %08x) - returned false", n, a, b, c);
    }
}
__declspec(naked) void coop_thunk_body_guard()
{
    __asm {
        pushad
        mov eax, dword ptr [esp + 0x24]
        mov dword ptr [g_bg_arg], eax
        call coop_body_guard_check
        popad
        cmp dword ptr [g_bg_skip], 0
        jne nobody
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_bg_resume]
    nobody:
        xor eax, eax
        ret
    }
}
static void install_body_guard(void)
{
    uint32_t site = xbe_va(0x00190470);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    if (memcmp(code, kExpect, 6) != 0) { logf("coop: body guard: 00190470 bytes mismatch - not installed"); return; }
    g_bg_resume = site + 6;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_body_guard - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("coop: body guard installed at FUN_00190470 (no body -> false)");
}
// CLONE RETIRE GUARD (owner play 20261112_205905: "joiner is invisible to the host after a while").
// The clone is built by piggybacking on the level's natural spawn #4, so the level treats it as one of
// its own actors: at 93.4 s the game retired it - FUN_001da240 (EAX = body; life state +0x384 = 1, brain
// swapped), then life 2 a second later; its brain went back on the free list (the "clone's private brain
// was reused" lines of sp5-sp7 were this) and the dead body stopped drawing. The clone belongs to the
// other player: the game never retires it. Entry 53 56 57 8B F8 (push ebx/esi/edi; mov edi,eax) re-run.
volatile uint32_t g_rt_resume, g_rt_eax, g_rt_skip, g_rt_hits;
extern "C" void coop_retire_check(void)
{
    g_rt_skip = ((g_p10_gdst && g_rt_eax == g_p10_gdst) || coop_is_self(g_rt_eax)) ? 1 : 0;
    if (g_rt_skip && g_rt_hits++ < 20) logf("coop: clone guard: the game tried to RETIRE the clone %08x (FUN_001da240) - blocked", g_rt_eax);
}
__declspec(naked) void coop_thunk_retire()
{
    __asm {
        pushad
        mov dword ptr [g_rt_eax], eax
        call coop_retire_check
        popad
        cmp dword ptr [g_rt_skip], 0
        jne noretire
        push ebx
        push esi
        push edi
        mov edi, eax
        jmp dword ptr [g_rt_resume]
    noretire:
        ret
    }
}
static void install_retire_guard(void)
{
    uint32_t site = xbe_va(0x001DA240);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[5] = {0x53, 0x56, 0x57, 0x8B, 0xF8};
    if (memcmp(code, kExpect, 5) != 0) { logf("coop: clone guard: 001da240 bytes mismatch - not installed"); return; }
    g_rt_resume = site + 5;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_retire - (site + 5));
    memcpy(code + 1, &rel, 4);
    logf("coop: clone guard installed at FUN_001da240 (the game never retires the clone)");
}
// CLONE DEATH GUARD: every death variant ends in FUN_002055f0 (EDI = body: a death controller from a
// pool goes on +0x324, then FUN_001da240 life 1; no controller -> life 2 directly). Their entries,
// with the body register and the return size:
//   FUN_002056a0 ECX ret      (from 0x1e6886)      FUN_002057d0 ECX ret 0x10 (from the generic dmg 0x1df9d0)
//   FUN_002059a0 ECX ret 0x10 (debris 0x1dfd4e)    FUN_00205b20 ECX ret 4    (0x1df7f3)
//   FUN_00205c60 EBX ret 0x18 (0x1dfbf0, 0x1e79aa)
// For the clone each one returns at once (the clone belongs to the other player; its health and death
// live there). Displaced entry bytes re-executed.
volatile uint32_t g_dg_body, g_dg_skip, g_dg_hits, g_dg_which;
volatile uint32_t g_dg_resume[5];
extern "C" void coop_death_guard_check(void)
{
    g_dg_skip = ((g_p10_gdst && g_dg_body == g_p10_gdst) || coop_is_self(g_dg_body)) ? 1 : 0;
    static const char *nm[5] = {"FUN_002056a0", "FUN_002057d0", "FUN_002059a0", "FUN_00205b20", "FUN_00205c60"};
    if (g_dg_skip && g_dg_hits++ < 30) logf("coop: clone guard: the game started a DEATH on the clone %08x (%s) - blocked", g_dg_body, nm[g_dg_which]);
    // play 20261113_204611 ("some enemies die from falling while they shouldn't", near the end): the LANDING
    // death FUN_001df630 -> hp = 0 -> FUN_00205b20 (0x1df7f3) runs on the joiner's puppets too - their
    // position is driven by the host's packets (a > 2 m correction snaps), which the game can take for a
    // hard landing. 801e5020 / 801e53e0 / 801e57a0 were already dead here when the host's ended. A puppet
    // the host has not ordered dead (not "killing") keeps living: the death is blocked and its hp restored.
    if (!g_dg_skip && g_dg_which == 3 && g_cfg.coop_role == 2 && g_dg_body) {
        bool pup = false, killing = false;
        for (int i = 0; i < g_pl_n; i++) if (g_pl[i].body == g_dg_body && g_pl[i].link) pup = true;
        for (int i = 0; i < g_enid_n; i++) if (g_enid[i].live && g_enid[i].body == g_dg_body && g_enid[i].killing) killing = true;
        if (pup && !killing) {
            g_dg_skip = 1;
            float hp = 100.0f;
            for (int i = 0; i < g_en_host_n; i++) if (coop_host_to_local(g_en_host[i].body) == g_dg_body && g_en_host[i].hp > 0.0f) hp = g_en_host[i].hp;
            *(volatile float *)(uintptr_t)(g_dg_body + 0x2F0) = hp;
            static int fl;
            if (fl++ < 30) logf("coop: fall guard: puppet %08x took a LANDING death here (the host's enemy lives) - blocked, hp back to %.0f", g_dg_body, hp);
        }
    }
}
__declspec(naked) void coop_thunk_death0()   // FUN_002056a0: 53 56 57 8B F9 ; ret
{
    __asm {
        pushad
        mov dword ptr [g_dg_body], ecx
        mov dword ptr [g_dg_which], 0
        call coop_death_guard_check
        popad
        cmp dword ptr [g_dg_skip], 0
        jne skip0
        push ebx
        push esi
        push edi
        mov edi, ecx
        jmp dword ptr [g_dg_resume + 0]
    skip0:
        ret
    }
}
#define COOP_DEATH_THUNK(NAME, IDX, REG, RETN) \
__declspec(naked) void NAME() \
{ \
    __asm pushad \
    __asm mov dword ptr [g_dg_body], REG \
    __asm mov dword ptr [g_dg_which], IDX \
    __asm call coop_death_guard_check \
    __asm popad \
    __asm cmp dword ptr [g_dg_skip], 0 \
    __asm jne skipit \
    __asm push ebp \
    __asm mov ebp, esp \
    __asm and esp, 0xFFFFFFF0 \
    __asm jmp dword ptr [g_dg_resume + IDX * 4] \
    __asm skipit: \
    __asm ret RETN \
}
COOP_DEATH_THUNK(coop_thunk_death1, 1, ecx, 0x10)
COOP_DEATH_THUNK(coop_thunk_death2, 2, ecx, 0x10)
COOP_DEATH_THUNK(coop_thunk_death3, 3, ecx, 4)
COOP_DEATH_THUNK(coop_thunk_death4, 4, ebx, 0x18)
static void install_death_guard(void)
{
    struct { uint32_t va; void *thunk; int len; uint8_t expect[6]; } site[5] = {
        {0x002056A0, (void *)coop_thunk_death0, 5, {0x53, 0x56, 0x57, 0x8B, 0xF9, 0}},
        {0x002057D0, (void *)coop_thunk_death1, 6, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}},
        {0x002059A0, (void *)coop_thunk_death2, 6, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}},
        {0x00205B20, (void *)coop_thunk_death3, 6, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}},
        {0x00205C60, (void *)coop_thunk_death4, 6, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}},
    };
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t a = xbe_va(site[i].va);
        auto *code = (uint8_t *)(uintptr_t)a;
        if (memcmp(code, site[i].expect, site[i].len) != 0) { logf("coop: clone guard: %08x bytes mismatch - not installed", site[i].va); continue; }
        g_dg_resume[i] = a + site[i].len;
        code[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)site[i].thunk - (a + 5));
        memcpy(code + 1, &rel, 4);
        if (site[i].len == 6) code[5] = 0x90;
        ok++;
    }
    logf("coop: clone guard: %d / 5 death variants guarded (the clone never dies on this copy)", ok);
}
// THE REAL CAUSE of "joiner is invisible to the host after a while": when a new enemy needs a pawn
// slot and all are full, FUN_001e6860 / FUN_001e68b0 / FUN_001da240 ask FUN_001c4a90 (EDX = the slot
// list: [+0] mode, [+4] count, [+0xC] array) for a VICTIM and kill it to free the slot. Mode 0 takes
// the first entry; the scored mode can pick allies - the far-away clone got recycled. Wrapper: when
// the picker returns the clone, the clone's entry is blanked for one more pick and put back.
// Displaced: 8B 02 (mov eax,[edx]) F3 0F 10 35 4C 5D 29 00 (movss xmm6,[0x295d4c]) = 10 bytes.
volatile uint32_t g_pk_resume, g_pk_edx, g_pk_ecx, g_pk_slot, g_pk_hits, g_pk_body;
__declspec(naked) void coop_pick_orig()
{
    __asm {
        mov eax, dword ptr [edx]
        movss xmm6, dword ptr ds:[0x00295D4C]
        jmp dword ptr [g_pk_resume]
    }
}
volatile uint32_t g_pk_last;
extern "C" void coop_pick_log(void)
{
    static int n;
    uint32_t mode = 0, cnt = 0; coop_safe_copy(&mode, g_pk_edx, 4); coop_safe_copy(&cnt, g_pk_edx + 4, 4);
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
    if (mgr >= 0x80000000 && g_pk_edx == mgr + 0x79B0 && n++ < 60)   // log only the RECYCLE picks (active list full), not allocations
        logf("coop: pawn recycle: slots full (%u) - the game picked %08x to free one%s", cnt, g_pk_last, g_pk_last && g_pk_last == g_p10_gdst ? " = the clone" : "");
    (void)mode;
}
extern "C" void coop_pick_blank(void)
{
    g_pk_slot = 0;
    uint32_t L = g_pk_edx, cnt = 0, arr = 0;
    if (L < 0x10000 || !coop_safe_copy(&cnt, L + 4, 4) || !coop_safe_copy(&arr, L + 0xC, 4) || arr < 0x10000 || cnt > 64) return;
    for (uint32_t i = 0; i < cnt; i++)
        if (*(volatile uint32_t *)(uintptr_t)(arr + 4 * i) == g_pk_last) { g_pk_slot = arr + 4 * i; g_pk_body = g_pk_last; *(volatile uint32_t *)(uintptr_t)g_pk_slot = 0; break; }
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
    bool free_pool = mgr >= 0x80000000 && L == mgr + 0x79A0;
    if (g_pk_hits++ < 20) logf("coop: clone guard: the game picked the %s %08x from %s %08x - %s", g_pk_last == g_p10_gdst ? "CLONE" : "SELF MODEL", g_pk_last,
                               free_pool ? "the FREE body pool" : "the ACTIVE pawn list (slots full: recycle)", L,
                               free_pool ? "removed from the pool, another body taken" : "another victim picked instead");
}
extern "C" void coop_pick_restore(void)
{
    // cg7: mode 0 lists are the FREE pawn-body pool (list 801eb2a0, 32 entries; a spawn takes the first
    // non-zero entry and clears it). The clone's body kept reappearing there (10 / 18 times in 215 s) and
    // the next spawn would re-initialise it as a new enemy = the clone vanished. In a free pool the entry
    // stays cleared (the body is in use); in a scored victim list it goes back.
    // by LIST, not mode: mgr+0x79A0 = the FREE body pool (the factory FUN_001df340 takes a body from it)
    // -> the clone's entry stays cleared; mgr+0x79B0 = the ACTIVE pawn list (FUN_001e6860 / FUN_001df340
    // pick a victim to kill when all 16 slots are full) -> the clone's entry goes back
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)0x2D1980;
    bool free_pool = mgr >= 0x80000000 && g_pk_edx == mgr + 0x79A0;
    if (g_pk_slot && !free_pool) *(volatile uint32_t *)(uintptr_t)g_pk_slot = g_pk_body;
    g_pk_slot = 0;
}
__declspec(naked) void coop_thunk_pick()
{
    __asm {
        mov dword ptr [g_pk_edx], edx
        mov dword ptr [g_pk_ecx], ecx
        call coop_pick_orig
        mov dword ptr [g_pk_last], eax
        pushad
        call coop_pick_log
        popad
        mov eax, dword ptr [g_pk_last]
        test eax, eax
        je pickdone
        cmp eax, dword ptr [g_p10_gdst]
        je pickguard
        cmp eax, dword ptr [g_coop_self_body]
        jne pickdone
    pickguard:
        pushad
        call coop_pick_blank
        popad
        mov edx, dword ptr [g_pk_edx]
        mov ecx, dword ptr [g_pk_ecx]
        call coop_pick_orig
        push eax
        pushad
        call coop_pick_restore
        popad
        pop eax
    pickdone:
        ret
    }
}
static void install_pick_guard(void)
{
    uint32_t site = xbe_va(0x001C4A90);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[10] = {0x8B, 0x02, 0xF3, 0x0F, 0x10, 0x35, 0x4C, 0x5D, 0x29, 0x00};
    if (memcmp(code, kExpect, 10) != 0) { logf("coop: clone guard: 001c4a90 bytes mismatch - picker not wrapped"); return; }
    g_pk_resume = site + 10;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_pick - (site + 5));
    memcpy(code + 1, &rel, 4);
    memset(code + 5, 0x90, 5);
    logf("coop: clone guard: pawn-recycle picker FUN_001c4a90 wrapped (never picks the clone)");
}
// AMMO-CHECK GUARD (play 20261113_195507 joiner crash 529.6 s, AV 0x199629): the AI behaviour
// FUN_00199600 ("clip empty -> reload", ESI = behaviour, [ESI+4] = brain) reads
// [[[brain+0x8C]+0x29C]+0xF4]+0x18 with no checks; enemy 801e66a0's weapon 80b97350 had no ammo object.
// Entry 8B 46 04 83 EC 0C re-executed; a missing body / weapon / ammo -> the behaviour returns at once.
volatile uint32_t g_ag_resume, g_ag_esi, g_ag_skip;
extern "C" void coop_ammo_guard_check(void)
{
    g_ag_skip = 0;
    uint32_t br = 0, body = 0, w = 0, am = 0;
    bool ok = coop_safe_copy(&br, g_ag_esi + 4, 4) && br && coop_safe_copy(&body, br + 0x8C, 4) && body &&
              coop_safe_copy(&w, body + 0x29C, 4) && w >= 0x80000000 && w < 0x84000000 &&
              coop_safe_copy(&am, w + 0xF4, 4) && am >= 0x80000000 && am < 0x84000000;
    if (!ok) {
        g_ag_skip = 1;
        static int n;
        if (n++ < 20) logf("coop: ammo guard: AI ammo check on body %08x skipped (weapon %08x ammo %08x missing)", body, w, am);
    }
}
__declspec(naked) void coop_thunk_ammo_guard()
{
    __asm {
        pushad
        mov dword ptr [g_ag_esi], esi
        call coop_ammo_guard_check
        popad
        cmp dword ptr [g_ag_skip], 0
        jne skipag
        mov eax, dword ptr [esi + 4]
        sub esp, 0xC
        jmp dword ptr [g_ag_resume]
    skipag:
        ret
    }
}
// REGISTRY LEAK FOUND (play 20261113_202414, the "registry dump"): the joiner's registry held ~25 TYPE-1
// entries of just two level logic objects (81a98c70 / 81a98cd0, vt 0028eff8 - scripted target points),
// each registered again and again. FUN_001c1ec0 (EAX = the logic object): it finds a pawn (FUN_001b9db0)
// and, for a brain in mode +0x90 == 1, registers ITSELF as a type-1 target via FUN_00188070 (EDX = mask
// R+0xFA8, stack = object, RET 4) and keeps the index at +0x30 - overwriting it each time, so every repeat
// call leaks the previous entry (only +0x30 is ever unregistered). The call at 0x1c1eef goes through
// coop_thunk_reg_once: an object that still owns a valid type-1 entry (+0x30 in range, bit set, type 1,
// entry object = itself) gets that index back instead of a new entry. Both roles.
volatile uint32_t g_ro_obj, g_ro_idx, g_ro_fn;
static uint32_t g_ro_hits;
extern "C" void coop_reg_once_check(void)
{
    g_ro_idx = 0xFFFFFFFF;
    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, o = g_ro_obj, idx = 0xFFFFFFFF, mask = 0, e3[3] = {0, 0, 0};
    if (!coop_safe_copy(&idx, o + 0x30, 4) || idx >= 32 || !coop_safe_copy(&mask, R + 0xFA8, 4) || !(mask & (1u << idx))) return;
    if (!coop_safe_copy(e3, R + 0xFB0 + idx * 0xC, 12) || e3[0] != 1 || e3[1] != o) return;
    g_ro_idx = idx;
    if (g_ro_hits++ < 20 || (g_ro_hits % 200) == 0) logf("coop: registry: logic object %08x already registered (entry %u) - repeat registration refused (%u so far)", o, idx, g_ro_hits);
}
__declspec(naked) void coop_thunk_reg_once()
{
    __asm {
        push eax
        mov eax, dword ptr [esp + 8]
        mov dword ptr [g_ro_obj], eax
        pop eax
        pushad
        call coop_reg_once_check
        popad
        cmp dword ptr [g_ro_idx], 0xFFFFFFFF
        je fresh
        mov eax, dword ptr [g_ro_idx]
        ret 4
    fresh:
        jmp dword ptr [g_ro_fn]
    }
}
static void install_reg_once(void)
{
    uint32_t site = xbe_va(0x001C1EEF), fn = xbe_va(0x00188070);
    auto *c = (uint8_t *)(uintptr_t)site;
    int32_t cur = 0; memcpy(&cur, c + 1, 4);
    if (c[0] != 0xE8 || site + 5 + cur != fn) { logf("coop: registry: 001c1eef is not a call to FUN_00188070 - not installed"); return; }
    g_ro_fn = fn;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_reg_once - (site + 5));
    memcpy(c + 1, &rel, 4);
    logf("coop: registry: one-entry-per-object guard installed at 0x1c1eef (FUN_001c1ec0)");
}
static void coop_melee_step(void)
{
    if (!g_cfg.coop_role) return;
    {   // COOP_MELEE_TEST=<sec> (joiner, test only): at that time hit the nearest puppet through its vt+0x48 with
        // OUR player as the attacker - what a real melee strike does (dmg 100)
        static int ms = -2;
        if (ms == -2) { char e[16] = {0}; ms = GetEnvironmentVariableA("COOP_MELEE_TEST", e, sizeof e) ? atoi(e) : -1; }
        uint32_t me = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
        if (ms > 0 && g_cfg.coop_role == 2 && me && g_coop_t0 && GetTickCount64() - g_coop_t0 > (uint64_t)ms * 1000) {
            uint32_t best = 0; float bd = 1e30f;
            for (int i = 0; i < g_pl_n; i++) {
                uint32_t b = g_pl[i].body; float hp = 0;
                if (!b || !g_pl[i].link || !coop_safe_copy(&hp, b + 0x2F0, 4) || !(hp > 0.0f)) continue;
                const volatile float *f = (const volatile float *)(uintptr_t)(b + 0xA0), *mf = (const volatile float *)(uintptr_t)(me + 0xA0);
                float d = (f[0] - mf[0]) * (f[0] - mf[0]) + (f[2] - mf[2]) * (f[2] - mf[2]);
                if (d < bd) { bd = d; best = b; }
            }
            if (best) {
                ms = -1;
                __declspec(align(16)) static float hv[4], dir[4];
                const volatile float *f = (const volatile float *)(uintptr_t)(best + 0xA0);
                hv[0] = f[0]; hv[1] = f[1] + 1.2f; hv[2] = f[2]; hv[3] = 1.0f; dir[0] = 0; dir[1] = 0; dir[2] = 1; dir[3] = 0;
                float dmg = 100.0f;
                uint32_t vt = *(volatile uint32_t *)(uintptr_t)best;
                g_bk_fn = *(volatile uint32_t *)(uintptr_t)(vt + 0x48); g_bk_this = best;
                g_bk_a[0] = (uint32_t)(uintptr_t)hv; g_bk_a[1] = (uint32_t)(uintptr_t)dir; memcpy((void *)&g_bk_a[2], &dmg, 4); g_bk_a[3] = me;
                g_bk_a[4] = g_bk_a[5] = g_bk_a[6] = 0;
                logf("coop: melee test: our player hits puppet %08x (%.1f m) through vt+0x48", best, sqrtf(bd));
                coop_thunk_blast_kill();
            }
        }
    }
    if (g_cfg.coop_role == 2 && g_ml_out_n) {
        struct { uint32_t magic, seq, n; MeleeMsg m[4]; } mp;
        mp.magic = 0x4C4D4C43; mp.seq = 0; mp.n = (uint32_t)g_ml_out_n;
        memcpy(mp.m, g_ml_out, g_ml_out_n * sizeof(MeleeMsg));
        sendto(g_coop_sock, (const char *)&mp, 12 + mp.n * (int)sizeof(MeleeMsg), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        g_ml_out_n = 0;
    }
    if (g_cfg.coop_role == 1 && g_ml_in_n) {
        for (int i = 0; i < g_ml_in_n; i++) {
            const MeleeMsg &m = g_ml_in[i];
            uint32_t b = m.host_body, vt = 0; float hp = 0;
            if (!coop_safe_copy(&vt, b, 4) || vt != 0x0028C1E8 || !coop_safe_copy(&hp, b + 0x2F0, 4) || !(hp > 0.0f) || !g_p10_gdst) { g_ml_stats[2]++; continue; }
            __declspec(align(16)) static float hv[4], dir[4];
            memcpy(hv, m.hv, 16); memcpy(dir, m.dir, 16);
            g_bk_fn = *(volatile uint32_t *)(uintptr_t)(vt + 0x48); g_bk_this = b;
            g_bk_a[0] = (uint32_t)(uintptr_t)hv; g_bk_a[1] = (uint32_t)(uintptr_t)dir; memcpy((void *)&g_bk_a[2], &m.dmg, 4); g_bk_a[3] = g_p10_gdst;
            g_bk_a[4] = g_bk_a[5] = g_bk_a[6] = 0;
            coop_thunk_blast_kill();   // (vt+0x48)(4 args): the thunk restores ESP, so its 3 extra pushes are harmless
            g_ml_stats[1]++;
            static int al;
            if (al++ < 30) logf("coop: melee sync: applied the JOINER's melee on %08x dmg %.1f -> hp %.1f -> %.1f", b, m.dmg, hp, *(volatile float *)(uintptr_t)(b + 0x2F0));
        }
        g_ml_in_n = 0;
    }
}
// DESTRUCTIBLE SYNC (owner: "sync destructibles"). Destructible props (vt 0028ED98, kind 4; world objects at
// the SAME address on both copies) are split in parts (+0x1A0 = 12-byte part records starting with health,
// +0x1F5 = part count). They break only through their damage slots: vt+0x20 FUN_001e5710 (bullet, 7 args,
// RET 0x1C), vt+0x24 FUN_001e4dd0 (debris / physics impact, 4 args, RET 0x10), vt+0x2C FUN_001e5410 (blast,
// 7 args, RET 0x1C). Until now only explosions travelled; each copy broke props from its own local hits.
// HOST: every damage call on a prop runs and is sent ("CLRP"): slot + the args, each classified - a vector
// pointer goes as a 16-byte COPY (never a host address), a prop pointer raw (same address), a pawn pointer
// as "a pawn" (the joiner passes the host's clone there), other values raw.
// JOINER: its own damage calls on props are dropped (the joiner's shots reach the host as rays and break
// there); the host's are replayed on the same prop with the game's own call -> the same break, debris,
// sound. Env COOP_PROP_SYNC=0 = off.
struct PropMsg { uint32_t prop, slot, nargs, kinds; uint32_t raw[7]; float vec[7][4]; float pos[3]; };   // pos: the prop's world position (identity check)   // kinds: 2 bits per arg (0 raw, 1 vec, 2 AI pawn, 3 player)
volatile uint32_t g_pr_resume[3], g_pr_ecx, g_pr_esp, g_pr_which, g_pr_skip, g_pr_replay;
static PropMsg g_pr_out[16]; static int g_pr_out_n;
static PropMsg g_pr_in[32]; static int g_pr_in_n;
static uint32_t g_pr_stats[4];   // host sent, joiner dropped, joiner replayed, host overflow
extern "C" void coop_prop_check(void)
{
    g_pr_skip = 0;
    if (g_pr_on != 1 || !g_cfg.coop_role || g_pr_replay) return;
    uint32_t prop = g_pr_ecx, vt = 0;
    if (!coop_safe_copy(&vt, prop, 4) || vt != 0x0028ED98) return;
    static const uint32_t slots[3] = {0x20, 0x24, 0x2C}; static const int nargs[3] = {7, 4, 7};
    int w = (int)g_pr_which;
    if (g_cfg.coop_role == 2) {
        g_pr_skip = 1; g_pr_stats[1]++;
        static int dl;
        if (dl++ < 20) logf("coop: prop sync: JOINER local damage (vt+0x%X) on prop %08x dropped - the host's arrives", slots[w], prop);
        return;
    }
    {   // pr5: the JOINER's shots arrive here as the CLONE's pellets (an AI-kind pawn) and props did not break
        // from them (40 hits, no break) - the game treats an AI attacker's damage on props differently. The
        // clone's hit counts as a PLAYER's: the host player is put in as the attacker (the clone's kind stays 1).
        volatile uint32_t *wa = (volatile uint32_t *)(uintptr_t)g_pr_esp;
        uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, me = 0;
        coop_safe_copy(&me, R + 0x22980, 4);
        for (int i = 0; i < nargs[w] && me && g_p10_gdst; i++) if (wa[1 + i] == g_p10_gdst) {
            wa[1 + i] = me;
            static int cl;
            if (cl++ < 10) logf("coop: prop sync: the clone's hit on prop %08x counts as a player's (attacker -> %08x)", prop, me);
        }
    }
    if (g_pr_out_n >= 16) { g_pr_stats[3]++; return; }
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_pr_esp;   // [0] ret, [1..] args
    PropMsg &m = g_pr_out[g_pr_out_n++];
    memset(&m, 0, sizeof m);
    m.prop = prop; m.slot = slots[w]; m.nargs = (uint32_t)nargs[w];
    coop_safe_copy(m.pos, prop + 0xA0, 12);
    for (int i = 0; i < nargs[w]; i++) {
        uint32_t v = a[1 + i], ovt = 0; m.raw[i] = v;
        bool heap = v >= 0x80000000 && v < 0x84000000, lowp = v >= 0x00100000 && v < 0x80000000;
        if (heap && coop_safe_copy(&ovt, v, 4) && (ovt == 0x0028C1E8 || ovt == 0x0028CFE0)) {                        // a pawn / player
            uint32_t k = 1; coop_safe_copy(&k, v + 0xC4, 4);
            m.kinds |= (k == 2 ? 3u : 2u) << (2 * i);
        }
        else if (heap && ovt == 0x0028ED98) {}                                                                        // a prop: raw
        else if ((heap || lowp) && coop_safe_copy(m.vec[i], v, 16)) m.kinds |= 1u << (2 * i);                          // a vector: copy
    }
    g_pr_stats[0]++;
    static int sl;
    if (sl++ < 40) logf("coop: prop sync: HOST damage vt+0x%X on prop %08x (kinds %04x) -> joiner", m.slot, prop, m.kinds);
}
__declspec(naked) void coop_thunk_prop_bullet()   // FUN_001e5710 entry: 55 8B EC 83 E4 F0 re-executed
{
    __asm {
        pushad
        mov dword ptr [g_pr_ecx], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_pr_esp], eax
        mov dword ptr [g_pr_which], 0
        call coop_prop_check
        popad
        cmp dword ptr [g_pr_skip], 0
        jne skipb
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_pr_resume]
    skipb:
        ret 0x1C
    }
}
__declspec(naked) void coop_thunk_prop_blast()    // FUN_001e5410 entry: 55 8B EC 83 E4 F0 re-executed
{
    __asm {
        pushad
        mov dword ptr [g_pr_ecx], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_pr_esp], eax
        mov dword ptr [g_pr_which], 2
        call coop_prop_check
        popad
        cmp dword ptr [g_pr_skip], 0
        jne skipx
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_pr_resume + 8]
    skipx:
        ret 0x1C
    }
}
__declspec(naked) void coop_thunk_prop_debris()   // FUN_001e4dd0 entry: 8B 44 24 10 8B 80 24 01 00 00 re-executed
{
    __asm {
        pushad
        mov dword ptr [g_pr_ecx], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_pr_esp], eax
        mov dword ptr [g_pr_which], 1
        call coop_prop_check
        popad
        cmp dword ptr [g_pr_skip], 0
        jne skipd
        mov eax, dword ptr [esp + 0x10]
        mov eax, dword ptr [eax + 0x124]
        jmp dword ptr [g_pr_resume + 4]
    skipd:
        ret 0x10
    }
}
volatile uint32_t g_prr_fn, g_prr_ecx, g_prr_a[7], g_prr_esp;
__declspec(naked) static void coop_thunk_prop_replay()   // thiscall, 7 pushes (the callee pops its own; ESP restored)
{
    __asm {
        pushad
        mov dword ptr [g_prr_esp], esp
        push dword ptr [g_prr_a + 24]
        push dword ptr [g_prr_a + 20]
        push dword ptr [g_prr_a + 16]
        push dword ptr [g_prr_a + 12]
        push dword ptr [g_prr_a + 8]
        push dword ptr [g_prr_a + 4]
        push dword ptr [g_prr_a]
        mov ecx, dword ptr [g_prr_ecx]
        call dword ptr [g_prr_fn]
        mov esp, dword ptr [g_prr_esp]
        popad
        ret
    }
}
// SIDE-OBJECTIVE DESTRUCTIBLES (owner day 14: "sync the side objectives destructibles"). play 20261114_090247:
// side objective 10 (flags 02) advanced through node 81a9b390 when its targets blew up - one of them
// (245.4 s, -79.2 0.9 22.5) only on the host: the joiner never saw it break. The targets are DEBRIS-PIECE
// class objects (vt 0028ECA8, kind 3; static level destructibles in native heap memory, not in the world
// list and NOT at the same address on both copies). Their bullet slot vt+0x20 = FUN_001e8f20 reads the
// piece type's flag word (def +0x58): with flag 8 only the attacker kinds the flags allow do damage - the
// objective targets take a PLAYER's (kind 2) only, and +0x124 = 3 records the player's credit. The other
// player's shots arrive on each copy as the CLONE's pellets (kind 1) -> no damage there. FIX (both copies,
// local, like the clone's rays themselves): a piece hit whose attacker is the clone counts as OUR player.
// Entry 51 56 8B F1 8B 86 A4 01 00 00 (push ecx; push esi; mov esi,ecx; mov eax,[esi+0x1a4]) re-executed;
// the attacker is arg 6 = [esp+0x18] at entry. Env COOP_PIECE_SYNC=0 = off.
volatile uint32_t g_pc_resume, g_pc_att, g_pc_new, g_pc_ecx, g_pc_hits;
extern "C" void coop_piece_check(void)
{
    g_pc_new = 0;
    uint32_t a = g_pc_att;
    if (!a || a != g_p10_gdst) return;
    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, me = 0;
    if (!coop_safe_copy(&me, R + 0x22980, 4) || me < 0x80000000 || me >= 0x84000000) return;
    g_pc_new = me;
    g_pc_hits++;
    static int n;
    if (n++ < 40) {
        float q[3] = {0, 0, 0}, hp = 0; uint32_t def = 0, ix = 0xFFFFFFFF; uint16_t fl = 0;
        coop_safe_copy(q, g_pc_ecx + 0xA0, 12); coop_safe_copy(&hp, g_pc_ecx + 0x1A0, 4);
        coop_safe_copy(&ix, g_pc_ecx + 0x1A4, 4); coop_safe_copy(&def, g_pc_ecx + 0x118, 4);
        if (ix != 0xFFFFFFFF && def >= 0x80000000 && def < 0x84000000) { uint32_t tb = 0; coop_safe_copy(&tb, def + 0x48, 4); coop_safe_copy(&fl, tb + ix * 0xD0 + 0x58, 2); }
        logf("coop: piece sync: %s's shot on piece %08x (at %.1f %.1f %.1f, health %.1f, flags %04x) counts as our player %08x (%u so far)",
             g_cfg.coop_role == 1 ? "the JOINER" : "the HOST", g_pc_ecx, q[0], q[1], q[2], hp, fl, me, g_pc_hits);
    }
}
__declspec(naked) void coop_thunk_piece_bullet()
{
    __asm {
        pushad
        mov dword ptr [g_pc_ecx], ecx
        mov eax, dword ptr [esp + 0x20 + 0x18]
        mov dword ptr [g_pc_att], eax
        call coop_piece_check
        popad
        cmp dword ptr [g_pc_new], 0
        je pcgo
        push eax
        mov eax, dword ptr [g_pc_new]
        mov dword ptr [esp + 4 + 0x18], eax
        pop eax
    pcgo:
        push ecx
        push esi
        mov esi, ecx
        mov eax, dword ptr [esi + 0x1A4]
        jmp dword ptr [g_pc_resume]
    }
}
static void install_piece_sync(void)
{
    char e[8] = {0};
    if (GetEnvironmentVariableA("COOP_PIECE_SYNC", e, sizeof e) && e[0] == '0') { logf("coop: piece sync: OFF (COOP_PIECE_SYNC=0)"); return; }
    static const uint8_t k[10] = {0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xA4, 0x01, 0x00, 0x00};
    uint32_t a = xbe_va(0x001E8F20); auto *c = (uint8_t *)(uintptr_t)a;
    if (memcmp(c, k, 10) != 0) { logf("coop: piece sync: FUN_001e8f20 bytes mismatch - not installed"); return; }
    g_pc_resume = a + 10;
    c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)coop_thunk_piece_bullet - (a + 5)); memcpy(c + 1, &rel, 4);
    for (int i = 5; i < 10; i++) c[i] = 0x90;
    logf("coop: piece sync installed at FUN_001e8f20 (debris-piece bullets from the clone count as our player)");
}
static void install_prop_sync(void)
{
    char e[8] = {0};
    g_pr_on = (GetEnvironmentVariableA("COOP_PROP_SYNC", e, sizeof e) && e[0] == '0') ? 0 : 1;
    if (!g_pr_on) { logf("coop: prop sync: OFF (COOP_PROP_SYNC=0)"); return; }
    static const uint8_t kP[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    static const uint8_t kD[10] = {0x8B, 0x44, 0x24, 0x10, 0x8B, 0x80, 0x24, 0x01, 0x00, 0x00};
    struct { uint32_t va; const uint8_t *k; int len; void *th; } site[3] = {
        {0x001E5710, kP, 6, (void *)coop_thunk_prop_bullet}, {0x001E4DD0, kD, 10, (void *)coop_thunk_prop_debris}, {0x001E5410, kP, 6, (void *)coop_thunk_prop_blast}};
    for (int i = 0; i < 3; i++) if (memcmp((void *)(uintptr_t)xbe_va(site[i].va), site[i].k, site[i].len) != 0) {
        logf("coop: prop sync: %08x bytes mismatch - not installed", site[i].va); g_pr_on = 0; return;
    }
    for (int i = 0; i < 3; i++) {
        uint32_t a = xbe_va(site[i].va); auto *c = (uint8_t *)(uintptr_t)a;
        g_pr_resume[i] = a + site[i].len;
        c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)site[i].th - (a + 5)); memcpy(c + 1, &rel, 4);
        for (int k = 5; k < site[i].len; k++) c[k] = 0x90;
    }
    logf("coop: prop sync installed (prop damage vt+0x20 / +0x24 / +0x2C: host decides, joiner replays)");
}
// TWO-PC PLAY 20261114 (owner: "props didn't load in later sections as the joiner - shelves, tree trunks, beam posts"):
// props are matched by ADDRESS (with the vtable as the only check). In mission 1's later sections the two copies'
// heaps had diverged, so the host's damage on prop X landed on a DIFFERENT prop at that address on the joiner - which
// broke there (the missing shelves / trunks / posts), while X itself never broke. Now the host also sends X's world
// position (+0xA0): the joiner takes the prop at that address only when it stands there (0.5 m), else the destructible
// (vt 0028ED98) at that spot from the world list (world *0x2D45D4 +0x5CB4, next +0xB0), else none (skipped).
static uint32_t coop_prop_find(uint32_t addr, const float pos[3])
{
    auto near_pos = [&](uint32_t o, float lim) {
        float q[3];
        if (!coop_safe_copy(q, o + 0xA0, 12)) return false;
        float dx = q[0] - pos[0], dy = q[1] - pos[1], dz = q[2] - pos[2];
        return dx * dx + dy * dy + dz * dz < lim * lim;
    };
    uint32_t vt = 0;
    if (coop_safe_copy(&vt, addr, 4) && vt == 0x0028ED98 && near_pos(addr, 0.5f)) return addr;
    uint32_t w = *(volatile uint32_t *)(uintptr_t)0x002D45D4, node = 0, best = 0;
    if (w >= 0x80000000 && w < 0x84000000) coop_safe_copy(&node, w + 0x5CB4, 4);
    float bd = 1e9f;
    for (int step = 0; node >= 0x80000000 && node < 0x84000000 && step < 4096; step++) {
        uint32_t nv = 0, nx = 0;
        coop_safe_copy(&nv, node, 4);
        if (nv == 0x0028ED98) {
            float q[3];
            if (coop_safe_copy(q, node + 0xA0, 12)) {
                float dx = q[0] - pos[0], dy = q[1] - pos[1], dz = q[2] - pos[2], d = dx * dx + dy * dy + dz * dz;
                if (d < bd) { bd = d; best = node; }
            }
        }
        if (!coop_safe_copy(&nx, node + 0xB0, 4) || nx == node) break;
        node = nx;
    }
    return bd < 1.0f ? best : 0;   // within 1 m
}
static void coop_prop_step(void)
{
    if (g_pr_on != 1 || !g_cfg.coop_role) return;
    if (g_cfg.coop_role == 1 && g_pr_out_n) {
        struct { uint32_t magic, seq, n; PropMsg m[16]; } pk;
        pk.magic = 0x50524C43; pk.seq = 0; pk.n = (uint32_t)g_pr_out_n;
        memcpy(pk.m, g_pr_out, g_pr_out_n * sizeof(PropMsg));
        sendto(g_coop_sock, (const char *)&pk, 12 + pk.n * (int)sizeof(PropMsg), 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
        g_pr_out_n = 0;
    }
    if (g_cfg.coop_role == 2 && g_pr_in_n) {
        for (int i = 0; i < g_pr_in_n; i++) {
            PropMsg m = g_pr_in[i];
            uint32_t vt = 0;
            {
                static int shift = -1;   // COOP_PROP_SHIFT=1 (test): pretend the address differs here (exercises the search)
                if (shift < 0) { char e[8] = {0}; shift = GetEnvironmentVariableA("COOP_PROP_SHIFT", e, sizeof e) && e[0] == '1'; }
                if (shift) m.prop += 0x40;
                uint32_t found = coop_prop_find(m.prop, m.pos);
                static uint32_t ml, mm;
                if (!found) {
                    if (ml++ < 30) logf("coop: prop sync: host prop %08x at %.1f %.1f %.1f is not here (no destructible at that spot) - hit skipped", m.prop, m.pos[0], m.pos[1], m.pos[2]);
                    continue;
                }
                if (found != m.prop && mm++ < 30) logf("coop: prop sync: host prop %08x at %.1f %.1f %.1f is OUR %08x (address differs) - the hit goes there", m.prop, m.pos[0], m.pos[1], m.pos[2], found);
                m.prop = found;
            }
            if (!coop_safe_copy(&vt, m.prop, 4) || vt != 0x0028ED98) continue;
            __declspec(align(16)) static float vec[7][4];
            for (int k = 0; k < 7; k++) {
                uint32_t kd = (m.kinds >> (2 * k)) & 3;
                if (k >= (int)m.nargs) { g_prr_a[k] = 0; continue; }
                if (kd == 1) { memcpy(vec[k], m.vec[k], 16); g_prr_a[k] = (uint32_t)(uintptr_t)vec[k]; }
                else if (kd == 2) g_prr_a[k] = g_p10_gdst;                 // an AI pawn: the host's clone here (a valid pawn)
                else if (kd == 3) {                                            // a PLAYER (pr4: with the clone as the attacker the
                    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, me = 0;   // joiner's barrel never broke from 40 hits -
                    coop_safe_copy(&me, R + 0x22980, 4);                        // props take a player's damage differently): our player
                    g_prr_a[k] = me ? me : g_p10_gdst;
                }
                else g_prr_a[k] = m.raw[k];
            }
            {   // rc1: right after a mission RESTART the clone is not rebuilt yet (g_p10_gdst 0) and our player can be
                // 0 during the reload - a pawn arg of 0 crashed FUN_001e5710 at 0x1e57e2 ([edi+0xC4]). Skip that hit.
                bool bad = false;
                for (int k = 0; k < 7 && k < (int)m.nargs; k++) {
                    uint32_t kd = (m.kinds >> (2 * k)) & 3;
                    if ((kd == 2 || kd == 3) && !g_prr_a[k]) bad = true;
                }
                if (bad) { static int nb; if (nb++ < 5) logf("coop: prop replay skipped - no pawn yet (restart/reload), prop %08x", m.prop); continue; }
            }
            uint32_t slot = m.slot == 0x20 || m.slot == 0x24 || m.slot == 0x2C ? m.slot : 0;
            if (!slot) continue;
            g_prr_fn = *(volatile uint32_t *)(uintptr_t)(vt + slot); g_prr_ecx = m.prop;
            g_pr_replay = 1; coop_thunk_prop_replay(); g_pr_replay = 0;
            g_pr_stats[2]++;
            static int rl;
            if (rl++ < 40) logf("coop: prop sync: JOINER replayed host damage vt+0x%X on prop %08x", slot, m.prop);
        }
        g_pr_in_n = 0;
    }
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last > 30000 && (g_pr_stats[0] | g_pr_stats[1] | g_pr_stats[2])) {
        last = now;
        logf("coop: prop sync: host sent %u | joiner dropped %u replayed %u | host overflow %u", g_pr_stats[0], g_pr_stats[1], g_pr_stats[2], g_pr_stats[3]);
    }
}
static void coop_prop_recv(const uint8_t *raw, int len)
{
    if (g_pr_on != 1 || g_cfg.coop_role != 2 || len < 12) return;
    uint32_t n = *(const uint32_t *)(raw + 8);
    if (n > 16 || len < (int)(12 + n * sizeof(PropMsg))) return;
    for (uint32_t i = 0; i < n && g_pr_in_n < 32; i++) g_pr_in[g_pr_in_n++] = ((const PropMsg *)(raw + 12))[i];
}
// NO-PAUSE (owner: "going to the pause menu no longer pauses the game for all players"). np2 trace: the in-level
// pause does NOT use the frame's mode 1 (that is the front end at boot). FUN_0020d400 (pause request
// session+0x21088 == 1) sets session(*0x2D45F0)+0x210C8 = 1, blocks game input ([*0x2D45E8]+0xAEC = 1), pauses the
// sound channels and opens the pause menu; the WORLD update FUN_001f1080 (EAX = world) returns at once while
// +0x210C8 is set. Co-op: the PLAY-mode call 0x20f7ff (E8 -> FUN_001f1080) goes through coop_thunk_world, which
// clears +0x210C8 for the world update only and sets it back - the world (AI, physics, our co-op step) keeps
// running while the menu (and the game's own input block) stays. Env COOP_NO_PAUSE=0 = off.
volatile uint32_t g_np_on, g_np_world_fn, g_np_runs;
__declspec(naked) void coop_thunk_world()
{
    __asm {
        cmp dword ptr [g_np_on], 0
        je plain
        push ecx
        mov ecx, dword ptr ds:[0x002D45F0]
        test ecx, ecx
        je plainpop
        cmp byte ptr [ecx + 0x210C8], 0
        je plainpop
        mov byte ptr [ecx + 0x210C8], 0
        pop ecx
        inc dword ptr [g_np_runs]
        call dword ptr [g_np_world_fn]
        push ecx
        mov ecx, dword ptr ds:[0x002D45F0]
        mov byte ptr [ecx + 0x210C8], 1
        pop ecx
        ret
    plainpop:
        pop ecx
    plain:
        jmp dword ptr [g_np_world_fn]
    }
}
// RESTART RULE (owner: "only the host has the capacity to restart the mission, the joiner can only quit"). The
// front end's command table ([*0x2D199C]: {name hash, handler} x 0x6E) - pause menu: RestartLevel FUN_00124250,
// QuitLevel FUN_00124850; mission failed screen: RestartMission FUN_00124190, ContinueMission FUN_001240d0,
// QuitFromMissionFailed FUN_00124750 (all void(), RET). JOINER: the three restarts do nothing (quit works).
// HOST: a restart first tells the joiner ("CLRT" {which}, 3x), whose copy then runs the same handler.
// The joiner's packet loop runs inside the player update (not on the failed screen) -> coop_fe_poll reads
// "CLRT" / "CLMF" from the front end update FUN_00124330 while the world has not stepped for 0.5 s.
volatile uint32_t g_rr_resume[3], g_rr_which, g_rr_skip, g_rr_replay;
static const char *k_rt_name[3] = {"RestartLevel (pause)", "RestartMission (failed)", "ContinueMission (failed)"};
extern "C" void coop_rt_check(void)
{
    g_rr_skip = 0;
    if (!g_cfg.coop_role || g_rr_replay) return;
    int w = (int)g_rr_which;
    if (g_cfg.coop_role == 2) { g_rr_skip = 1; logf("coop: restart rule: JOINER chose %s - only the host can restart", k_rt_name[w]); return; }
    static uint32_t seq;
    struct { uint32_t magic, which, seq; } m = {0x54524C43, (uint32_t)w, ++seq + (uint32_t)(GetTickCount64() & 0xFFFF0000u)};
    for (int r = 0; r < 3; r++) sendto(g_coop_sock, (const char *)&m, sizeof m, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
    logf("coop: restart rule: HOST chose %s -> the joiner restarts too", k_rt_name[w]);
    coop_restart_reset();
}
__declspec(naked) void coop_thunk_rt0()   // FUN_00124250: 8B 0D E4 12 2D 00
{
    __asm {
        pushad
        mov dword ptr [g_rr_which], 0
        call coop_rt_check
        popad
        cmp dword ptr [g_rr_skip], 0
        jne skip0
        mov ecx, dword ptr ds:[0x002D12E4]
        jmp dword ptr [g_rr_resume + 0]
    skip0:
        ret
    }
}
__declspec(naked) void coop_thunk_rt1()   // FUN_00124190: 53 56 8B 35 9C 19 2D 00
{
    __asm {
        pushad
        mov dword ptr [g_rr_which], 1
        call coop_rt_check
        popad
        cmp dword ptr [g_rr_skip], 0
        jne skip1
        push ebx
        push esi
        mov esi, dword ptr ds:[0x002D199C]
        jmp dword ptr [g_rr_resume + 4]
    skip1:
        ret
    }
}
__declspec(naked) void coop_thunk_rt2()   // FUN_001240d0: 56 8B 35 9C 19 2D 00
{
    __asm {
        pushad
        mov dword ptr [g_rr_which], 2
        call coop_rt_check
        popad
        cmp dword ptr [g_rr_skip], 0
        jne skip2
        push esi
        mov esi, dword ptr ds:[0x002D199C]
        jmp dword ptr [g_rr_resume + 8]
    skip2:
        ret
    }
}
static void install_restart_rule(void)
{
    struct { uint32_t va; uint8_t k[8]; int len; void *th; } site[3] = {
        {0x00124250, {0x8B, 0x0D, 0xE4, 0x12, 0x2D, 0x00}, 6, (void *)coop_thunk_rt0},
        {0x00124190, {0x53, 0x56, 0x8B, 0x35, 0x9C, 0x19, 0x2D, 0x00}, 8, (void *)coop_thunk_rt1},
        {0x001240D0, {0x56, 0x8B, 0x35, 0x9C, 0x19, 0x2D, 0x00}, 7, (void *)coop_thunk_rt2}};
    for (int i = 0; i < 3; i++) if (memcmp((void *)(uintptr_t)xbe_va(site[i].va), site[i].k, site[i].len) != 0) { logf("coop: restart rule: %08x mismatch - not installed", site[i].va); return; }
    for (int i = 0; i < 3; i++) {
        uint32_t a = xbe_va(site[i].va); auto *c = (uint8_t *)(uintptr_t)a;
        g_rr_resume[i] = a + site[i].len;
        c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)site[i].th - (a + 5)); memcpy(c + 1, &rel, 4);
        for (int k = 5; k < site[i].len; k++) c[k] = 0x90;
    }
    logf("coop: restart rule installed (joiner: no restart; host restart -> joiner restarts too)");
}
// a restart begins: nobody is downed any more and the mission is no longer failed (mf7: the flags survived
// the reload -> both still "downed" -> the failed screen came straight back)
static void coop_restart_reset(void)
{
    g_coop_downed = 0; g_coop_peer_downed = 0;
    extern volatile uint32_t g_coop_mission_failed; g_coop_mission_failed = 0;
    uint32_t S = *(volatile uint32_t *)(uintptr_t)0x002D45F0, cur = 0;
    if (S >= 0x80000000 && S < 0x84000000 && coop_safe_copy(&cur, S + 0x21070, 4) && cur >= 0x80000000 && cur < 0x84000000)
        *(volatile uint32_t *)(uintptr_t)(cur + 0xC) &= ~1u;
    logf("coop: restart: downed / mission failed state cleared");
}
typedef void(__cdecl *rt_fn)(void);
static void coop_rt_apply(uint32_t which, uint32_t seq)   // joiner: the host restarted
{
    static uint32_t last = 0xFFFFFFFF;
    if (g_cfg.coop_role != 2 || which > 2 || seq == last) return;   // mf5: the 3 copies restarted 3 times
    last = seq;
    static const uint32_t va[3] = {0x00124250, 0x00124190, 0x001240D0};
    logf("coop: restart rule: the HOST restarted (%s) -> restarting here", k_rt_name[which]);
    coop_restart_reset();
    g_rr_replay = 1; ((rt_fn)(uintptr_t)xbe_va(va[which]))(); g_rr_replay = 0;
}
// MISSION FAILED (owner: "if both players are downed it is a mission failed"). The session's game state
// (*0x2D45F0 +0x21070) during play is the play state (+0x20F78, vt 00295338); its update FUN_0020eb60 reads
// flags at state+0xC: bit 1 -> the FAILED overlay (session+0x2103C, vt 002952F4, enter FUN_0020c630 =
// "MissionFailed" screen: RestartMission / ContinueMission / QuitFromMissionFailed), bit 2 -> +0x2104C. HOST:
// both players downed for 1.5 s -> set bit 1 on its state and tell the joiner ("CLMF", 3x), who does the same.
// The failed screen's restarts follow the restart rule (host only, the joiner follows).
// Test: COOP_HOST_DOWN_AT=<sec> downs the host once (the joiner's --downed-at downs the joiner).
volatile uint32_t g_coop_mission_failed;   // set at the failure, cleared with the level reset
static void coop_set_mission_failed(const char *why)
{
    g_coop_mission_failed = 1;
    uint32_t S = *(volatile uint32_t *)(uintptr_t)0x002D45F0, cur = 0;
    if (S < 0x80000000 || S >= 0x84000000 || !coop_safe_copy(&cur, S + 0x21070, 4) || cur < 0x80000000 || cur >= 0x84000000) return;
    uint32_t vt = 0; coop_safe_copy(&vt, cur, 4);
    *(volatile uint32_t *)(uintptr_t)(cur + 0xC) |= 1;
    logf("coop: MISSION FAILED (%s): state %08x (+%05X, vt %08x) flags +0xC |= 1", why, cur, cur - S, vt);
}
static void coop_mission_fail_step(uint32_t obj)
{
    if (!g_cfg.coop_role || !g_cfg.coop_downed) return;
    {   // COOP_HOST_DOWN_AT=<sec> (test, host): down our player once
        static int hd = -2;
        if (hd == -2) { char e[16] = {0}; hd = GetEnvironmentVariableA("COOP_HOST_DOWN_AT", e, sizeof e) ? atoi(e) : -1; }
        if (hd > 0 && g_cfg.coop_role == 1 && !g_coop_downed && g_coop_t0 && GetTickCount64() - g_coop_t0 >= (uint64_t)hd * 1000 && obj >= 0x80000000) {
            hd = -1; logf("coop: host down test at %d s", (int)((GetTickCount64() - g_coop_t0) / 1000));
            extern volatile uint32_t g_pd_fn;
            if (!g_pd_fn) g_pd_fn = xbe_va(0x001ED990);   // the player damage fn (set on the joiner by coop_hit_apply only)
            coop_downed_test_fire(obj);
        }
    }
    if (g_cfg.coop_role != 1) return;
    static uint64_t both_since; static int sent;
    bool both = g_coop_downed && g_coop_peer_downed && g_coop_host_ms && GetTickCount64() - g_coop_host_ms < 2000;
    if (!both) { both_since = 0; sent = 0; return; }
    if (!both_since) { both_since = GetTickCount64(); logf("coop: BOTH players downed - mission failed in 1.5 s"); }
    if (sent || GetTickCount64() - both_since < 1500) return;
    sent = 1;
    struct { uint32_t magic; } m = {0x464D4C43};
    for (int r = 0; r < 3; r++) sendto(g_coop_sock, (const char *)&m, sizeof m, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
    coop_set_mission_failed("both players downed");
}
static void coop_mf_apply(void)
{
    static uint64_t last;
    if (g_cfg.coop_role != 2 || GetTickCount64() - last < 3000) return;   // the 3 copies of the packet count once
    last = GetTickCount64();
    coop_set_mission_failed("the host says both players are downed");
}
// STATE SCAN (diagnostic, the mission-failed state object): session(*0x2D45F0) dwords that are game-state vtables
static void coop_state_scan(void)
{
    static int done;
    if (done || g_cfg.coop_role != 1 || !g_coop_t0 || GetTickCount64() - g_coop_t0 < 70000) return;
    done = 1;
    uint32_t S = *(volatile uint32_t *)(uintptr_t)0x002D45F0;
    char b[700]; int bp = 0; b[0] = 0;
    for (uint32_t o = 0x20000; o < 0x21200 && bp < 650; o += 4) {
        uint32_t v = 0; coop_safe_copy(&v, S + o, 4);
        if (v >= 0x00294000 && v < 0x00296000) { uint32_t e3 = 0; coop_safe_copy(&e3, v + 0xC, 4); bp += snprintf(b + bp, sizeof b - bp, " +%05X vt %08x(+C %08x)", o, v, e3); }
    }
    uint32_t cur = 0; coop_safe_copy(&cur, S + 0x21070, 4);
    logf("coop: state scan: session %08x current state %08x (+%05X):%s", S, cur, cur - S, b);
}
bool coop_game_paused(void);
// called by coop_on_player_update (the bridge) every frame: skip the game's player update while paused in co-op
static void coop_pause_gate()
{
    bool p = g_cfg.coop_role && coop_game_paused();
    if (p != (g_skip_player != 0)) logf("coop: pause: the pause menu is %s - our player's update %s", p ? "OPEN" : "closed", p ? "is held (no move / fire / throw)" : "runs again");
    g_skip_player = p ? 1 : 0;
}
bool coop_game_paused(void)   // the in-level pause menu is open (input layer: no mouse-look while it is)
{
    uint32_t I = *(volatile uint32_t *)(uintptr_t)0x002D45E8;
    return g_np_on && I >= 0x80000000 && I < 0x84000000 && *(volatile uint8_t *)(uintptr_t)(I + 0xAEC) != 0;
}
static void install_no_pause(void)
{
    char e[8] = {0};
    if (GetEnvironmentVariableA("COOP_NO_PAUSE", e, sizeof e) && e[0] == '0') { logf("coop: no-pause: OFF (COOP_NO_PAUSE=0)"); return; }
    uint32_t site = xbe_va(0x0020F7FF), fn = xbe_va(0x001F1080);
    auto *c = (uint8_t *)(uintptr_t)site;
    int32_t cur = 0; memcpy(&cur, c + 1, 4);
    if (c[0] != 0xE8 || site + 5 + cur != fn) { logf("coop: no-pause: 0020f7ff is not a call to FUN_001f1080 - not installed"); return; }
    g_np_world_fn = fn;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_world - (site + 5));
    memcpy(c + 1, &rel, 4);
    g_np_on = 1;
    logf("coop: no-pause installed at 0x20f7ff (the world update runs under the in-level pause menu)");
}
// AGENT LIST GUARD (mf6 joiner crash 0x195e61 at the RESTART reload): FUN_001abd30 (level unload) runs
// FUN_00195e00 on each of the 16 brains; a brain whose +0x134 says "in the agent list" (1 / 2) is unlinked
// from the list at registry+0x1298 (next at +4) - on the joiner that list was EMPTY (head 0) while a puppet's
// brain still claimed membership, and the walk read [0+4]. Patch at 0x195e61 (39 48 04 74 0E = cmp [eax+4],
// ecx; je 0x195e74): an empty list skips to the function's own exit path 0x195e87.
volatile uint32_t g_al_found, g_al_next, g_al_exit, g_al_hits;
__declspec(naked) void coop_thunk_agent_list()
{
    __asm {
        test eax, eax
        jz empty
        cmp dword ptr [eax + 4], ecx
        je found
        jmp dword ptr [g_al_next]
    found:
        jmp dword ptr [g_al_found]
    empty:
        inc dword ptr [g_al_hits]
        jmp dword ptr [g_al_exit]
    }
}
static void install_agent_list_guard(void)
{
    uint32_t a = xbe_va(0x00195E61); auto *c = (uint8_t *)(uintptr_t)a;
    static const uint8_t k[5] = {0x39, 0x48, 0x04, 0x74, 0x0E};
    if (memcmp(c, k, 5) != 0) { logf("coop: agent list guard: 00195e61 mismatch - not installed"); return; }
    g_al_found = xbe_va(0x00195E74); g_al_next = xbe_va(0x00195E66); g_al_exit = xbe_va(0x00195E87);
    c[0] = 0xE9; int32_t rel = (int32_t)((uintptr_t)coop_thunk_agent_list - (a + 5)); memcpy(c + 1, &rel, 4);
    logf("coop: agent list guard installed at 0x195e61 (an empty agent list at unload is skipped)");
}
static void install_ammo_guard(void)
{
    uint32_t site = xbe_va(0x00199600);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x8B, 0x46, 0x04, 0x83, 0xEC, 0x0C};
    if (memcmp(code, kExpect, 6) != 0) { logf("coop: ammo guard: 00199600 bytes mismatch - not installed"); return; }
    g_ag_resume = site + 6;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_ammo_guard - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("coop: ammo guard installed at FUN_00199600");
}
static void install_weapon_guard(void)
{
    uint32_t site = xbe_va(0x001AF190);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x8B, 0x87, 0xAC, 0x03, 0x00, 0x00};
    if (memcmp(code, kExpect, 6) != 0) { logf("coop: weapon guard: 001af190 bytes mismatch - not installed"); return; }
    g_wg_resume = site + 6;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_weapon_guard - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("coop: weapon guard installed at FUN_001af190 (null weapon/ammo -> skip)");
}
static void install_clone_shield(void)
{
    struct { uint32_t va; volatile uint32_t *resume; volatile uint32_t *victim; void *thunk; const char *name; } sites[3] = {
        { 0x001DF830, &g_xd_resume, &g_xd_victim, (void *)coop_thunk_generic_damage,  "generic FUN_001df830" },
        { 0x001DFA90, &g_sd_resume, &g_sd_victim, (void *)coop_thunk_stealth_damage, "stealth FUN_001dfa90" },
        { 0x001DFC30, &g_pd24_resume, &g_pd24_victim, (void *)coop_thunk_debris_damage, "debris FUN_001dfc30" },
    };
    for (int i = 0; i < 3; i++) {
        uint32_t site = xbe_va(sites[i].va);
        auto *code = (uint8_t *)(uintptr_t)site;
        static const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
        if (memcmp(code, kExpect, 6) != 0) { logf("coop: clone shield: %08x bytes mismatch", sites[i].va); continue; }
        *sites[i].resume = site + 6;
        code[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)sites[i].thunk - (site + 5));
        memcpy(code + 1, &rel, 4);
        code[5] = 0x90;
        logf("coop: clone shield: %s hooked (damage on the clone is skipped)", sites[i].name);
    }
}
volatile uint32_t g_pd_fn, g_pd_this, g_pd_vec, g_pd_part, g_pd_p5, g_pd_p6, g_pd_att, g_pd_esp;
volatile float g_pd_dmg;
__declspec(naked) void coop_thunk_player_damage()
{
    __asm {
        pushad
        mov dword ptr [g_pd_esp], esp
        push dword ptr [g_pd_att]
        push dword ptr [g_pd_p6]
        push dword ptr [g_pd_p5]
        push dword ptr [g_pd_dmg]
        push dword ptr [g_pd_part]
        push dword ptr [g_pd_vec]
        mov ecx, dword ptr [g_pd_this]
        call dword ptr [g_pd_fn]           // FUN_001ed990 (thiscall)
        mov esp, dword ptr [g_pd_esp]      // whatever its cleanup convention
        popad
        ret
    }
}
static HitMsg g_hit_in[32]; static int g_hit_in_n;
static void coop_hit_recv(const HitMsg *h, int n)
{
    for (int i = 0; i < n && g_hit_in_n < 32; i++) {
        g_hit_in[g_hit_in_n] = h[i];
        // enemy identity: the host's attacker -> our enemy with the same id; not spawned here = any live
        // local enemy stands in (the damage is the host's decision and must still land)
        // play 20261113_191753 ("team ally still hurt the joiner"): the host's attacker was an ENEMY (801e4c60)
        // with no local pair; the stand-in loop took the first body with an enemy id - and squadmates get ids
        // too (801e39a0, spawn point 81a858a0) -> the joiner was "hit by an ally". Both the pair and the
        // stand-in must be HOSTILE to our player (team != ours: squad roster slot 3 = registry + 0x22980);
        // the stand-in is the nearest hostile soldier.
        uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, me = 0, myteam = 0;
        if (coop_safe_copy(&me, R + 0x22980, 4) && me) coop_safe_copy(&myteam, me + 0x39C, 4);
        auto hostile = [&](uint32_t b) -> bool {
            uint32_t t = myteam, vt = 0; float hp = 0;
            return b >= 0x80000000 && b < 0x84000000 && coop_safe_copy(&vt, b, 4) && vt == 0x0028C1E8 && b != g_p10_gdst &&
                   coop_safe_copy(&t, b + 0x39C, 4) && t != myteam && coop_safe_copy(&hp, b + 0x2F0, 4) && hp > 0.0f && hp < 1e30f;
        };
        uint32_t la = coop_host_to_local(h[i].attacker);
        if (la && !hostile(la)) la = 0;
        if (!la) {
            uint32_t s[16]; uint32_t ns = enemy_slots(s); float best = 1e30f;
            for (uint32_t k = 0; k < ns; k++) {
                if (!hostile(s[k])) continue;
                float d = 0.0f;
                if (me) { const volatile float *a2 = (const volatile float *)(uintptr_t)(s[k] + 0xA0), *b2 = (const volatile float *)(uintptr_t)(me + 0xA0);
                          float dx = a2[0] - b2[0], dz = a2[2] - b2[2]; d = dx * dx + dz * dz; }
                if (d < best) { best = d; la = s[k]; }
            }
        }
        g_hit_in[g_hit_in_n].attacker = la;   // 0 = no hostile soldier here: the hit is skipped (never an ally / a raw host address)
        g_hit_in_n++; g_hit_stats[1]++;
    }
    static int rl;
    if (rl++ < 10) logf("coop: hit relay: received %d host hit(s) (first from %08x dmg %.1f)", n, n ? h[0].attacker : 0, n ? h[0].dmg : 0.0f);
}
static void coop_hit_apply(uint32_t pobj)
{
    if (g_cfg.coop_role != 2 || pobj < 0x80000000 || pobj >= 0x84000000) { g_hit_in_n = 0; return; }
    if (!g_pd_fn) g_pd_fn = xbe_va(0x001ED990);
    for (int i = 0; i < g_hit_in_n; i++) {
        const HitMsg &h = g_hit_in[i];
        uint32_t att = h.attacker;
        uint32_t al = (att >= 0x80000000 && att < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(att + 0x324) : 0;
        if (att < 0x80000000 || att >= 0x84000000 || *(volatile uint32_t *)(uintptr_t)att != 0x0028C1E8 || al < 0x10000) {
            static int sk;
            if (sk++ < 10) logf("coop: hit relay: SKIP host hit from %08x (vt %08x link %08x)", att,
                                (att >= 0x80000000 && att < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)att : 0, al);
            continue;
        }
        __declspec(align(16)) static float vec[4], pvec[4];   // movaps: 16-byte aligned
        for (int k = 0; k < 4; k++) { vec[k] = h.vec[k]; pvec[k] = h.pvec[k]; }
        uint8_t old4d = *(volatile uint8_t *)(uintptr_t)(al + 0x4D);
        *(volatile uint8_t *)(uintptr_t)(al + 0x4D) = 1;
        float before = *(volatile float *)(uintptr_t)(pobj + 0x2F0);
        // --hit-test-death: pin our hp to this hit's damage ONCE so the hit lands on
        // exactly 0 and trips FUN_001ed990's own death branch (HUD event 4, hp = 0,
        // then the link-vtable death dispatch (link vt+0x10)(5, vec, dmg, attacker)).
        static bool death_pinned = false;
        if (g_cfg.hit_test_death && !death_pinned) {
            death_pinned = true;
            before = h.dmg;
            *(volatile float *)(uintptr_t)(pobj + 0x2F0) = h.dmg;
            logf("coop: hit relay: TEST DEATH armed - hp pinned to %.1f for the incoming hit", h.dmg);
        }
        if (before - h.dmg <= 0.0f) {
            // forensics BEFORE the call: the death dispatch target, so a crash inside
            // it still leaves the addresses in the log
            uint32_t lnk = *(volatile uint32_t *)(uintptr_t)(pobj + 0x324);
            uint32_t lvt = (lnk >= 0x10000 && lnk < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)lnk : 0;
            uint32_t dfn = (lvt >= 0x10000 && lvt < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(lvt + 0x10) : 0;
            logf("coop: hit relay: DEATH predicted (hp %.1f - dmg %.1f); dispatch link %08x vt %08x vt+0x10 -> %08x",
                 before, h.dmg, lnk, lvt, dfn);
        }
        g_pd_this = pobj; g_pd_vec = (uint32_t)(uintptr_t)vec; g_pd_part = (uint32_t)(uintptr_t)pvec; g_pd_dmg = h.dmg;
        g_pd_p5 = h.p5; g_pd_p6 = h.p6; g_pd_att = att;
        coop_thunk_player_damage();
        *(volatile uint8_t *)(uintptr_t)(al + 0x4D) = old4d;
        g_hit_stats[2]++;
        if (*(volatile float *)(uintptr_t)(pobj + 0x2F0) <= 0.0f)
            logf("coop: hit relay: PLAYER DIED via relayed hit (attacker %08x dmg %.1f)", att, h.dmg);
        static uint32_t ln;
        if (ln++ < 30) logf("coop: hit relay: applied host hit from %08x dmg %.1f -> our health %.1f -> %.1f",
                            att, h.dmg, before, *(volatile float *)(uintptr_t)(pobj + 0x2F0));
    }
    g_hit_in_n = 0;
}

// --downed-at: the synthetic "host hit" (see the call site in the enemy-sync tick).
// Attacker = the clone-of-host body (its private aim link is the +0x324 we toggle,
// the same force-may-hurt trick coop_hit_apply uses), part/p5/p6 mirror the real
// relayed hits (0x80202FB0 / 4 / 4). hp is pinned to the dmg first so the downed
// guard sees a killing blow and clamps the call to leave hp = 1 (DOWNED; the
// death branch never runs).
// generic thiscall with 7 stack args; ESP restored after the call, so the callee's own
// cleanup convention (or a decompiler's short arg list) cannot unbalance the stack
volatile uint32_t g_c7_fn, g_c7_this, g_c7_a[7], g_c7_esp;
__declspec(naked) void coop_thunk_call7()
{
    __asm {
        pushad
        mov dword ptr [g_c7_esp], esp
        push dword ptr [g_c7_a + 24]
        push dword ptr [g_c7_a + 20]
        push dword ptr [g_c7_a + 16]
        push dword ptr [g_c7_a + 12]
        push dword ptr [g_c7_a + 8]
        push dword ptr [g_c7_a + 4]
        push dword ptr [g_c7_a + 0]
        mov ecx, dword ptr [g_c7_this]
        call dword ptr [g_c7_fn]
        mov esp, dword ptr [g_c7_esp]
        popad
        ret
    }
}
static void coop_downed_test_fire(uint32_t pobj)
{
    uint32_t att = g_p10_gdst;
    if (att < 0x80000000 || att >= 0x84000000) return;
    if (g_cfg.downed_via_explosion) {
        // review test: a lethal hit through the player's EXPLOSION virtual (vt+0x2C =
        // FUN_001ed2b0), which the bullet clamp does not see -> the death backstop must
        // turn it into DOWNED. Disassembly (RET 0x1C = 7 args): [EBP+8] attacker (death
        // dispatch arg), [EBP+0xC] vec*, [EBP+0x10] dmg, [EBP+0x14] vec* (read at entry -
        // rv151 crashed with 0 there). Args (attacker, dir*, dmg, dir*, 0, kind 1, 0).
        static float dir[4] = {0.0f, 0.0f, 1.0f, 0.0f};
        uint32_t vt = *(volatile uint32_t *)(uintptr_t)pobj;
        float before = *(volatile float *)(uintptr_t)(pobj + 0x2F0);
        float big = 100000.0f;
        g_c7_fn = *(volatile uint32_t *)(uintptr_t)(vt + 0x2C);
        g_c7_this = pobj;
        g_c7_a[0] = att; g_c7_a[1] = (uint32_t)(uintptr_t)dir; memcpy((void *)&g_c7_a[2], &big, 4);
        g_c7_a[3] = (uint32_t)(uintptr_t)dir; g_c7_a[4] = 0; g_c7_a[5] = 1; g_c7_a[6] = 0;
        coop_thunk_call7();
        logf("coop: downed test: synthetic EXPLOSION via vt+0x2C %08x (hp %.1f -> %.1f, downed %u)", g_c7_fn, before,
             *(volatile float *)(uintptr_t)(pobj + 0x2F0), g_coop_downed);
        return;
    }
    uint32_t al = *(volatile uint32_t *)(uintptr_t)(att + 0x324);
    if (al < 0x10000 || al >= 0x84000000) {
        logf("coop: downed test: no link on the clone attacker %08x (skip)", att);
        return;
    }
    uint8_t old4d = *(volatile uint8_t *)(uintptr_t)(al + 0x4D);
    *(volatile uint8_t *)(uintptr_t)(al + 0x4D) = 1;
    static float vec[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float before = *(volatile float *)(uintptr_t)(pobj + 0x2F0);
    *(volatile float *)(uintptr_t)(pobj + 0x2F0) = 26.0f;
    g_pd_this = pobj; g_pd_vec = (uint32_t)(uintptr_t)vec; g_pd_part = 0x80202FB0;
    g_pd_dmg = 26.0f; g_pd_p5 = 4; g_pd_p6 = 4; g_pd_att = att;
    coop_thunk_player_damage();
    *(volatile uint8_t *)(uintptr_t)(al + 0x4D) = old4d;
    logf("coop: downed test: synthetic hit from clone %08x (hp %.1f pinned -> %.1f)", att, before,
         *(volatile float *)(uintptr_t)(pobj + 0x2F0));
}
// joiner: replay each relayed explosion / melee hit through OUR player's own damage slot
// (+0x2C FUN_001ed2b0 / +0x48 FUN_001ed5d0): HUD, hit direction, health and - on a
// killing blow - the death dispatch, which the downed backstop turns into DOWNED.
static void coop_xd_apply(uint32_t pobj)
{
    if (g_cfg.coop_role != 2 || pobj < 0x80000000 || pobj >= 0x84000000) { g_xd_in_n = 0; return; }
    for (int i = 0; i < g_xd_in_n; i++) {
        const XdMsg &m = g_xd_in[i];
        if (g_coop_downed) continue;                       // a downed player takes no more damage
        if ((m.slot != 0x2C && m.slot != 0x48) || m.nargs > 7) continue;
        static float data[7][8];
        uint32_t args[7] = {0};
        for (uint32_t k = 0; k < m.nargs; k++) {
            uint32_t v = m.raw[k];
            if (m.objmask & (1u << k)) {
                if (v == m.host_clone) v = pobj;                         // the host's clone of us = us
                else if (v == m.host_player) v = g_p10_gdst;             // the host player = our clone of them
                else if (coop_host_to_local(v)) v = coop_host_to_local(v); // a host enemy = our enemy with the same id
                else if (!coop_is_game_object(v)) v = g_p10_gdst;        // unknown here: a valid pawn
                args[k] = v;
            } else if (m.ptrmask & (1u << k)) {
                memcpy(data[k], m.data[k], sizeof data[k]);
                args[k] = (uint32_t)(uintptr_t)data[k];
            } else args[k] = v;
        }
        // the vector slots must be pointers whatever the packet says (FUN_001ed2b0 reads
        // its dir* and vec* at entry; rv151/xd192 crashed on non-pointers there)
        const uint32_t need = m.slot == 0x2C ? 0x0Bu : 0x03u;
        for (int k = 0; k < 7; k++)
            if ((need & (1u << k)) && !(m.ptrmask & (1u << k))) {
                memset(data[k], 0, sizeof data[k]);
                data[k][2] = 1.0f;
                args[k] = (uint32_t)(uintptr_t)data[k];
            }
        // melee: FUN_001dfa90 reads the attacker (arg 4) +0x39c on a kill - never null
        if (m.slot == 0x48 && !coop_is_game_object(args[3])) args[3] = g_p10_gdst;
        uint32_t vt = *(volatile uint32_t *)(uintptr_t)pobj;
        float before = *(volatile float *)(uintptr_t)(pobj + 0x2F0), dmg;
        memcpy(&dmg, &args[2], 4);
        g_c7_fn = *(volatile uint32_t *)(uintptr_t)(vt + m.slot);
        g_c7_this = pobj;
        for (int k = 0; k < 7; k++) g_c7_a[k] = args[k];
        coop_thunk_call7();
        g_xd_stats[2]++;
        static int ln;
        if (ln++ < 30)
            logf("coop: xd relay: applied host %s hit (slot +0x%X fn %08x dmg %.1f) -> our health %.1f -> %.1f%s",
                 m.slot == 0x2C ? "explosion" : "melee", m.slot, g_c7_fn, dmg, before,
                 *(volatile float *)(uintptr_t)(pobj + 0x2F0), g_coop_downed ? " (DOWNED)" : "");
    }
    g_xd_in_n = 0;
}
// --xd-test N (host): N s after launch hit the joiner's clone through its explosion slot,
// 3 s later through its melee slot (attacker = the host player). The clone shield
// captures + skips both, so this exercises capture -> CLXD -> apply end to end.
static void coop_xd_test(void)
{
    static int stage;
    if (stage >= 2 || !g_cfg.xd_test || g_cfg.coop_role != 1) return;
    uint32_t c = g_p10_gdst;
    if (c < 0x80000000 || c >= 0x84000000) return;
    int64_t t = (int64_t)(GetTickCount64() - g_coop_t0);
    if (t < (int64_t)g_cfg.xd_test * 1000 + stage * 3000) return;
    static float hv[4] = {0, 0, 0, 1}, dir[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    const float *cp = (const float *)(uintptr_t)(c + 0xA0);
    hv[0] = cp[0]; hv[1] = cp[1] + 1.0f; hv[2] = cp[2];
    uint32_t vt = *(volatile uint32_t *)(uintptr_t)c;
    uint32_t hostp = (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000)
        ? *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C) : 0;
    g_c7_this = c;
    for (int k = 0; k < 7; k++) g_c7_a[k] = 0;
    float d = stage == 0 ? 40.0f : 30.0f;
    { char e[24] = {0}; if (stage == 0 && GetEnvironmentVariableA("COOP_XD_DMG", e, sizeof e)) d = (float)atof(e); }   // test: lethal blast
    g_c7_fn = *(volatile uint32_t *)(uintptr_t)(vt + (stage == 0 ? 0x2C : 0x48));
    g_c7_a[0] = (uint32_t)(uintptr_t)hv; g_c7_a[1] = (uint32_t)(uintptr_t)dir; memcpy((void *)&g_c7_a[2], &d, 4);
    if (stage == 0) { g_c7_a[3] = (uint32_t)(uintptr_t)hv; g_c7_a[5] = 1; }
    else g_c7_a[3] = hostp;
    logf("coop: xd test: %s hit on the joiner's clone %08x via vt+0x%X (%08x)", stage == 0 ? "EXPLOSION" : "MELEE", c,
         stage == 0 ? 0x2C : 0x48, g_c7_fn);
    coop_thunk_call7();
    stage++;
}
static void install_damage_hook(void)
{
    uint32_t site = xbe_va(0x001E74B0);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    if (memcmp(code, kExpect, 6) != 0) { logf("coop: 001e74b0 bytes mismatch; no hit relay"); return; }
    g_dmg_resume = site + 6;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_pawn_damage - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("coop: hit relay: pawn damage hook installed at FUN_001e74b0");
}

// ---- DOWNED guard: entry hook on FUN_001ed990 (the player damage fn, 55 8B EC
// 83 E4 F0, resume +6). Killing blows on OUR player are clamped in the call's own
// damage argument so hp lands on exactly 1: the death branch (hp <= 0 -> the
// frozen death screen) never runs and the game keeps ticking. g_coop_downed is
// the spectator trigger. See the DOWNED block above coop_on_player_update.
volatile uint32_t g_pdh_victim, g_pdh_args;
volatile void *g_pdh_resume;
extern "C" void coop_on_player_damage(void)
{
    if (!g_cfg.coop_downed) return;
    uint32_t victim = g_pdh_victim;
    if (victim < 0x80000000 || victim >= 0x84000000) return;
    uint32_t po = 0;
    if (g_coop_last_ctrl >= 0x80000000 && g_coop_last_ctrl < 0x84000000)
        po = *(volatile uint32_t *)(uintptr_t)(g_coop_last_ctrl + 0x8C);
    if (!po || victim != po) return;    // not our player (enemies/pawns take real damage)
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_pdh_args;
    // a[0] = return, a[1] = p2, a[2] = p3, a[3] = dmg, a[4] = p5, a[5] = p6, a[6] = p7
    float dmg = *(const volatile float *)&a[3];
    float hp = *(volatile float *)(uintptr_t)(victim + 0x2F0);
    if (hp <= 0.0f || hp - dmg >= 1.0f) return;   // not a killing blow
    if (!g_coop_downed)
        logf("coop: DOWNED - killing blow %.1f on hp %.1f clamped to 1; spectator cam on", dmg, hp);
    g_coop_downed = 1;
    *(volatile float *)&a[3] = hp - 1.0f;
}
__declspec(naked) void coop_thunk_player_guard()
{
    __asm {
        pushad
        mov dword ptr [g_pdh_victim], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_pdh_args], eax
        call coop_on_player_damage
        popad
        push ebp                           // displaced: 55
        mov ebp, esp                       // displaced: 8B EC
        and esp, 0xFFFFFFF0                // displaced: 83 E4 F0
        jmp dword ptr [g_pdh_resume]
    }
}
// DEATH BACKSTOP (review fix): the clamp above only covers BULLETS. The player object's
// vtable (rv150: 0028cfe0) routes +0x20 bullets -> FUN_001ed990, but +0x2C generic/
// explosions -> FUN_001ed2b0 and +0x48 directional/melee/stealth -> FUN_001ed5d0
// (which runs FUN_001dfa90, where stealth classes are instakills that ignore the damage
// argument). Every player death ends in the controller's death virtual FUN_001d54c0
// (thiscall ECX = controller = the player's link, arg1 = event, 5 = death, RET 0x10;
// first instruction CMP dword [ESP+4],5 = 83 7C 24 04 05). In co-op a death event for
// OUR controller is swallowed: hp back to 1, downed - the same state as the clamp.
volatile uint32_t g_dd_this, g_dd_args, g_dd_skip, g_dd_resume;
extern "C" void coop_on_death_dispatch(void)
{
    g_dd_skip = 0;
    if (!g_cfg.coop_downed) return;
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_dd_args;   // [0] ret, [1] event
    uint32_t ctrl = g_coop_last_ctrl;
    if (a[1] != 5 || !ctrl || g_dd_this != ctrl) return;
    uint32_t po = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
    if (po < 0x80000000 || po >= 0x84000000) return;
    float hp = *(volatile float *)(uintptr_t)(po + 0x2F0);
    if (hp < 1.0f) *(volatile float *)(uintptr_t)(po + 0x2F0) = 1.0f;
    if (!g_coop_downed)
        logf("coop: DOWNED - death event caught at the controller (hp %.1f -> 1, caller %08x); spectator cam on", hp, a[0]);
    g_coop_downed = 1;
    g_dd_skip = 1;
}
__declspec(naked) void coop_thunk_death_dispatch()
{
    __asm {
        pushad
        mov dword ptr [g_dd_this], ecx
        lea eax, [esp + 0x20]
        mov dword ptr [g_dd_args], eax
        call coop_on_death_dispatch
        popad
        cmp dword ptr [g_dd_skip], 0
        jne skipdeath
        cmp dword ptr [esp + 4], 5         // displaced: 83 7C 24 04 05 (flags feed the JNZ at +8)
        jmp dword ptr [g_dd_resume]        // 0x1d54c5
    skipdeath:
        ret 0x10
    }
}
static void install_death_backstop(void)
{
    uint32_t site = xbe_va(0x001D54C0);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[5] = {0x83, 0x7C, 0x24, 0x04, 0x05};
    if (memcmp(code, kExpect, 5) != 0) { logf("coop: 001d54c0 bytes mismatch; no death backstop"); return; }
    g_dd_resume = site + 5;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_death_dispatch - (site + 5));
    memcpy(code + 1, &rel, 4);
    logf("coop: death backstop installed at FUN_001d54c0 (any player death -> downed)");
}

// ---- GAME UI (Flash/ActionScript front end). FUN_00101790(const char *path, const char
// *value) = "set variable" (cdecl, both strings; FUN_001208e0 hides/shows a HUD clip with
// "<clip>._visible" = "0"/"1"). --as-log records every distinct path the game sets, to
// find the HUD clip names (hide the HUD while downed; downed text in the game's font).
// Prologue 51 53 56 8B 35 10 43 2F 00 (push ecx/ebx/esi, mov esi,[0x2F4310]) re-run in
// the thunk, resume +9.
volatile uint32_t g_as_args, g_as_resume, g_as_fn;
extern "C" void coop_on_as_set(void)
{
    const volatile uint32_t *a = (const volatile uint32_t *)(uintptr_t)g_as_args;   // [0] ret [1] path [2] value
    const char *path = (const char *)(uintptr_t)a[1], *val = (const char *)(uintptr_t)a[2];
    if ((uintptr_t)path < 0x10000 || (uintptr_t)val < 0x10000) return;
    static std::vector<std::string> seen;
    if (seen.size() >= 400) return;
    char pb[96], vb[64];
    strncpy(pb, path, sizeof pb - 1); pb[sizeof pb - 1] = 0;
    strncpy(vb, val, sizeof vb - 1); vb[sizeof vb - 1] = 0;
    for (const std::string &x : seen) if (x == pb) return;
    seen.push_back(pb);
    logf("coop: AS set '%s' = '%s' (caller %08x)", pb, vb, a[0]);
}
__declspec(naked) void coop_thunk_as_set()
{
    __asm {
        pushad
        lea eax, [esp + 0x20]
        mov dword ptr [g_as_args], eax
        call coop_on_as_set
        popad
        push ecx                              // displaced: 51
        push ebx                              // displaced: 53
        push esi                              // displaced: 56
        mov esi, dword ptr ds:[0x002F4310]    // displaced: 8B 35 10 43 2F 00
        jmp dword ptr [g_as_resume]           // 0x101799
    }
}
static void install_as_log(void)
{
    uint32_t site = xbe_va(0x00101790);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[9] = {0x51, 0x53, 0x56, 0x8B, 0x35, 0x10, 0x43, 0x2F, 0x00};
    if (memcmp(code, kExpect, 9) != 0) { logf("coop: 00101790 bytes mismatch; no AS log"); return; }
    g_as_resume = site + 9;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_as_set - (site + 5));
    memcpy(code + 1, &rel, 4);
    for (int i = 5; i < 9; i++) code[i] = 0x90;
    logf("coop: AS set-variable logger installed at FUN_00101790");
}

// ---- RENDER PASS GATES (owner: hide the HUD and the weapon viewmodel while downed).
// dbg154 (--debug-frame 4320, Naszran): frame driver FUN_0020bbc0 = scene FUN_001ee820,
// UI layers FUN_0013c050(0/1), FUN_00121e60 (fades), HUD FUN_00129340 (one of five
// layouts via FUN_000da4b0 = draws 466-473, the last ones), FUN_001732a0. In the scene,
// the late pass FUN_00178c80 flushes model batches through FUN_00178c20 (17 model
// draws after the world - the viewmodel candidate). Each gated CALL site is re-pointed
// to a stub: skip (plain RET; all three are RET-only, caller-clean) when the gate is
// set, else JMP to the original. --hide-test stages the gates to identify the passes.
volatile uint32_t g_gate_hud, g_gate_vm, g_gate_ui;
volatile uint32_t g_orig_hud, g_orig_vm, g_orig_ui, g_orig_hudmov;
// sp159: the HUD stayed - in gameplay FUN_0020ba30 takes the [0x29D9A0] != 0 branch and
// draws the HUD movie directly: CALL FUN_000da4b0 at 0x20BA8B (push arg; RET 0x4 -> the
// skip stub must pop it too).
__declspec(naked) void coop_gate_hudmov() { __asm { cmp dword ptr [g_gate_hud], 0
                                                  jne skip
                                                  jmp dword ptr [g_orig_hudmov]
                                                skip:
                                                  ret 4 } }
__declspec(naked) void coop_gate_hud() { __asm { cmp dword ptr [g_gate_hud], 0
                                               jne skip
                                               jmp dword ptr [g_orig_hud]
                                             skip:
                                               ret } }
// The gate takes the first-person list call (0x1EE97C) before fov.cpp can: it marks the list's draws itself
// (g_fov_viewmodel), so the gun keeps the game's FOV and co-op SPRINT can lower it (sprint test 20261115: the flag was
// never set in co-op - "fov: the first-person list call at 0x001ee97c does not match").
extern "C" volatile uint8_t g_fov_viewmodel;
__declspec(naked) void coop_gate_vm()  { __asm { cmp dword ptr [g_gate_vm], 0
                                               jne skip
                                               mov byte ptr [g_fov_viewmodel], 1
                                               call dword ptr [g_orig_vm]
                                               mov byte ptr [g_fov_viewmodel], 0
                                             skip:
                                               ret } }
volatile uint32_t g_orig_vm2;   // the other first-person list call (0x1EEB2B: after the effects pass, FUN_001EE820)
__declspec(naked) void coop_gate_vm2() { __asm { cmp dword ptr [g_gate_vm], 0
                                               jne skip
                                               mov byte ptr [g_fov_viewmodel], 1
                                               call dword ptr [g_orig_vm2]
                                               mov byte ptr [g_fov_viewmodel], 0
                                             skip:
                                               ret } }
// hide156 verdict: FUN_00129340 is NOT the gameplay HUD (it stayed), FUN_00178c20 is
// the post-effects flush (sun glare/haze vanished, gun stayed), skipping the UI layers
// FUN_0013c050 drops the game into its "disc problem" screen -> those gates are gone.
// HUD = the Flash levels the game itself shows at level start (_level1/_level2, caller
// 0x13bfc6) -> hidden with the game's own set-variable call. Viewmodel candidate =
// FUN_00177000 (draws 405-412), called at 0x1EEA2A in FUN_001ee820's 2-entry loop over
// world+0x49D0 + i*0x880 (flag +0x38, model +0x6B8), EAX = element, plain RET.
volatile uint32_t g_asv_path, g_asv_val, g_asv_fn;
__declspec(naked) void coop_thunk_as_setvar()
{
    __asm {
        pushad
        push dword ptr [g_asv_val]
        push dword ptr [g_asv_path]
        call dword ptr [g_asv_fn]           // FUN_00101790 (cdecl)
        add esp, 8
        popad
        ret
    }
}
static void coop_as_set(const char *path, const char *value)
{
    if (!g_asv_fn) g_asv_fn = xbe_va(0x00101790);
    g_asv_path = (uint32_t)(uintptr_t)path; g_asv_val = (uint32_t)(uintptr_t)value;
    coop_thunk_as_setvar();
}
// hide157: hiding _level1/_level2 did NOT hide the HUD and gating FUN_00177000 did NOT
// hide the gun. skip158 (--skip-caller, one candidate per stage, frames inspected):
//   caller 0x1EE981 -> the GUN + ARMS vanish (HUD, world intact): the call at 0x1EE97C =
//     FUN_00176f60 (bucket flush, plain RET, ESI = *0x2D67F4 + 0xCAE4, flag byte 0x2F3C44
//     set around it) = the first-person viewmodel bucket.
//   caller 0x000DA507 -> the HUD vanishes (gun intact): FUN_000da4b0 (HUD movie draw).
//     In gameplay the frame driver is FUN_0020ba30: CALL FUN_00129340 at 0x20BAA5 (the
//     0x20BC21 site in FUN_0020bbc0 is the other driver - why hide156 changed nothing).
// ---- GAME-FONT TEXT probe: the HUD message box element (FUN_0013b9f0 builds each UI
// layer i at ui + i*0xA8, ui = *0x2D12E4; FUN_001340c0 initialises *(layer + 0x20) =
// the HUD_messagebox element: +0x80 text ptr -> +0xB4 wide buffer, +0x78 timer (> 0 =
// shown, FUN_00130bd0 counts it down, fade over +0x7C), +0x60/+0x68 position).
static uint32_t coop_msgbox(void)
{
    uint32_t ui = *(volatile uint32_t *)(uintptr_t)0x2D12E4;
    if (ui < 0x80000000 || ui >= 0x84000000) return 0;
    // msg164: *(ui + 0x20) shares the class but its +0x2A0.. hint fields read 0xCCCC (not
    // that object). The HINT box = *(ui + 0x18): FUN_0013b9f0 inits it with FUN_0012a230
    // (which writes +0x3B0 = 0, the hint state).
    uint32_t e = *(volatile uint32_t *)(uintptr_t)(ui + 0x18);
    if (e < 0x80000000 || e >= 0x84000000) return 0;
    if (*(volatile uint16_t *)(uintptr_t)(e + 0x2A4) > 4) return 0;   // sanity: queue count 0..4
    return e;
}
static void coop_msgbox_text(const char *text, float seconds)
{
    uint32_t e = coop_msgbox();
    if (!e) return;
    static int dumped;
    if (!dumped++) {
        char buf[1200]; int o = 0;
        for (int k = 0; k < 0x100 && o < 1150; k += 4)
            o += snprintf(buf + o, sizeof buf - o, "%s%08x", (k % 32) ? " " : "\n   ", *(volatile uint32_t *)(uintptr_t)(e + k));
        logf("coop: msgbox element %08x (ui %08x):%s", e, *(volatile uint32_t *)(uintptr_t)0x2D12E4, buf);
    }
    volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)(e + 0xB4);
    int i = 0;
    for (; text[i] && i < 120; i++) w[i] = (uint16_t)(unsigned char)text[i];
    w[i] = 0;
    *(volatile uint32_t *)(uintptr_t)(e + 0x80) = e + 0xB4;
    *(volatile float *)(uintptr_t)(e + 0x78) = seconds;
    if (*(volatile float *)(uintptr_t)(e + 0x7C) < seconds) *(volatile float *)(uintptr_t)(e + 0x7C) = seconds;
}

// ---- CO-OP MESSAGES IN THE GAME'S OWN HINT BOX (game font). msg163: writing the box
// buffer alone shows nothing - the box runs its own state machine (FUN_00129d80, vtable
// slot 7 of the message box class 0x290F34): queue ring +0x250 (4 ids), index +0x2A0,
// count +0x2A4; state +0x3B0 0 = build text (loc "HINT_INGAME_%d" -> obj+0x140) ->
// 1 fade in -> 2 hold -> 3 dequeue. The text is post-processed by FUN_00113bf0(obj+0x140)
// at 0x129EB2 (push EDI; call) - that CALL is re-pointed to coop_thunk_hinttext, which
// writes our wide text into the buffer when a co-op message is wanted, then runs the
// original. coop_hint_tick re-queues a hint (id 1) whenever the box's queue is empty.
static const char *volatile g_hint_text;   // the wanted co-op message (null = none)
volatile uint32_t g_hint_buf, g_orig_hinttext;
extern "C" void coop_on_hinttext(void)
{
    const char *t = g_hint_text;
    if (!t || g_hint_buf < 0x10000) return;
    volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)g_hint_buf;
    int i = 0;
    for (; t[i] && i < 0x70; i++) w[i] = (uint16_t)(unsigned char)t[i];
    w[i] = 0;
    static int ln;
    if (ln++ < 6) logf("coop: hint box text -> \"%s\"", t);
}
__declspec(naked) void coop_thunk_hinttext()
{
    __asm {
        pushad
        mov eax, dword ptr [esp + 0x24]      // our caller's arg = the text buffer
        mov dword ptr [g_hint_buf], eax
        call coop_on_hinttext
        popad
        jmp dword ptr [g_orig_hinttext]       // FUN_00113bf0 (cdecl, caller cleans)
    }
}
static void coop_hint_tick(const char *want)
{
    g_hint_text = want;
    uint32_t e = coop_msgbox();
    if (!want || !e) return;
    volatile uint16_t *count = (volatile uint16_t *)(uintptr_t)(e + 0x2A4);
    volatile uint16_t *idx = (volatile uint16_t *)(uintptr_t)(e + 0x2A0);
    static uint64_t dl;
    if (GetTickCount64() - dl > 1000) {
        dl = GetTickCount64();
        logf("coop: hint box %08x: count %u idx %u ids %d %d %d %d active %u state %d t %.2f vt7 %08x",
             e, *count, *idx, *(volatile int32_t *)(uintptr_t)(e + 0x250), *(volatile int32_t *)(uintptr_t)(e + 0x254),
             *(volatile int32_t *)(uintptr_t)(e + 0x258), *(volatile int32_t *)(uintptr_t)(e + 0x25C),
             *(volatile uint8_t *)(uintptr_t)(e + 0x3A9), *(volatile int32_t *)(uintptr_t)(e + 0x3B0),
             *(volatile float *)(uintptr_t)(e + 0x3AC), *(volatile uint32_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)e + 0x1C));
    }
    if (*(volatile int32_t *)(uintptr_t)(e + 0x3B0) == 2) {   // dump the two text nodes once while held
        static int nd;
        if (!nd++) {
            char buf[2400]; int o = 0;
            for (int k = 0; k < 0x140 && o < 2300; k += 4)
                o += snprintf(buf + o, sizeof buf - o, "%s%g", (k % 32) ? " " : "\n   ", *(volatile float *)(uintptr_t)(e + k));
            logf("coop: hint box nodes (floats, +0x00..+0x13C):%s", buf);
        }
    }
    if (*count != 0) return;                                  // a hint is queued / showing
    uint32_t slot = (*idx + *count) & 3;
    *(volatile int32_t *)(uintptr_t)(e + 0x250 + 4 * slot) = 1;   // any valid hint id: its text is replaced
    *count = (uint16_t)(*count + 1);
    static int ln;
    if (ln++ < 6) logf("coop: hint box: queued a co-op message (\"%s\")", want);
}

static void coop_update_gates(void)
{
    {   // co-op messages through the game's hint box (game font)
        const char *want = nullptr;
        if (g_coop_downed) want = "You are down - waiting for your partner to revive you";
        else if (g_coop_peer_downed && g_coop_host_ms && GetTickCount64() - g_coop_host_ms <= 400)
            want = "Your partner is down - press X to revive";
        if (g_cfg.msg_test && (GetTickCount64() - g_coop_t0) >= 72000) want = "Your partner is down - press X to revive";
        // msg165-168: the hint box runs its states but builds no text geometry (+0x84 stays
        // 0, nothing visible) -> only the --msg-test probe drives it until that is solved;
        // the downed banner stays on the ColorFill fallback.
        if (!g_cfg.msg_test) want = nullptr;
        coop_hint_tick(want);
    }
    uint32_t hide = g_coop_downed;
    if (g_cfg.hide_test) hide = (GetTickCount64() - g_coop_t0) >= 70000;
    static uint32_t last;
    if (hide != last) { last = hide; logf("coop: downed view: HUD + viewmodel %s", hide ? "HIDDEN" : "shown"); }
    g_gate_hud = hide;
    g_gate_vm = hide;
}
// chain: the call may already go to another port patch (fov.cpp's first-person thunk takes 0x1EE97C at start-up);
// then that patch becomes our "original". Two-PC play 20261115 #3 ("the host was downed and could see his weapon in
// spectator mode"): a LOBBY session activates co-op after fov.cpp, the viewmodel gate was skipped ("mismatch") and
// the downed view never hid the gun. Command-line test runs install co-op first, so they never showed it.
static void repoint_call(uint32_t site_va, uint32_t target_va, void *stub, volatile uint32_t *orig, const char *name, bool chain = false)
{
    uint32_t a = xbe_va(site_va);
    auto *code = (uint8_t *)(uintptr_t)a;
    int32_t rel; memcpy(&rel, code + 1, 4);
    uint32_t cur = a + 5 + rel;
    if (code[0] == 0xE8 && cur == (uint32_t)(uintptr_t)stub) { logf("coop: gate %s: call %08x already gated", name, site_va); return; }
    if (code[0] != 0xE8 || (cur != xbe_va(target_va) && !chain)) { logf("coop: gate %s: call %08x mismatch; skipped", name, site_va); return; }
    if (cur != xbe_va(target_va)) logf("coop: gate %s: call %08x already goes to a port patch (%08x) - chained after the gate", name, site_va, cur);
    *orig = cur;
    int32_t nrel = (int32_t)((uintptr_t)stub - (a + 5));
    memcpy(code + 1, &nrel, 4);
    logf("coop: gate %s: call %08x -> %08x gated", name, site_va, target_va);
}
static void install_render_gates(void)
{
    // three callers (only 0x20B7CC shows in the decompile; the XBE scan found 0x20C972 and 0x20CE5B - the mission path)
    repoint_call(0x0020B7CC, 0x001EDC90, (void *)coop_load_gate, &g_orig_load, "level load (co-op loading barrier)");
    repoint_call(0x0020C972, 0x001EDC90, (void *)coop_load_gate, &g_orig_load, "level load 2 (co-op loading barrier)");
    repoint_call(0x0020CE5B, 0x001EDC90, (void *)coop_load_gate, &g_orig_load, "level load 3 (co-op loading barrier)");
    repoint_call(0x001EE97C, 0x00176F60, (void *)coop_gate_vm, &g_orig_vm, "viewmodel", true);
    repoint_call(0x001EEB2B, 0x00176F60, (void *)coop_gate_vm2, &g_orig_vm2, "viewmodel 2", true);
    repoint_call(0x00129EB2, 0x00113BF0, (void *)coop_thunk_hinttext, &g_orig_hinttext, "hint box text (co-op messages)");
    // dbg161 (10-deep callers): the gameplay HUD = FUN_0020bbc0 (0x20BC04) -> UI layers
    // FUN_0013c050(0) -> FUN_0013bc30 -> CALL FUN_000da4b0 at 0x13C02A (RET 4). Only that
    // draw call is gated - skipping the whole layer pass broke the UI logic (hide156).
    repoint_call(0x0013C02A, 0x000DA4B0, (void *)coop_gate_hudmov, &g_orig_hudmov, "HUD movie (UI layer draw)");
    repoint_call(0x0020BA8B, 0x000DA4B0, (void *)coop_gate_hudmov, &g_orig_hudmov, "HUD movie (gameplay driver)");
    repoint_call(0x0020BAA5, 0x00129340, (void *)coop_gate_hud, &g_orig_hud, "HUD layouts (gameplay driver)");
    repoint_call(0x0020BC21, 0x00129340, (void *)coop_gate_hud, &g_orig_hud, "HUD (other driver)");
}

static void install_downed_hook(void)
{
    uint32_t site = xbe_va(0x001ED990);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    if (memcmp(code, kExpect, 6) != 0) { logf("coop: 001ed990 bytes mismatch; downed off"); return; }
    g_pdh_resume = (volatile void *)(site + 6);
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_player_guard - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    logf("coop: downed hook installed at FUN_001ed990 (killing blows clamp to hp 1)");
}

static void install_pellet_hook(void)
{
    uint32_t a = xbe_va(0x001DC008);
    auto *code = (uint8_t *)(uintptr_t)a;
    int32_t rel; memcpy(&rel, code + 1, 4);
    g_pel_fn = xbe_va(0x001CEF40);
    if (code[0] != 0xE8 || a + 5 + rel != g_pel_fn) { logf("coop: pellet call 001dc008 mismatch; no hit sync"); return; }
    int32_t nrel = (int32_t)((uintptr_t)coop_thunk_pellet - (a + 5));
    memcpy(code + 1, &nrel, 4);
    logf("coop: hit sync: pellet call 001dc008 re-pointed (FUN_001cef40 wrapper)");
}

static void install_skin_hook(void)
{
    uint32_t site = xbe_va(0x0016F420);
    auto *code = (uint8_t *)(uintptr_t)site;
    static const uint8_t kExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    if (memcmp(code, kExpect, sizeof kExpect) != 0) {
        logf("coop: 0016f420 bytes mismatch; no skin hook");
        return;
    }
    g_skin_resume = (volatile void *)(uintptr_t)(site + 6);
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_skin - (site + 5));
    memcpy(code + 1, &rel, 4);
    logf("coop: skin hook installed at FUN_0016f420 (resume +6)");
}

// Pilot 12: entry hook on FUN_001ce1f0 ("55 8B EC 83 E4", resume 001ce1f5 = the
// AND ESP,-16). On entry: EAX = the body, [esp] = the caller return address,
// [esp+4] = the world arg. Capture all three, log, then run the displaced
// prologue and resume the original body.
__declspec(naked) void coop_thunk_regcap()
{
    __asm {
        mov dword ptr [g_regcap_body], eax
        mov eax, [esp]
        mov dword ptr [g_regcap_ret], eax
        mov eax, [esp+4]
        mov dword ptr [g_regcap_world], eax
        mov eax, dword ptr [g_regcap_body]
        pushfd
        pushad
        push ecx
        call coop_log_registration
        add esp, 4
        popad
        popfd
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF0h
        jmp dword ptr [g_regcap_resume]
    }
}

__declspec(naked) void spawn_thunk()
{
    __asm {
        movups dword ptr [g_spawn_xmm], xmm0
        mov dword ptr [g_spawn_edi], edi
        lea eax, [esp + 4]
        mov dword ptr [g_spawn_args], eax
        pushad
        push dword ptr [g_spawn_args]
        push offset g_spawn_xmm
        push dword ptr [g_spawn_edi]
        call coop_log_spawn
        add esp, 12
        popad
        movups xmm0, dword ptr [g_spawn_xmm]
        mov eax, dword ptr [edi + 0x79b4]
        mov ecx, dword ptr [edi + 0x79b8]
        jmp dword ptr [g_spawn_resume]
    }
}

__declspec(naked) void interp_thunk()
{
    __asm {
        pushad
        lea edx, [esp + 36]                 // the interpreter call's arg1 (the stream ptr)
        push edx
        push edi                            // the caller's record context (FUN_001eb440 set it)
        call coop_journal_stream
        add esp, 8
        popad
        push ebp                            // original prologue (13 bytes, re-run inline)
        mov ebp, esp
        and esp, 0FFFFFFF0h
        sub esp, 1F8h
        jmp dword ptr [g_interp_out]
    }
}

// FORMATION GUARD (level survey 20261114: missions 4 / 5 crashed ~1 s in on BOTH copies, AV 0x1A0086). FUN_001A01A0
// (an AI action, EAX = the action, +0x140 = its brain) follows the brain's formation when the body's team manager
// (*0x2D1988 + 0x22C40 + team * 4) is active (+0x20 - in missions 4 / 5 team 0 follows the player) and the brain has a
// formation (+0x1EF0): FUN_001A0050(brain + 0x1EC0) reads [[brain + 0x1ED0] + 0xC4]. The formation (FUN_0018FC10) left
// OUR hand-built brains (the clone, the self model) - and once a natural squad mate behind them - without the body to
// follow (+0x1ED0 = 0). The guard gives such a brain its own body to follow (brain +0x8C) before the action runs; the
// game's code then runs unchanged. Every other brain has its link and passes untouched.
static volatile uint32_t g_fg_out;
extern "C" void __cdecl coop_formation_guard(uint32_t action)
{
    if (action < 0x80000000 || action >= 0x84000000) return;
    uint32_t brain = *(volatile uint32_t *)(uintptr_t)(action + 0x140);
    if (brain < 0x80000000 || brain >= 0x84000000) return;
    if (*(volatile uint32_t *)(uintptr_t)(brain + 0x1EF0) == 0 || *(volatile uint32_t *)(uintptr_t)(brain + 0x1ED0) != 0) return;
    uint32_t body = *(volatile uint32_t *)(uintptr_t)(brain + 0x8C);
    if (body < 0x80000000 || body >= 0x84000000) return;
    *(volatile uint32_t *)(uintptr_t)(brain + 0x1ED0) = body;
    static uint32_t n;
    if (n++ < 20) logf("coop: formation guard: brain %08x (body %08x) had a formation %08x with nothing to follow - follows itself",
                       brain, body, *(volatile uint32_t *)(uintptr_t)(brain + 0x1EF0));
}
__declspec(naked) void coop_thunk_formation()
{
    __asm {
        pushad
        push eax
        call coop_formation_guard
        add esp, 4
        popad
        push ebp                            // the displaced prologue (9 bytes), re-run
        mov ebp, esp
        and esp, 0FFFFFFF0h
        sub esp, 14h
        jmp dword ptr [g_fg_out]
    }
}
// OUR BODIES DO NOT THINK (owner 20261114: "don't share the clone's brain - give the clone its own class and functions,
// independent, borrowing nothing from other entities"). The clone and the self model are built with brains from the
// game's AI pool (own spawn), so the game's AI ran them like soldiers: squad formations (missions 4 / 5 crash), AI
// actions on a brain with no orders (0x19D493). FUN_001E5EF0 (per-brain update, EAX = brain) keeps its movement /
// animation / weapon parts; its THINK step FUN_00195A00 (action dispatch: FUN_0018D840, FUN_00195980, the current
// action's vt+8) is skipped for our own brains - their pose, aim and fire come from the partner (our code). The
// prologue 51 56 8B F0 + E8 (call FUN_0018D840, relative) is re-run with the call made absolute. Env COOP_CLONE_THINK=1
// = they think again (the old behaviour).
static volatile uint32_t g_think_out, g_think_call, g_think_skip;
extern "C" uint32_t __cdecl coop_brain_is_own(uint32_t brain, uint32_t edi)
{
    {   // which object arrives here (first calls): EAX = the action owner, EDI = the brain of FUN_001E5EF0
        static uint32_t dl;
        if (dl++ < 6) logf("coop: think step: eax %08x edi %08x (our brains %08x %08x)", brain, edi, g_own_brains[0], g_own_brains[1]);
    }
    static int think = -1;
    if (think < 0) { char e[8] = {0}; think = GetEnvironmentVariableA("COOP_CLONE_THINK", e, sizeof e) && e[0] == '1'; }
    if (think) return 0;
    if (g_cfg.coop_role == 2 && edi >= 0x80000000 && edi < 0x84000000) {
        // realcp2 (joiner AV 0x19D493 in the think step of a dying puppet, after a checkpoint move made the host despawn
        // the joiner's enemies): a puppet's real class link is put back for its kill and the brain - which never thought
        // while privatized - ran its AI. On the joiner every brain that drove a puppet this level is host-driven: it
        // never runs the think step (its movement / animation / death still update).
        for (int i = 0; i < 128; i++)
            if (g_pl_all[i].orig == edi) {
                static uint32_t pn;
                if (pn++ < 6) logf("coop: brain %08x drove a PUPPET here - its AI think step is skipped (the host drives it)", edi);
                return 1;
            }
    }
    for (int i = 0; i < g_own_brains_n; i++)
        if ((brain - g_own_brains[i]) < 0x1FE0 || (edi - g_own_brains[i]) < 0x1FE0) {   // inside one of our brain slots
            static uint32_t n;
            if (n++ < 4) logf("coop: brain %08x is one of OURS - its AI think step is skipped (the partner drives it)", brain);
            return 1;
        }
    return 0;
}
__declspec(naked) void coop_thunk_think()
{
    __asm {
        pushad
        push edi
        push eax
        call coop_brain_is_own
        mov dword ptr [g_think_skip], eax
        add esp, 8
        popad
        cmp dword ptr [g_think_skip], 0
        jne skip
        push ecx                            // the displaced prologue, re-run
        push esi
        mov esi, eax
        call dword ptr [g_think_call]       // was: call FUN_0018D840 (relative)
        jmp dword ptr [g_think_out]
    skip:
        ret
    }
}
static void install_think_skip()
{
    static bool done;
    if (done) return;
    const uint8_t expect[9] = {0x51, 0x56, 0x8B, 0xF0, 0xE8, 0x37, 0x7E, 0xFF, 0xFF};
    uint32_t site = xbe_va(0x00195A00);
    auto *code = (uint8_t *)(uintptr_t)site;
    if (memcmp(code, expect, sizeof expect) != 0) { logf("coop: think skip NOT installed (bytes differ at FUN_00195A00)"); return; }
    done = true;
    g_think_call = xbe_va(0x0018D840);
    g_think_out = site + sizeof expect;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_think - (site + 5));
    code[0] = 0xE9; memcpy(code + 1, &rel, 4);
    for (int i = 5; i < 9; i++) code[i] = 0x90;
    logf("coop: think skip installed at FUN_00195A00 (our own bodies' brains do not think)");
}
// REAL CHECKPOINTS (playtest 20261114 #7, owner: "the teleport rule is applied to some part-streamer event, not to the
// real checkpoint"). The game's "checkpoint reached" popup is the localized string C9ABBCB2 (MainUS.bin entry 488, the
// id comes from the level data). Every localized text goes through FUN_001F3A80 (stdcall, arg = the string id; ECX =
// the loc manager) - a lookup of C9ABBCB2 while playing = the game shows that popup on THIS copy = our player reached a
// real checkpoint. Hook: the 5-byte entry A0 0E 3B 2F 00 (mov al, [0x2F3B0E]) re-run.
static volatile uint32_t g_loc_out;
// (g_cp_popup_ms: declared with the forward declarations)
extern "C" void __cdecl coop_loc_lookup(uint32_t id)
{
    if (!g_cfg.coop_role) return;
    uint64_t now = GetTickCount64();
    if (id == 0xC9ABBCB2) {
        static uint64_t last;
        if (now - last > 3000) {   // one popup = a few lookups
            logf("coop: checkpoint: the game shows CHECKPOINT REACHED here (string C9ABBCB2)");
            g_cp_popup_ms = now;
        }
        last = now;
        return;
    }
    static uint32_t seen[64]; static int nseen;   // probe: other ids looked up in play (first 64 distinct)
    if (nseen >= 64) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == id) return;
    seen[nseen++] = id;
    logf("coop: loc lookup: string %08x", id);
}
__declspec(naked) void coop_thunk_loc()
{
    __asm {
        pushad
        mov eax, dword ptr [esp + 0x24]     // the string id (stdcall arg 1, above the return address)
        push eax
        call coop_loc_lookup
        add esp, 4
        popad
        mov al, byte ptr ds:[0x002F3B0E]    // the displaced entry, re-run
        jmp dword ptr [g_loc_out]
    }
}
static void install_loc_probe()
{
    static bool done;
    if (done) return;
    const uint8_t expect[5] = {0xA0, 0x0E, 0x3B, 0x2F, 0x00};
    uint32_t site = xbe_va(0x001F3A80);
    auto *code = (uint8_t *)(uintptr_t)site;
    if (memcmp(code, expect, sizeof expect) != 0) { logf("coop: checkpoint probe NOT installed (bytes differ at FUN_001F3A80)"); return; }
    done = true;
    g_loc_out = site + sizeof expect;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_loc - (site + 5));
    code[0] = 0xE9; memcpy(code + 1, &rel, 4);
    logf("coop: checkpoint probe installed at FUN_001F3A80 (string lookups)");
}
// CO-OP SPRINT (owner 20261115). The player's movement = FUN_001CC490(forward, strafe) (stdcall, RET 8; ESI = the player
// controller), called once per player update at 0x1ECFBC: speeds 6.0 forward / strafe, 5.5 back, capped at 4.5 (crouch
// 3.0 / 2.0, aiming 3.25); it leaves the world velocity at controller +0x60..+0x6C and the speed at +0x20 (the walk
// bob runs on it). The call goes through coop_thunk_move: the game's function, then - while our player sprints - the
// velocity and the speed times COOP_SPRINT_MUL (1.3; was 1.6). The accumulated local velocity (+0xF0..) is untouched, so nothing
// compounds and the stop is immediate.
volatile int g_coop_sprint_allowed;
volatile int g_coop_sprinting;
volatile float g_coop_sprint_blend;
static volatile uint32_t g_move_fn;
extern "C" void __cdecl coop_sprint_scale(uint32_t ctrl)
{
    {   // COOP_FIRE_PROBE=1 (test, Glock report 20261115): our weapon per player update while the trigger is held
        static int probe = -1;
        if (probe < 0) { char e[8] = {0}; probe = GetEnvironmentVariableA("COOP_FIRE_PROBE", e, sizeof e) && e[0] == '1'; }
        if (probe && ctrl >= 0x80000000 && ctrl < 0x84000000) {
            uint32_t body = *(volatile uint32_t *)(uintptr_t)(ctrl + 0x8C);
            uint32_t W = (body >= 0x80000000 && body < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(body + 0x278 + 0x28) : 0;
            if (W >= 0x80000000 && W < 0x84000000) {
                int st = *(volatile int *)(uintptr_t)(W + 0xD8), trig = *(volatile uint8_t *)(uintptr_t)(W + 0x105), clip = -1;
                uint32_t cp = *(volatile uint32_t *)(uintptr_t)(W + 0xF4);
                if (cp >= 0x80000000 && cp < 0x84000000) clip = *(volatile int16_t *)(uintptr_t)(cp + 0x18);
                static int lst = -9, lclip = -9, ltrig = -9;
                if (st == 1 || st != lst || clip != lclip)
                    logf("coop: fire probe: frame %u trig %d state %d clip %d | W+DC %.3f W+E0 %.3f W+E4 %.3f W+E8 %.3f inv+DC %.3f W+D8..: %08x %08x %08x %08x", (uint32_t)g_frame_count.load(), trig, st, clip,
                         *(volatile float *)(uintptr_t)(W + 0xDC), *(volatile float *)(uintptr_t)(W + 0xE0), *(volatile float *)(uintptr_t)(W + 0xE4), *(volatile float *)(uintptr_t)(W + 0xE8),
                         *(volatile float *)(uintptr_t)(body + 0x278 + 0xDC), *(volatile uint32_t *)(uintptr_t)(W + 0xD8), *(volatile uint32_t *)(uintptr_t)(W + 0xDC), *(volatile uint32_t *)(uintptr_t)(W + 0xFC), *(volatile uint32_t *)(uintptr_t)(W + 0x100));
                lst = st; lclip = clip; ltrig = trig;
            }
        }
    }
    if (!g_coop_sprinting || ctrl != g_coop_last_ctrl || ctrl < 0x80000000 || ctrl >= 0x84000000) return;
    static float mul = -1.0f;
    if (mul < 0.0f) { char e[16] = {0}; mul = GetEnvironmentVariableA("COOP_SPRINT_MUL", e, sizeof e) ? (float)atof(e) : 1.3f; if (!(mul > 0.5f && mul < 4.0f)) mul = 1.3f; }   // owner 20261115: 1.6 -> 1.3
    volatile float *v = (volatile float *)(uintptr_t)(ctrl + 0x60);
    for (int i = 0; i < 4; i++) v[i] *= mul;
    *(volatile float *)(uintptr_t)(ctrl + 0x20) *= mul;
}
__declspec(naked) void coop_thunk_move()
{
    __asm {
        push dword ptr [esp + 8]            // strafe (the caller's 2nd arg)
        push dword ptr [esp + 8]            // forward (the caller's 1st arg, now 8 above)
        call dword ptr [g_move_fn]          // FUN_001CC490 (RET 8: takes our two pushes)
        pushad
        push esi                            // the player controller
        call coop_sprint_scale
        add esp, 4
        popad
        ret 8                               // the caller's two args
    }
}
static void install_sprint()
{
    static bool done;
    if (done) return;
    {   // COOP_SPRINT=1 (tests without the lobby): sprint allowed
        char e[8] = {0};
        if (GetEnvironmentVariableA("COOP_SPRINT", e, sizeof e) && e[0] == '1') g_coop_sprint_allowed = 1;
    }
    uint32_t site = xbe_va(0x001ECFBC);
    auto *code = (uint8_t *)(uintptr_t)site;
    int32_t rel0; memcpy(&rel0, code + 1, 4);
    if (code[0] != 0xE8 || site + 5 + rel0 != xbe_va(0x001CC490)) { logf("coop: sprint NOT installed (the movement call at 0x1ECFBC differs)"); return; }
    done = true;
    g_move_fn = xbe_va(0x001CC490);
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_move - (site + 5));
    memcpy(code + 1, &rel, 4);
    logf("coop: sprint installed (the movement call at 0x1ECFBC; allowed %d)", g_coop_sprint_allowed);
}
static void install_formation_guard()
{
    static bool done;
    if (done) return;
    const uint8_t expect[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x14};
    uint32_t site = xbe_va(0x001A01A0);
    auto *code = (uint8_t *)(uintptr_t)site;
    if (memcmp(code, expect, sizeof expect) != 0) { logf("coop: formation guard NOT installed (bytes differ at FUN_001A01A0)"); return; }
    done = true;
    g_fg_out = site + sizeof expect;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk_formation - (site + 5));
    code[0] = 0xE9; memcpy(code + 1, &rel, 4);
    for (int i = 5; i < 9; i++) code[i] = 0x90;
    logf("coop: formation guard installed at FUN_001A01A0");
}

__declspec(naked) void level_thunk()
{
    __asm {
        mov eax, [esp + 4]
        pushad
        push eax
        call coop_log_level_arg
        call coop_level_prepare
        add esp, 4
        popad
        push ebp
        mov ebp, [esp + 8]
        jmp dword ptr [g_level_out]
    }
}

__declspec(naked) void level_loop_thunk()
{
    __asm {
        mov eax, [ebp + 0x34]
        xor edi, edi
        pushad
        push ebp
        call coop_level_tail_read
        add esp, 4
        popad
        jmp dword ptr [g_loop_out]
    }
}

// Stage 2 pilot helper thunks: replicate the loader loop's own register state at the
// moment it queues and registers each unit (loop body disasm 1EB4D0-1EB542):
//   queue:    esi = unit, ebx = *0x2D45D4 (world), edx = [unit] (vtable), ecx = -1,
//             eax = 8, then push 0 / push unit / push q (q = *0x2D45F4) and call
//             FUN_000e0820.
//   register: same esi/ebx/edx kept, eax = unit, push ebx (world), call FUN_001ce1f0.
__declspec(naked) void stage2_queue_loop()
{
    __asm {
        pushad
        mov esi, dword ptr [g_s2_a1]        // unit
        mov ebx, dword ptr [g_s2_a3]        // world value
        mov edx, dword ptr [g_s2_a4]        // vtable = [unit]
        mov ecx, -1
        mov eax, 8
        push 0
        push esi
        mov eax, dword ptr [g_s2_a2]        // q
        push eax
        call dword ptr [g_s2_fn]            // (q, unit, 0), stdcall; frees its args
        popad
        retn
    }
}

__declspec(naked) void stage2_wreg()
{
    __asm {
        pushad
        mov esi, dword ptr [g_s2_a1]        // unit
        mov ebx, dword ptr [g_s2_a3]        // world value
        mov edx, dword ptr [g_s2_a4]        // vtable
        mov eax, esi
        push ebx
        call dword ptr [g_s2_fn]            // stdcall (world); frees its arg
        popad
        retn
    }
}

// Pilot 11: FUN_001ce1f0 world-register (the spawn driver's tail call at 0x1df42f:
// MOV EAX,EBP; PUSH world; CALL 001ce1f0; the callee is stdcall RET 0x4 — verified
// in the disasm tail 001ce25f). Pushes the body onto the world tick list
// ([world+0x5CB4] head; body+0xB0 next; body+0x54 self; body+0x34 list insert).
__declspec(naked) void coop_thunk_wreg()
{
    __asm {
        pushad
        mov eax, dword ptr [g_wreg_body]
        mov ecx, dword ptr [g_wreg_world]
        push ecx
        mov eax, dword ptr [g_wreg_body]
        call dword ptr [g_wreg_fn]          // stdcall RET 0x4: cleans the pushed arg
        popad
        retn
    }
}

void udp_init()
{
    if (g_cfg.coop_send.empty()) return;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return;
    size_t colon = g_cfg.coop_send.rfind(L':');
    if (colon == std::wstring::npos) return;
    std::string host = narrow(g_cfg.coop_send.substr(0, colon));
    int port = _wtoi(g_cfg.coop_send.c_str() + colon + 1);
    g_coop_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    g_coop_t0 = GetTickCount64();   // the --downed-at epoch (process-ish start)
    if (g_coop_sock == INVALID_SOCKET) return;
    g_coop_target.sin_family = AF_INET;
    g_coop_target.sin_port = htons((uint16_t)port);
    InetPtonA(AF_INET, host.c_str(), &g_coop_target.sin_addr);
    if (g_coop_target.sin_addr.s_addr == 0) { g_coop_sock = INVALID_SOCKET; return; }
    logf("coop: telemetry -> udp:%s:%d", host.c_str(), port);
}

void marker_init()
{
    if (g_cfg.coop_listen_port <= 0 || g_cfg.coop_listen_port > 65535) return;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return;
    g_coop_listens = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_coop_listens == INVALID_SOCKET) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_cfg.coop_listen_port);
    if (bind(g_coop_listens, (const sockaddr *)&a, sizeof a) != 0) {
        logf("coop: cannot bind udp %d; marker off", g_cfg.coop_listen_port);
        g_coop_listens = INVALID_SOCKET;
        return;
    }
    u_long nb = 1;
    ioctlsocket(g_coop_listens, FIONBIO, &nb);
    g_coop_hTan = tanf(g_cfg.fov * 3.14159265f / 360.0f);
    g_coop_vTan = g_coop_hTan * 0.75f;
    logf("coop: marker listening on udp:%d (hTan %.4f vTan %.4f)", g_cfg.coop_listen_port, g_coop_hTan, g_coop_vTan);
}

// The pawn-capture install (split out for the hook-matrix bisect: the pure-read
// pawn hook can be enabled without its redirect).
static void install_pawncap()
{
    constexpr uint32_t kPawnSite = 0x001DF449;
    uint32_t psite = xbe_va(kPawnSite);
    auto *pcode = (uint8_t *)(uintptr_t)psite;
    if (pcode[0] == 0x57 && (pcode[1] == 0x89 || pcode[1] == 0x8B) &&
        pcode[3] == 0x33 && pcode[4] == 0xC0) {
        g_pawncap_resume = (volatile void *)(psite + 5);
        pcode[0] = 0xE9;
        int32_t prel = (int32_t)((uintptr_t)coop_thunk_pawncap - (psite + 5));
        memcpy(pcode + 1, &prel, 4);
        logf("coop: pawn capture installed at FUN_001df440 (site +9)");
    } else {
        logf("coop: 001df449 bytes %02x %02x %02x %02x %02x mismatch; no pawn capture",
             pcode[0], pcode[1], pcode[2], pcode[3], pcode[4]);
    }
}

// The hooks the co-op switches turn on (--coop-clone, --skin-hook, --coop-downed ...): at the start with the switches,
// or once from coop_activate when a CO-OP lobby starts a mission (fe/coop_menu.cpp). Never twice.
static void install_coop_features()
{
    static bool done;
    if (done) return;
    done = g_cfg.coop_clone;   // ms1: a start WITHOUT the co-op switches installs nothing here - the lobby's call must still run
    // Pilot 12b: the pawn-def capture gate (the LAST new hook relative to the
    // gun-having run 21; bisected with the register hook OFF).
    if (g_cfg.skin_hook) install_skin_hook();   // --skin-hook: arm-aim skeleton work
    if (g_cfg.coop_clone) install_pellet_hook();   // hit sync: per-pellet ray hook
    if (g_cfg.coop_clone) install_damage_hook();   // clone shield + relay: bullets on the clone are skipped (and relayed on the host)
    if (g_cfg.coop_clone) { install_weapon_guard(); install_null_guard(); install_body_guard(); install_puppet_brain(); install_lookup_guard(); install_trigger_sync(); install_spawn_gate(); install_retire_guard(); install_death_guard(); install_pick_guard(); coop_jp_install(); install_anim_sync(); install_projectile_sync(); install_ammo_guard(); install_agent_list_guard(); install_reg_once(); install_throw_trace(); install_objective_sync(); install_prop_sync(); install_piece_sync(); install_pickup_sync(); install_drop_sync(); if (g_cfg.coop_role) { install_no_pause(); install_restart_rule(); } install_clone_shield(); install_explosion_log(); }  // null-weapon guard; clone shield: explosions/stealth damage on the clone is skipped
    install_sight_trace();   // COOP_SIGHT_TRACE=1: every awareness record + its caller chain
    if (g_cfg.as_log) install_as_log();   // HUD clip discovery
    if (g_cfg.coop_downed || g_cfg.hide_test || g_cfg.msg_test) install_render_gates();   // hide HUD/viewmodel while downed
    install_exit_sync();   // the shared level exit (needs a co-op role)
    if (g_cfg.coop_downed) { install_downed_hook(); install_death_backstop(); }      // downed: killing blows clamp to hp 1 + spectator + X respawns
    // early bend (after the bone-hierarchy pass) = the default for --skin-bones 1;
    // --bend-at-draw keeps the old draw-time bend (helmet does not follow)
    if (g_cfg.skin_hook && g_cfg.skin_bones == 1 && !g_cfg.bend_at_draw) install_bonepass_hook();

}

static void install_loop_tail()
{
    static bool done;
    if (done) return;
    // The loop-tail injection hook: with --coop-clone the clone is prepared BEFORE the
    // loader body (coop_level_prepare from level_thunk), so the loop-tail thunk only
    // reads the level object for the clone's position overlay tracking.
    if (g_coop_clone) {
        done = true;
        uint32_t tsite = xbe_va(kLoopTailSite);
        auto *tcode = (uint8_t *)(uintptr_t)tsite;
        if (memcmp(tcode, kLoopTailExpect, sizeof kLoopTailExpect) == 0) {
            g_loop_out = tsite + sizeof kLoopTailExpect;
            tcode[0] = 0xE9;
            int32_t trel = (int32_t)((uintptr_t)level_loop_thunk - (tsite + 5));
            memcpy(tcode + 1, &trel, 4);
            logf("coop: loop-tail injection installed at FUN_001eb544");
        } else {
            logf("coop: loop tail 0x%08x bytes do not match this build; injection off", tsite);
        }
    }

}

// CO-OP lobby (owner, day 15, step 3): the lobby made the link - set what the play script's switches set
// (--coop-role, --coop-send / --coop-listen, --coop-clone --pawn-on --p13-on --skin-hook --skin-bones 1 --coop-log
// --role-label --coop-downed --no-markers) and install the hooks, in the menus before the mission loads. The host
// listens on 44001 and sends to the joiner's 44000, the joiner the other way (two copies on one PC work too).
// SHARED CINEMATIC SKIP (owner, day 15: "if there's a cinematic before a mission both players need to press a button to
// skip; a popup during the cutscene says that a player wants to skip"): "CLSK" = this player pressed skip (fe/coop_menu.cpp)
volatile uint32_t g_coop_peer_skip;
volatile uint64_t g_coop_peer_skip_ms;
// SHARED INTRO-TEXT SKIP (owner two-PC play 20261115: "the intro text at the beginning of the level should be skipped for
// all players when a player presses a button"): "CLIT" = this player skipped the intro text (fe/coop_menu.cpp
// coop_intro_gate); the partner's copy then presses START itself.
volatile uint32_t g_coop_peer_intro;
volatile uint64_t g_coop_peer_intro_ms;
void coop_send_intro_skip()
{
    if (g_coop_sock == INVALID_SOCKET) return;
    uint32_t m = 0x54494C43;   // "CLIT"
    for (int i = 0; i < 3; i++) sendto(g_coop_sock, (const char *)&m, 4, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
}
void coop_send_skip()
{
    if (g_coop_sock == INVALID_SOCKET) return;
    uint32_t m = 0x4B534C43;
    for (int i = 0; i < 3; i++) sendto(g_coop_sock, (const char *)&m, 4, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
}

// SESSION END (owner, day 15: "if a player quits the session, the co-op session ends for both players and they are
// brought back to the main menu with a message explaining the reason"). "CLQT" (x5) = this player left: the pause
// menu's QuitLevel (fe/coop_menu.cpp), LEAVE LOBBY / Back in a lobby after a mission, or the window closing
// (request_exit -> coop_on_exit). The partner: in a level it quits it through the game's own QuitLevel handler
// (0x124850, what the pause menu runs); a level with no partner state for 20 s ends the same way (connection lost).
// Then co-op goes off on this copy (a new lobby can start again) and the menus go to the title with the message
// (fe/coop_menu.cpp coop_menu_session_ended).
char g_coop_end_msg[96];
volatile unsigned long long g_coop_end_msg_until;
static volatile int g_quit_in;      // 1 = the partner's CLQT came
static bool g_session_over;
static void coop_send_quit()
{
    if (g_coop_sock == INVALID_SOCKET) return;
    uint32_t m = 0x54514C43;
    for (int i = 0; i < 5; i++) sendto(g_coop_sock, (const char *)&m, 4, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
}
bool coop_in_mission()
{
    uint32_t go = *(volatile uint32_t *)(uintptr_t)0x002D45F0;
    if (go < 0x80000000 || go >= 0x84000000) return false;
    return *(volatile uint32_t *)(uintptr_t)(go + 0x21070) == go + 0x20F78;
}
void coop_on_exit()
{
    if (g_cfg.coop_role && !g_session_over) { logf("coop: session: this window closes - telling the partner (CLQT)"); coop_send_quit(); }
}
static void coop_quit_recv()
{
    if (!g_cfg.coop_role || g_session_over || g_quit_in) return;
    g_quit_in = 1;
    logf("coop: session: the partner LEFT the co-op session (CLQT)");
}
void coop_session_end(int reason)
{
    if (g_session_over || !g_cfg.coop_role) return;
    g_session_over = true;
    if (coop_in_mission()) g_coop_teardown = 1;   // a level is being quit (pause QUIT, the partner left): no AI update until the next load
    if (reason == 0) coop_send_quit();
    const char *msg = reason == 0 ? "YOU LEFT THE CO-OP SESSION" : reason == 1 ? "YOUR PARTNER LEFT THE CO-OP SESSION" : "CONNECTION TO YOUR PARTNER LOST";
    strncpy(g_coop_end_msg, msg, sizeof g_coop_end_msg - 1);
    g_coop_end_msg_until = GetTickCount64() + 12000;
    logf("coop: session: ENDED (%s) - co-op off on this copy, back to the title", msg);
    // co-op off (a new lobby can activate again; the hooks stay, they test the role)
    g_cfg.coop_role = 0;
    g_coop_clone = 0; g_p13_on = 0; g_cfg.coop_clone = false;
    cheats_apply();   // 0.4.0 merge: the solo cheats come back
    g_coop_marker_on = 0;
    g_coop_exit_ui = 0; g_coop_load_ui = 0;
    if (g_coop_sock != INVALID_SOCKET) { closesocket(g_coop_sock); g_coop_sock = INVALID_SOCKET; }
    if (g_coop_listens != INVALID_SOCKET) { closesocket(g_coop_listens); g_coop_listens = INVALID_SOCKET; }
    g_coop_host_ms = 0;
    coop_menu_session_ended(reason);
}
// play tick: the partner left (or went silent for 20 s) while we are in a level -> quit it here
static void coop_quit_step()
{
    if (!g_cfg.coop_role || g_session_over) return;
    int reason = 0;
    if (g_quit_in) reason = 1;
    else if (g_coop_host_ms && GetTickCount64() - g_coop_host_ms > 20000) reason = 2;
    if (!reason) return;
    logf("coop: session: %s while in a level - quitting it (QuitLevel)", reason == 1 ? "the partner left" : "no partner state for 20 s");
    g_coop_teardown = 1;   // quit crashes 0x1A7F5E (#6, cpprobe): no AI update / weapon re-arm while the level is torn down
    typedef void(__cdecl * Handler)(const char *);
    ((Handler)(uintptr_t)xbe_va(0x00124850))("");
    coop_session_end(reason);
}

// SHARED LEVEL EXIT (owner, day 15: "if one player reaches a level exit a 20 s countdown starts; the level exits at
// zero or when both players are in the exit; if a player is downed while the other is at the exit, the level ends for
// both"). The level end = the mission mode object's vt+0x18 (FUN_001F45B0: mode +0xC |= 2; the mode loop then runs the
// end cutscene, results, autosave, mission select), called by the objective code when an objective with flag bit 0
// (the final "reach the exit" one) completes - inside the exit trigger's activation (Veblensk trigger #17 -> objective
// node 81A943E0). The vtable slot (0x295350) is re-pointed to coop_thunk_exit:
// - HOST: the call starts the 20 s countdown instead. The player who reached the exit: the joiner when the call comes
//   from the trigger sync's joiner block (g_exit_by_joiner), else the host; the exit point = that player's feet.
//   Ends (mode +0xC |= 2 here, "CLXE" x5 to the joiner) at zero, when the other player stands within 6 m of the exit
//   point, or when the other player is downed. "CLXT" {seconds left} every 250 ms for the joiner's screen.
// - JOINER: its own calls (the host's objective replayed) are swallowed; "CLXE" ends the level here (fallback: its
//   countdown ran out and no CLXE came within 3 s).
// - No co-op role: the original.
volatile int g_coop_exit_ui;
volatile uint32_t g_exit_by_joiner;
volatile uint32_t g_exit_pass, g_exit_mode, g_exit_arg;
static volatile uint32_t g_exit_orig = 0x001F45B0;
static bool g_exit_on, g_exit_done;
static int g_exit_who;            // 1 host, 2 joiner (host side)
static float g_exit_pt[3];
static ULONGLONG g_exit_t0, g_exit_send_ms, g_exit_peer_ms;
static int g_exit_peer_left = -1; // joiner: the host's seconds left
static const int kExitSecs = 20;
static void coop_exit_reset()
{
    g_exit_on = g_exit_done = false; g_exit_who = 0; g_coop_exit_ui = 0; g_exit_peer_left = -1; g_exit_peer_ms = 0;
}
static void coop_exit_apply(const char *why)
{
    if (g_exit_done) return;
    g_exit_done = true; g_exit_on = false; g_coop_exit_ui = 0;
    uint32_t go = *(volatile uint32_t *)(uintptr_t)0x002D45F0, mode = 0;
    if (go >= 0x80000000 && go < 0x84000000) mode = *(volatile uint32_t *)(uintptr_t)(go + 0x21070);
    if (mode < 0x80000000 || mode >= 0x84000000) { logf("coop: level exit: no mode object - not ended (%s)", why); return; }
    *(volatile uint32_t *)(uintptr_t)(mode + 0xC) |= 2;   // what FUN_001F45B0 does
    logf("coop: level exit: THE LEVEL ENDS (%s) - mode %08x", why, mode);
    coop_menu_after_mission(g_cfg.coop_role);   // results + autosave, then back to the lobby
}
static void coop_exit_send(uint32_t magic, int left)
{
    if (g_coop_sock == INVALID_SOCKET) return;
    uint32_t m[2] = {magic, (uint32_t)left};
    sendto(g_coop_sock, (const char *)m, 8, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
}
extern "C" void coop_exit_request(void)
{
    g_exit_pass = 0;
    if (!g_cfg.coop_role || g_coop_sock == INVALID_SOCKET) { g_exit_pass = 1; return; }
    if (g_cfg.coop_role == 2) {
        static int jl; if (jl++ < 10) logf("coop: level exit: JOINER local level end swallowed - the host decides (CLXE)");
        return;
    }
    if (g_exit_on || g_exit_done) return;
    g_exit_on = true; g_exit_t0 = GetTickCount64(); g_exit_send_ms = 0;
    g_exit_who = g_exit_by_joiner ? 2 : 1;
    uint32_t pobj = 0;
    { uint32_t ctrl = 0x801FD6A0; coop_safe_copy(&pobj, ctrl + 0x8C, 4); }
    uint32_t src = g_exit_who == 2 ? g_p10_gdst : pobj;
    if (src >= 0x80000000 && src < 0x84000000) coop_safe_copy(g_exit_pt, src + 0xA0, 12);
    logf("coop: level exit: the %s reached the exit at %.1f %.1f %.1f - %d s countdown for both", g_exit_who == 2 ? "JOINER" : "HOST",
         g_exit_pt[0], g_exit_pt[1], g_exit_pt[2], kExitSecs);
}
__declspec(naked) void coop_thunk_exit()   // mission mode vt+0x18 (thiscall, 1 arg, ret 4)
{
    __asm {
        pushad
        mov dword ptr [g_exit_mode], ecx
        call coop_exit_request
        popad
        cmp dword ptr [g_exit_pass], 0
        je skipexit
        jmp dword ptr [g_exit_orig]
    skipexit:
        ret 4
    }
}
static void coop_exit_recv(const uint8_t *raw, int len)
{
    if (g_cfg.coop_role != 2 || len < 8) return;
    uint32_t magic = *(const uint32_t *)raw;
    int left = (int)*(const uint32_t *)(raw + 4);
    if (magic == 0x45584C43) { coop_exit_apply("the host ended it (CLXE)"); return; }
    if (g_exit_done) return;
    if (g_exit_peer_left < 0) logf("coop: level exit: the host reports the exit reached - %d s", left);
    g_exit_peer_left = left; g_exit_peer_ms = GetTickCount64();
    g_coop_exit_ui = left > 0 ? left : 1;
}
static void coop_exit_step(uint32_t obj)
{
    if (!g_cfg.coop_role) return;
    ULONGLONG now = GetTickCount64();
    {   // COOP_EXIT_TEST=<s>[,<metres>[,j]] (host, test): at <s> s fire the level end as if a player reached the exit; the
        // exit point is moved <metres> along +x (the other player counts as away); j = the joiner reached it
        static float at = -2.0f, off = 0.0f; static int byj = 0, fired = 0;
        if (at < -1.0f) {
            char e[32] = {0}; at = -1.0f;
            if (GetEnvironmentVariableA("COOP_EXIT_TEST", e, sizeof e)) { char j = 0; sscanf(e, "%f,%f,%c", &at, &off, &j); byj = j == 'j'; }
        }
        if (g_cfg.coop_role == 1 && at > 0 && !fired && g_coop_t0 && (float)(now - g_coop_t0) / 1000.0f >= at) {
            fired = 1;
            logf("coop: level exit: TEST - the level end fires now (%s at the exit, point moved %.1f m)", byj ? "joiner" : "host", off);
            g_exit_by_joiner = byj; coop_exit_request(); g_exit_by_joiner = 0;
            g_exit_pt[0] += off;
        }
    }
    if (g_cfg.coop_role == 2) {   // the fallback: our shown countdown ran out and no CLXE came
        if (!g_exit_done && g_exit_peer_left >= 0 && now - g_exit_peer_ms > (ULONGLONG)(g_exit_peer_left * 1000 + 3000))
            coop_exit_apply("countdown over and no word from the host for 3 s");
        return;
    }
    if (!g_exit_on || g_exit_done) return;
    int left = kExitSecs - (int)((now - g_exit_t0) / 1000);
    g_coop_exit_ui = left > 0 ? left : 1;
    if (now - g_exit_send_ms >= 250) { g_exit_send_ms = now; coop_exit_send(0x54584C43, left > 0 ? left : 0); }
    // the OTHER player (not the one at the exit)
    float o[3] = {0, 0, 0};
    uint32_t other = g_exit_who == 2 ? obj : g_p10_gdst;
    bool other_down = g_exit_who == 2 ? g_coop_downed != 0 : g_coop_peer_downed != 0;
    bool have = other >= 0x80000000 && other < 0x84000000 && coop_safe_copy(o, other + 0xA0, 12);
    float dx = o[0] - g_exit_pt[0], dy = o[1] - g_exit_pt[1], dz = o[2] - g_exit_pt[2];
    const char *why = nullptr;
    if (left <= 0) why = "the countdown reached zero";
    else if (other_down) why = "the other player is downed";
    else if (have && dx * dx + dz * dz < 36.0f && fabsf(dy) < 3.0f) why = "both players are at the exit";
    if (why) {
        for (int i = 0; i < 5; i++) coop_exit_send(0x45584C43, 0);
        coop_exit_apply(why);
    }
}
static void install_exit_sync()
{
    if (!g_cfg.coop_role) return;
    static bool done; if (done) return; done = true;
    volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)0x00295350;
    if (*slot != 0x001F45B0) { logf("coop: level exit: mission mode vt+0x18 is %08x, not 001F45B0 - not installed", *slot); return; }
    DWORD old = 0;
    VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old);
    *slot = (uint32_t)(uintptr_t)coop_thunk_exit;
    VirtualProtect((void *)slot, 4, old, &old);
    logf("coop: level exit: mission mode vt+0x18 (the level end) re-pointed - shared exit, %d s countdown", kExitSecs);
}

// LOADING BARRIER (owner, day 15: "each player doesn't have the same computer and the loading speed depends on that -
// the level starts once every player has finished loading"). The level-load state machine FUN_001EDC90 (stdcall, the
// world *0x2D45D4) is called every frame by the game-mode loop FUN_0020B770 (call at 0x20B7CC) until it returns 1; its
// last step (state 0x16: FUN_00203610 ready) sets state 0x1C and returns 1 - the level starts. The call is re-pointed
// to coop_load_gate: the first time a load finishes, this copy sends "CLLR" + its load number (counted at every load
// start, state 1 / 0x37, so both copies number their loads alike) and, while the partner's CLLR for that load has not
// come, puts the state back to 0x16 and reports "not yet" - the loading screen stays, with a bottom note. CLLR is
// repeated every 250 ms until 5 s after we went on (the partner may still wait). 120 s without the partner = go on.
volatile int g_coop_load_ui;
volatile uint32_t g_orig_load;
uint32_t g_load_seq; static uint32_t g_peer_load_seq, g_load_sent_seq;
static ULONGLONG g_load_wait_since, g_load_send_ms, g_load_go_ms;
static void coop_load_recv(uint32_t seq)
{
    if (seq > g_peer_load_seq) {
        g_peer_load_seq = seq;
        logf("coop: loading: the partner finished load #%u (ours #%u%s)", seq, g_load_seq, g_load_wait_since ? ", we wait" : "");
    }
}
static void coop_load_send(uint32_t seq)
{
    if (g_coop_sock == INVALID_SOCKET) return;
    uint32_t m[2] = {0x524C4C43, seq};
    sendto(g_coop_sock, (const char *)m, 8, 0, (const sockaddr *)&g_coop_target, sizeof g_coop_target);
}
void coop_poll_control();
// keeps telling the partner we are loaded (it may still be waiting); from the load gate and the play tick
static void coop_load_keepalive()
{
    if (!g_load_sent_seq || !g_cfg.coop_role) return;
    ULONGLONG now = GetTickCount64();
    if (g_load_go_ms && now - g_load_go_ms > 5000) return;
    if (now - g_load_send_ms >= 250) { g_load_send_ms = now; coop_load_send(g_load_sent_seq); }
}
static uint32_t __stdcall coop_load_gate(uint32_t world)
{
    typedef uint32_t(__stdcall * LoadFn)(uint32_t);
    uint32_t prev = (world >= 0x80000000 && world < 0x84000000) ? *(volatile uint32_t *)(uintptr_t)(world + 0x5AE0) : 0;
    if (prev == 1 || prev == 0x37) {   // a load starts
        g_load_seq++;
        g_load_wait_since = 0; g_load_go_ms = 0; g_coop_load_ui = 0;
        if (g_cfg.coop_role) logf("coop: loading: load #%u starts", g_load_seq);
    }
    uint32_t r = ((LoadFn)(uintptr_t)g_orig_load)(world);
    if (!g_cfg.coop_role || g_coop_sock == INVALID_SOCKET) return r;
    if (prev != 0x1C) coop_poll_control();   // the play tick does not run while loading: read the partner here
    coop_load_keepalive();
    uint32_t st = *(volatile uint32_t *)(uintptr_t)(world + 0x5AE0);
    if ((r & 0xFF) && prev != 0x1C && st == 0x1C) {   // this copy just finished loading
        ULONGLONG now = GetTickCount64();
        {   // COOP_LOAD_DELAY=<s> (test): this copy pretends its load took that much longer (a slower PC)
            static int dl = -1; static ULONGLONG first; static uint32_t fseq;
            if (dl < 0) { char e[16] = {0}; dl = GetEnvironmentVariableA("COOP_LOAD_DELAY", e, sizeof e) ? atoi(e) : 0; }
            if (dl > 0) {
                if (fseq != g_load_seq) { fseq = g_load_seq; first = now; logf("coop: loading: test delay %d s (COOP_LOAD_DELAY)", dl); }
                if (now - first < (ULONGLONG)dl * 1000) { *(volatile uint32_t *)(uintptr_t)(world + 0x5AE0) = 0x16; return r & 0xFFFFFF00; }
            }
        }
        if (g_load_sent_seq != g_load_seq) {
            g_load_sent_seq = g_load_seq; g_load_send_ms = 0;
            g_load_wait_since = now;
            logf("coop: loading: load #%u done here - telling the partner%s", g_load_seq,
                 g_peer_load_seq >= g_load_seq ? " (it is done too)" : ", waiting for it");
            coop_load_keepalive();
        }
        if (g_peer_load_seq < g_load_seq && now - g_load_wait_since < 120000) {
            *(volatile uint32_t *)(uintptr_t)(world + 0x5AE0) = 0x16;   // not yet: the last load step again next frame
            g_coop_load_ui = 1;
            return r & 0xFFFFFF00;
        }
        g_coop_load_ui = 0;
        g_load_go_ms = now;
        logf("coop: loading: load #%u - %s after %.1f s of waiting - the level starts", g_load_seq,
             g_peer_load_seq >= g_load_seq ? "both loaded" : "NO WORD FROM THE PARTNER (120 s)", (now - g_load_wait_since) / 1000.0);
    }
    return r;
}

// sk1: the co-op receive loop runs in the play tick - nothing of play runs during a briefing video, so a CLSK sent then
// was never read (both players waited for ever). The skip logic drains the socket itself while a video is up: CLSK
// is taken, anything else dropped (no play state to feed).
void coop_poll_control()
{
    if (g_coop_listens == INVALID_SOCKET) return;
    char buf[2048];
    sockaddr_in from{};
    int fl = sizeof from, r;
    while ((r = recvfrom(g_coop_listens, buf, sizeof buf, 0, (sockaddr *)&from, &fl)) >= 4) {
        fl = sizeof from;
        if (*(const uint32_t *)buf == 0x524C4C43 && r >= 8) coop_load_recv(*(const uint32_t *)(buf + 4));
        if (*(const uint32_t *)buf == 0x54514C43) coop_quit_recv();
        if (*(const uint32_t *)buf == 0x4B534C43) {
            if (!g_coop_peer_skip) logf("coop: skip: the partner wants to skip the cinematic");
            g_coop_peer_skip = 1;
            g_coop_peer_skip_ms = GetTickCount64();
        }
        if (*(const uint32_t *)buf == 0x54494C43) { g_coop_peer_intro = 1; g_coop_peer_intro_ms = GetTickCount64(); }   // "CLIT"
    }
}

void coop_activate(int role, uint32_t peer_ip)
{
    g_coop_host_ms = 0;   // a new session: no partner packet yet (the 20-s silence check starts at the first one)
    if (g_cfg.coop_role && g_coop_sock != INVALID_SOCKET) { logf("coop: activate: already active (role %d)", g_cfg.coop_role); return; }
    g_session_over = false; g_quit_in = 0; g_coop_end_msg[0] = 0;
    in_addr ia; ia.s_addr = peer_ip;
    char ip[32] = {0};
    inet_ntop(AF_INET, &ia, ip, sizeof ip);
    g_cfg.coop_role = role;
    cheats_apply();   // 0.4.0 merge: the solo cheats go off for the session
    g_cfg.coop_clone = true; g_cfg.pawn_cap = true; g_cfg.p13_redirect = true; g_cfg.skin_hook = true; g_cfg.skin_bones = 1;
    // owner (20261115): "the HOST / JOINER banner on top keeps showing in regular play - only during our tests": the
    // lobby no longer turns it on; only --role-label (the test scripts) does
    g_cfg.coop_log = true; g_cfg.coop_downed = true; g_cfg.no_markers = true;
    g_cfg.coop_send = widen(std::string(ip) + (role == 1 ? ":44000" : ":44001"));
    g_cfg.coop_listen_port = role == 1 ? 44001 : 44000;
    if (g_coop_sock != INVALID_SOCKET) closesocket(g_coop_sock);
    if (g_coop_listens != INVALID_SOCKET) closesocket(g_coop_listens);
    g_coop_sock = INVALID_SOCKET; g_coop_listens = INVALID_SOCKET;
    udp_init();
    marker_init();
    g_coop_marker_on = g_coop_listens != INVALID_SOCKET ? 1 : 0;
    g_coop_clone = 1;
    if (!g_pawncap_on) { g_pawncap_on = 1; install_pawncap(); }
    g_p13_on = 1;
    install_coop_features();
    install_loop_tail();
    install_formation_guard();   // missions 4 / 5: an AI formation with nothing to follow (level survey 20261114)
    install_think_skip();        // our own bodies' brains do not think (owner 20261114)
    install_loc_probe();         // the game's real checkpoint popup (playtest 20261114 #7)
    install_sprint();            // co-op sprint (owner 20261115)
    logf("coop: ACTIVATED from the CO-OP lobby as %s - peer %s, send :%s, listen :%d (%s)", role == 1 ? "HOST" : "JOINER", ip,
         role == 1 ? "44000" : "44001", g_cfg.coop_listen_port, g_coop_sock != INVALID_SOCKET && g_coop_listens != INVALID_SOCKET ? "sockets up" : "SOCKET FAILED");
}

// Install everything. Called once from main after the other input patches.
void install_coop_module()
{
    if (!g_coop_t0) g_coop_t0 = GetTickCount64();   // epoch for --downed-at / --hide-test (udp_init only sets it with --coop-send)
    if (g_cfg.coop_off) { logf("coop: module skipped (--no-coop baseline run)"); return; }
    if (!g_cfg.known_build && !g_cfg.pal_build) return;

    // Route B sockets first (cheap, no game memory touched).
    g_coop_sock = INVALID_SOCKET;
    g_coop_listens = INVALID_SOCKET;
    udp_init();
    marker_init();
    g_coop_marker_on = g_coop_listens != INVALID_SOCKET ? 1 : 0;

    // Stage 1: the player bridge (this-call patch at FUN_001ec450).
    uint32_t site = xbe_va(kSite);
    auto *code = (uint8_t *)(uintptr_t)site;
    if (memcmp(code, kExpect, sizeof kExpect) != 0) {
        logf("coop: code at 0x%08x does not match this build; the co-op bridge stays off", site);
        return;
    }
    g_coop_resume = site + 6;
    if (g_cfg.coop_log)
        logf("coop: bridge installed at FUN_001ec450, logging every %u calls (%s%s)",
             g_coop_log_every, g_coop_sock != INVALID_SOCKET ? "telemetry on" : "telemetry off",
             g_coop_listens != INVALID_SOCKET ? ", marker on" : "");
    else
        logf("coop: bridge installed at FUN_001ec450, logging off");
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)coop_thunk - (site + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0x90;

    // Stage 2 pilot: the clone flag gates the legacy paths; the hook matrix runs on
    // dedicated flags (the regcap stays OFF after the gun bisect until isolated).
    g_coop_clone = g_cfg.coop_clone;
    if (g_cfg.pawn_cap) { g_pawncap_on = 1; install_pawncap(); }
    if (g_cfg.p13_redirect) g_p13_on = 1;
    g_regc_quiet = 1;
    g_regcap_on = g_cfg.pawn_cap ? 0 : 0;   // kept off pending the viewmodel isolation
    uint32_t ssite = xbe_va(kSpawnSite);
    auto *scode = (uint8_t *)(uintptr_t)ssite;
    if (memcmp(scode, kSpawnExpect, sizeof kSpawnExpect) == 0) {
        g_spawn_resume = ssite + sizeof kSpawnExpect;
        scode[0] = 0xE9;
        int32_t srel = (int32_t)((uintptr_t)spawn_thunk - (ssite + 5));
        memcpy(scode + 1, &srel, 4);
        memset(scode + 5, 0x90, sizeof kSpawnExpect - 5);
        logf("coop: spawn capture installed at FUN_001df340");
    }

    // Pilot 12: the world-register capture (the spawner identity hunt: every
    // body registration through FUN_001ce1f0 logs its caller's return address).
    // VIEWMODEL GATE: the hook is suspect #1 for the vanishing first-person gun
    // (bisected: the gun is present in the pre-regcap runs with the same redirect
    // era missing). Default OFF this round; the pawn-side adoption is deterministic.
    constexpr uint32_t kRegSite = 0x001CE1F0;
    if (g_regcap_on) {
    uint32_t rsite = xbe_va(kRegSite);
    auto *rcode = (uint8_t *)(uintptr_t)rsite;
    static const uint8_t kRegExpect[5] = {0x55, 0x8B, 0xEC, 0x83, 0xE4};
    if (memcmp(rcode, kRegExpect, sizeof kRegExpect) == 0) {
        g_regcap_resume = (volatile void *)(rsite + 6);   // after the AND ESP,-16 straddle
        rcode[0] = 0xE9;
        int32_t rrel = (int32_t)((uintptr_t)coop_thunk_regcap - (rsite + 5));
        memcpy(rcode + 1, &rrel, 4);
        logf("coop: register capture installed at FUN_001ce1f0");
    } else {
        logf("coop: 001ce1f0 bytes mismatch; no register capture");
    }
    } else {
        logf("coop: register capture skipped (g_regcap_on = 0, the viewmodel gate)");
    }

    install_coop_features();   // the flag-gated hooks (also run later by coop_activate, from the CO-OP lobby)

    // The spawn-#4 pose redirect is armed by --p13-on above (the def overwrite of
    // the natural soldier = the beat hijack that cost the viewmodel, replaced by
    // the pilot-13e factory replay in coop_pilot10_try).

    // Stage 2 pilot: the clone flag gates the loop-tail injection.
    g_coop_clone = g_cfg.coop_clone;
    g_regc_quiet = 1;   // the capture loggers stay silent (the logf flood was the
                        // prime suspect for the missing first-person viewmodel)

    // Stage 2 capture B: the level driver.
    uint32_t lsite = xbe_va(kLevelSite);
    auto *lcode = (uint8_t *)(uintptr_t)lsite;
    if (memcmp(lcode, kLevelExpect, sizeof kLevelExpect) == 0) {
        g_level_out = lsite + sizeof kLevelExpect;
        lcode[0] = 0xE9;
        int32_t lrel = (int32_t)((uintptr_t)level_thunk - (lsite + 5));
        memcpy(lcode + 1, &lrel, 4);
        logf("coop: level capture installed at FUN_001eb440");
    }

    install_loop_tail();
    install_formation_guard();   // missions 4 / 5: an AI formation with nothing to follow (level survey 20261114)
    install_think_skip();        // our own bodies' brains do not think (owner 20261114)
    install_loc_probe();         // the game's real checkpoint popup (playtest 20261114 #7)
    install_sprint();            // co-op sprint (owner 20261115)

    // Stage 2 capture C: the stream interpreter journal (FUN_001e1800's 12-byte prologue;
    // the 0x33 after it is the first byte of the next instruction — not part of it).
    constexpr uint32_t kInterpSite = 0x001E1800;
    const uint8_t kInterpExpect[12] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, // push ebp; mov ebp,esp; and esp,-16
        0x81, 0xEC, 0xF8, 0x01, 0x00, 0x00, // sub esp,0x1f8
    };
    uint32_t isite = xbe_va(kInterpSite);
    auto *icode = (uint8_t *)(uintptr_t)isite;
    if (memcmp(icode, kInterpExpect, sizeof kInterpExpect) == 0) {
        g_interp_out = isite + 12;
        icode[0] = 0xE9;
        int32_t irel = (int32_t)((uintptr_t)interp_thunk - (isite + 5));
        memcpy(icode + 1, &irel, 4);
        memset(icode + 5, 0x90, sizeof kInterpExpect - 5);
        logf("coop: stream journal installed at FUN_001e1800");
    } else {
        logf("coop: interpreter site 0x%08x does not match this build; journal off", isite);
    }
}
