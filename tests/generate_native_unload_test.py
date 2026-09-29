"""Actual-source regression for native console unload and loading-zone lifetime."""
from pathlib import Path
import sys

source = Path(sys.argv[1] if len(sys.argv) > 1 else "src/demo/demo_native.cpp").read_text(encoding="utf-8-sig")
start = source.index("std::int64_t db_load_xassets_stub(")
end = source.index("// A NULL name matches", start)
mode_start = source.find("using IsMultiplayer_fn =")
mode_hook = source[mode_start:start] if mode_start >= 0 else ""
prelude = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
struct zone_request { const char* name; std::uint32_t flags_a, flags_b; };
std::uintptr_t callerRva;
void* test_return_address() { return reinterpret_cast<void*>(callerRva); }
#define _ReturnAddress test_return_address
std::uint64_t to_ida(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
bool canRead = true, g_release_level = true, g_release_frontend = true, frontendGate = false;
bool g_console_map_load = true, multiplayer = true;
std::uint8_t loadingGuard = 0;
int zones = 15, modeCalls = 0;
bool originalMode() { ++modeCalls; return multiplayer; }
bool readable(const void* p, std::size_t) { return p && canRead; }
struct Console { static void printf(const char*, ...) {} };
std::vector<std::string> order;
int loaded_zone_count() { return zones; }
void dump_zone_list(const char*) {}
void levelPreparation() { order.emplace_back("level"); }
void frontendPreparation() { order.emplace_back("frontend"); }
bool gate() { return frontendGate; }
std::uintptr_t operator"" _b(unsigned long long value) {
    switch (value) {
    case 0x18F6B0: return reinterpret_cast<std::uintptr_t>(levelPreparation);
    case 0x195110: return reinterpret_cast<std::uintptr_t>(frontendPreparation);
    case 0x38E590: return reinterpret_cast<std::uintptr_t>(gate);
    case 0x11102EB: return reinterpret_cast<std::uintptr_t>(&loadingGuard);
    default: assert(false); return 0;
    }
}
using engine_void_fn = void(*)();
struct Call { void* pointer; zone_request entry; unsigned count; int mode; };
std::vector<Call> calls;
std::int64_t original(void* p, unsigned n, int mode) {
    zone_request entry{};
    if (p && canRead && n == 1) entry = *static_cast<zone_request*>(p);
    calls.push_back({p, entry, n, mode}); order.emplace_back("original");
    zones = 8; return 0x13579ABCDEFLL;
}
std::int64_t (*DB_LoadXAssets_orig)(void*, unsigned, int) = original;
'''
tests = r'''
void reset() { callerRva = 0x6DC4A6; g_release_level = g_release_frontend = canRead = true;
    zones = 15; loadingGuard = 0; calls.clear(); order.clear(); }
void unchanged(zone_request* p, unsigned count = 1, int mode = 0) {
    const auto saved = p ? *p : zone_request{};
    assert(db_load_xassets_stub(p, count, mode) == 0x13579ABCDEFLL);
    if (calls.size() != 1 || calls[0].pointer != p) {
        std::cerr << "Native loading request was modified or extra zones were freed\n"; std::exit(1);
    }
    assert(calls[0].count == count && calls[0].mode == mode && order.size() == 1 && loadingGuard == 0);
    if (p) assert(std::memcmp(p, &saved, sizeof(saved)) == 0);
}
int main() {
    reset(); callerRva = 0xD7B73;
    zone_request loading{"mp_sandbox_01_load", 0x10, 0x30}; unchanged(&loading, 1, 5);
    for (auto mask : {0x184u, 0x384u}) {
        reset(); callerRva = 0x48C1CA; zone_request menu{nullptr, 0, mask}; unchanged(&menu);
    }
    for (auto mask : {0x104u, 0x184u, 0x304u, 0x384u}) {
        reset(); zone_request request{nullptr, 0, mask};
        assert(db_load_xassets_stub(&request, 1, 0) == 0x13579ABCDEFLL);
        assert(calls.size() == 1 && calls[0].pointer != &request && calls[0].entry.flags_b == (mask | 0x88u));
        assert(calls[0].count == 1 && calls[0].mode == 0 && request.flags_b == mask);
        assert(order == (std::vector<std::string>{"level", "original"}) && loadingGuard == 0);
    }
    for (auto caller : {0x6DC4A5u, 0x6DC4A7u, 0xA496Cu, 0x48C1CAu, 0xD7B73u, 0u}) {
        reset(); callerRva = caller; zone_request request{nullptr, 0, 0x184}; unchanged(&request);
    }
    for (auto mask : {0x88u, 0x200u, 0x100u, 0x18Cu, 0x185u, 0x504u}) {
        reset(); zone_request request{nullptr, 0, mask}; unchanged(&request);
    }
    reset(); zone_request named{"map", 0, 0x184}; unchanged(&named);
    reset(); zone_request allocated{nullptr, 8, 0x184}; unchanged(&allocated);
    reset(); zone_request request{nullptr, 0, 0x184}; unchanged(&request, 2);
    reset(); unchanged(&request, 1, 3);
    reset(); g_release_level = false; unchanged(&request);
    reset(); canRead = false; unchanged(&request);
    reset(); unchanged(nullptr);
MODE_TESTS
    std::cout << "Native unload, menu/loading forwarding and caller boundaries passed\n";
}
'''
mode_tests = r'''
    IsMultiplayer_orig = originalMode;
    for (auto caller : {0x6DC448u, 0x6DC561u, 0x48C1CFu, 0x6DC447u, 0x6DC449u}) {
        for (bool mp : {false, true}) for (bool enabled : {false, true}) {
            reset(); callerRva = caller; multiplayer = mp; g_release_frontend = enabled; modeCalls = 0;
            assert(live_unload_mode_stub() == (mp && enabled && caller == 0x6DC448 ? false : mp));
            assert(modeCalls == 1 && calls.empty() && loadingGuard == 0 && order.empty());
        }
    }
'''
Path("obj").mkdir(exist_ok=True)
Path("obj/test_live_unload.cpp").write_text(
    prelude + mode_hook + source[start:end] + tests.replace("MODE_TESTS", mode_tests if mode_hook else ""), encoding="utf-8")
