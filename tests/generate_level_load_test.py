"""Exercise the actual level loader hook with live and demo caller addresses."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("src/demo/demo_native.cpp")
source = path.read_text(encoding="utf-8-sig")
start = source.index("std::int64_t db_load_level_xassets_stub(")
end = source.index("void cmd_zone_list()", start)
prelude = r'''
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
std::uintptr_t callerRva = 0;
void* test_return_address() { return reinterpret_cast<void*>(callerRva); }
#define _ReturnAddress test_return_address
std::uint64_t to_ida(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
bool readable(const void* p, std::size_t) { return p != nullptr; }
struct GameUtil {
    static const char* safeCString(const char* p, std::size_t) { return p ? p : "<null>"; }
};
struct Console {
    static inline std::vector<std::string> logs;
    static void printf(const char* format, ...) {
        char buf[512]{};
        va_list args; va_start(args, format);
        std::vsnprintf(buf, sizeof(buf), format, args); va_end(args);
        logs.emplace_back(buf);
    }
};
bool g_release_level = true;
int zones = 16, releases = 0, originalCalls = 0;
std::vector<std::string> order;
const char* forwardedMap = nullptr;
char forwardedFlags = 0;
int loaded_zone_count() { return zones; }
void release_level_zones() { ++releases; zones -= 5; order.emplace_back("release"); }
std::int64_t original(const char* map, char flags) {
    ++originalCalls; forwardedMap = map; forwardedFlags = flags;
    zones += 3; order.emplace_back("original"); return 0x12345678ABCDEFLL;
}
std::int64_t (*DB_LoadLevelXAssets_orig)(const char*, char) = original;
'''
tests = r'''
void reset(std::uintptr_t caller, bool enabled = true) {
    callerRva = caller; g_release_level = enabled;
    zones = 16; releases = originalCalls = 0;
    forwardedMap = nullptr; forwardedFlags = 0;
    Console::logs.clear(); order.clear();
}
bool live_case(std::uintptr_t caller, const char* map) {
    reset(caller);
    const auto result = db_load_level_xassets_stub(map, 2);
    if (releases || originalCalls != 1 || forwardedMap != map || forwardedFlags != 2
        || result != 0x12345678ABCDEFLL || zones != 19 || order != std::vector<std::string>{"original"}) {
        std::cerr << "Live map startup invoked demo cleanup or changed the original call\n";
        return false;
    }
    assert(Console::logs.front().find("engine-owned load (no extra release)") != std::string::npos);
    return true;
}
int main() {
    if (!live_case(0x6DC8BB, "mp_sandbox_01")) return 1; // Supplied Build 36 log.
    if (!live_case(0x48C285, "mp_house")) return 1;
    if (!live_case(0, "mp_london")) return 1;
    if (!live_case(0x91064F, "mp_london")) return 1;
    if (!live_case(0x910BF0, "mp_london")) return 1;
    for (auto caller : {0x910650u, 0x910B2Fu, 0x910BEFu}) {
        reset(caller);
        const char* map = "mp_sandbox_01";
        assert(db_load_level_xassets_stub(map, 0) == 0x12345678ABCDEFLL);
        assert(releases == 1 && originalCalls == 1 && forwardedMap == map && forwardedFlags == 0);
        assert(zones == 14 && order == (std::vector<std::string>{"release", "original"}));
    }
    reset(0x910B2F, false);
    assert(db_load_level_xassets_stub("mp_house", 0) == 0x12345678ABCDEFLL);
    assert(releases == 0 && originalCalls == 1 && zones == 19);
    reset(0x6DC8BB);
    assert(db_load_level_xassets_stub(nullptr, 2) == 0x12345678ABCDEFLL);
    assert(releases == 0 && forwardedMap == nullptr && originalCalls == 1);
    std::cout << "Level loading: live startup unchanged; direct demo cleanup preserved\n";
}
'''
Path("obj").mkdir(exist_ok=True)
Path("obj/test_level_load.cpp").write_text(prelude + source[start:end] + tests, encoding="utf-8")
