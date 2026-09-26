#pragma once

// =====================================================================
//  DEVELOPER MODE
// =====================================================================
//
// S2MP-Mod carries two audiences in one binary:
//
//   the PRODUCT   record a demo, play it back, fly a camera, film it.
//   the WORKBENCH ~60 investigation probes and half a dozen engine
//                 subsystems (matchmaking, hosting, bots, HUD models)
//                 that exist because this is a reverse-engineering
//                 project.
//
// Everything is KEPT. Only the workbench is hidden, so the default
// surface is the product.
//
// Modules do not test the flag directly at init time -- they hand us a
// callback with on_enable(). That way turning developer mode on at
// RUNTIME registers the extra commands immediately, and turning it on
// via the persisted file registers them at boot. A callback fires at
// most once, so a command can never be registered twice (the engine's
// Cmd_AddCommandInternal has no duplicate check -- a second entry just
// silently shadows the first).
//
// Persisted to <mod dir>/dev_mode so it survives a restart.

namespace dev_mode
{
	// Read the persisted flag. MUST run before any module init() so that
	// on_enable() callbacks fire in the right order.
	void init();

	[[nodiscard]] bool enabled();

	// Turn developer mode on or off and persist it. Turning it ON fires
	// every registered callback that has not fired yet. Turning it OFF
	// hides the UI but leaves already-registered commands in place --
	// unregistering them mid-session buys nothing and risks the engine's
	// command list.
	void set(bool on);

	// Register extra commands / features that belong to the workbench.
	// Fires immediately if developer mode is already on.
	void on_enable(void (*fn)());

	// Convenience for the common case: a console command that should only
	// exist in developer mode. Same contract as GameUtil::addCommand, so a
	// module can gate a command by changing one call.
	void add_command(const char* name, void (*fn)());

	// Registers the `s2_dev` command. Called once from Console.cpp.
	void register_command();
}
