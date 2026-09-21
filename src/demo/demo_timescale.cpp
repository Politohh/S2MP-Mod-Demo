#include "pch.h"
#include "demo_timescale.hpp"

#include "demo/demo_native.hpp"
#include "demo/demo_playback.hpp"
#include "demo/demo_game.hpp"
#include "demo/fps.hpp"

#include "Console.hpp"
#include "DevMode.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"
#include "Hook.hpp"
#include "time_scale_hook.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>

namespace demo_timescale
{
	namespace
	{
		dvar_t* dvar_demotimescale = nullptr;
		float g_factor = 1.0f;

		// The playback clock integrates factor() every frame, so these bounds are
		// load-bearing: 0 would freeze playback outright and 1000 would drain the
		// demo file in a handful of frames.
		//
		// 2026-09-21: floor lowered 0.05 -> 0.01 (1/100 speed) on request, for
		// ramp and impact shots. Safe for the CUSTOM theater because advance_clock()
		// integrates wall_delta * scale into our own clock, and scale_lui_delta's
		// per-stream `carry` already accumulates sub-millisecond remainders -- at
		// 0.01 a 16 ms frame contributes 0.16 ms, which is exactly what that carry
		// was written for, just exercised more often.
		// ⛔ NATIVE playback does NOT take its speed from here: that is the engine's
		// own PlaybackData+28. See demo_native::set_timescale for its own floor.
		constexpr float MIN_SCALE = 0.01f;
		constexpr float MAX_SCALE = 8.0f;

		// The audio pitch multiplier keeps the OLD 0.05 floor, deliberately.
		// Pitching every voice down 100x is inaudible rumble, and the mixer applies
		// its own base-pitch clamp BEFORE this multiplier (see effective_audio_scale),
		// so handing it 0.01 is untested territory for no audible gain. Slow motion
		// below 1/20 speed simply holds 1/20 pitch.
		constexpr float MIN_AUDIO_SCALE = 0.05f;

		// S2 has no Com_TimeScaleMsec symbol (H1/MWR do). Real world speed is driven by:
		//   1) demo_clock_ms() * factor  → theater feed / cl_serverTime
		//   2) engine `timescale` dvar if present (many IW titles register it)
		//   3) LUI delta scaling for HUD animations
		//   4) clearing newSnapshots so CL_AdjustTimeDelta cannot fight the clock

		using LUI_Layout_fn = void (*)(int, void*, int, int, int, void*);
		LUI_Layout_fn LUI_Layout_orig = nullptr;

		using UI_UpdateTime_fn = void (*)(unsigned int, void*, void*, int, int, int);
		UI_UpdateTime_fn UI_UpdateTime_orig = nullptr;

		// ── AUDIO TIMESCALE ────────────────────────────────────────────────────
		// The engine has a NATIVE master audio pitch: `g_snd.timescale` (the field
		// name comes from MWR-PS4's own assert, snd.cpp:7011). Every voice gets
		//     pitch *= busLerp * ts + (1 - busLerp)
		// per SND_UpdateChannel (S2 0x71C8A0 @ 0x71ca9b..0x71caf5 -> SND_SetVoicePitch
		// 0x705560), with the per-bus lerp array (g_snd_timescaleLerp, 0xD3674F8)
		// measured all-1.0 live, so every bus follows it. Its SOLE writer is
		// SND_UpdateTimeScale (0x7202B0), once per SND_Update, and in S2 the source
		// (Com_GetTimescaleForSnd, 0x92E60) is fminf-clamped so it is effectively a
		// CONSTANT 1.0 — a dormant, shipped pipeline waiting for a value.
		//
		// We hook the writer and feed it the DEMO timescale (theater demotimescale /
		// native PlaybackData+28), so slow-mo recordings carry tape-style pitched
		// audio: speed the footage back up in post and it sounds normal again.
		// RULE A17: the target is void(void) — verified from its own disassembly.
		using SND_UpdateTimeScale_fn = void (*)();
		SND_UpdateTimeScale_fn SND_UpdateTimeScale_orig = nullptr;
		bool g_audio_pitch = true;   // demo_snd_timescale 0|1

		// Same source selection as scale_lui_delta: theater -> demotimescale,
		// native -> the engine's own demo speed. 1.0 when no demo is driving time.
		// Pause deliberately returns the demo's scale unchanged — snapping to 1.0
		// on pause would pitch-pop any lingering voices mid-slow-mo.
		float effective_audio_scale()
		{
			if (!g_audio_pitch)
			{
				return 1.0f;
			}
			const bool theater = demo_playback::is_playing();
			const bool native = demo_native::native_playing();
			if (!theater && !native)
			{
				return 1.0f;
			}
			const float scale = theater ? factor() : demo_native::engine_timescale();
			// The engine clamps the BASE pitch before this multiplier, not the
			// product, so keep the audio scale inside the range both demo systems
			// actually use rather than trusting the mixer with extremes.
			return std::clamp(scale, MIN_AUDIO_SCALE, 4.0f);
		}

		void SND_UpdateTimeScale_stub()
		{
			const float ts = effective_audio_scale();
			if (ts > 0.0f && (ts < 0.999f || ts > 1.001f))
			{
				// The same aligned 4-byte store the engine's own updater performs.
				// Skipping the original is what stops its constant 1.0 fighting us;
				// g_snd_timescale has no other writer (proven by xref).
				*reinterpret_cast<float*>(0xD34585C_b) = ts;   // IDA 0xD34685C g_snd_timescale
				return;
			}
			if (SND_UpdateTimeScale_orig)
			{
				SND_UpdateTimeScale_orig();   // stock path: restores 1.0 on change
			}
		}

		void cmd_demo_snd_timescale()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2)
			{
				g_audio_pitch = std::atoi(args->argv[args->nesting][1]) != 0;
			}
			// Report unconditionally (RULE A15) — including whether the hook is live,
			// so "no audio change" is diagnosable from this one line.
			GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format(
				"echo [demo] audio timescale (pitch follows demo speed): {} (hook {})",
				g_audio_pitch ? "ON" : "OFF",
				SND_UpdateTimeScale_orig ? "live" : "DEAD - not installed"));
		}

		// `carry` holds the sub-millisecond remainder and MUST be per-stream: sharing
		// one accumulator between delta_a and delta_b steals a millisecond from one
		// and hands it to the other, which reads as HUD judder at non-1x speeds.
		int scale_lui_delta(const int delta_time, float& carry)
		{
			// TWO playback systems feed this now:
			//   theater -> our own demotimescale
			//   native  -> the ENGINE's demo speed (PlaybackData+28), which its own
			//              up/down-arrow control drives. Without this the HUD keeps
			//              animating at 1x while the world slows -- the reported bug.
			const bool theater = demo_playback::is_playing();
			const bool native = demo_native::native_playing();
			if (!theater && !native)
			{
				carry = 0.0f;
				return delta_time;
			}
			// Freeze completely while paused, in BOTH systems. Native playback has
			// its own pause command (cl_demo_pause), so the theater flag never sees
			// it -- which is why the HUD kept animating on a paused native demo.
			if (theater && demo_playback::paused())
			{
				return 0;
			}
			if (native && demo_native::engine_paused())
			{
				carry = 0.0f;
				return 0;
			}
			if (delta_time <= 0)
			{
				return delta_time;
			}
			const float scale = theater ? factor() : demo_native::engine_timescale();
			if (scale >= 0.999f && scale <= 1.001f)
			{
				carry = 0.0f;
				return delta_time;
			}
			carry += static_cast<float>(delta_time) * scale;
			const int scaled = static_cast<int>(carry);
			carry -= static_cast<float>(scaled);
			return scaled;
		}

		void LUI_Layout_stub(const int local_client_num, void* root, int delta_a, int delta_b,
			const int a5, void* lua_vm)
		{
			static float carry_a = 0.0f;
			static float carry_b = 0.0f;
			LUI_Layout_orig(local_client_num, root,
				scale_lui_delta(delta_a, carry_a), scale_lui_delta(delta_b, carry_b),
				a5, lua_vm);
		}

		void UI_UpdateTime_stub(const unsigned int local_client_num, void* a2, void* a3,
			const int a4, const int a5, const int a6)
		{
			// Drive HUD absolute clock off theater serverTime when playing (H1 killfeed pattern).
			if (demo_playback::is_playing())
			{
				const int st = demo_game::server_time();
				if (st > 0)
				{
					// a4 is commonly the realtime arg on IW titles; pass through scaled clock.
					UI_UpdateTime_orig(local_client_num, a2, a3, st, a5, a6);
					return;
				}
			}
			UI_UpdateTime_orig(local_client_num, a2, a3, a4, a5, a6);
		}

		void cmd_demotimescale()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("echo demotimescale = {:.2f}", g_factor));
				return;
			}
			demo_playback::set_timescale(
				static_cast<float>(std::atof(args->argv[args->nesting][1])));
		}
	}

	void init()
	{
		if (Functions::_Dvar_RegisterFloat)
		{
			dvar_demotimescale = Functions::_Dvar_RegisterFloat(
				"demotimescale", 1.0f, MIN_SCALE, MAX_SCALE, 0);
		}
		Console::printf(
			"[demo] world speed = theater clock * demotimescale (+ LUI deltas); "
			"engine `timescale` deliberately untouched to avoid double-scaling");
		// Superseded by `demo_speed`, which drives whichever demo engine is
		// playing. Kept for scripts and binds that already use it.
		dev_mode::add_command("demotimescale", cmd_demotimescale);
		TimeScale_Enable(true);

		Hook::create("LUI_Layout", reinterpret_cast<void*>(demo_game::addr_LUI_Layout()),
			reinterpret_cast<void*>(LUI_Layout_stub),
			reinterpret_cast<void**>(&LUI_Layout_orig));

		Hook::create("UI_UpdateTime", reinterpret_cast<void*>(demo_game::addr_UI_UpdateTime()),
			reinterpret_cast<void*>(UI_UpdateTime_stub),
			reinterpret_cast<void**>(&UI_UpdateTime_orig));

		// Audio pitch follows the demo timescale (both systems). Target verified
		// unhooked elsewhere in src/ (RULE A3.1); 0x31 bytes, comfortably above the
		// 0x16 trampoline minimum. IDA 0x7202B0 - 0x1000 = 0x71F2B0.
		const bool snd_ok = Hook::create("SND_UpdateTimeScale",
			reinterpret_cast<void*>(0x71F2B0_b),
			reinterpret_cast<void*>(SND_UpdateTimeScale_stub),
			reinterpret_cast<void**>(&SND_UpdateTimeScale_orig));
		Console::printf("[demo] audio timescale hook: %s (orig=%p) — demo_snd_timescale 0|1",
			(snd_ok && SND_UpdateTimeScale_orig) ? "OK" : "FAILED",
			reinterpret_cast<void*>(SND_UpdateTimeScale_orig));
		GameUtil::addCommand("demo_snd_timescale", cmd_demo_snd_timescale);
	}

	float factor()
	{
		if (dvar_demotimescale)
		{
			g_factor = dvar_demotimescale->current.value;
		}
		// Written as a NaN-safe clamp — `!(x > MIN)` also catches NaN, which would
		// otherwise poison the playback clock permanently on the first frame.
		if (!(g_factor > MIN_SCALE))
		{
			g_factor = MIN_SCALE;
		}
		else if (g_factor > MAX_SCALE)
		{
			g_factor = MAX_SCALE;
		}
		return g_factor;
	}

	void set_factor(const float value)
	{
		g_factor = std::clamp(value, MIN_SCALE, MAX_SCALE);
		if (dvar_demotimescale)
		{
			dvar_demotimescale->current.value = g_factor;
		}
		// Deliberately NOT mirrored onto the engine `timescale` dvar.  The theater
		// clock is the single source of world speed; if that dvar is live on S2 the
		// mirror double-scales (0.5 lands as 0.25) and it stays dirty after playback.
	}
}
