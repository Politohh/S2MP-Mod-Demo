#pragma once

#include <string>

struct IDXGISwapChain;

// =============================================================================
//  DEMO CAPTURE — back buffer -> ffmpeg -> ProRes .mov
// =============================================================================
//
// The old Call of Duty `avidemo` idea, done with a modern encoder: every frame
// the game presents is read back off the swap chain and pushed down a pipe to
// ffmpeg, which writes ProRes 422 straight to disk. No screen recorder, no
// desktop compositor in the way, no second encode — what the engine drew is
// what lands in the file, at full precision, ready to cut.
//
// WHERE IT RUNS. demo_gui's EXISTING Present hook calls on_present() BEFORE it
// draws the ImGui overlay (RULE A3.1 — the swap chain is not hooked twice).
// That ordering is the whole reason the recording is clean game footage with no
// tool window in it, so it must not be moved below the overlay draw.
//
// ⚠ WHAT THIS IS NOT, YET: TRUE FRAME LOCK.
//
// A real avidemo steps the engine clock by exactly 1/fps per rendered frame, so
// a scene that renders at 8 fps still produces smooth output. Doing that here
// means hooking Sys_Milliseconds (0x7B1290, confirmed unhooked) and feeding the
// whole engine a synthetic clock — and demo_game.hpp already documents a render
// thread that Sleep-SPINS until cl_serverTime reaches a target. Feeding that
// thread a clock that only advances when it renders is a deadlock waiting to
// happen, and it is not something to ship unproven.
//
// So this captures every PRESENTED frame and tags the stream at the output rate
// below. The workflow that gets you smooth footage today is the one the slow
// motion work just unlocked: slow the demo to a stable rate (the tester found
// 0.05 to be the practical floor), let the engine render comfortably, and
// capture at 60. Frame lock
// proper is the next step, and it is a separate, testable change.
namespace demo_capture
{
	void init();

	// From demo_gui's Present hook, BEFORE the overlay is drawn. No-op unless
	// recording. Safe to call every frame.
	void on_present(IDXGISwapChain* swap);

	[[nodiscard]] bool recording();
	[[nodiscard]] std::string status();

	// `name` may be empty, in which case the file is named from the clock.
	bool start(const std::string& name);
	void stop();
}
