// The game's memory manager (0x2D12E8) splits its 42 MB arena (MmAllocateContiguousMemoryEx(0x2A4D800) in the init
// FUN_001F49F0, called at 0xE1A53 with esi = the manager) into fixed regions, 0x24-byte records from manager +4:
// +8 start, +0xC end, +0x10 next free, +0x14 alignment, +0x20 base; +0x364 = the current region. Region 2
// (arena +0xCF5800, 0xC5D000 bytes = 12.4 MB) is current in the front end and holds a mission's level.
//
// 0.4.0 + co-op merge (20261115): FEMain.bin with the port's pages and the co-op pages is 0x782000 bytes; region 2 had
// 7,624 KB free at its load, the allocation came back null and the game showed "there is a problem with the disc"
// (NtReadFile into 0). The regions sit at fixed arena offsets, so region 2 cannot grow in place: right after the init
// it moves to its own contiguous block kExtra larger, and its old range goes back to the free list (the kernel's
// contiguous pool, 17.5 MB free at that point), so the cost is only kExtra. Same idea as bigbuf.cpp (weapon buffers).
// BLACK_REGION2_EXTRA_MB=<n> sets kExtra (0 = off).
#include "region_grow.h"

#include "common.h"
#include "kernel/kexports.h"
#include "xbe.h"

#include <cstdlib>
#include <cstring>

void contig_release(void *at, uint32_t size);   // kernel/k_memory.cpp

extern "C" {
uint32_t g_rg_init;   // FUN_001F49F0
void __cdecl rg_after_init();
}

namespace {

constexpr uint32_t kSite = 0x000E1A53;      // call FUN_001F49F0
constexpr uint32_t kInit = 0x001F49F0;
constexpr uint32_t kManager = 0x002D12E8;
constexpr uint32_t kRegion = 2;
constexpr uint32_t kOldSize = 0x00C5D000;
uint32_t g_extra = 3u << 20;

__declspec(naked) void rg_thunk()
{
    __asm {
        call dword ptr [g_rg_init]
        pushad
        call rg_after_init
        popad
        ret
    }
}

} // namespace

extern "C" void __cdecl rg_after_init()
{
    uint8_t *m = (uint8_t *)(uintptr_t)xbe_va(kManager);
    uint32_t *r = (uint32_t *)(m + 4 + kRegion * 0x24);
    uint32_t start = r[2], end = r[3], cur = r[4], base = r[8];
    if (end - start != kOldSize || cur != start || base != start) {
        logf("region grow: region %u is not as expected (start %08x end %08x next %08x base %08x) - left alone", kRegion, start, end, cur, base);
        return;
    }
    uint32_t size = kOldSize + g_extra;
    uint8_t *block = (uint8_t *)xk::MmAllocateContiguousMemoryEx(size, 0, 0xFFFFFFFF, 0x1000, 4);
    if (!block) { logf("region grow: no contiguous block of %u KB - region %u stays at %u KB", size >> 10, kRegion, kOldSize >> 10); return; }
    uint32_t b = (uint32_t)(uintptr_t)block;
    r[2] = b; r[3] = b + size; r[4] = b; r[8] = b;
    contig_release((void *)(uintptr_t)start, kOldSize);
    logf("region grow: region %u moved %08x..%08x -> %08x..%08x (%u KB, +%u KB); the old range is free again", kRegion, start, end,
         b, b + size, size >> 10, g_extra >> 10);
}

void install_region_grow_patch()
{
    if (const char *e = getenv("BLACK_REGION2_EXTRA_MB")) g_extra = (uint32_t)atoi(e) << 20;
    if (!g_extra) { logf("region grow: off"); return; }
    uint8_t *site = (uint8_t *)(uintptr_t)xbe_va(kSite);
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || (uint32_t)(uintptr_t)site + 5 + rel != xbe_va(kInit)) {
        logf("region grow: the call at 0x%08x does not match this build - not installed", xbe_va(kSite));
        return;
    }
    g_rg_init = xbe_va(kInit);
    int32_t nrel = (int32_t)((uintptr_t)rg_thunk - ((uintptr_t)site + 5));
    memcpy(site + 1, &nrel, 4);
    logf("region grow: installed (region %u +%u KB after the memory manager's init)", kRegion, g_extra >> 10);
}
