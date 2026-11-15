// On-disk cache of compiled shader bytecode (shadercache\ next to black.exe).
//
// The NV2A vertex/pixel programs are translated to HLSL and compiled with D3DCompile the first time a
// combination is drawn, which costs 20-30 ms per new effect in every session. This cache keeps the
// compiled bytecode across sessions: an entry is keyed by a hash of the exact HLSL text plus the
// compile settings, and stores that text too, so a hit is reused only on an exact text match.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace xd3d {

// Compile `hlsl` with the first profile in `profiles` that succeeds (D3DCOMPILE_OPTIMIZATION_LEVEL2),
// or load the bytecode a previous session stored for the same text and settings.
// Returns true with the bytecode in `out`; false with the compiler message in `errors`.
bool compile_cached(const std::string &hlsl, const char *name, std::initializer_list<const char *> profiles,
                    std::vector<uint8_t> &out, std::string &errors);

} // namespace xd3d
