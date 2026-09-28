"""Verify the native startup hook's scope, request copy and call ordering."""
from pathlib import Path

source = Path("src/demo/demo_native.cpp").read_text(encoding="utf-8-sig")
start = source.index("std::int64_t db_load_xassets_stub(")
end = source.index("// A NULL name matches", start)
prelude = r'''
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
struct zone_request { const char* name; std::uint32_t flags_a, flags_b; };
std::uintptr_t callerRva = 0x6DC4A6;
void* test_return_address() { return reinterpret_cast<void*>(callerRva); }
#define _ReturnAddress test_return_address
std::uint64_t to_ida(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
bool canRead = true, g_release_level = true;
bool readable(const void* p, std::size_t) { return p && canRead; }
struct Console { static void printf(const char*, ...) {} };
int zones = 16, originalCalls = 0, preparationCalls = 0;
std::vector<std::string> order;
int loaded_zone_count() { return zones; }
void dump_zone_list(const char*) {}
void preparation() { ++preparationCalls; order.emplace_back("prepare"); }
std::uintptr_t operator"" _b(unsigned long long value) {
    assert(value == 0x18F6B0); return reinterpret_cast<std::uintptr_t>(preparation);
}
using engine_void_fn = void(*)();
void* forwarded = nullptr;
zone_request forwardedEntry{};
unsigned forwardedCount = 0;
int forwardedMode = -1;
std::int64_t original(void* p, unsigned n, int mode) {
    ++originalCalls; forwarded = p; forwardedCount = n; forwardedMode = mode;
    if (p && canRead && n == 1) forwardedEntry = *static_cast<zone_request*>(p);
    order.emplace_back("original"); zones = 9; return 0x13579ABCDEFLL;
}
std::int64_t (*DB_LoadXAssets_orig)(void*, unsigned, int) = original;
'''
tests = r'''
void reset() {
    callerRva = 0x6DC4A6; g_release_level = true; canRead = true;
    zones = 16; originalCalls = preparationCalls = 0;
    forwarded = nullptr; forwardedCount = 0; forwardedMode = -1;
    forwardedEntry = {}; order.clear();
}
void unchanged(zone_request* p, unsigned count = 1, int mode = 0) {
    zone_request saved = p ? *p : zone_request{};
    assert(db_load_xassets_stub(p, count, mode) == 0x13579ABCDEFLL);
    assert(originalCalls == 1 && preparationCalls == 0 && forwarded == p);
    assert(forwardedCount == count && forwardedMode == mode);
    assert(order == (std::vector<std::string>{"original"}));
    if (p) assert(std::memcmp(p, &saved, sizeof(saved)) == 0);
}
int main() {
    for (auto mask : {0x104u, 0x184u, 0x304u, 0x384u}) {
        reset();
        zone_request request{nullptr, 0, mask};
        assert(db_load_xassets_stub(&request, 1, 0) == 0x13579ABCDEFLL);
        assert(preparationCalls == 1 && originalCalls == 1 && forwarded != &request);
        assert(forwardedEntry.name == nullptr && forwardedEntry.flags_a == 0);
        assert(forwardedEntry.flags_b == (mask | 0x88u));
        assert(forwardedCount == 1 && forwardedMode == 0 && request.flags_b == mask);
        assert(order == (std::vector<std::string>{"prepare", "original"}));
    }
    for (auto caller : {0x6DC4A5u, 0x6DC4A7u, 0xA496Cu, 0x48C1CAu, 0x910B2Fu, 0u}) {
        reset(); callerRva = caller;
        zone_request request{nullptr, 0, 0x104}; unchanged(&request);
    }
    for (auto mask : {0x88u, 0x200u, 0x18Cu, 0x105u, 0x504u}) {
        reset(); zone_request request{nullptr, 0, mask}; unchanged(&request);
    }
    reset(); zone_request named{"mp_sandbox_01", 0, 0x104}; unchanged(&named);
    reset(); zone_request allocated{nullptr, 8, 0x104}; unchanged(&allocated);
    reset(); zone_request request{nullptr, 0, 0x104}; unchanged(&request, 2);
    reset(); unchanged(&request, 1, 3);
    reset(); g_release_level = false; unchanged(&request);
    reset(); canRead = false; unchanged(&request);
    reset(); unchanged(nullptr);
    std::cout << "Native unload hook: scoped forwarding and preparation order passed\n";
}
'''
Path("obj").mkdir(exist_ok=True)
Path("obj/test_live_unload.cpp").write_text(prelude + source[start:end] + tests, encoding="utf-8")
