"""Verify the network-asset diagnostic preserves the engine's result/state."""
from pathlib import Path

source = Path("src/demo/demo_native.cpp").read_text(encoding="utf-8-sig")
start = source.index("using NCS_AddTable_fn =")
end = source.index("bool g_absent_installed", start)
prelude = r'''
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
struct Count { std::uintptr_t head; std::uint32_t used, padding; } counts[26]{};
struct Range { std::uint32_t base, size; } ranges[26]{};
int lookups = 0;
std::size_t operator"" _b(unsigned long long value) {
    ++lookups;
    if (value == 0x59FFC30) return reinterpret_cast<std::size_t>(counts);
    if (value == 0xB2D590) return reinterpret_cast<std::size_t>(ranges);
    assert(false); return 0;
}
bool readable(const void* p, std::size_t) { return p != nullptr; }
struct GameUtil { static const char* safeCString(const char* s, std::size_t) { return s ? s : "<null>"; } };
struct Console {
    static inline std::vector<std::string> logs;
    static void printf(const char* fmt, ...) {
        char buffer[512]{};
        va_list args; va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args); logs.emplace_back(buffer);
    }
};
int calls = 0;
const void* forwarded = nullptr;
std::int64_t engineResult = 1;
std::int64_t __fastcall engine(const void* asset) { ++calls; forwarded = asset; return engineResult; }
struct Asset { const char* name; int type, reserved; std::uint32_t entries, padding; const char** strings; };
static_assert(sizeof(Asset) == 32);
'''
tests = r'''
int main() {
    NCS_AddTable_orig = engine;
    Asset asset{"ncs_example_level", 2, 0, 6, 0, nullptr};
    counts[2].used = 10; ranges[2] = {100, 16};
    const auto savedCounts = std::to_array(counts);
    const auto savedRanges = std::to_array(ranges);

    engineResult = 71;
    assert(ncs_add_table_stub(&asset) == 71 && calls == 1 && forwarded == &asset);
    assert(Console::logs.empty());
    engineResult = 0;
    assert(ncs_add_table_stub(&asset) == 0 && calls == 2);
    assert(Console::logs.size() == 1);
    assert(Console::logs.back().find("ncs_example_level") != std::string::npos);
    assert(Console::logs.back().find("type=2 entries=6 used=10 capacity=15") != std::string::npos);
    assert(std::memcmp(counts, savedCounts.data(), sizeof(counts)) == 0);
    assert(std::memcmp(ranges, savedRanges.data(), sizeof(ranges)) == 0);

    lookups = 0; asset.type = 26;
    assert(ncs_add_table_stub(&asset) == 0 && calls == 3 && lookups == 0);
    assert(Console::logs.back().find("type=26") != std::string::npos);
    assert(ncs_add_table_stub(nullptr) == 0 && calls == 4 && forwarded == nullptr);
    assert(Console::logs.back().find("<unreadable>") != std::string::npos);
    asset.type = -1;
    assert(ncs_add_table_stub(&asset) == 0 && calls == 5 && lookups == 0);
    assert(std::memcmp(counts, savedCounts.data(), sizeof(counts)) == 0);
    assert(std::memcmp(ranges, savedRanges.data(), sizeof(ranges)) == 0);
    std::cout << "NCS diagnostic: original arguments/result/state preserved\n";
}
'''
Path("obj/test_ncs_failure.cpp").write_text(prelude + source[start:end] + tests, encoding="utf-8")
