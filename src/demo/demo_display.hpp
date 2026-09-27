#pragma once

// com_maxfps is registered by WWII as a secure int with a 0..250 domain.
// Once it exists, this module widens its real domain to 0..1000 so the
// engine console and the mod UI use the same range. The value is read
// through the engine's secure-int getter and set through its normal command
// path; writing the raw encoded storage would corrupt it.
namespace demo_display
{
	void init();
	void tick();

	// -1 until com_maxfps is registered.
	[[nodiscard]] int fps_cap();
	[[nodiscard]] bool fps_cap_available();

	// 0 means uncapped. The mod's chosen cap is saved for future launches.
	void set_fps_cap(int fps);
}
