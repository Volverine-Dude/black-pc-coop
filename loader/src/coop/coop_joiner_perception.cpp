// JOINER PERCEPTION (host only): lets the host's enemies notice, target and engage the joiner's clone.
//
// How an enemy perceives (RE, Veblensk runs rg2 / wl1 / sp13 / sp14):
//   FUN_001addf0 (per enemy brain, team 1 only) clears agent+4 / +8 / +0xC, then FUN_001ad440(agent),
//   agent = brain + 0x700. FUN_001ad440 loops the SQUAD ROSTER, 4 entity pointers at
//   registry(*0x2D1988) + 0x22974: slots 0..2 = squadmates (filled by FUN_001b3130 at 0x1b31b3),
//   slot 3 = the local player (0x1a1c6a). For each it tests distance (stance offsets +0x3a2), the view
//   cone (_DAT_00295cbc) and line of sight FUN_001d6dd0(world, 0x221, own body, 1); it keeps a seen
//   bitmask agent[0xb] by entity +0x378 and the nearest visible entity in agent[1] (= brain + 0x704,
//   the ATTENTION entity) with its distance in agent[2].
//   Then FUN_001aef40 -> FUN_001a7bd0(threat slots, entity, 1) files it as a threat. FUN_001a7bd0
//   requires life +0x384 == 0 and, in brain mode +0x12C == 1, kind +0xC4 == 2 (a player). It finds
//   the entity in the target registry (R + 0xFB0, type 0) -> FUN_001a7930 -> FUN_001ab4a0 picks the
//   best threat -> brain + 0x2A8.
//
// The clone (kind 1, registry idx 5, +0x378 = 5) is in neither path: not in the roster, and mode 1
// rejects a kind 1 entity. The owner's rules: do not put the clone in the squad roster (FUN_001b3130 /
// FUN_0018cdf0 / squad orders read it) and do not flip its kind. So this module adds two dedicated
// steps of its own:
//   1. SIGHT: after the game's own FUN_001ad440 pass, a SECOND pass of the same function in which the
//      roster holds only the clone (saved and restored around the call, same thread, nothing else runs
//      in between). The game's own distance / cone / LOS / nearest rules decide; the clone becomes the
//      attention entity only when it is nearer than whatever the first pass chose.
//   2. THREAT: FUN_001a7bd0 treats the clone as the player it stands for (mode 1 accepts it). Life and
//      the registry lookup are the game's own code.
// Env COOP_JOINER_PERCEPTION=0 = off.

#include "common.h"
#include "xbe.h"

#include <cstdint>
#include <cstring>

namespace {

volatile uint32_t g_jp_clone;          // the clone body to perceive (host only; 0 = off this frame)
volatile uint32_t g_jp_on;             // installed
volatile uint32_t g_jp_sight_resume;   // FUN_001ad440 + 6
volatile uint32_t g_jp_threat_resume;  // FUN_001a7bd0 + 5
volatile uint32_t g_jp_threat_accept;  // FUN_001a7bd0's registry lookup (0x1a7c04)
volatile uint32_t g_jp_threat_reject;  // FUN_001a7bd0's "return -1" (0x1a7bf4)
uint32_t g_jp_stats[4];                // second passes, clone chosen, threat accepts, skipped (unsafe)

bool jp_read(void *dst, uint32_t src, size_t n)
{
    if (src < 0x80000000u || src >= 0x84000000u) return false;
    __try { memcpy(dst, (const void *)(uintptr_t)src, n); return true; }
    __except (1) { return false; }
}

}  // namespace

// the game's FUN_001ad440 with its 6 displaced entry bytes (55 8B EC 83 E4 F0) re-executed
extern "C" __declspec(naked) void coop_jp_sight_orig()
{
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        jmp dword ptr [g_jp_sight_resume]
    }
}
typedef void(__stdcall *jp_sight_fn)(int *agent);   // FUN_001ad440 ends in ret 4

extern "C" void __stdcall coop_jp_sight(int *agent)
{
    ((jp_sight_fn)coop_jp_sight_orig)(agent);   // the game's own pass over the squad roster

    uint32_t c = g_jp_clone;
    if (!c) return;
    uint32_t life = 1, kind = 0, bit = 99, link = 0;
    if (!jp_read(&life, c + 0x384, 4) || !jp_read(&kind, c + 0xC4, 4) || !jp_read(&bit, c + 0x378, 4)) return;
    if (life != 0 || kind != 1 || bit >= 32) return;
    // FUN_001ad440 has a squadmate branch (kind 1 + link +0x90 == 1 + link +0xCA8 in 0x1D..0x1F) that
    // writes into the entity's link: the clone must never take it
    jp_read(&link, c + 0x324, 4);
    if (link) {
        uint32_t l90 = 0, lca8 = 0;
        jp_read(&l90, link + 0x90, 4); jp_read(&lca8, link + 0xCA8, 4);
        if (l90 == 1 && lca8 >= 0x1D && lca8 <= 0x1F) {
            if (g_jp_stats[3]++ < 5) logf("coop: joiner perception: clone link %08x looks like a squadmate (+90 1, +CA8 %x) - sight pass skipped", link, lca8);
            return;
        }
    }

    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988;
    if (R < 0x80000000u || R >= 0x84000000u) return;
    volatile uint32_t *roster = (volatile uint32_t *)(uintptr_t)(R + 0x22974);
    uint32_t save[4];
    for (int i = 0; i < 4; i++) save[i] = roster[i];
    int before = agent[1];
    roster[0] = c; roster[1] = 0; roster[2] = 0; roster[3] = 0;
    ((jp_sight_fn)coop_jp_sight_orig)(agent);   // the same rules, the clone only
    for (int i = 0; i < 4; i++) roster[i] = save[i];

    g_jp_stats[0]++;
    if ((uint32_t)agent[1] == c && before != (int)c) {
        g_jp_stats[1]++;
        uint32_t self = 0; jp_read(&self, (uint32_t)agent[0] + 0x8C, 4);
        if (g_jp_stats[1] <= 40 || (g_jp_stats[1] % 200) == 0)
            logf("coop: joiner perception: enemy %08x NOTICED the joiner clone %08x (dist %.1f; first pass had %08x) - %u notices / %u passes",
                 self, c, *(float *)&agent[2], before, g_jp_stats[1], g_jp_stats[0]);
    }
}

// play 20261113_190252: SQUADMATES targeted the clone on the host (allies shot it, the joiner took the
// hits). The clone is filed as a threat ONLY for an observer of another team. Observer: ECX = the brain's
// threat slots = brain + 0x160 (FUN_001aef40(brain + 0x160)); brain +0x8C = its body (checked: that
// body's +0x324 is the brain); team = body +0x39C. An unknown observer -> the game's own rules.
extern "C" volatile uint32_t g_coop_self_body;   // coop.cpp: the self model body (0 = none)
volatile uint32_t g_jp_obs_ecx, g_jp_obs_ally;
extern "C" void coop_jp_threat_observer(void)
{
    g_jp_obs_ally = 0;
    uint32_t brain = g_jp_obs_ecx - 0x160, body = 0, link = 0, t = 0xFFFFFFFF, ct = 0xFFFFFFFE;
    if (!jp_read(&body, brain + 0x8C, 4) || !jp_read(&link, body + 0x324, 4) || link != brain) return;
    // the reference team is OUR PLAYER's (squad roster slot 3, registry + 0x22980): the clone's own +0x39C
    // was still 1 (the enemy team) early in play 20261113_191753
    uint32_t R = *(volatile uint32_t *)(uintptr_t)0x002D1988, me = 0;
    if (!jp_read(&t, body + 0x39C, 4)) return;
    if (!(jp_read(&me, R + 0x22980, 4) && me && jp_read(&ct, me + 0x39C, 4)) && !jp_read(&ct, g_jp_clone + 0x39C, 4)) return;
    if (t == ct) {
        g_jp_obs_ally = 1;
        if (g_jp_stats[3]++ < 10) logf("coop: joiner perception: ally %08x (team %u) wanted the clone as a threat - refused", body, t);
    }
}
extern "C" __declspec(naked) void coop_jp_threat_thunk()   // FUN_001a7bd0 entry: 56 8B 74 24 08 re-executed
{
    __asm {
        push esi
        mov esi, dword ptr [esp + 8]
        test esi, esi
        je notself
        cmp esi, dword ptr [g_coop_self_body]   // the SELF MODEL (our own downed body, local only): never a threat
        je reject
    notself:
        cmp esi, dword ptr [g_jp_clone]
        jne normal
        test esi, esi
        je normal
        cmp dword ptr [esi + 0x384], 0
        jne reject
        pushad
        mov dword ptr [g_jp_obs_ecx], ecx
        call coop_jp_threat_observer
        popad
        cmp dword ptr [g_jp_obs_ally], 0
        jne reject
        lock inc dword ptr [g_jp_stats + 8]
        jmp dword ptr [g_jp_threat_accept]   // the clone stands for a player: any brain mode files it
    reject:
        jmp dword ptr [g_jp_threat_reject]
    normal:
        jmp dword ptr [g_jp_threat_resume]
    }
}

static bool jp_patch(uint32_t va, const uint8_t *expect, int len, void *thunk, const char *what)
{
    uint32_t a = xbe_va(va);
    auto *code = (uint8_t *)(uintptr_t)a;
    if (memcmp(code, expect, len) != 0) { logf("coop: joiner perception: %08x (%s) bytes mismatch - not installed", va, what); return false; }
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)thunk - (a + 5));
    memcpy(code + 1, &rel, 4);
    for (int i = 5; i < len; i++) code[i] = 0x90;
    return true;
}

void coop_jp_install(void)
{
    char e[8] = {0};
    if (GetEnvironmentVariableA("COOP_JOINER_PERCEPTION", e, sizeof e) && e[0] == '0') { logf("coop: joiner perception: OFF (COOP_JOINER_PERCEPTION=0)"); return; }
    static const uint8_t kSight[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    static const uint8_t kThreat[5] = {0x56, 0x8B, 0x74, 0x24, 0x08};
    static const uint8_t kAccept[5] = {0xA1, 0x88, 0x19, 0x2D, 0x00};   // 0x1a7c04 mov eax, [0x2d1988]
    static const uint8_t kReject[3] = {0x83, 0xC8, 0xFF};               // 0x1a7bf4 or eax, -1
    if (memcmp((void *)(uintptr_t)xbe_va(0x001A7C04), kAccept, 5) || memcmp((void *)(uintptr_t)xbe_va(0x001A7BF4), kReject, 3)) {
        logf("coop: joiner perception: FUN_001a7bd0 body mismatch - not installed"); return;
    }
    g_jp_sight_resume = xbe_va(0x001AD440) + 6;
    g_jp_threat_resume = xbe_va(0x001A7BD0) + 5;
    g_jp_threat_accept = xbe_va(0x001A7C04);
    g_jp_threat_reject = xbe_va(0x001A7BF4);
    if (!jp_patch(0x001AD440, kSight, 6, (void *)coop_jp_sight, "sight FUN_001ad440")) return;
    if (!jp_patch(0x001A7BD0, kThreat, 5, (void *)coop_jp_threat_thunk, "threat FUN_001a7bd0")) return;
    g_jp_on = 1;
    logf("coop: joiner perception installed (sight: second roster pass with the clone; threat: the clone counts as a player)");
}

// every frame from the co-op step: the clone to perceive (host: the joiner's clone; joiner: 0, its enemies are puppets)
void coop_jp_frame(uint32_t clone_body)
{
    g_jp_clone = g_jp_on ? clone_body : 0;
    static uint32_t f;
    if (g_jp_on && clone_body && (++f % 1800) == 0)
        logf("coop: joiner perception: %u notices / %u passes, %u threat accepts", g_jp_stats[1], g_jp_stats[0], g_jp_stats[2]);
}
