#pragma once

#include <cstdint>

// =====================================================================
//  DEMO CAMERA — framing controls for filming
// =====================================================================
//
// Everything here was recovered from the S2 IDB on 2026-08-31 (see
// CLAUDE.md). Three of the four need no code patching at all; the fourth
// repoints two single instructions using the same technique already
// proven for the free-camera speed.
//
//   FOV          ⛔ CORRECTED 2026-09-15. `cg_fov` is a real dvar, but it is
//                registered 50..100 and its domain callback (sub_3E750 ->
//                sub_45830) narrows the top further, so `cg_fov 120` or
//                `cg_fov 30` is silently REJECTED. A dvar slider could never
//                do cinematic framing.
//                The demo FOV is instead applied at sub_48460, the engine's
//                FINAL fov (in third/free camera it takes the cg_fov chooser
//                sub_45760 outright). One hook covers first, third and free
//                camera in BOTH demo systems. Live play is left alone.
//                Priority while a demo plays: dolly key > override > engine.
//
//   3rd person   CL_Demo_UpdatePlaybackView @0x9135D0 raises the camera by
//                +35.0 (instruction 0x9137C1) and CL_Demo_TraceViewForward
//                @0x913830 pulls it back 85.0 (instruction 0x913939).
//                BOTH constants are SHARED -- dword_B378E8 has 17 xrefs and
//                dword_B2D3A4 has 12 -- so the constants must NOT be
//                patched. We repoint the two INSTRUCTIONS instead, exactly
//                as the free-camera speed already does.
//
//   Roll         both camera movers write roll to cg+2355668 from usercmd
//                angle[2], which the mouse never produces. So the field is
//                plumbed and simply never fed: writing it after the mover
//                runs gives a dutch angle.
//
//   Screenshot   CL_Demo_CaptureScreenshot @0x916740 is the engine's own
//                TILED high-resolution capture (240x180 tiles at 1440). It
//                is action 17, which we block by default because it freezes
//                playback and moves the camera -- which is exactly what a
//                tiled capture does. Exposed here as a deliberate action
//                rather than a surprising key.

namespace demo_camera
{
	void init();

	// ---- field of view ----------------------------------------------
	// DEMO fov: what is on screen while a demo plays (dolly key, else the
	// override, else the engine's own value). Same in native and custom.
	[[nodiscard]] float fov();
	void set_fov(float degrees);          // override, clamped 5..170
	void clear_fov();                     // back to the engine's own fov
	[[nodiscard]] bool fov_overridden();
	[[nodiscard]] bool fov_available();   // the CG_CalcFov hook is live
	// Called by the dolly every frame it drives. Expires by itself.
	void set_dolly_fov(float degrees);

	// LIVE-PLAY fov: the cg_fov dvar, which the engine limits to 50..100.
	// -1 when it cannot be read.
	[[nodiscard]] float game_fov();
	void set_game_fov(float degrees);

	// ---- third person framing ---------------------------------------
	[[nodiscard]] bool framing_patched();
	[[nodiscard]] float* height();        // stock 35.0, nullptr if unpatched
	[[nodiscard]] float* distance();      // stock 85.0, nullptr if unpatched

	// ---- camera roll -------------------------------------------------
	[[nodiscard]] float roll();
	void set_roll(float degrees);         // clamped -180..180

	// Called from the game window's WM_MOUSEWHEEL handler (demo_gui.cpp).
	// notches is the raw delta / WHEEL_DELTA (so 1.0 = one detent). Plain
	// wheel adjusts roll, Alt+wheel adjusts FOV -- matching the MWR reference
	// this was modelled on. No-op outside free camera.
	void on_wheel(float notches, bool alt_held);

	// Called from dolly's EXISTING CL_Demo_FreeCameraMove hook, BEFORE the
	// engine's own mover. Feeds roll into cl.viewangles (the usercmd side),
	// which is the source the mover derives cg's freecam angles from -- see the
	// long note in the .cpp for why the post-write alone was not enough.
	void apply_before_camera_move();

	// Called from dolly's EXISTING CL_Demo_FreeCameraMove hook, after the
	// engine's own mover and after bonecam. Never installs a hook of its own
	// (RULE A3.1 -- that address must not be hooked twice).
	void apply_after_camera_move();

	// Rebuilds the refdef view AXIS from the camera angles with our roll in it.
	// Called from the CG_CalcFov hook (view setup) -- the angles were proven to
	// carry roll correctly while the picture stayed level, so the axis is what
	// drops it. Idempotent: deriving rather than rotating, so repeated calls in
	// one frame cannot compound into a spin.
	void apply_roll_to_view_axis();

	// ---- high-resolution screenshot ----------------------------------
	void screenshot();
}
