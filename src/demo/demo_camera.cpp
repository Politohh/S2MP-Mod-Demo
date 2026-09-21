#include "pch.h"
#include "demo/demo_camera.hpp"

#include "Console.hpp"
#include "GameUtil.hpp"
#include "Hook.hpp"
#include "demo/demo_game.hpp"
#include "demo/demo_native.hpp"
#include "demo/demo_playback.hpp"
#include "demo/theater_camera.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace demo_camera
{
	namespace
	{
		// ---- addresses (RULE A1: the arithmetic is written out) ---------
		//
		//   cg_fov dvar ptr        IDA 0x11111B8 - 0x1000 = 0x11101B8
		//   eye height instr       IDA 0x9137C1  - 0x1000 = 0x9127C1
		//   pull-back instr        IDA 0x913939  - 0x1000 = 0x912939
		//   freecam roll field     cg + 2355668  (angles[2])
		//
		// S2 dvar layout, proven from Dvar_SetBool @0xB1FD0:
		//   dvar+12 = type, dvar+16 = current value.
		//
		// RULE A14: the _b literal is NEVER cached at namespace scope -- `base`
		// is assigned in init(), so a file-scope initialiser would resolve to
		// the bare literal. It is applied inside the accessor below.
		constexpr std::uintptr_t CG_FOV_DVAR_LITERAL = 0x11101B8;
		constexpr std::size_t    CAM_ANGLES_OFF = 2355660;   // pitch, yaw, roll
		constexpr std::size_t    CAM_ROLL_OFF = CAM_ANGLES_OFF + 8;

		constexpr float STOCK_HEIGHT = 35.0f;
		constexpr float STOCK_DISTANCE = 85.0f;

		float g_height_storage = STOCK_HEIGHT;
		float g_distance_storage = STOCK_DISTANCE;
		float* g_height = &g_height_storage;
		float* g_distance = &g_distance_storage;
		bool g_framing_patched = false;

		float g_roll = 0.0f;

		// Roll instrumentation -- written on the client thread inside the camera
		// hook, read by demo_roll_probe on the console thread. Relaxed atomics:
		// these are a diagnostic, and a torn read of a counter is not worth a lock
		// on a per-frame path.
		std::atomic<float> g_roll_pre{ 0.0f };          // what the mover left
		std::atomic<float> g_roll_post{ 0.0f };         // what our write left
		std::atomic<std::uint64_t> g_roll_writes{ 0 };  // times we wrote it

		// -----------------------------------------------------------------
		//  FIELD OF VIEW — overridden at the engine's FINAL fov function
		// -----------------------------------------------------------------
		// PROVEN 2026-09-15 from the S2 IDB:
		//
		//   CG_RegisterFovDvars @0x507E0 registers cg_fov with default 65.0,
		//   min 50.0, max 100.0 (dword_B37914 / B37904 / B37930), then REPLACES
		//   its domain callback with sub_3E750, which accepts a value only
		//   inside sub_45830's range: floor 50, ceiling somewhere in 70..100.
		//   Anything outside is rejected by Dvar_SetVariant, silently. The old
		//   slider offered 45..160 through `cg_fov N`, so a wide shot (120) or a
		//   zoomed one (20-40) simply never applied -- "FOV doesn't work".
		//
		//   sub_48460 is the final fov (callers: CG_ApplyFov, sub_33CF0,
		//   sub_3F050, sub_2C4E0, sub_4EAD60). In theater third/free camera it
		//   takes sub_45760 (the cg_fov chooser) outright, then clamps to
		//   [min dvar, 170]. So overriding ITS return value is the one place
		//   that covers first, third and free camera in BOTH demo systems, with
		//   no dvar range in the way.
		//
		//   `float __fastcall(unsigned int localClientNum)` -- one arg in ecx
		//   (`mov edi, ecx` at 0x4847B), a real function rather than a forwarder
		//   (RULE A23), return in xmm0 (RULE A17). No retaddr check in its body
		//   (RULE A22), and we never call it ourselves anyway.
		//
		//   sub_48460  IDA 0x48460 - 0x1000 = 0x47460
		constexpr std::uintptr_t CG_CALC_FOV_LITERAL = 0x47460;
		using CG_CalcFov_t = float(__fastcall*)(unsigned int);
		CG_CalcFov_t g_calc_fov_orig = nullptr;
		bool g_fov_hook_ok = false;

		// The engine clamps its own result to 170; 5 is a long lens.
		constexpr float FOV_MIN = 5.0f;
		constexpr float FOV_MAX = 170.0f;

		// 0 = off. Only applied while a demo is playing; live play keeps the
		// engine's own fov.
		std::atomic<float> g_fov_override{ 0.0f };
		// Written every frame by the dolly while it drives. Expires on its own
		// once the dolly stops, so a stale key can never stick.
		std::atomic<float> g_dolly_fov{ 0.0f };
		std::atomic<std::uint64_t> g_dolly_fov_stamp{ 0 };
		constexpr std::uint64_t DOLLY_FOV_TTL_MS = 250;
		// What the engine itself computed last, before any override.
		std::atomic<float> g_engine_fov{ -1.0f };

		[[nodiscard]] bool demo_is_playing()
		{
			return demo_native::native_playing() || demo_playback::is_playing();
		}

		[[nodiscard]] float fresh_dolly_fov()
		{
			const auto stamp = g_dolly_fov_stamp.load(std::memory_order_relaxed);
			if (stamp == 0 || GetTickCount64() - stamp > DOLLY_FOV_TTL_MS)
			{
				return 0.0f;
			}
			return g_dolly_fov.load(std::memory_order_relaxed);
		}

		float __fastcall cg_calc_fov_stub(const unsigned int client)
		{
			const float engine = g_calc_fov_orig(client);
			if (client != 0)
			{
				return engine;
			}
			if (std::isfinite(engine) && engine > 1.0f && engine < 180.0f)
			{
				g_engine_fov.store(engine, std::memory_order_relaxed);
			}
			if (!demo_is_playing())
			{
				return engine;
			}
			if (const float d = fresh_dolly_fov(); d > 0.0f)
			{
				return d;
			}
			const float o = g_fov_override.load(std::memory_order_relaxed);
			return (o > 0.0f) ? o : engine;
		}

		// Resolved inside a function, never at namespace scope (RULE A14).
		// On the Store build this address is not in BuildMap.Store.inc, so _b
		// hands back POISON -- which the readable() check below then declines,
		// leaving FOV reported as unavailable rather than faulting.
		[[nodiscard]] std::uintptr_t CG_FOV_DVAR_LITERAL_b()
		{
			return _b(CG_FOV_DVAR_LITERAL);
		}

		bool readable(const void* p, const std::size_t n)
		{
			// RULE A6: a null check is not enough in this game -- a global
			// belonging to an inactive subsystem can hold arbitrary non-zero
			// junk, and dereferencing it faults.
			if (!p)
			{
				return false;
			}
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
			{
				return false;
			}
			if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
			{
				return false;
			}
			const auto start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const auto end = start + mbi.RegionSize;
			const auto want = reinterpret_cast<std::uintptr_t>(p);
			return want >= start && want + n <= end;
		}

		// -----------------------------------------------------------------
		//  Repoint ONE instruction's rip-relative operand at a float we own.
		// -----------------------------------------------------------------
		// This mirrors demo_native's proven free-camera-speed patch. It is
		// duplicated rather than shared deliberately: that one works, is
		// user-confirmed, and is not worth destabilising for tidiness.
		//
		// It exists because BOTH camera constants are SHARED (dword_B378E8 has
		// 17 xrefs, dword_B2D3A4 has 12), so patching the constant itself would
		// change unrelated engine behaviour. Repointing the instruction touches
		// exactly one call site.
		bool repoint(const char* what, const std::uintptr_t literal,
			const std::uint8_t (&expect)[4], float** slot)
		{
			auto* instr = reinterpret_cast<std::uint8_t*>(literal);
			if (!readable(instr, 8))
			{
				Console::printf("[cam] %s: instruction not readable, left stock", what);
				return false;
			}
			// RULE A4: verify the exact opcode before writing. Arxan puts jumps
			// and thunks all over this image, so "it looks like an instruction"
			// proves nothing.
			if (std::memcmp(instr, expect, sizeof(expect)) != 0)
			{
				Console::printf(
					"[cam] %s: unexpected opcode (%02X %02X %02X %02X), left stock",
					what, instr[0], instr[1], instr[2], instr[3]);
				return false;
			}

			const auto rip = reinterpret_cast<std::uintptr_t>(instr) + 8;
			auto tgt = reinterpret_cast<std::uintptr_t>(*slot);
			std::intptr_t delta = static_cast<std::intptr_t>(tgt)
				- static_cast<std::intptr_t>(rip);

			// Our DLL can load many GB from the game module (measured ~10 GB)
			// and a rip-relative operand only reaches +/-2GB. Allocate a page
			// within reach and keep the float there instead of giving up.
			if (delta > INT32_MAX || delta < INT32_MIN)
			{
				constexpr std::uintptr_t STEP = 0x10000;         // alloc granularity
				constexpr std::uintptr_t REACH = 0x60000000ull;  // well inside 2GB
				float* near_slot = nullptr;
				for (std::uintptr_t off = STEP; off < REACH && !near_slot; off += STEP)
				{
					for (const std::uintptr_t cand : { rip - off, rip + off })
					{
						auto* p = VirtualAlloc(reinterpret_cast<void*>(cand & ~(STEP - 1)),
							sizeof(float), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
						if (p)
						{
							near_slot = static_cast<float*>(p);
							break;
						}
					}
				}
				if (!near_slot)
				{
					Console::printf("[cam] %s: no page within rip range, left stock", what);
					return false;
				}
				*near_slot = **slot;      // carry the current value over
				*slot = near_slot;        // the slider now edits this
				tgt = reinterpret_cast<std::uintptr_t>(near_slot);
				delta = static_cast<std::intptr_t>(tgt) - static_cast<std::intptr_t>(rip);
				if (delta > INT32_MAX || delta < INT32_MIN)
				{
					Console::printf("[cam] %s: still out of range, left stock", what);
					return false;
				}
			}

			DWORD old = 0;
			if (!VirtualProtect(instr, 8, PAGE_EXECUTE_READWRITE, &old))
			{
				Console::printf("[cam] %s: VirtualProtect failed, left stock", what);
				return false;
			}
			const std::int32_t d32 = static_cast<std::int32_t>(delta);
			std::memcpy(instr + 4, &d32, 4);
			VirtualProtect(instr, 8, old, &old);
			FlushInstructionCache(GetCurrentProcess(), instr, 8);
			return true;
		}

		void patch_framing()
		{
			// 0x9137C1  F3 0F 58 05 disp32   addss  xmm0, [rip+d]   (+35.0 eye height)
			// 0x913939  F3 0F 10 0D disp32   movss  xmm1, [rip+d]   (85.0 pull-back)
			static const std::uint8_t addss_xmm0[4] = { 0xF3, 0x0F, 0x58, 0x05 };
			static const std::uint8_t movss_xmm1[4] = { 0xF3, 0x0F, 0x10, 0x0D };

			const bool h = repoint("camera height", 0x9127C1_b, addss_xmm0, &g_height);
			const bool d = repoint("camera distance", 0x912939_b, movss_xmm1, &g_distance);
			g_framing_patched = h && d;

			Console::printf(
				"[cam] third-person framing: %s (height %.0f, distance %.0f)",
				g_framing_patched ? "adjustable" : "stock - one or both patches declined",
				*g_height, *g_distance);
		}

		// ---- commands ---------------------------------------------------

		void cmd_fov()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				const float o = g_fov_override.load(std::memory_order_relaxed);
				Console::printf("demo fov: %s  | engine fov %.0f | hook %s   "
					"(demo_fov <%.0f..%.0f> | demo_fov off)",
					o > 0.0f ? std::format("{:.0f}", o).c_str() : "off",
					g_engine_fov.load(std::memory_order_relaxed),
					g_fov_hook_ok ? "live" : "NOT INSTALLED",
					FOV_MIN, FOV_MAX);
				return;
			}
			const char* a = args->argv[args->nesting][1];
			if (_stricmp(a, "off") == 0 || std::atof(a) <= 0.0)
			{
				clear_fov();
				Console::printf("demo fov: off (the engine's own fov)");
				return;
			}
			set_fov(static_cast<float>(std::atof(a)));
			Console::printf("demo fov: %.0f", g_fov_override.load(std::memory_order_relaxed));
		}

		void cmd_third()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!g_framing_patched)
			{
				Console::printf("[cam] third-person framing is not patchable in this build");
				return;
			}
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("third person: distance %.0f  height %.0f"
					"   (demo_thirdperson <distance> [height]; stock 85 35)",
					*g_distance, *g_height);
				return;
			}
			*g_distance = std::clamp(
				static_cast<float>(std::atof(args->argv[args->nesting][1])), 0.0f, 600.0f);
			if (args->argc[args->nesting] >= 3)
			{
				*g_height = std::clamp(
					static_cast<float>(std::atof(args->argv[args->nesting][2])), -100.0f, 200.0f);
			}
			Console::printf("third person: distance %.0f  height %.0f", *g_distance, *g_height);
		}

		void cmd_roll()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("camera roll: %.1f deg   (demo_roll <-180..180>)", g_roll);
				return;
			}
			set_roll(static_cast<float>(std::atof(args->argv[args->nesting][1])));
			Console::printf("camera roll: %.1f deg", g_roll);
		}

		// RULE A15 -- "roll does nothing" has to be diagnosable without a debugger.
		// Prints the value we want next to BOTH fields it is written into, plus the
		// two gates, so one line says whether the writes are landing and being
		// overwritten, or never running at all.
		void cmd_roll_probe()
		{
			const bool playing = demo_is_playing();
			const bool freecam = theater_camera::get_mode()
				== theater_camera::THEATER_CAMERA_FREECAM;

			float cg_roll = 0.0f;
			bool cg_ok = false;
			if (void* cg = demo_game::cg_globals_for(0))
			{
				const auto* f = reinterpret_cast<const float*>(
					static_cast<char*>(cg) + CAM_ROLL_OFF);
				if (readable(f, sizeof(float)))
				{
					cg_roll = *f;
					cg_ok = true;
				}
			}

			float cmd_roll_val = 0.0f;
			bool cmd_ok = false;
			if (void* cl = demo_game::client_active_for(0))
			{
				const auto* f = reinterpret_cast<const float*>(
					static_cast<char*>(cl) + demo_game::CA_CMD_VIEWANGLES);
				if (readable(f, 12))
				{
					cmd_roll_val = f[2];
					cmd_ok = true;
				}
			}

			Console::printf("[cam] roll probe: want %.1f | demo %s | freecam %s",
				g_roll, playing ? "playing" : "NOT PLAYING",
				freecam ? "yes" : "NO (roll only drives the free camera)");
			Console::printf("[cam]   cl.viewangles[2] (usercmd, written BEFORE mover): %s",
				cmd_ok ? std::format("{:.1f}", cmd_roll_val).c_str() : "unreadable");
			Console::printf("[cam]   cg+2355668 (derived, written AFTER mover):        %s",
				cg_ok ? std::format("{:.1f}", cg_roll).c_str() : "unreadable");
			// The in-hook samples. These are what actually answer the question;
			// the two reads above are taken on the wrong thread at the wrong time.
			const auto writes = g_roll_writes.load(std::memory_order_relaxed);
			Console::printf("[cam]   in-hook: mover left %.1f, we wrote %.1f, %llu write(s)",
				g_roll_pre.load(std::memory_order_relaxed),
				g_roll_post.load(std::memory_order_relaxed),
				static_cast<unsigned long long>(writes));
			if (writes == 0)
			{
				Console::printf("[cam]   -> 0 writes: the camera hook is NOT running. "
					"Nothing about roll can work until that is fixed.");
			}
			else
			{
				Console::printf("[cam]   -> the write lands each frame and the field still "
					"reads 0 from here, so something downstream zeroes it. Roll has to be "
					"applied to the view AXIS instead, not to these angles.");
			}
		}

		void cmd_screenshot() { screenshot(); }
	}

	// =====================================================================

	bool fov_available()
	{
		return g_fov_hook_ok;
	}

	float fov()
	{
		if (const float d = fresh_dolly_fov(); d > 0.0f)
		{
			return d;
		}
		if (const float o = g_fov_override.load(std::memory_order_relaxed); o > 0.0f)
		{
			return o;
		}
		if (const float e = g_engine_fov.load(std::memory_order_relaxed); e > 1.0f)
		{
			return e;
		}
		return game_fov();
	}

	bool fov_overridden()
	{
		return g_fov_override.load(std::memory_order_relaxed) > 0.0f;
	}

	void set_fov(const float degrees)
	{
		// No dvar, so no engine range check: the value goes straight into the
		// engine's final fov while a demo plays.
		g_fov_override.store(std::clamp(degrees, FOV_MIN, FOV_MAX),
			std::memory_order_relaxed);
	}

	void clear_fov()
	{
		g_fov_override.store(0.0f, std::memory_order_relaxed);
	}

	void set_dolly_fov(const float degrees)
	{
		if (!(degrees > 0.0f))
		{
			return;
		}
		g_dolly_fov.store(std::clamp(degrees, FOV_MIN, FOV_MAX), std::memory_order_relaxed);
		g_dolly_fov_stamp.store(GetTickCount64(), std::memory_order_relaxed);
	}

	float game_fov()
	{
		// off_11111B8 is a POINTER to the dvar; the value sits at dvar+16.
		auto* slot = reinterpret_cast<std::uintptr_t*>(CG_FOV_DVAR_LITERAL_b());
		if (!readable(slot, sizeof(std::uintptr_t)))
		{
			return -1.0f;
		}
		const auto dvar = *slot;
		// 32 bytes: the secure decode reads the whole +0x10..+0x1F block.
		if (!readable(reinterpret_cast<void*>(dvar), 32))
		{
			return -1.0f;
		}
		// ⛔ cg_fov is NOT a plain float. Measured live 2026-09-15: its type byte
		// (dvar+12) is 0x0B = DVAR_TYPE_FLOAT_SECURE, and so are cg_fov1,
		// cg_fov_intermission, "3078" and cg_fov_override. Dvar_SetVariant's
		// case 0xB XOR-encodes the value across +16..+31, so reading +16 as a
		// float gave junk, failed the range check below, and FOV reported
		// "unavailable" for as long as this feature has existed.
		const auto type = *reinterpret_cast<const std::uint8_t*>(dvar + 12);
		const float v = (type == DVAR_TYPE_FLOAT_SECURE)
			? GameUtil::getDvarSecureFloat(reinterpret_cast<const dvar_t*>(dvar))
			: *reinterpret_cast<float*>(dvar + 16);
		// A dvar that has not been registered yet reads as junk, so sanity-check
		// rather than handing the GUI a slider position of 1e38.
		return (v > 1.0f && v < 200.0f) ? v : -1.0f;
	}

	void set_game_fov(const float degrees)
	{
		// The live-play dvar, through the console so the engine's own setter and
		// change callback run on the client thread. Clamped to the range the
		// engine registers (50..100); its domain callback may narrow the top
		// further, in which case the engine keeps the old value.
		const float v = std::clamp(degrees, 50.0f, 100.0f);
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("cg_fov {:.0f}", v));
	}

	bool framing_patched() { return g_framing_patched; }
	float* height() { return g_framing_patched ? g_height : nullptr; }
	float* distance() { return g_framing_patched ? g_distance : nullptr; }

	float roll() { return g_roll; }

	void set_roll(const float degrees)
	{
		g_roll = std::clamp(degrees, -180.0f, 180.0f);
	}

	void on_wheel(const float notches, const bool alt_held)
	{
		// Only while actually flying. Third/first person don't have a use for
		// this, and the wheel is free for whatever the game itself does there.
		if (theater_camera::get_mode() != theater_camera::THEATER_CAMERA_FREECAM)
		{
			return;
		}
		if (alt_held)
		{
			if (!fov_available() || !demo_is_playing())
			{
				return; // don't jump from "unavailable" straight to a clamp edge
			}
			// wheel up (positive notches) = zoom in = smaller FOV, matching the
			// MWR reference's convention.
			set_fov(fov() - notches * 2.0f);
		}
		else
		{
			set_roll(g_roll + notches * 2.0f);
		}
	}

	// ---------------------------------------------------------------------
	//  ROLL, SECOND ATTEMPT — fed on the USERCMD side, before the mover
	// ---------------------------------------------------------------------
	// ⛔ THE ORIGINAL APPROACH (apply_after_camera_move, below) WAS AN
	// INFERENCE AND THE USER HAS NOW DISPROVEN IT. CLAUDE.md's "ROLL IS
	// PLUMBED AND SIMPLY NEVER FED" reasoned that because both movers write
	// cg+2355668 from usercmd angle[2], writing that field after the mover
	// would give a dutch angle. It shipped labelled "NOT run in game -- a
	// CANDIDATE". Reported 2026-09-21: it does nothing.
	//
	// WHAT THE RECORD ALREADY SAYS, and what the old fix contradicted.
	// theater_camera::seed_freecam_from_current_view has carried this note
	// since it was written:
	//
	//     "Angles live on the usercmd side -- CL_Demo_FreeCameraMove derives
	//      cg's freecam angles from the usercmd, so seeding cl.viewangles is
	//      what actually stops the camera snapping to wherever the mouse is."
	//
	// So the authoritative store is cl.viewangles (clientActive+25900), and
	// cg+2355660..668 is a DERIVED copy. CL_Demo_SetCameraMode's own seed
	// ends `*(float*)(clientActive + 25908) = 0` -- the engine zeroing the
	// roll slot -- which pins +25908 as roll on that side.
	//
	// Writing the derived copy after the mover therefore lands one step too
	// far downstream: whatever consumes the angles to build the view axis
	// either reads the usercmd side, or reads a copy taken before we wrote.
	// Feeding the SOURCE before the mover runs lets the engine's own
	// derivation carry roll through every step it already performs.
	//
	// STATUS: HYPOTHESIS, consistent with the record but NOT yet confirmed in
	// game. The old post-write is deliberately LEFT IN PLACE -- both fields are
	// rewritten by the engine every frame, so feeding both costs nothing and
	// covers the case where the view is built from the derived copy after all.
	// If roll works now, one of the two did it; `demo_roll_probe` prints both
	// fields so the next session can tell which and delete the other.
	void apply_before_camera_move()
	{
		if (g_roll == 0.0f)
		{
			return;                       // stock behaviour, touch nothing
		}
		// BOTH GATES ARE LOAD-BEARING. This writes cl.viewangles, which during
		// LIVE play is the player's own aim -- rolling that would be a bug, not
		// a feature. Free camera is additionally the only mode routed through
		// the mover this rides.
		if (!demo_is_playing()
			|| theater_camera::get_mode() != theater_camera::THEATER_CAMERA_FREECAM)
		{
			return;
		}
		void* cl = demo_game::client_active_for(0);
		if (!cl)
		{
			return;
		}
		auto* ang = reinterpret_cast<float*>(
			static_cast<char*>(cl) + demo_game::CA_CMD_VIEWANGLES);
		if (readable(ang, 12))
		{
			ang[2] = g_roll;
		}
	}

	void apply_after_camera_move()
	{
		if (g_roll == 0.0f)
		{
			return;                       // stock behaviour, touch nothing
		}
		// The engine writes roll here from usercmd angle[2] every frame, so this
		// is an overwrite of a field it owns and refreshes -- not manufactured
		// state that could go stale.
		void* cg = demo_game::cg_globals_for(0);
		if (!cg)
		{
			return;
		}
		auto* roll_field = reinterpret_cast<float*>(
			static_cast<char*>(cg) + CAM_ROLL_OFF);
		if (readable(roll_field, sizeof(float)))
		{
			// SAMPLED IN THE HOOK, not from the console. A user log showed this
			// field reading 0.0 from demo_roll_probe while cl.viewangles[2] held
			// 20.0 -- but the probe runs on the console thread at an arbitrary
			// point in the frame, so it could not distinguish:
			//   (a) our write never ran, from
			//   (b) it ran and something downstream zeroed the field.
			// g_roll_pre is what the ENGINE'S OWN MOVER left here, read before we
			// touch it. If the mover really derived roll from the usercmd, this
			// would already equal g_roll -- the log says it is 0, which is what
			// disproves the "plumbed, just never fed" theory in CLAUDE.md.
			g_roll_pre.store(*roll_field, std::memory_order_relaxed);
			*roll_field = g_roll;
			g_roll_post.store(*roll_field, std::memory_order_relaxed);
			g_roll_writes.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void screenshot()
	{
		if (!demo_native::native_playing())
		{
			Console::printf("[cam] screenshot: only available during demo playback");
			return;
		}
		Console::printf("[cam] high-resolution screenshot -- playback will freeze and "
			"the camera will sweep while it captures the tiles.");
		// Action 17 is the engine's own CL_Demo_CaptureScreenshot. transport()
		// calls the ORIGINAL handler, so our key-block on 17 (which stops F3
		// doing this by surprise) does not get in the way of asking for it.
		demo_native::transport(17);
	}

	void init()
	{
		patch_framing();

		// RULE A3: a hook is only live when it says so, and a duplicate reports
		// success with a null trampoline -- so the trampoline is the proof.
		g_fov_hook_ok = Hook::create("CG_CalcFov", _b(CG_CALC_FOV_LITERAL),
			&cg_calc_fov_stub, &g_calc_fov_orig) && g_calc_fov_orig != nullptr;
		Console::printf("[cam] demo field of view: %s (target=%p orig=%p)",
			g_fov_hook_ok ? "adjustable in both demo systems" : "HOOK FAILED -- stock fov only",
			reinterpret_cast<void*>(_b(CG_CALC_FOV_LITERAL)),
			reinterpret_cast<void*>(g_calc_fov_orig));

		GameUtil::addCommand("demo_fov", cmd_fov);
		GameUtil::addCommand("demo_thirdperson", cmd_third);
		GameUtil::addCommand("demo_roll", cmd_roll);
		GameUtil::addCommand("demo_roll_probe", cmd_roll_probe);
		GameUtil::addCommand("demo_screenshot", cmd_screenshot);
	}
}
