#pragma once
// =============================================================================
//  demo/demo_native.hpp — S2's OWN engine demo system (main/demo/*.demo)
//
//  This is deliberately SEPARATE from our custom theater (demo_playback.*).
//  Nothing here touches the custom path; it only drives the engine's native
//  cl_demo_play and watches Com_Error.
//
//  Native demo facts established 2026-08-08:
//    - CL_Demo_Play_f is wrapped in `if (!com_sv_running)`. With a server up it
//      does NOTHING and prints NOTHING, so we check and report that ourselves.
//    - The engine appends ".demo" and prefixes "demo/", so we pass the stem.
//    - Header +0x04 is sizeof(header) == 79384 and must match this build.
// =============================================================================

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace demo_native
{
	// Serializes the dolly marker renderer with synchronous native seeks.
	std::mutex& seek_render_mutex();
	void init();

	// main/demo — where the engine keeps its own demos. Not our demos folder.
	std::optional<std::filesystem::path> native_demos_directory();

	void refresh();
	const std::vector<std::filesystem::path>& files();
	int selected();
	void set_selected(int index);

	// True while a server is running, in which case native playback is a no-op.
	bool server_running();

	// Native recording is AUTOMATIC: the engine's CL_Demo_StartRecord runs at
	// every cgame init and we force its permission gate. This is that switch.
	// Takes effect on the NEXT connect, because that is when the engine asks.
	bool auto_record();
	void set_auto_record(bool on);

	// Is the ENGINE writing a .demo for this match right now? The engine's own
	// append gate (sub_910440: clc[client].demoState == 1).
	bool native_recording();
	// Finalise and close the current native recording NOW, through the engine's
	// own CL_Demo_StopRecord -- the same teardown a disconnect runs, so the file
	// gets its footer (and our public-match repair + rename). CLIENT THREAD ONLY:
	// call it from a console command, never from the GUI thread.
	bool stop_native_recording();

	// Issues the engine's own cl_demo_play. Returns false (and explains) when
	// the engine would silently refuse.
	// Repair a PUBLIC-match recording whose footer lacks netconststring type 21
	// (the GSC script-string table). Without it playback cannot resolve the
	// type-21 indices the demo's own snapshots carry, and it stalls at connstate
	// 9 or dies with Com_Error "4780".
	//
	// Normally this runs by itself when recording stops. It is exposed for the
	// case where the game was killed mid-match, so CL_Demo_StopRecord never ran.
	//
	// Returns a human-readable result either way; `ok` distinguishes "repaired"
	// from "nothing to do" / "could not".
	std::string repair_selected(bool& ok);

	bool play_selected();
	bool play(const std::filesystem::path& path);

	// Com_Error watch — records code, message and the ENGINE return address of
	// every Com_Error while the game runs. This is how we catch the asset-limit
	// error, whose string reference is Arxan-computed and invisible statically.
	struct error_record
	{
		int code{};
		std::string message;
		std::uint64_t ida_addr{};   // caller, converted to an IDA address
	};

	const std::vector<error_record>& errors();
	void clear_errors();

	// ---- native-playback interception -------------------------------------
	// CL_Demo_ReadDemoMessage (IDA 0x9188C0) is ALREADY hooked by the custom
	// theater (demo_playback.cpp, cl_demo_read_message_stub). A second
	// Hook::create on the same target returns MH_ERROR_ALREADY_CREATED, which
	// Hook::create treats as success while MinHook leaves `original` null — so
	// the duplicate detour is silently discarded and never runs.
	//
	// So instead of hooking again, the theater's existing stub calls these two
	// on the native-playback path (theater not armed). Purely additive.

	// Returns true if the read was fully handled; *result is what to return.
	// Used to stop CL_SetCGameTime's priming loop (0x86D97) from reading past
	// the type-0 terminator once the engine has already signalled end-of-stream.
	bool intercept_read(unsigned int local_client_num, int* result);

	// Bookkeeping after the ORIGINAL ran: counts packets, logs connstate
	// transitions, and latches end-of-stream on a 0 return.
	void note_read_result(unsigned int local_client_num, int result);

	// ---- the gamestate repair ---------------------------------------------
	// ROOT CAUSE (proven offline over all five shipped demos, 2026-08-08):
	// packet 0 of every demo carries the svc_gamestate WITHOUT the 2-byte length
	// prefix that CL_ParseServerMessage unconditionally reads. The engine
	// therefore consumes the first two bytes of gamestate as a length, gets
	// 0xDAA9, MSG_ReadShort SIGN-EXTENDS it to -9559, and sub_DCAB0's
	// `if (8 * srcLen <= 0)` early-out returns SUCCESS with ZERO bytes decoded.
	// The gamestate vanishes with no error, connstate never leaves 5, every
	// snapshot is discarded, and the priming loop eats the whole file.
	//
	// Called from demo_recording.cpp's existing CL_ParseServerMessage hook
	// (RULE A3.1: that address is already hooked — never add a second one).
	// Returns true if the message was repaired. No-op unless a native demo is
	// playing, so live play is never touched.
	bool repair_gamestate_message(void* msg);

	// True between a successful play() and end-of-stream/abort.
	bool native_playing();

	// cl.snap.serverTime, the clock the keyframe ring and every seek in this
	// file already use. -1 when unavailable. The dolly stamps its points on
	// this, so seeking moves the dolly camera correctly for free.
	int demo_time();

	// cl.serverTime (dword 6368) — the CONTINUOUS demo clock, recomputed every
	// frame by CL_SetCGameTime as serverTimeDelta + cls_realtime.
	//
	// Use this for ANYTHING THAT ANIMATES. demo_time() above is
	// cl.snap.serverTime, a step function that only moves when a snapshot is
	// consumed; sampling it per frame makes motion jump once per snapshot rather
	// than move. Seeking must keep using demo_time(), because keyframe slot times
	// are stored from that same field.
	int demo_time_smooth();

	// True only at CA_ACTIVE, i.e. cgame fully up. Anything reading cg from
	// outside the engine's own call graph must gate on this: during a seek the
	// engine's cg pointer is NULL while the renderer back-pointer is still
	// stale-but-set, and acting on that disagreement crashed the game.
	bool cgame_active();

	// True while the engine is in clip-capture mode (PlaybackData+3336304 == 1).
	// Review overlays that must not appear in cinematic capture gate on this.
	// F3 hi-res screenshot captures the game's own buffers, not ImGui; this
	// covers a capture path that shares Present.

	// Theater camera mode: 0 first person, 1 third person, 2 free. -1 when
	// unavailable. Free camera is the ONLY mode in which CG_PredictPlayerState
	// routes to CL_Demo_FreeCameraMove, so it is the only mode a dolly can
	// drive — and the only one it can capture a pose from.
	int camera_mode();

	// The engine's own CL_Demo_SetCameraMode. Returns true if the mode took.
	bool set_camera_mode(int mode);

	// The ENGINE's own demo speed, read from PlaybackData+28.
	//
	// CL_Demo_Play_f writes 1.0f there when playback starts and the end-of-stream
	// branch of CL_Demo_ReadDemoMessage resets it to 1065353216 (also 1.0f) -- a
	// float initialised at play and reset at stop is the demo timescale, and the
	// engine's own up/down-arrow speed control moves it.
	//
	// Returns 1.0f when not playing, when the pointer is unusable, or when the
	// value is not a sane speed, so callers can multiply unconditionally.
	float engine_timescale();

	// The ENGINE's own demo pause state, via CL_Demo_IsPaused @0x916E30:
	//     PlaybackData && cl_demo_pause->value && !PlaybackData[10]
	// Native playback has its own pause command, so the HUD must freeze on that
	// rather than on the theater's flag. False when not playing.
	bool engine_paused();

	// Free-camera movement speed. Stock is 190.0 and is baked into the engine as a
	// constant, which is why the theater freecam flies so fast and has no control.
	// See demo_native.cpp for how it is made adjustable.
	float* freecam_speed();
	bool freecam_speed_patched();

	// Call immediately before/after the ORIGINAL CL_Demo_FreeCameraMove call in
	// dolly's shared hook stub -- that is the one place per frame that reads
	// *freecam_speed(). Shift = sprint (x4), Alt = slow/precise (x0.25). A no-op
	// pair when the speed patch never took (returns/restores the base value
	// unchanged either way, so it is always safe to call).
	float begin_speed_modifier();
	void end_speed_modifier(float base);

	// PROBE ONLY. Polls the viewmodel `hide` byte (cg+0x256C2B) every rendered
	// frame during NATIVE playback and logs its transitions, to answer "when
	// does the gun pop in". Polled rather than hooked because demo_playback.cpp
	// already hooks every function on that path (RULE A3.1), and because its
	// existing `hide 1->0` probe is gated on the theater being armed and so is
	// silent on the native path. Call once per frame.
	void watch_viewmodel();

	// Withhold LUI model 100 (cg.Shared.connectionStateActive) while set.
	//
	// sub_357AD0 publishes that model OUTSIDE its `connstate >= CA_ACTIVE` gate, and
	// LUI drives the in-game menu from it. A theater rewind deliberately drops
	// connstate ACTIVE -> PRIMED (so CL_SetCGameTime re-promotes through
	// CL_FirstSnapshot) by writing the field directly, which bypasses
	// CL_SetClientState -- but this publisher POLLS connstate every frame and opens
	// the menu anyway. Holding the publish leaves LUI's model at its previous value.
	//
	// MUST be cleared once connstate is back at CA_ACTIVE. The theater clears it on
	// the ACTIVE transition and also on stop/close, so an aborted rewind cannot leave
	// the menu permanently suppressed.
	void hold_connection_state_active(bool on);
	bool connection_state_held();

	// ---- THEATER TRANSPORT ------------------------------------------------
	//
	// S2 ships a complete theater ENGINE and an incomplete front-end (PROVEN
	// 2026-08-10): the uiScript dispatcher implements PlayDemo / PreviewSegment
	// / MoveSegment / DeleteSegment / SwitchSegmentTransition, and every
	// PLATFORM_DEMO_DVR_* prompt is drawn by CG_OwnerDraw — but the timeline
	// materials (demo_timeline_solid / _faded / _arrow / _bookmark, demo_play)
	// have NO code references, and the LUI screen that would drive it all lives
	// in Lua inside the fastfiles.
	//
	// So rather than revive the menus, these call the engine's OWN action
	// handler, CL_Demo_HandleAction. Nothing here is a reimplementation; the
	// ids are the cases in that function's switch.
	// ---- CLIPS -------------------------------------------------------
	// The engine ships a complete segment/clip system with no front-end:
	//   cl_demo_savesegment 1   mark IN   (records the current time)
	//   cl_demo_savesegment 0   mark OUT  (appends the segment)
	//   cl_demo_previewsegment <n>        seek to segment n
	//   cl_demo_previewclip               play the marked clip
	//   cl_demo_deleteclip                clear every segment
	// All are gated on clc+262752 == 2, i.e. a demo is playing -- which is
	// exactly when we offer them. Segment count lives at playbackData+6096400.
	int  clip_count();
	void clip_mark_in();
	void clip_mark_out();
	void clip_preview();
	void clip_clear();

	void transport(int action);
	void toggle_pause();     // 1  — cl_demo_pause
	void cycle_camera();     // 4  — first / third / free
	// Seeking does NOT use the engine's actions 18/19. Their selection function
	// (sub_915A10) returns the CURRENT keyframe index unless the current one is
	// newer than a fixed 800 ms cutoff or is registered in a 32-entry baseline
	// list — so "rewind" jumps to where you already are. We choose the keyframe
	// by time and drive the engine's own ProcessKeyFrameJump, which is a full
	// seek: it repositions the demo file, reparses the gamestate, restores the
	// 79,356-byte state block, replays buffered messages and resyncs the clock.
	void seek_back();
	void seek_forward();
	// Queues an absolute seek (safe from any thread).
	void seek_to_time(int ms);
	// The absolute seek itself: keyframe jump if backward, then the exact
	// remainder through the engine's feed, pause state untouched. CLIENT THREAD
	// ONLY -- call it from a console command.
	bool seek_absolute_now(int ms, bool resume_after = false);
	bool seek_in_progress();
	void cancel_restart_seek();
	// Ends the native session once the engine's demoState leaves 2. Once a frame.
	void poll_session();

	// ⭐ FAST FORWARD, ported from IWXMVM (reallyluckyy/IWXMVM) 2026-08-11.
	//
	// Their whole fast-forward is `*cls.realtime += ticks`, and it needs NO
	// keyframes -- which is exactly the gap S2 had, since keyframes are written
	// as playback passes and so forward seeking could never pass where you had
	// already been.
	//
	// The same mechanism is present in S2 verbatim. CL_SetCGameTime derives
	// cl.serverTime = cls_realtime + cl.serverTimeDelta, then runs a feed loop
	// that reads demo packets until cl.serverTime catches cl.snap.serverTime. So
	// raising cls_realtime by N ms advances playback by N ms through the
	// engine's own pump.
	//
	// FORWARD ONLY, and the engine says why: cl.serverTime is clamped against
	// cl.oldFrameServerTime, so lowering cls_realtime does nothing. That is the
	// same asymmetry that makes IWXMVM restore a whole gamestate to rewind while
	// fast-forward is one line -- backward stays on S2's keyframes.
	void skip_forward_ms(int ms);

	// What can actually be seeked to right now. Keyframes are written as
	// playback passes, so `last_ms` never runs ahead of where you have been —
	// forward seeking only works back over ground already played.
	struct SeekRange
	{
		bool valid = false;
		int  keyframes = 0;
		int  first_ms = -1;
		int  last_ms = -1;
		int  now_ms = -1;
	};
	SeekRange seek_range();
	void speed_up();         // timescale +0.1, engine clamp 4.0
	void speed_down();       // timescale -0.1, engine clamp 0.1
	// Writes PlaybackData+28 directly (the engine's own timescale field) rather
	// than routing through CL_Demo_HandleAction, whose entry guards silently
	// swallow the action in several states.
	void set_timescale(float v);

	// ⛔ DO NOT USE cg_draw2D FOR THE STRANDED DIVISION WIDGETS.
	// Its global (off_8B0DA20) has three readers: one is Arxan integrity noise
	// in CG_DrawActiveFrame, and the other two (sub_E7980, sub_EB030) each gate
	// an ENTIRE native draw function — hitmarkers, killfeed, score popups. It is
	// a blanket switch, and the stranded widgets are LUI, drawn elsewhere.
	//
	// The stranded widgets come from a LUI model subscription that throws:
	//   ui/s2/mphud_uc.lua -> equippedperks_uc.lua -> divisions_utils.lua
	// dispatched by sub_36A550 via lua_pcall. `divisionsGlobalOverhaul` is a
	// bool dvar (default 1) whose dvar_t* is never read natively — only from
	// Lua — so it is the targeted candidate for steering that code elsewhere.
	// UNPROVEN; opt-in only, since it is a gameplay-data flag.
	// (Tested in game 2026-08-10: no effect on the stranded widgets.)

	// THE TARGETED LEVER: withhold LUI HUD model 86 (cg.hud.currentDivision)
	// while a native demo plays.
	//
	// sub_357AD0 republishes ~70 LUI HUD models every frame through
	// sub_3573B0(client, descIndex, "cg.hud.<name>"); model 86 is the one
	// equippedperks_uc.lua -> divisions_utils.lua subscribes to before it throws.
	// Suppressing it touches nothing else — unlike cg_draw2D, which gates two
	// whole native draw functions.
	void suppress_division_model(bool on);

	// MINIMAL HUD — keep only hitmarkers, killfeed and score popups.
	//
	// Those three do not share a system, which is exactly why this works:
	//   hitmarkers  = NATIVE (crosshair path)          -> untouched
	//   killfeed    = LUI but EVENT-driven             -> untouched
	//   score popup = LUI but EVENT-driven             -> untouched
	//   the rest    = LUI and MODEL-driven via sub_3573B0 -> withheld
	//
	// NOT cg_draw2D (gates two whole native draw functions, takes the hitmarkers
	// with it) and NOT the CG_ShouldDrawHud twin (MWR shows it also gates
	// CG_DrawCrosshair). Enable before playback starts — withholding a publish
	// stops a model updating, it does not tear down an already-built element.
	bool hud_minimal();
	void set_hud_minimal(bool on);

	// 0..1 through the packet stream, from the FILE cursor (PlaybackData+24,
	// which CL_Demo_Read accumulates) between the header size and the footer.
	// -1 when not playing or not determinable.
	float playback_progress();

	// State of the engine's keyframe ring (250 slots, 48-byte stride, at
	// PlaybackData+2176592; the scan index is at +2188584).
	//
	// WHY THIS EXISTS: both seek functions return -1 when no slot matches, and
	// their callers then do nothing at all — silently. "Seek is broken" and
	// "the ring is empty" are indistinguishable from outside the engine, so
	// this is the only way to tell them apart without a debugger.
	struct KeyframeRing
	{
		bool valid = false;
		// Slots the ENGINE will consider: sub_915A10 requires slot+20 > 0 before
		// it even looks at the timestamp. This is the number that matters.
		int  used = 0;
		// Slots that merely carry a plausible timestamp (slot+0 >= 0). If `timed`
		// is high while `used` is 0, keyframes are being written but without
		// whatever slot+20 represents — and every seek will skip them.
		int  timed = 0;
		int  current_index = -1;
		int  min_time = -1;
		int  max_time = -1;
	};
	KeyframeRing keyframe_ring();

	// Release the frontend + previous level exactly as the engine's own map
	// spawn does, for callers that issue a bare `map <name>`.
	//
	// WHY: a `map` command does NOT release anything. The frontend (ui_mp) holds
	// roughly 36,890 of the 40,192 image slots, so only ~3,300 remain and a map
	// needs ~2,600-3,500 — loading one on top of the hub overflows the pool and
	// produces "Exceeded limit of 40192 'image' assets".
	//
	// MUST be called BEFORE the map load begins (that is what sub_48C0C0 does).
	// Releasing ui_mp after the loading screen has restarted LUI is a Lua
	// use-after-free — it crashed gibraltar once already.
	//
	// Honours the demo_release_frontend / demo_release_level toggles.
}
