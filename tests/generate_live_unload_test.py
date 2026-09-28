"""Exercise actual hooks against native menu and console Multiplayer routes."""
from pathlib import Path
import sys

source = Path(sys.argv[1] if len(sys.argv) > 1 else "src/demo/demo_native.cpp").read_text(encoding="utf-8-sig")
start = source.index("thread_local bool g_console_map_load") if "thread_local bool g_console_map_load" in source else source.index("std::int64_t db_load_xassets_stub(")
end = source.index("// A NULL name matches", start)
prelude = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
struct zone_request { const char* name; std::uint32_t flags_a, flags_b; };
std::uintptr_t callerRva;
void* test_return_address() { return reinterpret_cast<void*>(callerRva); }
#define _ReturnAddress test_return_address
std::uint64_t to_ida(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
bool canRead = true, g_release_level = true, g_release_frontend = true, frontendGate = true;
int engineMode = 1, zones = 16;
std::uint8_t loadingGuard = 0;
bool readable(const void* p, std::size_t) { return p && canRead; }
struct Console { static void printf(const char*, ...) {} };
std::vector<std::string> order;
int loaded_zone_count() { return zones; }
void dump_zone_list(const char*) {}
void levelPreparation() { order.emplace_back("level"); }
void frontendPreparation() { order.emplace_back("frontend"); }
bool gate() { return frontendGate; }
int modeGetter() { return engineMode; }
std::uintptr_t operator"" _b(unsigned long long value) {
    switch (value) {
    case 0x18F6B0: return reinterpret_cast<std::uintptr_t>(levelPreparation);
    case 0x195110: return reinterpret_cast<std::uintptr_t>(frontendPreparation);
    case 0x38E590: return reinterpret_cast<std::uintptr_t>(gate);
    case 0x8545C0: return reinterpret_cast<std::uintptr_t>(modeGetter);
    case 0x11102EB: return reinterpret_cast<std::uintptr_t>(&loadingGuard);
    default: assert(false); return 0;
    }
}
using engine_void_fn = void(*)();
using LoadingScreen_fn = std::int64_t(*)(const char*);
struct Call { void* pointer; zone_request entry; unsigned count; int mode; };
std::vector<Call> calls;
std::int64_t original(void* p, unsigned n, int mode) {
    zone_request entry{};
    if (p && canRead && n == 1) entry = *static_cast<zone_request*>(p);
    calls.push_back({p, entry, n, mode});
    order.emplace_back(mode == 5 ? "loading-zone" : "unload");
    zones = 9; return mode == 5 ? 0x2468ABCDELL : 0x13579ABCDEFLL;
}
std::int64_t (*DB_LoadXAssets_orig)(void*, unsigned, int) = original;
'''
tests = r'''
void reset() {
    callerRva = 0x48C1CA; g_release_level = g_release_frontend = canRead = frontendGate = true;
    g_console_map_load = false; engineMode = 1; loadingGuard = 0; zones = 16;
    calls.clear(); order.clear();
}
void unchanged(zone_request* p, unsigned count = 1, int mode = 0) {
    const auto saved = p ? *p : zone_request{};
    assert(db_load_xassets_stub(p, count, mode) == (mode == 5 ? 0x2468ABCDELL : 0x13579ABCDEFLL));
    assert(calls.size() == 1 && calls[0].pointer == p);
    assert(calls[0].count == count && calls[0].mode == mode && order.size() == 1 && loadingGuard == 0);
    if (p) assert(std::memcmp(p, &saved, sizeof(saved)) == 0);
}
int loadingCalls;
const char* forwardedMap;
zone_request loadingRequest{"mp_sandbox_01_load", 0x10, 0x30};
std::int64_t loadingOriginal(const char* map) {
    ++loadingCalls; forwardedMap = map;
    order.emplace_back("renderer-sync"); // Native D7B0F precedes D7B6E.
    callerRva = 0xD7B73;
    return db_load_xassets_stub(&loadingRequest, 1, 5);
}
int main() {
    for (auto mask : {0x184u, 0x384u}) {
        reset(); loadingGuard = 1; zone_request request{nullptr, 0, mask};
        const auto result = db_load_xassets_stub(&request, 1, 0);
        if (calls.size() != 1 || calls[0].pointer == &request || calls[0].entry.flags_b != (mask | 0x88u)) {
            std::cerr << "Private menu retained the old level group\n"; return 1;
        }
        assert(result == 0x13579ABCDEFLL && calls[0].count == 1 && calls[0].mode == 0);
        assert(request.flags_b == mask && order == (std::vector<std::string>{"level", "unload"}));
    }
    for (bool gateValue : {true, false}) {
        reset(); frontendGate = gateValue; LoadingScreen_orig = loadingOriginal; loadingCalls = 0;
        const char* map = "mp_sandbox_01"; callerRva = 0x6DC518;
        assert(loading_screen_stub(map) == 0x2468ABCDELL);
        assert(loadingCalls == 1 && forwardedMap == map && !g_console_map_load && loadingGuard == 1);
        assert(calls.size() == 2 && calls[0].entry.name == nullptr && calls[0].mode == 0);
        assert(calls[0].entry.flags_a == 0 && calls[0].entry.flags_b == (gateValue ? 0x18Cu : 0x38Cu));
        assert(calls[1].pointer == &loadingRequest && calls[1].count == 1 && calls[1].mode == 5);
        assert(loadingRequest.flags_a == 0x10 && loadingRequest.flags_b == 0x30);
        assert(order == (std::vector<std::string>{"renderer-sync", "frontend", "level", "unload", "loading-zone"}));
        loadingGuard = 0; // Native 6DC53C clears it after LUI restart.
    }
    for (auto caller : {0x48C22Au, 0x910895u, 0x6DC517u, 0x6DC519u}) {
        reset(); callerRva = caller; LoadingScreen_orig = loadingOriginal;
        assert(loading_screen_stub("mp_house") == 0x2468ABCDELL);
        assert(!g_console_map_load && loadingGuard == 0 && calls.size() == 1);
    }
    reset(); engineMode = 2; callerRva = 0x6DC518; LoadingScreen_orig = loadingOriginal;
    loading_screen_stub("zombies"); assert(calls.size() == 1 && loadingGuard == 0);
    for (auto caller : {0x6DC4A6u, 0xA496Cu, 0x48C1C9u, 0x48C1CBu, 0xD7B73u, 0u}) {
        reset(); callerRva = caller; zone_request request{nullptr, 0, 0x184}; unchanged(&request);
    }
    for (auto mask : {0x88u, 0x200u, 0x18Cu, 0x185u, 0x504u}) {
        reset(); zone_request request{nullptr, 0, mask}; unchanged(&request);
    }
    reset(); zone_request named{"mp_sandbox_01", 0, 0x184}; unchanged(&named);
    reset(); zone_request allocated{nullptr, 8, 0x184}; unchanged(&allocated);
    reset(); zone_request request{nullptr, 0, 0x184}; unchanged(&request, 2);
    reset(); unchanged(&request, 1, 3);
    reset(); g_release_level = false; unchanged(&request);
    reset(); canRead = false; unchanged(&request);
    reset(); unchanged(nullptr);
    reset(); g_console_map_load = true; callerRva = 0xD7B73;
    g_release_level = g_release_frontend = false; unchanged(&loadingRequest, 1, 5);
    reset(); g_console_map_load = true; callerRva = 0xD7B72; unchanged(&loadingRequest, 1, 5);
    reset(); g_console_map_load = true; callerRva = 0xD7B73; unchanged(&loadingRequest, 2, 5);
    reset(); g_console_map_load = true; callerRva = 0xD7B73;
    zone_request other{"unrelated", 0x10, 0x20}; unchanged(&other, 1, 5);
    for (bool frontendOnly : {true, false}) {
        reset(); g_console_map_load = true; callerRva = 0xD7B73;
        g_release_frontend = frontendOnly; g_release_level = !frontendOnly;
        db_load_xassets_stub(&loadingRequest, 1, 5);
        assert(calls.size() == 2 && calls[0].entry.flags_b == (frontendOnly ? 0x184u : 0x88u));
    }
    std::cout << "MP cleanup: lifecycle, forwarding, unrelated paths and toggles passed\n";
}
'''
legacy = "thread_local bool g_console_map_load = false;\nLoadingScreen_fn LoadingScreen_orig = nullptr;\nstd::int64_t loading_screen_stub(const char* map) { return LoadingScreen_orig(map); }\n" if "thread_local bool g_console_map_load" not in source else ""
Path("obj").mkdir(exist_ok=True)
Path("obj/test_live_unload.cpp").write_text(prelude + legacy + source[start:end] + tests, encoding="utf-8")
