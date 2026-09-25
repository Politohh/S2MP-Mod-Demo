#pragma once

#include <string>
#include <cstdint>

struct IDXGISwapChain;
struct ID3D11Texture2D;

// =============================================================================
//  DEMO CAPTURE — back buffer -> ffmpeg -> ProRes .mov
// =============================================================================
//
// The old Call of Duty `avidemo` idea, done with a modern encoder: every frame
// the game presents is read back and pushed down a pipe to
// ffmpeg, which writes ProRes 422 straight to disk. No screen recorder, no
// desktop compositor in the way, no second encode — what the engine drew is
// what lands in the file, at full precision, ready to cut.
//
// WHERE IT RUNS. demo_gui's existing Present hook calls on_present() before
// the ImGui overlay. When compatible ReShade add-on support is available, its
// finish-effects callback supplies the actual post-effect render target to
// on_post_effect_texture() instead. Both routes avoid baking in the tool UI.
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
// So this captures every PRESENTED frame and tags the stream at the selected
// constant output rate. Uneven source delivery or readback pressure can cause
// exported motion to jolt even when the live dolly preview looks smooth; the
// capture stop log reports timing and queue pressure to diagnose this. True
// frame lock is a separate, testable change.
namespace demo_capture
{
	void init();

	// From demo_gui's Present hook, BEFORE the overlay is drawn. No-op unless
	// recording. Safe to call every frame.
	void on_present(IDXGISwapChain* swap);
	// ReShade's finish-effects callback supplies its actual post-effect target.
	void on_post_effect_texture(ID3D11Texture2D* texture);
	void set_post_effect_capture(bool enabled);
	[[nodiscard]] bool post_effect_capture();

	[[nodiscard]] bool recording();
	[[nodiscard]] std::uint64_t frame_count();
	[[nodiscard]] std::uint64_t dropped_count();
	[[nodiscard]] std::string status();

	// `name` may be empty, in which case the file is named from the clock.
	bool start(const std::string& name);
	void stop();
}
