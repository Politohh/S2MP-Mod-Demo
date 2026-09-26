#include "pch.h"
#include "demo.hpp"

#include "demo/dolly.hpp"
#include "demo/bonecam.hpp"
#include "hud/nametags.hpp"
#include "hud/aimassist.hpp"
#include "hud/dynamic_crosshair.hpp"
#include "hud/wii_aim.hpp"
#include "demo/demo_gui.hpp"
#include "demo/demo_native.hpp"
#include "demo/demo_playback.hpp"
#include "demo/demo_camera.hpp"
#include "demo/demo_capture.hpp"
#include "net/cinebot.hpp"
#include "demo/demo_display.hpp"
#include "demo/demo_player.hpp"
#include "demo/demo_recording.hpp"
#include "demo/demo_timescale.hpp"
#include "demo/theater_camera.hpp"


#include "hud/broadcaster.hpp"

#include "net/bots.hpp"
#include "net/dlc.hpp"
#include "net/force_host.hpp"
#include "net/hidden_maps.hpp"
#include "net/server_browser.hpp"

#include "Console.hpp"

namespace demo
{
	void init()
	{
		demo_recording::init();
		demo_playback::init();
		// Engine's own demo system + Com_Error watch. Independent of the custom
		// theater above — it only drives cl_demo_play and observes errors.
		demo_native::init();
		// One list, one transport, dispatching to whichever engine owns the
		// file. Must come after BOTH demo systems so it can query them.
		// Filming controls: FOV, third-person framing, roll, screenshot.
		// After demo_native -- it repoints two instructions and uses
		// demo_native::transport() for the engine screenshot.
		demo_camera::init();
		// Frame-rate cap unlocked past the engine's own 250 ceiling. No
		// dependency on either demo system -- works in live play too.
		demo_display::init();
		demo_player::init();
		// ProRes capture. Registers commands only -- it installs no hook of its
		// own and rides demo_gui's existing Present hook, so the order here is
		// free. Nothing happens until demo_capture_start.
		demo_capture::init();
		demo_timescale::init();
		theater_camera::init();
		// Dolly camera for native playback. Must come after demo_native::init()
		// — it drives through demo_native's demo clock and camera-mode accessors.
		dolly::init();
		// Rides dolly's CL_Demo_FreeCameraMove hook, so it must come after it.
		bonecam::init();
		nametags::init();
		aimassist::init();
		// MWII-style crosshair sway. One hook on CG_CalcCrosshairPosition, whose
		// five callers are ALL draw paths — so it moves the reticle only, never
		// aim or targeting. Off by default; works in live play and in demos.
		dynamic_crosshair::init();
		// Wii-style pointer aiming. Commands only -- it rides the existing
		// CL_CreateCmd / UpdateLocalPlayerState / CalcCrosshairPosition /
		// R_EndFrame stubs and installs no hook of its own. Off by default.
		wii_aim::init();
		// The server browser. Deliberately hook-free: it only reads the
		// engine's own LAN server table and issues console commands, so it
		// cannot affect matchmaking the way the deleted force_host did.
		server_browser::init();
		dlc::init();
		// Minimal HUD, driven by the game's OWN broadcaster settings. Installs
		// no hooks; its once-per-frame latch is a call-out from demo_native's
		// existing CG_PublishHudModel stub. Off by default.
		broadcaster::init();
		force_host::init();
		// Bot renaming. Hooks the engine's own SV_AddBot, which a real player's
		// connection cannot reach — so it structurally cannot touch a human.
		bots::init();
		cinebot::init();
		// Puts mp_house (Groesten House) into the GAME's own private-match map
		// picker -- hooks GameInfo_UpdateArenas and appends to its own table
		// after the real mp/mapLoad.csv-driven list has been built.
		hidden_maps::init();
		demo_gui::init();
		Console::printf("[demo] theater initialized");
	}
}
