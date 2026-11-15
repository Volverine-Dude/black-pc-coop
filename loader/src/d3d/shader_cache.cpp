// On-disk cache of compiled shader bytecode. See shader_cache.h.
#include "d3d/shader_cache.h"

#include <windows.h>
#include "d3d/compiler.h"   // xd3d::compile: the compiler loaded at run time (0.4.0)

#include <atomic>
#include <cstdio>
#include <mutex>

#include "common.h"
#include "perf.h"

namespace xd3d {
namespace {

constexpr uint32_t kMagic = 0x43485342;   // "BSHC"
constexpr uint32_t kVersion = 1;          // bump when the entry layout or compile flags change
constexpr UINT kFlags = D3DCOMPILE_OPTIMIZATION_LEVEL2;

struct Header {
    uint32_t magic, version, flags, text_len, code_len;
    char profile[16];   // the profile that compiled it
};

uint64_t fnv1a(const void *p, size_t n, uint64_t h = 1469598103934665603ull)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

std::mutex g_dir_mutex;
std::wstring g_dir;          // empty = not resolved yet; L"-" = disabled (no writable folder)
std::atomic<uint32_t> g_hits{0}, g_misses{0};

const std::wstring &cache_dir()
{
    std::lock_guard<std::mutex> lock(g_dir_mutex);
    if (!g_dir.empty()) return g_dir;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(exe, n) : std::wstring();
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos || g_cfg.no_shader_cache) { g_dir = L"-"; return g_dir; }
    dir = dir.substr(0, slash) + L"\\shadercache";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        logf("[shader cache] cannot create the cache folder (error %lu); caching off", GetLastError());
        g_dir = L"-";
        return g_dir;
    }
    g_dir = dir;
    logf("[shader cache] folder ready");
    return g_dir;
}

std::wstring entry_path(const std::wstring &dir, uint64_t key)
{
    wchar_t name[32];
    swprintf(name, 32, L"\\%016llx.bin", (unsigned long long)key);
    return dir + name;
}

uint64_t entry_key(const std::string &hlsl, const char *name, std::initializer_list<const char *> profiles)
{
    uint64_t h = fnv1a(hlsl.data(), hlsl.size());
    h = fnv1a(name, strlen(name), h);
    for (const char *p : profiles) h = fnv1a(p, strlen(p) + 1, h);
    uint32_t tag[2] = {kVersion, kFlags};
    return fnv1a(tag, sizeof tag, h);
}

bool load(const std::wstring &path, const std::string &hlsl, std::vector<uint8_t> &out)
{
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    Header hd{};
    bool ok = fread(&hd, sizeof hd, 1, f) == 1 && hd.magic == kMagic && hd.version == kVersion &&
              hd.flags == kFlags && hd.text_len == hlsl.size() && hd.code_len > 0 && hd.code_len < (8u << 20);
    if (ok) {
        std::string text(hd.text_len, '\0');
        ok = fread(&text[0], 1, hd.text_len, f) == hd.text_len && text == hlsl;   // exact text, never just the hash
        if (ok) {
            out.resize(hd.code_len);
            ok = fread(out.data(), 1, hd.code_len, f) == hd.code_len;
        }
    }
    fclose(f);
    return ok;
}

void store(const std::wstring &path, const std::string &hlsl, const char *profile, const std::vector<uint8_t> &code)
{
    Header hd{kMagic, kVersion, kFlags, (uint32_t)hlsl.size(), (uint32_t)code.size(), {}};
    strncpy(hd.profile, profile, sizeof hd.profile - 1);
    // write to a private temp name, then rename: a reader (or a second game copy) never sees a partial entry
    wchar_t tmp_suffix[32];
    swprintf(tmp_suffix, 32, L".%lu.tmp", GetCurrentProcessId());
    std::wstring tmp = path + tmp_suffix;
    FILE *f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    bool ok = fwrite(&hd, sizeof hd, 1, f) == 1 && fwrite(hlsl.data(), 1, hlsl.size(), f) == hlsl.size() &&
              fwrite(code.data(), 1, code.size(), f) == code.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

} // namespace

bool compile_cached(const std::string &hlsl, const char *name, std::initializer_list<const char *> profiles,
                    std::vector<uint8_t> &out, std::string &errors)
{
    perf::Scope perf_scope(perf::kCompile);
    const std::wstring &dir = cache_dir();
    bool cache_on = dir != L"-";
    std::wstring path;
    if (cache_on) {
        path = entry_path(dir, entry_key(hlsl, name, profiles));
        if (load(path, hlsl, out)) {
            uint32_t h = ++g_hits;
            if (h <= 3 || (h % 100) == 0) logf("[shader cache] hit #%u (%s)", h, name);
            return true;
        }
    }
    perf::add_compile();
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = E_FAIL;
    const char *used = "";
    for (const char *profile : profiles) {
        if (err) { err->Release(); err = nullptr; }
        hr = xd3d::compile(hlsl.data(), hlsl.size(), name, nullptr, nullptr, "main", profile, kFlags, 0, &code, &err);
        if (SUCCEEDED(hr)) { used = profile; break; }
    }
    if (FAILED(hr)) {
        errors = err ? (const char *)err->GetBufferPointer() : "?";
        if (err) err->Release();
        return false;
    }
    out.assign((const uint8_t *)code->GetBufferPointer(), (const uint8_t *)code->GetBufferPointer() + code->GetBufferSize());
    code->Release();
    if (err) err->Release();
    if (cache_on) {
        store(path, hlsl, used, out);
        uint32_t m = ++g_misses;
        if (m <= 3 || (m % 100) == 0) logf("[shader cache] compiled + stored #%u (%s, %s)", m, name, used);
    }
    return true;
}

} // namespace xd3d
