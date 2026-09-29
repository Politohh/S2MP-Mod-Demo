"""Verify the actual packet hook still forwards live/native-demo calls once."""
from pathlib import Path

source = Path("src/demo/demo_playback.cpp").read_text(encoding="utf-8-sig")
start = source.index("void (*CL_WritePacket_orig)")
end = source.index("// Clamp target seq", start)
prelude = r'''
#include <atomic>
#include <cassert>
#include <iostream>
struct Playback { bool file = false, armed = false; bool open() const { return file; } } g_play;
namespace demo_native { bool playing = false; bool native_playing() { return playing; } }
namespace live_match_diagnostics { std::atomic<unsigned> write_enter{}, write_leave{}; }
int calls = 0, client = -1;
void original(int n) { ++calls; client = n; }
'''
tests = r'''
int main() {
    CL_WritePacket_orig = original;
    for (bool file : {false, true}) for (bool armed : {false, true})
    for (bool native : {false, true}) for (int n : {0, 1}) {
        g_play.file = file; g_play.armed = armed; demo_native::playing = native;
        calls = 0; client = -1;
        live_match_diagnostics::write_enter = live_match_diagnostics::write_leave = 0;
        cl_write_packet_stub(n);
        const bool suppressed = file && armed;
        const bool live = n == 0 && !file && !native;
        assert(calls == (suppressed ? 0 : 1));
        assert(client == (suppressed ? -1 : n));
        assert(live_match_diagnostics::write_enter == (!suppressed && live ? 1u : 0u));
        assert(live_match_diagnostics::write_leave == live_match_diagnostics::write_enter);
    }
    std::cout << "Live/native-demo packet forwarding and custom replay suppression preserved\n";
}
'''
Path("obj/test_live_flow.cpp").write_text(prelude + source[start:end] + tests, encoding="utf-8")
