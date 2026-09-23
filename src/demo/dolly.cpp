// =============================================================================
//  demo/dolly.cpp — dolly camera for native demo playback
// =============================================================================
//
//  Read dolly.hpp first: it records the two plan corrections found by
//  decompiling before any of this was written (the integrator hook would never
//  have fired, and sub_917240 is already an interpolator), plus the four cg
//  field offsets and how each was cross-checked.
//
//  THREE THREADS TOUCH THE POINT LIST:
//    drive()   client thread, inside CG_PredictPlayerState
//    render()  render thread, inside R_EndFrame
//    the GUI   DXGI Present thread
//  so it is guarded by a mutex and both hot paths copy under the lock rather
//  than holding it across engine calls.
//
//  RULE A18: nothing here logs per frame. Only add/delete/enable and the
//  one-shot first-projection line print at all.
// =============================================================================

#include "pch.h"
#include "dolly.hpp"

#include "demo/demo_game.hpp"
#include "demo/demo_native.hpp"
#include "demo/demo_playback.hpp"
#include "demo/theater_camera.hpp"
#include "demo/bonecam.hpp"
#include "demo/demo_camera.hpp"

#include "Console.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"
#include "Hook.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace dolly
{
	namespace
	{
		// ---- engine addresses ------------------------------------------------
		// RULE A1 — the arithmetic is written out, not done in the head.
		// `_b` literal = IDA - 0x1000.  Runtime VA = module_base + IDA.
		//
		//   CL_Demo_FreeCameraMove       0x913AE0 - 0x1000 = 0x912AE0   <- the hook
		//   CG_ProjectViewDirToScreenPos 0x38EFD0 - 0x1000 = 0x38DFD0
		//   CG_GetRefdefView             0x4A0170 - 0x1000 = 0x49F170
		//
		// RULE A3.1 checked before installing: grepping src/ for both forms of
		// 0x913AE0 and for "FreeCamera" finds only comments, so nothing else
		// hooks this target and there is no silent MH_ERROR_ALREADY_CREATED.
		constexpr std::uintptr_t ADDR_FREECAM_MOVE = 0x912AE0;
		constexpr std::uintptr_t ADDR_PROJECT_VIEW_DIR = 0x38DFD0;
		constexpr std::uintptr_t ADDR_GET_REFDEF_VIEW = 0x49F170;

		// ⛔ WE DO NOT CALL CG_WorldPosToScreenPos (IDA 0x3E6250). IT CRASHED THE
		// GAME, and the minidump named it exactly (RULE A13):
		//
		//     ACCESS_VIOLATION at IDA_0x3E628E, READ from 0x1E6C44 (unmapped)
		//     0x1E6C44 == 1993796 == the refdef view origin offset
		//
		// Its first two instructions after the prologue are
		//     call CG_GetLocalClientGlobals
		//     subss xmm4, dword ptr [rax+1E6C44h]        <- NO NULL CHECK
		// so when cg is not up it dereferences NULL + 1993796. That happens for a
		// real window during a SEEK: ProcessKeyFrameJump memsets the configstrings
		// and reparses the whole gamestate, and render() runs on the render thread
		// meanwhile.
		//
		// THE LESSON, and it is general: validating `demo_game::cg_globals_for()`
		// did NOT protect this call, because that accessor derives cg from the
		// RENDERER BACK-POINTER while CG_GetLocalClientGlobals resolves it
		// independently from g_cg_encrypted. The two genuinely disagreed.
		// NEVER validate pointer A and then call a function that resolves B.
		//
		// So we inline the wrapper ourselves and call only the LEAF projector,
		// which touches nothing but the pointers we hand it. Every operand below
		// is read straight off the disassembly of 0x3E6250 — none is inferred:
		//
		//     call CG_GetLocalClientGlobals
		//     xmm4/5/6 = world[0..2] - [rax+1E6C44/48/4C]      view origin
		//     len = sqrt(x*x+y*y+z*z); if (len <= 0) len = 1.0
		//     dir = delta * (1.0/len)
		//     rcx  = rax + 1E6C50h                              view AXIS
		//     rdx  = refdef + 20h                               viewport w/h
		//     xmm3 = [rax+1E6C30h]                              tan half fov X
		//     [rsp+20h] = [rax+1E6C34h]                         tan half fov Y
		//     [rsp+28h] = outScreenXY
		//     call CG_ProjectViewDirToScreenPos
		constexpr std::size_t CG_TAN_HALF_FOV_X = 1993776;   // 0x1E6C30
		constexpr std::size_t CG_TAN_HALF_FOV_Y = 1993780;   // 0x1E6C34
		constexpr std::size_t CG_REFDEF_VIEW_ORIGIN = 1993796; // 0x1E6C44 float[3]
		constexpr std::size_t CG_REFDEF_VIEW_AXIS = 1993808;   // 0x1E6C50 float[3][3]
		// 1993776 .. 1993808+36 == 1993844, so ONE 68-byte probe covers all four.
		constexpr std::size_t CG_VIEW_BLOCK = 1993776;
		constexpr std::size_t CG_VIEW_BLOCK_BYTES = 68;

		// cg byte offsets — see the header for how each was proven.
		constexpr std::size_t CG_FREECAM_ORIGIN = 2355648;   // float[3]
		constexpr std::size_t CG_FREECAM_ANGLES = 2355660;   // float[3]
		constexpr std::size_t CG_FREECAM_VELOCITY = 2355672; // float[3]
		// origin..velocity are contiguous, so one 36-byte probe validates all three.
		constexpr std::size_t CG_FREECAM_BLOCK = 36;

		// The refdef view struct is 108 bytes (CG_GetRefdefView returns
		// &unk_90BB140 + 108*client). sub_38EFD0 uses +32 / +36 as the viewport
		// width and height, in floats — that is the space its output is in.
		constexpr std::size_t REFDEF_VIEWPORT_W = 32;
		constexpr std::size_t REFDEF_VIEWPORT_H = 36;

		// Render-target size. THE SAME GLOBALS InternalConsole::drawConsole uses
		// as windowWidth/windowHeight, and the same two sub_7B0B0 reads when
		// LUI_WorldToScreenPosition normalises CG_WorldPosToScreenPos's output —
		// which is what makes that output directly usable as R_AddCmdDraw* pixels.
		//   IDA 0x1C86268 - 0x1000 = 0x1C85268
		//   IDA 0x1C8626C - 0x1000 = 0x1C8526C
		constexpr std::uintptr_t ADDR_SCREEN_W = 0x1C85268;
		constexpr std::uintptr_t ADDR_SCREEN_H = 0x1C8526C;

		constexpr int LOCAL_CLIENT = 0;
		constexpr int CAMERA_MODE_FREE = 2;

		// ---- dual-system dispatch ---------------------------------------------
		// Dolly used to be native-`.demo`-only. bonecam.cpp already proved the
		// pattern for driving the SAME freecam block regardless of which demo
		// engine is playing: check theater_camera's unified mode (it makes the
		// engine take the free-camera code path for either system) rather than
		// demo_native::native_playing() alone.
		[[nodiscard]] bool camera_ready()
		{
			return theater_camera::get_mode() == theater_camera::THEATER_CAMERA_FREECAM
				&& (demo_native::native_playing() || demo_playback::is_playing());
		}

		// The smooth, continuously-advancing clock for whichever system is
		// playing -- never the stepped snapshot value. Native reads cl.serverTime
		// (demo_time_smooth()); the custom theater integrates its own clock every
		// frame in advance_clock() and exposes it as current_time(). -1 when
		// neither system is playing.
		[[nodiscard]] int active_time()
		{
			if (demo_native::native_playing())
			{
				return demo_native::demo_time_smooth();
			}
			if (demo_playback::is_playing())
			{
				return demo_playback::current_time().value_or(-1);
			}
			return -1;
		}

		// Seek whichever system is playing to an absolute demo time and make sure
		// it is actually running, not paused. Used by the "Go" button and by the
		// new J hotkey (play_from_start).
		// ⛔ Never seek directly from here. J and the Go button run on the DXGI
		// Present thread, and a custom seek rewinds the demo file stream and
		// rewrites clock state that the client thread is feeding from in the same
		// instant. Queued as demo_player's own command it runs on the client
		// thread, through the same code path as every other seek, so native and
		// custom behave identically.
		void seek_and_play(const int time_ms)
		{
			GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
				std::format("demo_seek_to {} play", time_ms));
		}

		using CL_Demo_FreeCameraMove_t = std::int64_t(__fastcall*)(std::int64_t, std::int64_t);
		CL_Demo_FreeCameraMove_t g_freecam_move_orig = nullptr;

		// RULE A17 — exact widths. CG_ProjectViewDirToScreenPos returns `char`
		// (1 = in front, 0 = behind/clamped); declaring it wider reads register
		// residue. Arg slots follow the disassembly exactly: rcx, rdx, r8, xmm3,
		// then [rsp+0x20] and [rsp+0x28].
		using CG_ProjectViewDirToScreenPos_t = char(__fastcall*)(
			const float* axis, const float* viewport, const float* dir,
			float tan_half_x, float tan_half_y, float* out_xy);
		using CG_GetRefdefView_t = void*(__fastcall*)(int);

		std::mutex g_lock;
		std::vector<point_t> g_points;

		bool g_enabled = false;
		bool g_show_markers = true;
		bool g_hook_ok = false;
		bool g_logged_first_projection = false;

		// ---- safety ----------------------------------------------------------
		// RULE A6 — `if (!p)` is not enough in this game: a global belonging to an
		// idle subsystem holds junk, not zero.
		[[nodiscard]] bool readable(const void* p, const std::size_t n)
		{
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
			const auto addr = reinterpret_cast<std::uintptr_t>(p);
			return addr + n <= start + mbi.RegionSize;
		}

		// The freecam pose block, validated. Null when cg is not up yet — the
		// renderer back-pointer demo_game::cg_globals_for derives from is often
		// unset early in a frame, which is exactly why it is checked every time.
		[[nodiscard]] float* freecam_block()
		{
			void* cg = demo_game::cg_globals_for(LOCAL_CLIENT);
			if (!cg)
			{
				return nullptr;
			}
			auto* p = reinterpret_cast<float*>(static_cast<char*>(cg) + CG_FREECAM_ORIGIN);
			return readable(p, CG_FREECAM_BLOCK) ? p : nullptr;
		}

		// The whole view block — tan-half-fov pair, view origin and view axis — in
		// one validated 68-byte probe. Returns the block base, or null.
		[[nodiscard]] const float* view_block()
		{
			void* cg = demo_game::cg_globals_for(LOCAL_CLIENT);
			if (!cg)
			{
				return nullptr;
			}
			const auto* p = reinterpret_cast<const float*>(
				static_cast<char*>(cg) + CG_VIEW_BLOCK);
			return readable(p, CG_VIEW_BLOCK_BYTES) ? p : nullptr;
		}

		// BACKSTOP, not the fix. The fix is that every pointer handed to the
		// engine below is one we validated ourselves; this catches the case where
		// a page is unmapped between the VirtualQuery and the call, and any
		// mistake of mine that is not yet known. Deliberately a LEAF with no C++
		// objects, so __try/__except is legal and cheap here.
		[[nodiscard]] bool project_guarded(const CG_ProjectViewDirToScreenPos_t fn,
			const float* axis, const float* viewport, const float* dir,
			const float tan_half_x, const float tan_half_y, float* out_xy,
			char& in_front)
		{
			__try
			{
				in_front = fn(axis, viewport, dir, tan_half_x, tan_half_y, out_xy);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				in_front = 0;
				return false;
			}
		}

		// ---- maths -----------------------------------------------------------
		[[nodiscard]] float ang_norm180(float a)
		{
			a = std::fmod(a + 180.0f, 360.0f);
			if (a < 0.0f)
			{
				a += 360.0f;
			}
			return a - 180.0f;
		}

		// Uniform Catmull-Rom. With two points it degenerates to a straight lerp
		// (p0 == p1 and p3 == p2), which is what we want at the ends.
		[[nodiscard]] float catmull_rom(const float p0, const float p1,
			const float p2, const float p3, const float t)
		{
			const float t2 = t * t;
			const float t3 = t2 * t;
			return 0.5f * ((2.0f * p1)
				+ (-p0 + p2) * t
				+ (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
				+ (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
		}

		// Evaluate the dolly at demo time `t`. Returns false outside the span, so
		// the camera stays free before the first point and after the last — that
		// is what makes "fly, add a point, fly on" work as an editing workflow.
		[[nodiscard]] bool evaluate(const std::vector<point_t>& pts, const int t,
			float out_pos[3], float out_ang[3], float* out_fov = nullptr)
		{
			if (out_fov)
			{
				*out_fov = 0.0f;
			}
			const int n = static_cast<int>(pts.size());
			if (n < 2 || t < pts.front().time || t > pts.back().time)
			{
				return false;
			}

			int i = 0;
			for (int k = 0; k + 1 < n; ++k)
			{
				if (t >= pts[k].time && t <= pts[k + 1].time)
				{
					i = k;
					break;
				}
			}

			const int dt = pts[i + 1].time - pts[i].time;
			// Guarded rather than assumed: add_point() merges points closer than
			// 10 ms, but a hand-edited time could still collide.
			const float f = (dt > 0)
				? std::clamp(static_cast<float>(t - pts[i].time) / static_cast<float>(dt),
					0.0f, 1.0f)
				: 0.0f;

			const point_t& p1 = pts[i];
			const point_t& p2 = pts[i + 1];
			const point_t& p0 = pts[(i > 0) ? (i - 1) : i];
			const point_t& p3 = pts[(i + 2 < n) ? (i + 2) : (i + 1)];

			for (int a = 0; a < 3; ++a)
			{
				out_pos[a] = catmull_rom(p0.pos[a], p1.pos[a], p2.pos[a], p3.pos[a], f);
			}

			// Angles: unwrap the three neighbours onto p1's branch FIRST, so the
			// spline never takes the long way round a wrap. Doing it after would
			// smooth across a 359 -> 1 jump and spin the camera.
			for (int a = 0; a < 3; ++a)
			{
				const float b = p1.angles[a];
				const float u0 = b + ang_norm180(p0.angles[a] - b);
				const float u2 = b + ang_norm180(p2.angles[a] - b);
				const float u3 = b + ang_norm180(p3.angles[a] - b);
				out_ang[a] = catmull_rom(u0, b, u2, u3, f);
			}

			// FOV: a spline when both ends are keyed (a neighbour that is not
			// keyed stands in as its segment end, so the curve does not dip to 0),
			// held when only one end is, and left alone when neither is.
			if (out_fov)
			{
				if (p1.fov > 0.0f && p2.fov > 0.0f)
				{
					const float f0 = (p0.fov > 0.0f) ? p0.fov : p1.fov;
					const float f3 = (p3.fov > 0.0f) ? p3.fov : p2.fov;
					*out_fov = (std::max)(1.0f, catmull_rom(f0, p1.fov, p2.fov, f3, f));
				}
				else if (p1.fov > 0.0f || p2.fov > 0.0f)
				{
					*out_fov = (p1.fov > 0.0f) ? p1.fov : p2.fov;
				}
			}
			return true;
		}

		// ---- projection ------------------------------------------------------
		struct Projector
		{
			CG_ProjectViewDirToScreenPos_t fn{};
			const float* viewport{};     // refdef + 32
			const float* view_origin{};  // cg + 1993796
			const float* view_axis{};    // cg + 1993808
			float tan_half_x{};
			float tan_half_y{};
			float screen_w{};
			float screen_h{};
			float scale_x = 1.0f;
			float scale_y = 1.0f;

			[[nodiscard]] bool valid() const
			{
				return fn && viewport && view_origin && view_axis;
			}

			// Returns false when the point is behind the camera OR when the call
			// could not be made safely. The engine still writes clamped edge
			// coordinates for a point behind, so its return value is the only way
			// to tell — and on the guarded-failure path we report false too, so a
			// caller can never draw from uninitialised output.
			[[nodiscard]] bool project(const float world[3], float& sx, float& sy) const
			{
				// The normalisation CG_WorldPosToScreenPos does, transcribed from
				// its disassembly — done HERE so cg is never resolved by the
				// engine behind our back.
				const float dx = world[0] - view_origin[0];
				const float dy = world[1] - view_origin[1];
				const float dz = world[2] - view_origin[2];
				float len = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (!(len > 0.0f))
				{
					len = 1.0f;
				}
				const float inv = 1.0f / len;
				const float dir[3] = { dx * inv, dy * inv, dz * inv };

				float out[2]{};
				char in_front = 0;
				if (!project_guarded(fn, view_axis, viewport, dir,
					tan_half_x, tan_half_y, out, in_front))
				{
					sx = 0.0f;
					sy = 0.0f;
					return false;
				}
				sx = out[0] * scale_x;
				sy = out[1] * scale_y;
				// A NaN would propagate silently into every draw call.
				if (!std::isfinite(sx) || !std::isfinite(sy))
				{
					return false;
				}
				return in_front != 0;
			}

			[[nodiscard]] float distance_to(const float world[3]) const
			{
				const float dx = world[0] - view_origin[0];
				const float dy = world[1] - view_origin[1];
				const float dz = world[2] - view_origin[2];
				return std::sqrt(dx * dx + dy * dy + dz * dz);
			}
		};

		[[nodiscard]] bool make_projector(Projector& p)
		{
			p.fn = reinterpret_cast<CG_ProjectViewDirToScreenPos_t>(
				_b(ADDR_PROJECT_VIEW_DIR));

			// One validated probe covers the fov pair, the origin and the axis.
			const float* vb = view_block();
			if (!vb)
			{
				return false;
			}
			p.tan_half_x = vb[0];                                        // +1993776
			p.tan_half_y = vb[1];                                        // +1993780
			p.view_origin = vb + (CG_REFDEF_VIEW_ORIGIN - CG_VIEW_BLOCK) / 4;
			p.view_axis = vb + (CG_REFDEF_VIEW_AXIS - CG_VIEW_BLOCK) / 4;

			// A zero or absurd fov means cg is stale rather than merely mapped —
			// exactly the state a null engine cg would have produced. Refusing
			// here is what turns "wrong pointer" into "no markers this frame".
			if (!std::isfinite(p.tan_half_x) || !std::isfinite(p.tan_half_y)
				|| p.tan_half_x <= 0.0001f || p.tan_half_y <= 0.0001f
				|| p.tan_half_x > 100.0f || p.tan_half_y > 100.0f)
			{
				return false;
			}

			const auto get_view = reinterpret_cast<CG_GetRefdefView_t>(_b(ADDR_GET_REFDEF_VIEW));
			void* refdef = get_view(LOCAL_CLIENT);
			if (!readable(refdef, 108))
			{
				return false;
			}
			p.viewport = reinterpret_cast<const float*>(
				static_cast<char*>(refdef) + REFDEF_VIEWPORT_W);

			const auto* sw = reinterpret_cast<const int*>(_b(ADDR_SCREEN_W));
			const auto* sh = reinterpret_cast<const int*>(_b(ADDR_SCREEN_H));
			if (!readable(sw, 4) || !readable(sh, 4) || *sw <= 0 || *sh <= 0)
			{
				return false;
			}
			p.screen_w = static_cast<float>(*sw);
			p.screen_h = static_cast<float>(*sh);

			// The projector emits coordinates in REFDEF VIEWPORT pixels.
			// LUI_WorldToScreenPosition then divides by the render-target size, so
			// the two are the same in the ordinary fullscreen case and this is a
			// no-op — but doing it explicitly is what makes the markers land
			// correctly if they ever differ, instead of silently drifting.
			const float vw = p.viewport[0];
			const float vh = p.viewport[1];
			if (!std::isfinite(vw) || !std::isfinite(vh)
				|| vw <= 1.0f || vw > 16384.0f || vh <= 1.0f || vh > 16384.0f)
			{
				return false;      // a nonsense viewport means stale state
			}
			p.scale_x = p.screen_w / vw;
			p.scale_y = p.screen_h / vh;
			return true;
		}

		// ---- drawing ---------------------------------------------------------
		float g_col_marker[4] = { 1.00f, 0.78f, 0.20f, 1.00f };   // amber
		float g_col_marker_edge[4] = { 0.08f, 0.06f, 0.02f, 0.85f };
		float g_col_path[4] = { 0.30f, 0.85f, 1.00f, 0.85f };   // cyan
		float g_col_text[4] = { 1.00f, 1.00f, 1.00f, 1.00f };
		float g_col_live[4] = { 0.35f, 1.00f, 0.45f, 1.00f };   // green

		void draw_quad(const float cx, const float cy, const float size, float* colour)
		{
			const float h = size * 0.5f;
			Functions::_R_AddCmdDrawStretchPic(cx - h, cy - h, size, size,
				0.0f, 0.0f, 0.0f, 0.0f, colour, InternalConsole::getMaterialWhite());
		}

		// A marker sized by the world, not by a guess: project the point and a
		// second point one MARKER_WORLD_RADIUS above it, and use the pixel
		// distance between them. That tracks FOV and resolution automatically,
		// which is what "properly scaled" has to mean.
		constexpr float MARKER_WORLD_RADIUS = 6.0f;
		constexpr float MARKER_MIN_PX = 5.0f;
		constexpr float MARKER_MAX_PX = 44.0f;

		[[nodiscard]] float marker_size_px(const Projector& p, const float world[3],
			const float sx, const float sy)
		{
			const float top[3] = { world[0], world[1], world[2] + MARKER_WORLD_RADIUS };
			float tx = 0.0f;
			float ty = 0.0f;
			if (!p.project(top, tx, ty))
			{
				return MARKER_MIN_PX;
			}
			const float dx = tx - sx;
			const float dy = ty - sy;
			return std::clamp(std::sqrt(dx * dx + dy * dy) * 2.0f,
				MARKER_MIN_PX, MARKER_MAX_PX);
		}
	}

	// =========================================================================
	//  DRIVING
	// =========================================================================
	void drive()
	{
		// Same gate as render(), for the same reason: a stale-but-mapped cg would
		// be WRITTEN here, not just read.
		if (!g_enabled || demo_native::seek_in_progress() || !camera_ready() || !demo_native::cgame_active())
		{
			return;
		}

		std::vector<point_t> pts;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (g_points.size() < 2)
			{
				return;
			}
			pts = g_points;
		}

		// THE SMOOTH CLOCK, not a stepped snapshot value. See active_time().
		const int t = active_time();
		if (t < 0)
		{
			return;
		}

		float pos[3]{};
		float ang[3]{};
		float key_fov = 0.0f;
		if (!evaluate(pts, t, pos, ang, &key_fov))
		{
			return;
		}
		// Keyed FOV rides the same spline. demo_camera applies it at the engine's
		// final fov, so it works identically in native and custom playback.
		if (key_fov > 0.0f)
		{
			demo_camera::set_dolly_fov(key_fov);
		}

		float* blk = freecam_block();
		if (!blk)
		{
			return;
		}

		// blk[0..2] origin, blk[3..5] angles, blk[6..8] velocity.
		// The velocity MUST be zeroed: it is the only field the engine
		// accumulates across frames (CL_Demo_FreeCameraMove damps and then adds
		// to it), so leaving it set would have the engine's own physics fighting
		// the path. Origin and angles are recomputed from scratch every frame,
		// so overwriting them here does not accumulate anything.
		blk[0] = pos[0];
		blk[1] = pos[1];
		blk[2] = pos[2];
		blk[3] = ang[0];
		blk[4] = ang[1];
		blk[5] = ang[2];
		blk[6] = 0.0f;
		blk[7] = 0.0f;
		blk[8] = 0.0f;
	}

	namespace
	{
		std::int64_t __fastcall cl_demo_freecameramove_stub(const std::int64_t a1,
			const std::int64_t a2)
		{
			// Run the engine's camera update FIRST, then overwrite its result.
			// Both of its branches — sub_917240 on the native-playback path and
			// the usercmd freecam elsewhere — write the same three fields, so
			// this placement covers both without caring which one ran.
			//
			// Sprint/slow is applied AROUND this call specifically because this
			// is the one place per frame that reads *freecam_speed() to move the
			// camera -- everything below runs AFTER movement and would be too
			// late to affect it. Only for the local client: *freecam_speed() is
			// one shared global, and scaling it for some other client's move
			// would just be wasted key-state polling with no effect of our own.
			const bool is_local = (a1 == LOCAL_CLIENT);
			const float speed_base = is_local ? demo_native::begin_speed_modifier() : 0.0f;
			// ROLL is fed BEFORE the mover, into the usercmd angles it derives
			// cg's freecam angles from. The post-write below is kept as well --
			// see the note above demo_camera::apply_before_camera_move.
			if (is_local)
			{
				demo_camera::apply_before_camera_move();
			}
			const std::int64_t r = g_freecam_move_orig(a1, a2);
			if (is_local)
			{
				demo_native::end_speed_modifier(speed_base);
				drive();
				// BONE CAM shares this hook rather than installing a second one
				// on the same address (RULE A3.1). It runs last so that when both
				// are enabled the bone wins the POSITION; neither touches the
				// angles, so mouse look survives either way.
				bonecam::apply();
				// Camera ROLL rides this hook too, for the same reason. It only
				// writes the roll component, so it composes with both of the above.
				demo_camera::apply_after_camera_move();
			}
			return r;
		}
	}

	// =========================================================================
	//  DRAWING — engine primitives, from the EXISTING R_EndFrame hook
	// =========================================================================
	void render()
	{
		// cgame_active() is not belt-and-braces here, it is THE gate: during a
		// seek the engine's cg is NULL while the back-pointer we derive from is
		// still set, and that disagreement is what crashed the game.
		if (!g_show_markers || !camera_ready() || !demo_native::cgame_active())
		{
			return;
		}
		if (!Functions::_R_AddCmdDrawStretchPic || !Functions::_R_AddCmdDrawText
			|| !InternalConsole::getMaterialWhite())
		{
			return;
		}

		std::vector<point_t> pts;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (g_points.empty())
			{
				return;
			}
			pts = g_points;
		}

		Projector proj{};
		if (!make_projector(proj) || !proj.valid())
		{
			return;
		}

		// RULE A15 in miniature: say ONCE that projection works, so "no markers"
		// can be told apart from "markers are drawing off-screen". Once, not per
		// frame (RULE A18).
		if (!g_logged_first_projection)
		{
			g_logged_first_projection = true;
			float sx = 0.0f;
			float sy = 0.0f;
			const bool front = proj.project(pts.front().pos, sx, sy);
			Console::printf("[dolly] projection live: point 0 -> screen (%.0f, %.0f) %s, "
				"screen %.0fx%.0f, viewport scale %.3f/%.3f",
				sx, sy, front ? "in front" : "BEHIND camera",
				proj.screen_w, proj.screen_h, proj.scale_x, proj.scale_y);
		}

		// ---- the path ----------------------------------------------------
		// R_AddCmdDrawStretchPic has no rotation parameter, so there is no line
		// primitive to use. Dense dots along the curve read as a line and cost
		// one draw each, which is why the sample count is capped rather than
		// derived from the span.
		if (pts.size() >= 2)
		{
			const int span = pts.back().time - pts.front().time;
			if (span > 0)
			{
				const int samples = std::clamp(static_cast<int>(pts.size()) * 24, 48, 400);
				for (int s = 0; s <= samples; ++s)
				{
					const int t = pts.front().time
						+ static_cast<int>(static_cast<std::int64_t>(span) * s / samples);
					float pos[3]{};
					float ang[3]{};
					if (!evaluate(pts, t, pos, ang))
					{
						continue;
					}
					float sx = 0.0f;
					float sy = 0.0f;
					if (!proj.project(pos, sx, sy))
					{
						continue;      // behind the camera
					}
					if (sx < -64.0f || sy < -64.0f
						|| sx > proj.screen_w + 64.0f || sy > proj.screen_h + 64.0f)
					{
						continue;
					}
					// Cheap perspective for the path: full projection per dot
					// would double an already 400-call loop, and dots do not
					// need to be exact.
					const float d = std::max(1.0f, proj.distance_to(pos));
					const float dot = std::clamp(900.0f / d, 1.5f, 6.0f);
					draw_quad(sx, sy, dot, g_col_path);
				}
			}
		}

		// ---- the points --------------------------------------------------
		font_t* font = Functions::_R_RegisterFont("fonts/consoleFont", 15);
		for (std::size_t i = 0; i < pts.size(); ++i)
		{
			float sx = 0.0f;
			float sy = 0.0f;
			if (!proj.project(pts[i].pos, sx, sy))
			{
				continue;
			}
			if (sx < -128.0f || sy < -128.0f
				|| sx > proj.screen_w + 128.0f || sy > proj.screen_h + 128.0f)
			{
				continue;
			}

			const float size = marker_size_px(proj, pts[i].pos, sx, sy);
			draw_quad(sx, sy, size + 2.0f, g_col_marker_edge);
			draw_quad(sx, sy, size, g_col_marker);

			if (font)
			{
				const auto label = std::format("{}  {}.{:02}s",
					i + 1, pts[i].time / 1000, (pts[i].time % 1000) / 10);
				Functions::_R_AddCmdDrawText(label.c_str(), 0x7FFFFFFF, font, 0, 0,
					font->pixelHeight, sx + size * 0.5f + 4.0f,
					sy + static_cast<float>(font->pixelHeight) * 0.5f,
					1.0f, 1.0f, 0.0f, g_col_text, 0);
			}
		}

		// ---- where the dolly is right now --------------------------------
		// Smooth clock, same as drive() — this marker must sit exactly where the
		// camera is, so it has to be evaluated on the same timebase.
		if (g_enabled && pts.size() >= 2)
		{
			const int t = active_time();
			float pos[3]{};
			float ang[3]{};
			if (t >= 0 && evaluate(pts, t, pos, ang))
			{
				float sx = 0.0f;
				float sy = 0.0f;
				if (proj.project(pos, sx, sy))
				{
					draw_quad(sx, sy, 7.0f, g_col_live);
				}
			}
		}
	}

	// =========================================================================
	//  EDITING
	// =========================================================================
	bool add_point()
	{
		if (!demo_native::native_playing() && !demo_playback::is_playing())
		{
			Console::printf("[dolly] add: no demo playing (cl_demo_play, or Play a "
				"custom recording, first)");
			return false;
		}
		const int mode = theater_camera::get_mode();
		if (mode != CAMERA_MODE_FREE)
		{
			Console::printf("[dolly] add: camera is %s, not free. Press F2 (or the "
				"Free camera button) — the dolly records and drives the FREE camera, "
				"and it is the only mode the engine routes through "
				"CL_Demo_FreeCameraMove.",
				mode == 0 ? "first person" : (mode == 1 ? "third person" : "unknown"));
			return false;
		}

		// Stamp on the SAME clock drive() evaluates on, or a point captured
		// mid-snapshot would sit slightly off where the camera actually was.
		const float* blk = freecam_block();
		const int t = active_time();
		if (!blk || t < 0)
		{
			Console::printf("[dolly] add: camera state not readable yet (cg=%s, demoTime=%d)",
				blk ? "ok" : "null", t);
			return false;
		}

		point_t p{};
		p.time = t;
		p.pos[0] = blk[0];
		p.pos[1] = blk[1];
		p.pos[2] = blk[2];
		p.angles[0] = blk[3];
		p.angles[1] = blk[4];
		p.angles[2] = blk[5];
		// Key what is on screen right now, so zoom can be animated along the path.
		p.fov = demo_camera::fov_available() ? demo_camera::fov() : 0.0f;

		std::size_t count = 0;
		bool replaced = false;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			// Points closer than one render frame are a violent snap rather than a
			// move, and an exact collision would divide by zero in evaluate().
			// Replacing is what the user meant anyway.
			for (auto& existing : g_points)
			{
				if (std::abs(existing.time - p.time) < 10)
				{
					existing = p;
					replaced = true;
					break;
				}
			}
			if (!replaced)
			{
				g_points.push_back(p);
			}
			std::sort(g_points.begin(), g_points.end(),
				[](const point_t& a, const point_t& b) { return a.time < b.time; });
			count = g_points.size();
		}

		Console::printf("[dolly] %s point at %d ms  pos (%.0f %.0f %.0f)  ang (%.1f %.1f) "
			"— %zu point(s)",
			replaced ? "replaced" : "added", p.time,
			p.pos[0], p.pos[1], p.pos[2], p.angles[0], p.angles[1], count);
		return true;
	}

	bool delete_point(const int index)
	{
		std::size_t count = 0;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (index < 0 || index >= static_cast<int>(g_points.size()))
			{
				Console::printf("[dolly] delete: no point %d (have %zu)",
					index + 1, g_points.size());
				return false;
			}
			g_points.erase(g_points.begin() + index);
			count = g_points.size();
		}
		Console::printf("[dolly] deleted point %d — %zu left", index + 1, count);
		return true;
	}

	void clear_points()
	{
		std::size_t had = 0;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			had = g_points.size();
			g_points.clear();
		}
		Console::printf("[dolly] cleared %zu point(s)", had);
	}

	// Classic dollycam J: jump to the first marker and let the demo run, so the
	// path plays from its own start. Whichever demo system is active drives it
	// from there (drive() evaluates the spline every frame regardless).
	bool play_from_start()
	{
		std::int32_t first_time = 0;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (g_points.empty())
			{
				Console::printf("[dolly] play: no points yet (K to add one)");
				return false;
			}
			first_time = g_points.front().time;
		}
		seek_and_play(first_time);
		Console::printf("[dolly] requesting playback from first camera at %d ms", first_time);
		return true;
	}

	bool retime_point(const int index, const std::int32_t time)
	{
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (index < 0 || index >= static_cast<int>(g_points.size()))
			{
				return false;
			}
			g_points[index].time = time;
			std::sort(g_points.begin(), g_points.end(),
				[](const point_t& a, const point_t& b) { return a.time < b.time; });
		}
		Console::printf("[dolly] point %d moved to %d ms", index + 1, time);
		return true;
	}

	bool go_to_point(const int index)
	{
		std::int32_t t = 0;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (index < 0 || index >= static_cast<int>(g_points.size()))
			{
				return false;
			}
			t = g_points[index].time;
		}
		// Points are stamped on active_time(); demo_seek_to seeks whichever
		// system is playing on that same clock, on the client thread.
		if (!demo_native::native_playing() && !demo_playback::is_playing())
		{
			Console::printf("[dolly] go: no demo playing");
			return false;
		}
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("demo_seek_to {}", t));
		return true;
	}

	int current_time()
	{
		return active_time();
	}

	std::vector<point_t> points()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_points;
	}

	int point_count()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return static_cast<int>(g_points.size());
	}

	bool enabled() { return g_enabled; }
	bool show_markers() { return g_show_markers; }
	void set_show_markers(const bool on) { g_show_markers = on; }

	void set_enabled(const bool on)
	{
		if (on && !g_hook_ok)
		{
			Console::printf("[dolly] cannot drive: the CL_Demo_FreeCameraMove hook is "
				"not installed. Markers and the path still draw.");
			return;
		}
		g_enabled = on;
		const int n = point_count();
		if (on && n < 2)
		{
			Console::printf("[dolly] ON, but %d point(s) — needs at least 2 to "
				"interpolate. Nothing will move yet.", n);
			return;
		}
		Console::printf("[dolly] %s", on
			? "ON — the camera follows the path over the points' own time span, "
			  "and is free outside it"
			: "OFF — camera released");
	}

	// =========================================================================
	void init()
	{
		// RULE A3 — a hook is not installed until it says so, and `create` also
		// returns true on MH_ERROR_ALREADY_CREATED while leaving `original` null.
		// So the null check on the trampoline is the real proof, not the bool.
		g_hook_ok = Hook::create("CL_Demo_FreeCameraMove",
			_b(ADDR_FREECAM_MOVE), &cl_demo_freecameramove_stub, &g_freecam_move_orig)
			&& g_freecam_move_orig != nullptr;

		Console::printf("[dolly] camera hook: %s (target=%p orig=%p)",
			g_hook_ok ? "OK" : "FAILED",
			reinterpret_cast<void*>(_b(ADDR_FREECAM_MOVE)),
			reinterpret_cast<void*>(g_freecam_move_orig));

		GameUtil::addCommand("dolly", []
		{
			// RULE A15 — reports unconditionally, so "no points" and "not playing"
			// and "hook dead" are three distinguishable answers.
			const auto pts = points();
			Console::printf("[dolly] %s | camera hook %s | %zu point(s) | markers %s",
				g_enabled ? "ON" : "OFF", g_hook_ok ? "live" : "DEAD",
				pts.size(), g_show_markers ? "on" : "off");
			// BOTH clocks, deliberately: if `smooth` is not advancing between two
			// `dolly` calls while `snap` is, the camera cannot move and this line
			// says so directly instead of leaving it to be guessed.
			Console::printf("[dolly]   native demo: %s | camera mode: %d (2 = free) | "
				"clock smooth(cl.serverTime)=%d ms  snap(cl.snap.serverTime)=%d ms",
				demo_native::native_playing() ? "playing" : "not playing",
				demo_native::camera_mode(), demo_native::demo_time_smooth(),
				demo_native::demo_time());
			for (std::size_t i = 0; i < pts.size(); ++i)
			{
				Console::printf("[dolly]   %2zu  t=%7d ms  pos (%8.1f %8.1f %8.1f)  "
					"ang (%6.1f %6.1f %6.1f)",
					i + 1, pts[i].time, pts[i].pos[0], pts[i].pos[1], pts[i].pos[2],
					pts[i].angles[0], pts[i].angles[1], pts[i].angles[2]);
			}
			if (pts.size() >= 2)
			{
				Console::printf("[dolly]   span %d -> %d ms (%.1f s)",
					pts.front().time, pts.back().time,
					static_cast<float>(pts.back().time - pts.front().time) / 1000.0f);
			}
		});

		GameUtil::addCommand("dolly_add", [] { add_point(); });
		GameUtil::addCommand("dolly_clear", [] { clear_points(); });
		GameUtil::addCommand("dolly_play", [] { play_from_start(); });

		GameUtil::addCommand("dolly_del", []
		{
			auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[dolly] usage: dolly_del <point number, 1-based>");
				return;
			}
			delete_point(GameUtil::safeStringToInt(args->argv[args->nesting][1]) - 1);
		});

		GameUtil::addCommand("dolly_on", [] { set_enabled(true); });
		GameUtil::addCommand("dolly_off", [] { set_enabled(false); });

		GameUtil::addCommand("dolly_markers", []
		{
			auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[dolly] markers are %s. usage: dolly_markers <0|1>",
					g_show_markers ? "ON" : "OFF");
				return;
			}
			g_show_markers = GameUtil::safeStringToInt(args->argv[args->nesting][1]) != 0;
			Console::printf("[dolly] in-world markers and path: %s",
				g_show_markers ? "ON" : "OFF");
		});
	}
}
