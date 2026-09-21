// =============================================================================
//  demo_native.cpp â€” S2's OWN demo system (main/demo/*.demo), made to work
// =============================================================================
//
//  NEW HERE? Read docs/ARCHITECTURE.md first, then demo_native.hpp (the public
//  surface, ~135 lines), then init() at the BOTTOM of this file â€” every hook is
//  installed there with a printed OK/FAILED line, so it doubles as a contents
//  page for what this file actually does.
//
//  This file is long because it is an accumulation of fixes for engine defects,
//  each proven before it was written. The comments record MEASUREMENTS, not
//  intentions: S2 is Arxan-protected, so the call graph is obfuscated, strings
//  have no xrefs, and much is reachable only by indirect dispatch. Re-deriving
//  any of it is expensive, so it is written down where it is used.
//
//  SECTION MAP (line numbers drift; search the quoted title)
//    "VIEWMODEL `hide` WATCHER"        probe: polls the byte gating the gun model
//    "THE VIEWMODEL FIX"               let the engine's own demo StreamSync run
//    "StreamSync PRE-SCAN"             replay the connect-time load request a
//                                      recording never captured
//    "DIRECT VIEWMODEL STREAM REQUEST" ask the streamer for the local weapon
//    "REMOTE PLAYERS"                  same, for other players' world models
//    "ASSET POOL CENSUS"               per-type accounting; solved the image limit
//    "THE FIX â€” give the demo path"    the asset releases the live paths perform
//    "CL_Demo_StartRecord"             the engine's own recorder
//    "ENABLING THE ENGINE'S OWN"       opening the gate config closes
//    "PUBLIC-MATCH DEMOS"              splice ncs type 21 into the footer
//    "CL_Demo_HandleAction"            demo key actions (pause/camera/seek/speed)
//    "FREE CAMERA SPEED"               make the baked-in constant adjustable
//    "PROBE ONLY -- \"4780\""            delta-index history for snapshot desyncs
//
//  CONVENTIONS
//    0x912D65_b   an engine address in `_b` form: runtime = base + 0x1000 + lit,
//                 so IDA 0x913D65 is written 0x912D65_b. Comments saying
//                 "IDA 0x..." mean the IDA address.
//    sub_XXXXXX   an engine function whose PURPOSE is known but whose real name
//                 is not. Deliberately not renamed to a guess â€” a wrong name is
//                 worse than none, because it misleads silently.
// =============================================================================

#include "pch.h"
#include "demo_native.hpp"

#include "demo/demo_game.hpp"
#include "demo/demo_playback.hpp"
#include "demo/demo_utils.hpp"

#include "hud/broadcaster.hpp"
#include "demo/bonecam.hpp"

#include "Console.hpp"
#include "demo/demo_library.hpp"
#include "DevMode.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"
#include "Hook.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>

#include "demo/demo_ncs21.inc"
#include <zlib.h>
#include <intrin.h>
#include <mutex>
#include <string>

namespace demo_native
{
	namespace
	{
		std::vector<std::filesystem::path> g_files;
		int g_selected = 0;

		// Set by play(), cleared at end-of-stream/abort. Declared here because the
		// viewmodel watcher and the StreamSync fix (both further down) gate on it.
		bool g_native_playing = false;

		std::mutex g_err_lock;
		std::vector<error_record> g_errors;
		constexpr std::size_t MAX_ERRORS = 64;

		// PROVEN 2026-08-08. CL_Demo_Play_f @ IDA 0x910650 opens with
		//     if (!*((BYTE*)dvar_com_sv_running + 16)) { ...whole function... }
		// MW3's twin is `if (!*(com_sv_running + 12))` â€” x86 dvar value at +12,
		// x64 at +16. Dvar_SetBool @ 0xB1FD0 reads dvar+12 as the TYPE, which
		// places the value at +16.
		constexpr std::size_t DVAR_VALUE_OFFSET = 16;

		[[nodiscard]] void* dvar_com_sv_running()
		{
			return *reinterpret_cast<void**>(0x1BD2778_b); // IDA 0x1BD3778
		}

		// Com_Error @ IDA 0x90750. Variadic: Com_Error(int code, const char* fmt, ...).
		// S2 replaces most Q3 error strings with numeric ids ("439", "440", "428",
		// "430", "460", "461"), so the message is usually a short number â€” the
		// decoded meanings live in CLAUDE.md.
		[[nodiscard]] std::uintptr_t addr_Com_Error() { return 0x8F750_b; }

		using Com_Error_fn = void(*)(int, const char*, ...);
		Com_Error_fn Com_Error_orig = nullptr;

		[[nodiscard]] std::uint64_t module_base()
		{
			return reinterpret_cast<std::uint64_t>(GetModuleHandleA(nullptr));
		}

		// Runtime address -> the address shown in IDA.
		// game.cpp: `base = GetModuleHandle(NULL) + 0x1000` and `_b(v) = base + v`,
		// and every literal in demo_game.hpp is (IDA - 0x1000). So
		//     VA = module_base + 0x1000 + (IDA - 0x1000) = module_base + IDA
		// i.e. IDA = VA - module_base, with NO 0x1000 adjustment. This matches
		// tools/dump_hang.py. Getting this wrong shifts every reported address by
		// 0x1000 and sends you to the neighbouring function.
		[[nodiscard]] std::uint64_t to_ida(const void* runtime)
		{
			const auto base = module_base();
			const auto addr = reinterpret_cast<std::uint64_t>(runtime);
			if (!base || addr < base)
			{
				return 0;
			}
			return addr - base;
		}

		// ---- native demo playback state -------------------------------------
		// PROVEN 2026-08-08 from CL_Demo_Play_f / CL_Demo_ReadDemoMessage /
		// CL_Demo_Read / CL_Demo_ReadFooter.
		//
		// CL_Demo_GetPlaybackData @ IDA 0x915DF0 is literally `return
		// qword_10F340A0`, so this global IS the playback state pointer.
		//
		// The single most useful field is +24: CL_Demo_Read does
		//     *(DWORD*)(playbackData + 24) += bytesRead;
		// making it a running count of bytes consumed from the .demo file.
		// Compare it against tools/s2_demo_read.py's packet walk to name the
		// exact packet the engine died on.
		// Is [p, p+n) actually committed and readable? The playback-state global is
		// NOT null when idle â€” measured 0xB442783145C80DB9 live with Cheat Engine at
		// the main menu â€” so a plain null check passes and the deref faults.
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

		[[nodiscard]] std::uintptr_t demo_playback_data()
		{
			const auto g = *reinterpret_cast<std::uintptr_t*>(0x10F330A0_b); // IDA qword_10F340A0
			// Reject anything that is not a plausible user-mode pointer, then confirm
			// the page is really there. Without this, every read below can fault.
			if (g < 0x10000 || g > 0x00007FFFFFFFFFFFull || (g & 7) != 0)
			{
				return 0;
			}
			return readable(reinterpret_cast<const void*>(g), 64) ? g : 0;
		}

		// clientConnection_t base. `mov rbp, cs:clc` @ 0x9108FA is a LOAD, so the
		// global holds a pointer. IDA 0x1BD3D00 -> literal 0x1BD2D00.
		// This read this global directly because demo_game::clc_for() used to point
		// at the wrong one (IDA 0x1BD3C00). THAT IS FIXED as of 2026-08-17, so the
		// two now agree; this is kept only to avoid a needless churn of a working
		// probe. The stride is sizeof(clientConnection_t) -- see re/clientConnection.h,
		// where it is proven by the struct summing to exactly this number.
		constexpr std::size_t CLC_STRIDE_NATIVE = 482656;

		[[nodiscard]] char* clc_native(const int client)
		{
			auto* base = *reinterpret_cast<char**>(0x1BD2D00_b); // IDA 0x1BD3D00 `clc`
			if (!base || client < 0 || client >= 4)
			{
				return nullptr;
			}
			return base + CLC_STRIDE_NATIVE * static_cast<std::size_t>(client);
		}

		// =====================================================================
		//  REAL SEEKING — pick the keyframe ourselves, drive the engine's jump
		// =====================================================================
		//
		// PROVEN 2026-08-10 from the three functions involved.
		//
		// sub_917ED0(client, kfIndex) is a COMPLETE seek primitive, not a helper:
		//   * sub_9123E0(fileHandle, slot[+2176588], 2)  <- repositions the demo
		//     FILE to the position saved when the keyframe was written
		//   * memset(cls_gameState_stringOffsets, 0, 0x26D8C) then
		//     CL_ParseConfigStrings_Internal              <- rebuilds the gamestate
		//   * MSG_ReadData(msg, 79356, PlaybackData + 76) <- the state block
		//   * replays the buffered messages:
		//       for (j = slot[+2176616]; j != slot[+2176620]; j = (j+1)%256)
		//           CL_Demo_ReadDemoMessage(...)
		//   * resyncs clientActive / cls_realtime to the keyframe time and calls
		//     CL_SetCGameTime
		//
		// So ANY slot with length > 0 is a valid jump target, and we do not need
		// GetKeyFrameForJumpBack at all. Its SELECTION is where seeking broke:
		// sub_915A10 only scans backwards when the current keyframe is newer
		// than a fixed 800 ms cutoff OR its index is in the 32-entry baseline
		// list, and otherwise returns the CURRENT index — "jump to where you
		// already are", which is the tiny nudge instead of a rewind.
		//
		// We still perform the baseline-then-target pair sub_916FC0 does,
		// because keyframes are delta-coded against a baseline:
		//     b = GetBaselineForKeyframe(client, kf);   // sub_9158C0, TWO args
		//     if (b != -1 && b != kf) ProcessKeyFrameJump(client, b);
		//     ProcessKeyFrameJump(client, kf);
		//
		//   sub_9158C0             IDA 0x9158C0 - 0x1000 = 0x9148C0
		//   sub_917ED0             IDA 0x917ED0 - 0x1000 = 0x916ED0
		//   CL_GetLocalClientActive IDA 0x795D0 - 0x1000 = 0x785D0
		//     (Arxan-encrypted, so there is no pointer to read — but we are
		//      in-process and can simply CALL it.)
		constexpr std::size_t ADDR_GET_BASELINE = 0x9148C0;
		constexpr std::size_t ADDR_PROCESS_JUMP = 0x916ED0;

		// Force the clock onto the keyframe's own time after a jump (see
		// jump_to_slot). Default ON; `demo_seek_forceclock 0` reverts to the
		// engine's own tail behaviour for comparison.
		bool g_force_seek_clock = true;
		constexpr std::size_t ADDR_CL_GETLOCALCLIENTACTIVE = 0x785D0;

		struct Slot { int index; int time; int length; };

		[[nodiscard]] std::uintptr_t local_client_active(const int client)
		{
			const auto fn = reinterpret_cast<std::uintptr_t(__fastcall*)(int)>(
				_b(ADDR_CL_GETLOCALCLIENTACTIVE));
			const auto p = fn(client);
			return readable(reinterpret_cast<const void*>(p), 25384) ? p : 0;
		}

		[[nodiscard]] int current_demo_time()
		{
			const auto ca = local_client_active(LOCAL_CLIENT_0);
			return ca ? *reinterpret_cast<const std::int32_t*>(ca + 25380) : -1;
		}

		// ⭐ WHY SEEKING WORKS AT FIRST AND THEN STOPS.
		//
		// A keyframe's PAYLOAD lives in a CIRCULAR buffer at PlaybackData+79432
		// that wraps at 0x200000 (2 MB) — ProcessKeyFrameJump reads it from there
		// and sub_914790 writes it there, both wrapping at 0x1FFFFF. Measured
		// payloads are ~115 KB each, so only ~18 keyframes fit before the ring
		// starts overwriting the oldest.
		//
		// A slot's `length` field keeps saying "I have data" after its bytes have
		// been overwritten, so a naive `length > 0` test hands out stale targets
		// and the jump restores garbage. That is exactly "it worked at the start
		// and stopped later".
		//
		// So walk NEWEST-FIRST from the write index and accumulate payload bytes.
		// Once the running total passes the buffer size, everything older has
		// been overwritten by definition of a ring buffer — no extra bookkeeping
		// needed, and it degrades correctly as keyframe sizes vary.
		constexpr std::int64_t KEYFRAME_BUFFER_BYTES = 0x200000;

		[[nodiscard]] std::vector<Slot> usable_slots()
		{
			std::vector<Slot> out;
			const auto g = demo_playback_data();
			if (!g)
			{
				return out;
			}
			const auto* rec = reinterpret_cast<const std::uint8_t*>(g + 2176584);
			const auto* widx = reinterpret_cast<const std::int32_t*>(g + 2188584);
			if (!readable(rec, 250 * 48) || !readable(widx, 4))
			{
				return out;
			}
			const int start = *widx;
			std::int64_t acc = 0;
			out.reserve(32);
			for (int k = 0; k < 250; ++k)
			{
				const int i = ((start - k) % 250 + 250) % 250;
				const auto* s = reinterpret_cast<const std::int32_t*>(
					rec + static_cast<std::size_t>(i) * 48);
				if (s[7] <= 0 || s[2] < 0)
				{
					continue;
				}
				// ⭐ REJECT AN EMPTY REPLAY RANGE. Measured 2026-08-11.
				//
				// ProcessKeyFrameJump restores the world through
				//     for (j = slot[+32]; j != slot[+36]; j = (j + 1) % 256)
				//         CL_Demo_ReadDemoMessage(...)
				// so when s[8] == s[9] the loop runs ZERO times and NO snapshot is
				// put back. Its tail then derives the whole clock from
				// cl.snap.serverTime, which is still NOW.
				//
				// Such a slot used to be offered as a seek target, and the
				// clock-force in jump_to_slot made the CLOCK land correctly while
				// the world state stayed stale — so the seek looked like it worked
				// and the game glitched. That is the reported "rewind works very
				// rarely and glitches out".
				//
				// Real ranges look like [0..153] or [153..156]; the first keyframe
				// of a session is typically [0..0] because nothing is buffered yet.
				//
				// ⚠ THE BYTES ARE COUNTED BEFORE THIS TEST, ON PURPOSE. A rejected
				// slot still OCCUPIES its payload in the ring, so skipping it
				// before `acc += s[7]` would under-count and let the walk reach
				// back past genuinely overwritten keyframes. (Introduced and
				// caught the same day; the offline sim reproduces it.)
				acc += s[7];
				if (acc > KEYFRAME_BUFFER_BYTES)
				{
					break;   // older than this, the payload has been overwritten
				}
				if (s[8] == s[9])
				{
					continue;   // restores no snapshot — see above
				}
				out.push_back(Slot{ i, s[2], s[7] });
			}
			return out;
		}

		// The engine's own baseline-then-target pair.
		// Drives the engine's own ProcessKeyFrameJump (sub_917ED0), which is a
		// COMPLETE rewind -- decompiled 2026-08-11, and it does by itself
		// everything IWXMVM has to do by hand:
		//
		//     sub_9123E0(demoFileHandle, slot.fileOffset /*+4*/, 2)   // seek the file
		//     PlaybackData[24] = slot.fileOffset                      // reset the cursor
		//     PlaybackData[8]  = 0                                    // clear "completed"
		//     CG_GetServerCommandsState(c)[24] = slot.cmdSeq /*+24*/
		//     clc[+131396] = clc[+131400] = slot.cmdSeq
		//     clc[+346144] = sub_9191D0                               // read from MEMORY
		//     memset(cls_gameState_stringOffsets, 0, 0x26D8C)         // wipe gamestate
		//     CL_ParseConfigStrings_Internal(c, msg)                  // reinstall it
		//     MSG_ReadData(msg, 79356, PlaybackData + 76)             // the state block
		//     ...replay the buffered messages (slot+32 .. slot+36)...
		//     clc[+346144] = CL_Demo_Read                             // back to FILE
		//     cl.oldFrameServerTime = cl.serverTime = cls_realtime
		//         = cl.oldServerTime = cl.snap.serverTime
		//     cl.serverTimeDelta = cl.snap.serverTime - cls_realtime
		//     CL_SetCGameTime(c)
		//
		// Slot layout, base PlaybackData+2176584, stride 48, PROVEN from this
		// function and from the scan in sub_915A10:
		//     +0  keyframe-memory offset      +4  FILE offset
		//     +8  time (== cl.snap.serverTime, clientActive+25380 = dword 6345)
		//     +24 command sequence            +28 length -- the scan needs > 0
		//     +32/+36 replay message range
		//
		// REPORTS UNCONDITIONALLY (RULE A15). "Rewind does nothing" is otherwise
		// indistinguishable between: the jump was never called, it was called and
		// returned without moving the file, or it moved and the clock did not.
		void jump_to_slot(const int index)
		{
			const auto baseline = reinterpret_cast<int(__fastcall*)(unsigned int, int)>(
				_b(ADDR_GET_BASELINE));
			const auto jump = reinterpret_cast<void(__fastcall*)(unsigned int, int)>(
				_b(ADDR_PROCESS_JUMP));
			const auto cl = static_cast<unsigned int>(LOCAL_CLIENT_0);

			const auto g = demo_playback_data();
			const auto slot_i32 = [g](const int slot, const std::size_t byte_off) -> std::int32_t
			{
				if (!g)
				{
					return -1;
				}
				const auto* p = reinterpret_cast<const std::int32_t*>(
					g + 2176584 + static_cast<std::size_t>(slot) * 48 + byte_off);
				return readable(p, 4) ? *p : -1;
			};

			// ⛔ PlaybackData+24 is a DWORD (ProcessKeyFrameJump stores it with
			// `mov [rax+18h], ecx`). Reading it as a QWORD picks up +28 -- the
			// timescale float -- as the high dword, which is why the first
			// diagnostic printed 4575657221410973728 = 0x3F800000_0026E7A0.
			const auto cursor = [g]() -> std::int32_t
			{
				if (!g) { return -1; }
				const auto* p = reinterpret_cast<const std::int32_t*>(g + 24);
				return readable(p, 4) ? *p : -1;
			};
			// cl.snap.serverTime, clientActive dword 6345. This is the value
			// ProcessKeyFrameJump's tail copies into the whole clock, so it is the
			// one that decides whether a rewind lands or snaps back.
			const auto snap_time = []() -> std::int32_t
			{
				const auto ca = reinterpret_cast<std::uint8_t*>(
					reinterpret_cast<std::uintptr_t(__fastcall*)(unsigned int)>(
						_b(0x785D0))(static_cast<unsigned int>(LOCAL_CLIENT_0)));
				if (!ca || !readable(ca + 6345 * 4, 4)) { return -1; }
				return *reinterpret_cast<const std::int32_t*>(ca + 6345 * 4);
			};

			const int t_before = current_demo_time();
			const int c_before = cursor();
			const int s_before = snap_time();
			const int b = baseline(cl, index);

			// slot+32 / slot+36 are the replay message range. If they are EQUAL the
			// engine replays NOTHING, no snapshot is restored, and the tail then
			// sets the clock from the CURRENT snap.serverTime -- which is exactly
			// the "+50 ms and back where we started" symptom.
			// The replay SPAN, not just the endpoints. ProcessKeyFrameJump counts
			// with `for (j = a; j != b; j = (j + 1) % 256)`, so it performs
			// ((b - a) mod 256) iterations and CANNOT represent a count of 256 or
			// more -- such a range aliases to (true mod 256) and replays the wrong
			// number of messages, silently.
			//
			// tools/s2_keyframe_sim.py flags this as the dominant failure mode IF
			// the span really is "messages since the previous keyframe". That
			// reading is an INFERENCE (slot+32 comes from clc+346128 and slot+36
			// from clientActive+90472 -- two different objects), so this prints the
			// span rather than acting on it. A span seen near 256 in a real
			// session confirms the hypothesis; spans that stay small refute it.
			const int ra = slot_i32(index, 32);
			const int rb = slot_i32(index, 36);
			const int span = (ra >= 0 && rb >= 0) ? ((rb - ra) & 255) : -1;
			Console::printf("[demo] jump slot %d: t=%d fileOff=%d len=%d cmdSeq=%d "
				"memOff=%d replay=[%d..%d] span=%d%s baseline=%d",
				index, slot_i32(index, 8), slot_i32(index, 4), slot_i32(index, 28),
				slot_i32(index, 24), slot_i32(index, 0), ra, rb, span,
				(ra == rb) ? " EMPTY"
					: (span > 200 ? " <<< SPAN NEAR THE 256 RING LIMIT" : ""),
				b);
			Console::printf("[demo]   before: demoT=%d snapT=%d cursor=%d",
				t_before, s_before, c_before);

			if (b != -1 && b != index)
			{
				jump(cl, b);
			}
			jump(cl, index);

			// =============================================================
			//  FORCE THE CLOCK TO THE KEYFRAME — from Caball009's CoD4-X
			//  Demo Rewinding (Call-of-Duty-4-X-Demo-Rewinding), 2026-08-11
			// =============================================================
			//
			//  ProcessKeyFrameJump's tail derives the whole clock FROM
			//  cl.snap.serverTime as it stands after the restore:
			//
			//      v39 = cl.snap.serverTime;
			//      cl.oldFrameServerTime = cl.serverTime = cls_realtime
			//          = cl.oldServerTime = v39;
			//      cl.serverTimeDelta = cl.snap.serverTime - cls_realtime;
			//      CL_SetCGameTime(client);
			//
			//  So if the restore does not itself put a snapshot back, v39 is
			//  still NOW, the clock is set to now, and CL_SetCGameTime replays
			//  straight forward again. Measured exactly that: +50 ms per press
			//  and the file cursor back where it started.
			//
			//  Caball009 inverts it — ResetOldClientData WRITES cl.snap.serverTime
			//  from the restore point rather than reading it. Same idea here, but
			//  using the engine's own tail with the value it should have had, so
			//  nothing is invented: every field below is one ProcessKeyFrameJump
			//  already writes, and the keyframe's own time (slot+8) is the value
			//  it was supposed to land on.
			//
			//  ⚠ oldServerTime MUST move with the rest. CL_SetCGameTime does
			//      if (cl.snap.serverTime < cl.oldServerTime) Com_Error(1, "440")
			//  which CLAUDE.md decodes as exactly that condition — the same error
			//  Caball009 has to NOP out at 0x45C511 on CoD4. Setting them together
			//  keeps it satisfied instead of patching it out.
			//
			//  clientActive dword indices, all proven and already in the IDB
			//  comment on CL_SetCGameTime:
			//      6345 snap.serverTime   6365 oldFrameServerTime
			//      6368 serverTime        6369 oldServerTime
			//      6370 serverTimeDelta
			if (g_force_seek_clock)
			{
				const int kf_time = slot_i32(index, 8);
				const auto ca = reinterpret_cast<std::uint8_t*>(
					reinterpret_cast<std::uintptr_t(__fastcall*)(unsigned int)>(
						_b(0x785D0))(cl));
				auto* rt = reinterpret_cast<std::int32_t*>(_b(0x1C7D1F0));
				if (kf_time > 0 && ca && readable(ca + 6371 * 4, 4) && readable(rt, 4))
				{
					const auto fld = [ca](const int i) -> std::int32_t&
					{
						return *reinterpret_cast<std::int32_t*>(ca + i * 4);
					};
					fld(6345) = kf_time;   // snap.serverTime  <- the one the tail reads
					fld(6365) = kf_time;   // oldFrameServerTime
					fld(6368) = kf_time;   // serverTime
					fld(6369) = kf_time;   // oldServerTime (keeps Com_Error 440 happy)
					*rt       = kf_time;   // cls_realtime
					fld(6370) = 0;         // serverTimeDelta = snap - cls_realtime
					Console::printf("[demo]   clock forced to keyframe t=%d "
						"(snap/old/serverTime/cls_realtime), delta=0", kf_time);
				}
			}

			Console::printf("[demo]   after : demoT=%d snapT=%d cursor=%d   "
				"(demoT %+d, snapT %+d, cursor %+d)",
				current_demo_time(), snap_time(), cursor(),
				current_demo_time() - t_before, snap_time() - s_before,
				cursor() - c_before);
			if (snap_time() > s_before - 1000)
			{
				Console::printf("[demo]   snap.serverTime did NOT go back -> the tail sets "
					"the whole clock from it, so CL_SetCGameTime replays straight back to "
					"now. The keyframe payload restored no snapshot.");
			}
		}

		void dump_playback_state(const char* when)
		{
			const auto g = demo_playback_data();
			if (!g)
			{
				Console::printf("[native] %s: qword_10F340A0 == 0 (no playback state)", when);
				return;
			}

			const auto rd32 = [g](const std::size_t off)
			{
				return *reinterpret_cast<std::int32_t*>(g + off);
			};
			const auto rd8 = [g](const std::size_t off)
			{
				return *reinterpret_cast<std::uint8_t*>(g + off);
			};

			const int client = rd32(0);
			const int filepos = rd32(24);      // CL_Demo_Read accumulator
			const int mode = rd32(3336304);    // 3 = playing, 2 = ended, 1 = clip capture
			const int fill = rd32(5433468);    // 2MB membuf fill level
			const int blk_count = rd32(6096400);
			const int blk_index = rd32(6096404);
			const int cursor = rd32(6096408);  // membuf read cursor

			Console::printf(
				"[native] %s: client=%d FILEPOS=%d (0x%X) mode=%d",
				when, client, filepos, filepos, mode);
			Console::printf(
				"[native]   membuf fill=%d cursor=%d block=%d/%d flags=%u/%u",
				fill, cursor, blk_index, blk_count,
				static_cast<unsigned>(rd8(6096396)), static_cast<unsigned>(rd8(6096413)));

			if (auto* slot = clc_native(client))
			{
				const auto cb = *reinterpret_cast<std::uintptr_t*>(slot + 346144);
				Console::printf(
					"[native]   readcb=IDA_0x%llX (912260=file, 919190=membuf) "
					"hdr ver=%d size=%d cl=%d",
					static_cast<unsigned long long>(to_ida(reinterpret_cast<void*>(cb))),
					*reinterpret_cast<int*>(slot + 262760),
					*reinterpret_cast<int*>(slot + 262764),
					*reinterpret_cast<int*>(slot + 262768));
			}
		}

		void cmd_native_state()
		{
			dump_playback_state("state");
		}

		// =====================================================================
		//  VIEWMODEL `hide` WATCHER â€” NATIVE PATH  (PROBE ONLY)
		// =====================================================================
		// USER OBSERVATION: on native playback the viewmodel and the third-person
		// models appear after roughly 20 seconds. `hide` is the byte
		// CG_UpdateViewModel @0x5D0C0 computes as
		//     hide = (XModel_AreImagesResident(BG_GetViewModel(weapon)) == 0)
		// and CG_BuildViewmodelDObj @0x5E980 uses to omit the gun XModel. So
		// "when does the gun pop in" is exactly "when does hide go 1 -> 0".
		//
		// POLLED, NOT HOOKED, on purpose. Every function on this path is ALREADY
		// hooked by demo_playback.cpp â€” CG_UpdateViewModel (3715),
		// CG_BuildViewmodelDObj, CG_AddPlayerWeapon and XModel_AreImagesResident â€”
		// and RULE A3.1 forbids a second hook on any of them. Reading one byte per
		// frame needs no hook at all.
		//
		// âš  WHY THE EXISTING PROBE CANNOT ANSWER THIS: demo_playback.cpp's
		// `[probe] hide 1->0` sits inside the AreImagesResident stub behind
		//     if (!theater || !active) return result;
		// so it is SILENT whenever the theater is not armed â€” i.e. on the whole
		// native path. That is why every viewmodel conclusion in this file is
		// theater-only and has to be re-measured here.
		int g_vm_last_hide = -2;      // -2 = not sampled yet
		unsigned g_vm_last_held = 0;
		void* g_vm_last_vm = nullptr;
		int g_vm_t0 = 0;              // cls_realtime at the first ACTIVE sample
		int g_vm_last_beat = -100000;
		bool g_vm_reported_pop = false;

		// CHEAP half: weapon index + viewmodel XModel + its name. Safe to call every
		// frame -- both engine calls are table lookups.
		//
		// USER OBSERVATION (gibraltar): the gun appeared "the moment I set up the
		// bipod". A bipod deploy is a weapon/attachment state change, so
		// BG_GetViewModel can start returning a DIFFERENT XModel -- one that may
		// already be resident. The first version of this watcher only logged `hide`
		// transitions, so a held/vm change would have been invisible. That is why
		// held and vm are now sampled every frame and logged on ANY change.
		void vm_describe_cheap(void* cg, unsigned& held, void*& vm, const char*& name)
		{
			held = 0; vm = nullptr; name = "<none>";
			using BG_GetHeldWeapon_fn = unsigned short* (*)(void*);
			using BG_GetViewModel_fn = void* (*)(void*, char, int);

			auto* weapon = reinterpret_cast<BG_GetHeldWeapon_fn>(
				demo_game::addr_BG_GetHeldWeapon())(cg);
			if (!weapon || !readable(weapon, 2) || !*weapon)
			{
				return;
			}
			held = *weapon;
			const auto flags = *reinterpret_cast<const unsigned int*>(
				static_cast<char*>(cg) + demo_game::PS_WEAPON_FLAGS);
			vm = reinterpret_cast<BG_GetViewModel_fn>(demo_game::addr_BG_GetViewModel())(
				weapon, (flags & 0x4000u) != 0 ? 1 : 0, 0);
			if (!vm || !readable(vm, 8))
			{
				vm = nullptr;
				return;
			}
			const auto* candidate = *reinterpret_cast<const char* const*>(vm);
			name = (candidate > reinterpret_cast<const char*>(0x10000)
				&& readable(candidate, 1)) ? candidate : "<null name>";
		}

		// EXPENSIVE half, only on a log line: XModel_AreImagesResident is hot AND is
		// already hooked by demo_playback.cpp, so it is not called per frame.
		[[nodiscard]] int vm_resident(void* vm)
		{
			if (!vm)
			{
				return -1;
			}
			using AIR_fn = std::int64_t(*)(void*);
			return static_cast<int>(
				reinterpret_cast<AIR_fn>(demo_game::addr_XModel_AreImagesResident())(vm));
		}

		// Population of a tier-0 image bitmap. PROVEN layout (CLAUDE.md): the space
		// is 4 tiers of 40192 bits, tier 0 first.
		//   residency IDA 0x79DB100 - 0x1000 = 0x79DA100   (the bit `hide` tests)
		//   requests  IDA 0x1506700 - 0x1000 = 0x1505700   (what XModel_Stream* ORs
		//                                                   in; DB_LoadLevelXAssets
		//                                                   clears 4*40192 of these)
		// Counting both each heartbeat distinguishes the two explanations for the
		// 40-second wait: a streamer steadily working through a queue (residency
		// climbs the whole time) versus a streamer that is idle until something
		// triggers it (residency flat, then a step).
		// âš  A POPULATION CANNOT SEE A SET CHANGE. One bit cleared and another set
		// leaves the count identical, so the gibraltar reading "requested = 11304
		// throughout" means the request COUNT never moved, NOT that the set is
		// frozen. Hence the hash alongside it.
		void bitmap_stats(const std::uintptr_t base, int& pop, std::uint32_t& hash)
		{
			pop = -1;
			hash = 0;
			const auto* w = reinterpret_cast<const std::uint32_t*>(base);
			if (!readable(w, 40192 / 8))
			{
				return;
			}
			int n = 0;
			std::uint32_t h = 2166136261u;
			for (int i = 0; i < 40192 / 32; ++i)
			{
				const auto v = w[i];
				n += static_cast<int>(__popcnt(v));
				h = (h ^ v) * 16777619u;
			}
			pop = n;
			hash = h;
		}

		[[nodiscard]] int bitmap_population(const std::uintptr_t base)
		{
			int pop; std::uint32_t h;
			bitmap_stats(base, pop, h);
			return pop;
		}

		// Per-frame streamer activity, coalesced so a burst is one line. Answers
		// "is the 40-second gap truly idle, or many small changes the 2s heartbeat
		// steps over" â€” and whether the request SET moves at constant population.
		int g_str_last_res_pop = -1;
		int g_str_last_req_pop = -1;
		std::uint32_t g_str_last_res_hash = 0;
		std::uint32_t g_str_last_req_hash = 0;
		int g_str_pending_res = 0;
		int g_str_req_set_changes = 0;
		int g_str_last_emit = -100000;
		int g_str_last_sample = -100000;

		void streamer_tick(const int t)
		{
			// Each bitmap_stats scans 1256 dwords; doing two of them EVERY frame is
			// pure diagnostic cost.
			//
			// BUG THIS FIXES: the first version bailed on `t - g_str_last_emit`,
			// but that field was only updated when a line was actually PRINTED.
			// Whenever residency was not moving -- i.e. most of the time -- the
			// timer stayed expired and both bitmaps were rescanned every frame.
			// Use a dedicated sample timestamp that advances on every sample.
			if (t - g_str_last_sample < 250)
			{
				return;
			}
			g_str_last_sample = t;
			int rp, qp; std::uint32_t rh, qh;
			bitmap_stats(0x79DA100_b, rp, rh);    // IDA 0x79DB100 residency
			bitmap_stats(0x1505700_b, qp, qh);    // IDA 0x1506700 requests
			if (rp < 0 || qp < 0)
			{
				return;
			}
			if (g_str_last_res_pop < 0)
			{
				g_str_last_res_pop = rp; g_str_last_res_hash = rh;
				g_str_last_req_pop = qp; g_str_last_req_hash = qh;
				return;
			}
			// A request-set change at CONSTANT population is exactly what the
			// population probe was blind to, so count it explicitly.
			if (qh != g_str_last_req_hash)
			{
				++g_str_req_set_changes;
				g_str_last_req_hash = qh;
			}
			g_str_pending_res += rp - g_str_last_res_pop;
			const bool moved = (rp != g_str_last_res_pop) || (rh != g_str_last_res_hash);
			g_str_last_res_pop = rp;
			g_str_last_res_hash = rh;

			if (moved && t - g_str_last_emit >= 250)
			{
				Console::printf(
					"[vmstream] t=%+7dms residency %+d -> %-6d | requests pop=%-6d "
					"setChanges=%d", t, g_str_pending_res, rp, qp, g_str_req_set_changes);
				g_str_pending_res = 0;
				g_str_req_set_changes = 0;
				g_str_last_emit = t;
			}
		}

		// =====================================================================
		//  THE VIEWMODEL FIX â€” let the engine's OWN demo StreamSync path run
		// =====================================================================
		// PROVEN OFFLINE (2026-08-08). sub_439EA0, the clientinfo parser, is
		// reached every snapshot via CG_DrawActiveFrame -> sub_43ADA0 ->
		// sub_43BCB0, and contains a branch Sledgehammer wrote FOR DEMOS:
		//
		//     if ( *(int*)(a4 + 236) > 0 ) {
		//         sub_43E310(...);                       // build the local model set
		//         if ( CL_IsDemoPlaying(a1)
		//              && !CL_StreamSync_IsCustomizationEnabled(a1) )
		//             CL_StreamSync_BuildAndCommitLocalPackage(a1, a4+236, &pkg);
		//
		// A demo has no connect-time server StreamSync (the recording starts AFTER
		// connect), so the engine builds the load package from local clientinfo
		// instead â€” but ONLY when customization is disabled. With it enabled (the
		// normal MP case) the engine waits for a server request that the file does
		// not contain, and the local player's weapon is never latched. Measured:
		// the gun appears 40.8s (egyptbots) / 68.1s (gibraltar) in, each time
		// within ~2s of the first incidental server StreamSync after a 35-58
		// second silence.
		//
		// `CL_StreamSync_IsCustomizationEnabled` @0x193630 has EXACTLY ONE code
		// caller â€” sub_439EA0 @0x43A077 â€” so returning 0 from it during native
		// playback takes the engine's own demo branch and changes nothing else.
		// The real global is untouched, so CL_StreamSync_ParseServerLoadRequest
		// (which reads it directly, not through this accessor) still works.
		//
		// Literals (RULE A1): 0x193630 - 0x1000 = 0x192630
		//                     0x439EA0 - 0x1000 = 0x438EA0
		// RULE A3.1 checked: nothing else in src/ hooks either target.
		// DEFAULT OFF â€” MEASURED 2026-08-08 to be a NO-OP. The flag is ALREADY 0
		// during playback ("customizationFlag=0"), so forcing it to 0 changes
		// nothing. The real blocker is the outer `*(int*)(a4+236) > 0` test, which
		// skips the whole block before the StreamSync call is reached. Kept only
		// because the hook is the vehicle for the diagnostic below; shipping it as
		// a "fix" would be dishonest.
		bool g_streamsync_local = false;

		using StreamSyncGate_fn = std::int64_t(*)(int);
		StreamSyncGate_fn CL_StreamSync_IsCustomizationEnabled_orig = nullptr;

		bool g_ss_gate_logged = false;

		std::int64_t streamsync_gate_stub(const int client)
		{
			const auto real = CL_StreamSync_IsCustomizationEnabled_orig(client);
			if (!g_native_playing || !g_streamsync_local)
			{
				return real;
			}
			if (!g_ss_gate_logged)
			{
				g_ss_gate_logged = true;
				Console::printf(
					"[vmfix] CL_StreamSync_IsCustomizationEnabled(%d) real=%lld -> "
					"forcing 0 so the engine's own demo branch runs "
					"(CL_StreamSync_BuildAndCommitLocalPackage)",
					client, static_cast<long long>(real));
			}
			return 0;
		}



		// =====================================================================
		//  StreamSync PRE-SCAN -- reconstruct the missing connect-time request
		// =====================================================================
		// PROVEN OFFLINE (tools/s2_streamsync.py + tools/s2_ss_decode.py): the
		// viewmodel appears at a FIXED demo time, matching the first StreamSync
		// command that adds an item to LIST 8:
		//     egyptbots  t=40550  list8   += 788891      measured pop 40780
		//     gibraltar  t=66050  list6+8 += 50595085    measured pop 68135
		// A live client receives that list at CONNECT. The recording began after
		// connect, so the demo carries only later incremental requests -- and the
		// gun waits until one of them happens to name the local weapon.
		//
		// So walk the .demo ONCE up front, collect every opcode-98 (switch case
		// 53) binary server command, and replay them at the first ACTIVE frame.
		// The bytes are the demo's OWN recorded requests: nothing is synthesised
		// or fabricated, they are merely delivered before they are needed rather
		// than 40 s late.
		//
		// The packet and svc walk are implemented here rather than through the
		// engine's MSG_* set so that no function signature has to be assumed.
		// The ONLY engine call is the Huffman decoder, whose signature is proven
		// from its own body:
		//     char sub_DCAB0(src, dst, srcLen, dstMax, int* outLen)   @ IDA 0xDCAB0
		std::vector<std::vector<std::uint8_t>> g_ss_replay;
		bool g_ss_prescan_done = false;
		bool g_ss_replay_done = false;
		bool g_ss_prescan_enabled = true;

		// Transcribed from MSG_ReadBits @0xDC910: the byte cursor and the bit
		// cursor advance independently, and a bit read resyncs from the byte
		// cursor when it is byte-aligned. Validated bit-exact against the engine.
		struct scan_msg
		{
			const std::uint8_t* d{};
			int size{};
			int rc{};
			int bp{};
			bool ovf{};

			std::uint8_t at(const int i) const
			{
				return (i < 0 || i >= size) ? 0xFFu : d[i];
			}

			std::uint32_t bits(int n)
			{
				std::uint32_t val = 0;
				int shift = 0;
				const int rem = bp & 7;
				if (rem)
				{
					val = static_cast<std::uint32_t>(at(bp >> 3)) >> rem;
					shift = 8 - rem;
					if (n <= 8 - rem)
					{
						bp += n;
						return val & ((1u << n) - 1u);
					}
					n -= 8 - rem;
				}
				int r = rc;
				bp = n + 8 * r;
				for (;;)
				{
					val |= static_cast<std::uint32_t>(at(r)) << shift;
					++r;
					n -= 8;
					if (n <= 0)
					{
						rc = r;
						break;
					}
					shift += 8;
				}
				return val;
			}

			const std::uint8_t* take(const int n)
			{
				if (n < 0 || rc + n > size)
				{
					ovf = true;
					rc = size;
					return nullptr;
				}
				const auto* p = d + rc;
				rc += n;
				return p;
			}

			int rd_long()
			{
				const auto* p = take(4);
				if (!p) return 0;
				int v = 0;
				std::memcpy(&v, p, 4);
				return v;
			}

			int rd_short()
			{
				const auto* p = take(2);
				if (!p) return 0;
				std::int16_t v = 0;
				std::memcpy(&v, p, 2);
				return v;
			}

			int rd_byte()
			{
				const auto* p = take(1);
				return p ? *p : 0;
			}

			void skip_string(const int limit)
			{
				for (int i = 0; i < limit; ++i)
				{
					if (rc >= size)
					{
						ovf = true;
						return;
					}
					if (d[rc++] == 0) return;
				}
			}
		};

		void collect_from_message(const std::uint8_t* body, const int len)
		{
			scan_msg m{body, len, 0, 0, false};
			for (int guard = 0; guard < 512; ++guard)
			{
				if (m.rc >= len && (m.bp >> 3) >= len) return;
				const auto op = m.bits(4);
				// 7 EOF, 0 gamestate, 6 snapshot (rest is delta-encoded) -> stop
				if (op == 7 || op == 0 || op == 6) return;
				if (op == 2)
				{
					m.rd_long();
					m.skip_string(1024);
				}
				else if (op == 3)
				{
					m.rd_long();
					const int ln = m.rd_short();
					if (ln <= 0 || ln > 0x3FC) return;
					const auto* p = m.take(ln);
					if (!p) return;
					if (p[0] == 98)          // 98 - 45 == switch case 53
					{
						g_ss_replay.emplace_back(p, p + ln);
					}
				}
				else if (op == 4)
				{
					const int c = m.rd_byte();
					if (c >= 0x90) return;
					for (int i = 0; i < c; ++i)
					{
						m.take(8);
						m.rd_short();
					}
				}
				else if (op == 5)
				{
					m.skip_string(64);
					m.take(5760);
				}
				else
				{
					return;
				}
				if (m.ovf) return;
			}
		}


		// ---- StreamSync payload decoder (list 8 membership only) --------------
		// Transcribed from CL_StreamSync_DataList_ParseRequiredData @0x6608D0.
		// Per-list index widths asc_B419D8 (u16[6]) and caps word_B419F8 (u16[6]),
		// read from the IDB. Only list 8 membership is retained; the other lists
		// are still parsed because they carry the bit alignment.
		//
		// The count width depends on sub_B8C40(), a runtime value. Rather than
		// assume it, BOTH are tried and the one that consumes the payload exactly
		// wins -- a wrong model overruns or leaves bits, so this is a check, not a
		// guess. Offline over all five shipped demos the clean answer is always
		// "narrow", with slack 0 on every command.
		const int SS_IDX_BITS[6] = {9, 9, 8, 8, 12, 9};
		const int SS_MAX_CNT[6]  = {389, 150, 100, 120, 3000, 386};

		int ss_count_bits(const int list, const bool wide)
		{
			if (list >= 6)
			{
				if (list == 6) return wide ? 9 : 7;
				return 3;
			}
			return wide ? 6 : 5;
		}

		struct ss_decoded
		{
			bool ok{};
			bool has_list8{};
			std::vector<std::uint32_t> list8;
		};

		bool ss_parse_list(scan_msg& m, const int list, const bool wide,
			std::vector<std::uint32_t>* out8)
		{
			const int n = static_cast<int>(m.bits(ss_count_bits(list, wide)));
			for (int i = 0; i < n; ++i)
			{
				if (m.ovf) return false;
				if (list >= 6 && list != 7)
				{
					std::uint32_t v = m.bits(10);
					if (m.bits(1))
					{
						v |= m.bits(6) << 10;
						if (m.bits(1))
						{
							v |= m.bits(6) << 16;
							if (m.bits(1))
							{
								v |= m.bits(6) << 22;
								if (m.bits(1))
								{
									m.bits(6);
									if (m.bits(1))
									{
										m.bits(6);
										if (m.bits(1)) m.bits(6);
									}
								}
							}
						}
					}
					m.bits(2);
					m.bits(3);
					if (list == 8 && out8) out8->push_back(v);
				}
				else if (list == 7)
				{
					m.bits(13);
					m.bits(2);
					m.bits(2);
				}
				else
				{
					const std::uint32_t idx = m.bits(SS_IDX_BITS[list]) + 1;
					m.bits(2);
					m.bits(2);
					if (list == 1) m.bits(6);
					if (idx > static_cast<std::uint32_t>(SS_MAX_CNT[list])) return false;
				}
			}
			return true;
		}

		ss_decoded ss_decode_one(const std::vector<std::uint8_t>& pay, const bool wide)
		{
			ss_decoded r{};
			scan_msg m{pay.data(), static_cast<int>(pay.size()), 0, 0, false};
			if (m.rd_byte() != 98) return r;
			bool good = true;
			for (int list = 0; list < 9; ++list)
			{
				if (m.bits(1))
				{
					if (list == 8) r.has_list8 = true;
					if (!ss_parse_list(m, list, wide, list == 8 ? &r.list8 : nullptr))
					{
						good = false;
						break;
					}
				}
			}
			const int used = (m.bp > m.rc * 8) ? m.bp : m.rc * 8;
			const int total = static_cast<int>(pay.size()) * 8;
			const int slack = total - used;
			r.ok = good && !m.ovf && slack >= 0 && slack < 8;
			return r;
		}

		ss_decoded ss_decode(const std::vector<std::uint8_t>& pay)
		{
			auto narrow = ss_decode_one(pay, false);
			if (narrow.ok) return narrow;
			auto wide = ss_decode_one(pay, true);
			if (wide.ok) return wide;
			return narrow;      // neither clean; caller treats !ok as "replay as-is"
		}


		void replay_streamsync(const int client)
		{
			if (g_ss_replay_done || g_ss_replay.empty()) return;
			g_ss_replay_done = true;

			// CG_ExecuteBinaryServerCommand does the MSG_Init/BeginReading and
			// dispatches on (firstByte - 45); every payload we collected is 98,
			// so each lands on case 53 -> CL_StreamSync_ParseServerLoadRequest,
			// through our stub, which opens the gate for the call.
			using ExecBin_fn = void(*)(unsigned int, void*);
			const auto exec = reinterpret_cast<ExecBin_fn>(0x430B20_b);  // IDA 0x431B20

			// REPLAY POLICY -- validated offline on both demos before it was
			// written here. CommitNewSyncData DIFFS: items absent from the new
			// list are released (sub_194170). Replaying every command in order
			// therefore ends in the demo's FINAL state, which for egyptbots does
			// NOT contain the item the viewmodel needs:
			//     replay all         -> list8 [9569,198,46794107,34,853403,307]  (no 788891)
			//     monotone-list8     -> list8 [854309,9569,459969,198,788891]    (has it)
			// So skip any command whose list 8 would DROP an item we have already
			// accumulated. List 6 is unaffected either way (31 items in both), and
			// gibraltar replays all 4 commands under both policies.
			std::vector<std::uint8_t> cmd;
			std::vector<std::uint32_t> acc8;
			int issued = 0, skipped = 0, undecoded = 0;
			for (const auto& pay : g_ss_replay)
			{
				if (pay.empty() || pay.size() > 0x3FC) continue;

				const auto dec = ss_decode(pay);
				if (!dec.ok)
				{
					++undecoded;        // do not let a decode miss drop real data
				}
				else if (dec.has_list8)
				{
					bool shrinks = false;
					for (const auto have : acc8)
					{
						if (std::find(dec.list8.begin(), dec.list8.end(), have)
							== dec.list8.end())
						{
							shrinks = true;
							break;
						}
					}
					if (shrinks)
					{
						++skipped;
						continue;
					}
					for (const auto v : dec.list8)
					{
						if (std::find(acc8.begin(), acc8.end(), v) == acc8.end())
						{
							acc8.push_back(v);
						}
					}
				}

				cmd.assign(4, 0);
				cmd[0] = static_cast<std::uint8_t>('Z');
				const auto n = static_cast<std::uint16_t>(pay.size());
				std::memcpy(cmd.data() + 2, &n, 2);
				cmd.insert(cmd.end(), pay.begin(), pay.end());
				exec(static_cast<unsigned int>(client), cmd.data());
				++issued;
			}
			Console::printf("[ss] REPLAYED %d recorded StreamSync request(s) up front "
				"(%d skipped as list-8 shrinking, %d undecodable), list8 now holds %zu "
				"item(s) -- this is the connect-time request the demo never carried",
				issued, skipped, undecoded, acc8.size());
		}

		// The viewmodel fix must cover BOTH playback paths. The custom theater
		// (.dm_s2) is a completely separate feed from native cl_demo_play, and
		// gating on g_native_playing alone silently skipped it.
		bool playback_active()
		{
			return g_native_playing || demo_playback::is_active_replay();
		}

		// Diagnostics OFF by default. The [vmnative]/[vmstream]/[vmimg] watcher
		// costs several VirtualQuery syscalls plus two 40192-bit bitmap scans per
		// frame, which measurably cost frames during playback. The FIX itself is
		// one engine call at 2 Hz and always runs. `demo_vm_debug` brings the
		// instrumentation back when something needs measuring.
		bool g_vm_debug = false;
		int g_vm_fix_last = -100000;

		// ---- StreamSync execution trace ---------------------------------------
		// The pop is at a FIXED demo time in every run, including builds that
		// predate the frontend release -- so it is a demo-STREAM event, not a
		// streaming race. Offline, the pop lands 230 ms (egyptbots) / 2085 ms
		// (gibraltar) after the first opcode-98 StreamSync command that follows a
		// 35 s / 58 s silence. That is the candidate trigger.
		//
		// CONTRADICTION this exists to resolve: CL_StreamSync_ParseServerLoadRequest
		// opens `if (!flag) return`, and the flag measured 0 in the first six
		// clientinfo parses (all within ~1 s). If it is 0 at 40 s too, the demo's
		// StreamSync commands do nothing and the correlation cannot be the
		// mechanism. Both cannot be true. RULE A15: report unconditionally.
		using SS_Parse_fn = void(*)(int, void*);
		SS_Parse_fn CL_StreamSync_ParseServerLoadRequest_orig = nullptr;
		using SS_Commit_fn = void(*)(int, void*, int);
		SS_Commit_fn CL_StreamSync_DataList_CommitNewSyncData_orig = nullptr;
		int g_ss_parse_calls = 0, g_ss_commit_calls = 0;

		// Gate 1 (must be NON-zero) and gate 2 (must be ZERO), read AT the call so
		// the values are contemporaneous rather than sampled at t=0 (RULE A8).
		std::int64_t ss_gate1(const int client)
		{
			const auto* p = reinterpret_cast<const std::uint32_t*>(
				0x6CEAB84_b + static_cast<std::uintptr_t>(51872) * client);
			return readable(p, 4) ? *p : -1;
		}
		std::int64_t ss_gate2(const int client)
		{
			const auto* p = reinterpret_cast<const std::uint32_t*>(
				0x1BAE454_b + static_cast<std::uintptr_t>(1976) * client);
			return readable(p, 4) ? *p : -1;
		}

		// CANDIDATE FIX, on by default so one run tests it and traces it.
		//
		// CL_StreamSync_ParseServerLoadRequest opens
		//     if (!s_streamSyncCustomizationEnabled[51872*client]) return;
		// and that flag measured 0 during playback. The demo carries a burst of
		// ~26 distinct StreamSync commands at t=3950..5100 (up to 180 B each);
		// if the gate is shut then, every one is DISCARDED and the viewmodel
		// waits for whichever later command happens to arrive after the gate
		// opens -- t=40550 for egyptbots, t=66050 for gibraltar, both of which
		// match the measured pop offline to within 230 / 2085 ms.
		//
		// So the gate is opened ONLY for the duration of this one call and then
		// restored, rather than writing the global for the session. Five other
		// functions read it (sub_192230, sub_193790, sub_195F30, sub_193D70,
		// CommitNewSyncData) and none of them see the forced value. Nothing is
		// fabricated: the request data is the demo's OWN recorded bytes, merely
		// no longer thrown away.
		bool g_ss_force_gate = true;

		void streamsync_parse_stub(const int client, void* msg)
		{
			const auto g1 = ss_gate1(client), g2 = ss_gate2(client);
			++g_ss_parse_calls;
			const bool would_run = (g1 != 0 && g2 == 0);
			const bool forcing = g_native_playing && g_ss_force_gate
				&& !would_run && g2 == 0;

			if (g_native_playing)
			{
				Console::printf("[ss] ParseServerLoadRequest #%d client=%d "
					"gate1=%lld (needs != 0) gate2=%lld (needs == 0) -> %s",
					g_ss_parse_calls, client,
					static_cast<long long>(g1), static_cast<long long>(g2),
					would_run ? "PROCEEDS"
						: forcing ? "was EARLY RETURN -> FORCING gate open"
						: "EARLY RETURN, does nothing");
			}

			if (!forcing)
			{
				CL_StreamSync_ParseServerLoadRequest_orig(client, msg);
				return;
			}
			auto* gate = reinterpret_cast<std::uint32_t*>(
				0x6CEAB84_b + static_cast<std::uintptr_t>(51872) * client);
			if (!readable(gate, 4))
			{
				CL_StreamSync_ParseServerLoadRequest_orig(client, msg);
				return;
			}
			const auto saved = *gate;
			*gate = 1;
			CL_StreamSync_ParseServerLoadRequest_orig(client, msg);
			*gate = saved;          // restored immediately; no other reader sees 1
		}

		void streamsync_commit_stub(const int client, void* data, const int list_idx)
		{
			++g_ss_commit_calls;
			if (g_native_playing)
			{
				Console::printf("[ss] CommitNewSyncData #%d client=%d list=%d "
					"<- this is what actually triggers the loads",
					g_ss_commit_calls, client, list_idx);
			}
			CL_StreamSync_DataList_CommitNewSyncData_orig(client, data, list_idx);
		}

		// PROBE half: sub_439EA0 is hooked ONLY to report the four conditions that
		// gate the local build, so one run both applies the fix and diagnoses which
		// gate was actually shut. Three alternatives to the flag were never
		// excluded offline: CL_IsDemoPlaying false, *(int*)(a4+236) <= 0, or this
		// function never being reached for the local client.
		using ClientInfoParse_fn = std::int64_t(*)(unsigned int, unsigned int, void*,
			std::int64_t, unsigned int*, std::int64_t);
		ClientInfoParse_fn sub_439EA0_orig = nullptr;
		int g_ci_calls = 0;

		std::int64_t client_info_parse_stub(const unsigned int a1, const unsigned int a2,
			void* a3, const std::int64_t a4, unsigned int* a5, const std::int64_t a6)
		{
			if (g_native_playing && g_ci_calls < 6)
			{
				++g_ci_calls;
				int n236 = -1;
				if (a4 && readable(reinterpret_cast<void*>(a4 + 236), 4))
				{
					n236 = *reinterpret_cast<int*>(a4 + 236);
				}
				using IsDemo_fn = bool(*)(unsigned int);
				const bool demo = reinterpret_cast<IsDemo_fn>(
					demo_game::addr_CL_IsDemoPlaying())(a1);
				const auto flag = CL_StreamSync_IsCustomizationEnabled_orig
					? CL_StreamSync_IsCustomizationEnabled_orig(static_cast<int>(a1))
					: -1;
				Console::printf(
					"[vmfix] clientinfo parse #%d client=%u ent=%u | *(a4+236)=%d (>0 required) "
					"| CL_IsDemoPlaying=%d | customizationFlag=%lld (0 required) -> local build %s",
					g_ci_calls, a1, a2, n236, demo ? 1 : 0,
					static_cast<long long>(flag),
					(n236 > 0 && demo) ? "REACHED (gate forced open)" : "BLOCKED upstream");
			}
			return sub_439EA0_orig(a1, a2, a3, a4, a5, a6);
		}

		// ---- PROBE: per-image state of the held viewmodel --------------------
		// THE question for "stream the viewmodel locally": are its images already
		// REQUESTED and merely not serviced, or is nothing asking at all? Those
		// need OPPOSITE fixes, and `demo_stream_hard` failing earlier is
		// consistent with both, which is why it settled nothing.
		//
		// The material->image walk is duplicated from demo_playback.cpp rather
		// than shared: that file has two anonymous-namespace blocks and its
		// helpers are not linkable from outside the first one. Offsets are the
		// same ones sub_1A42F0 uses and are documented here so the copy is
		// checkable:
		//     streamable node: +32 flags (bit 0x200 = has children), +42 child
		//                      count, +8 child array (stride 48),
		//                      +43 material count, +16 material list
		//     material:        +162 image count, +192 image array (stride 16,
		//                      the GfxImage* at +8 of each entry)
		//     image index:     (GfxImage - unk_476C138) / 96
		//
		// âš  REQUEST-BIT CAVEAT: sub_A68C0 tests the request bit at
		//     *((_DWORD*)&unk_1506700 + (imageIndex >> 5) + 941344)
		// i.e. a +941344-DWORD offset, NOT the base. An earlier probe of mine
		// sampled the BASE and reported "requests never change" â€” that reading is
		// UNRELIABLE. Both are printed, labelled, until one is proven.
		void collect_mats(const std::uintptr_t node, std::uintptr_t* mats,
			int* mat_n, const int cap, int depth = 0)
		{
			if (!node || *mat_n >= cap || depth > 8 || !readable(
				reinterpret_cast<const void*>(node), 48))
			{
				return;
			}
			const auto* base = reinterpret_cast<const std::uint8_t*>(node);
			const auto flags = *reinterpret_cast<const std::uint32_t*>(base + 32);
			if ((flags & 0x200u) != 0)
			{
				const int n = base[42];
				const auto* arr = *reinterpret_cast<const std::uint8_t* const*>(base + 8);
				if (!arr || !readable(arr, 8)) return;
				for (int i = 0; i < n && *mat_n < cap; ++i)
				{
					collect_mats(*reinterpret_cast<const std::uintptr_t*>(arr + 48 * i),
						mats, mat_n, cap, depth + 1);
				}
				return;
			}
			const int n = base[43];
			const auto* list = *reinterpret_cast<const std::uintptr_t* const*>(base + 16);
			if (!list || !readable(list, 8)) return;
			for (int i = 0; i < n && *mat_n < cap; ++i)
			{
				if (list[i]) mats[(*mat_n)++] = list[i];
			}
		}

		// The name pointer inside a record is found EMPIRICALLY rather than from a
		// guessed struct offset: scan the record's qwords for the first that points
		// at plausible ASCII. Per the Absolute Rule, an unproven offset gets no
		// semantic name -- but a printable string at a stable slot is self-evident.
		const char* scan_name(const std::uintptr_t rec, const std::size_t rec_size)
		{
			if (!rec || !readable(reinterpret_cast<const void*>(rec), rec_size))
			{
				return nullptr;
			}
			for (std::size_t off = 0; off + 8 <= rec_size; off += 8)
			{
				const auto p = *reinterpret_cast<const std::uintptr_t*>(rec + off);
				if (p < 0x10000 || p > 0x00007FFFFFFFFFFFull) continue;
				const auto* c = reinterpret_cast<const char*>(p);
				if (!readable(c, 8)) continue;
				int len = 0;
				while (len < 96 && c[len] >= 32 && c[len] < 127) ++len;
				if (len >= 4 && len < 96 && c[len] == 0) return c;
			}
			return nullptr;
		}

		void dump_vm_image_state(const char* why, void* vm)
		{
			if (!vm || !readable(vm, 40))
			{
				return;
			}
			// Which residency TIER this model is actually held to, from
			// XModel_AreImagesResident @0x1961F0:
			//   flags & 0x200000 -> tier 1, fallback bitmap allowed
			//   else !(flags & 0x400000) -> tier 0, fallback allowed
			//   else -> tier 0, fallback NOT allowed (the strict case)
			// XModel_TestImageResidencyBits indexes 40192*tier + streamIndex, so a
			// tier-0-only probe misreads any model held to tier 1.
			const auto xflags = *reinterpret_cast<const std::uint32_t*>(
				static_cast<const char*>(vm) + 32);
			const int want_tier = (xflags & 0x200000u) ? 1 : 0;
			const bool fallback_ok = (xflags & 0x200000u) || !(xflags & 0x400000u);
			std::uintptr_t mats[64]{};
			int mat_n = 0;
			collect_mats(reinterpret_cast<std::uintptr_t>(vm), mats, &mat_n, 64);

			const auto gfx_base = 0x476B138_b;   // IDA unk_476C138
			const auto* res_bits = reinterpret_cast<const std::uint32_t*>(0x79DA100_b);
			const auto* req_base = reinterpret_cast<const std::uint32_t*>(0x1505700_b);
			const auto* req_off = req_base + 941344;

			int n = 0, res = 0, qb = 0, qo = 0, tier[4] = {0,0,0,0}, alt = 0;
			unsigned miss_idx[32]{}; std::uintptr_t miss_mat[32]{}; int miss_n = 0;
			std::string first;
			for (int mi = 0; mi < mat_n; ++mi)
			{
				const auto* mat = reinterpret_cast<const std::uint8_t*>(mats[mi]);
				if (!readable(mat, 200)) continue;
				const unsigned img_n = mat[162];
				const auto* arr = *reinterpret_cast<const std::uint8_t* const*>(mat + 192);
				if (!arr || !img_n || !readable(arr, 16)) continue;
				for (unsigned ii = 0; ii < img_n && n < 512; ++ii)
				{
					const auto img = *reinterpret_cast<const std::uintptr_t*>(arr + 16u * ii + 8);
					if (!img || img < gfx_base) continue;
					const unsigned idx = static_cast<unsigned>((img - gfx_base) / 96u);
					if (idx >= 40192u) continue;
					const unsigned w = idx >> 5, m = 0x80000000u >> (idx & 31);
					// per-tier: slot = 40192*t + idx
					for (int t = 0; t < 4; ++t)
					{
						const unsigned sl = 40192u * t + idx;
						if ((res_bits[sl >> 5] & (0x80000000u >> (sl & 31))) != 0) ++tier[t];
					}
					const unsigned wslot = 40192u * want_tier + idx;
					const bool r = (res_bits[wslot >> 5]
						& (0x80000000u >> (wslot & 31))) != 0;
					const auto* alt_bits = reinterpret_cast<const std::uint32_t*>(0x189CB80_b);
					if (!r && fallback_ok && (alt_bits[w] & m) != 0) ++alt;
					if (!r)
					{
						bool seen = false;
						for (int q = 0; q < miss_n; ++q) if (miss_idx[q] == idx) { seen = true; break; }
						if (!seen && miss_n < 32)
						{
							miss_idx[miss_n] = idx;
							miss_mat[miss_n] = mats[mi];
							++miss_n;
						}
					}
					const bool b = (req_base[w] & m) != 0;
					const bool o = (req_off[w] & m) != 0;
					res += r; qb += b; qo += o;
					if (n < 8)
					{
						char t[48];
						std::snprintf(t, sizeof(t), " %u%s%s%s", idx,
							r ? "R" : "-", b ? "b" : "-", o ? "o" : "-");
						first += t;
					}
					++n;
				}
			}
			if (!n)
			{
				Console::printf("[vmimg] %s: %d materials, NO images resolved", why, mat_n);
				return;
			}
			Console::printf("[vmimg] %s: %d mats %d images (cap 512) | xflags=%08X wantTier=%d fallbackOK=%d",
				why, mat_n, n, xflags, want_tier, fallback_ok ? 1 : 0);
			Console::printf("[vmimg]    resident(wantTier) %d/%d  altBitmap covers %d | per-tier resident: t0=%d t1=%d t2=%d t3=%d",
				res, n, alt, tier[0], tier[1], tier[2], tier[3]);
			Console::printf("[vmimg]    reqBASE %d/%d  reqOFF(+941344) %d/%d | %d DISTINCT missing image(s) of %d refs",
				qb, n, qo, n, miss_n, n - res);
			for (int q = 0; q < miss_n; ++q)
			{
				const auto rec = gfx_base + 96ull * miss_idx[q];
				const char* iname = scan_name(rec, 96);
				const char* mname = scan_name(miss_mat[q], 8);
				Console::printf("[vmimg]      MISSING #%d idx=%-6u image='%s'  via material='%s'",
					q, miss_idx[q], iname ? iname : "<no ascii>", mname ? mname : "<no ascii>");
			}
		}


		// =====================================================================
		//  DIRECT VIEWMODEL STREAM REQUEST
		// =====================================================================
		// WHY THIS AND NOT THE REPLAY ALONE (proven offline, all five demos):
		// the viewmodel appears exactly when list 8 gains a THIRD entry. Two
		// entries is a base pair that every demo carries; the third is the local
		// player's weapon.
		//
		//     egyptbots  list8 2 -> 3 at t=40550   gun 40780
		//     gibraltar  list8 2 -> 3 at t=66050   gun 68135
		//     airship    list8 stays [9569, 198]        gun NEVER   (user-confirmed)
		//     dday       list8 stays [21234957, 308]    gun NEVER   (predicted)
		//
		// So for airship and dday there is NOTHING to replay -- the recording
		// never contains a request for the local weapon at all. Replaying only
		// helps egyptbots and gibraltar.
		//
		// THE REQUEST PATH, traced end to end:
		//   CommitNewSyncData -> sub_193F90 -> sub_196220(model, list, idx, tier, fb)
		//                     -> sub_1A4240(model, cat, prio, tier, fb)
		//                     -> sub_1A2F40   (the streamer's own request)
		// and the streamer tick (sub_A68C0 -> sub_A6DD0/sub_A6BD0 -> sub_A73A0)
		// is what finally ORs the residency bit at 0x79DB100.
		//
		// sub_196220 maps list 8 -> category 2 and idx 0 -> priority 127 (max).
		// tier/fallback are the SAME pair XModel_AreImagesResident tests with:
		//   flags & 0x200000        -> (1, 5)
		//   else !(flags & 0x400000)-> (0, 5)      <-- our viewmodel: xflags 0x00076A00
		//   else                    -> (0, 0)
		// so (0, 5) is requested, matching the test that gates `hide`. The XModel
		// flags are deliberately NOT modified: setting 0x400000 would switch the
		// test to the STRICT variant and could make things worse.
		//
		// This is NOT the old demo_stream_hard, which failed. That used
		// XModel_StreamHard @0x4DD3A0 to OR bits into unk_1506700 -- advisory
		// request bits that nothing services. This is the customization streamer's
		// own entry point, the one the engine itself uses for every StreamSync
		// item, and it is issued for the model the ENGINE selected as the local
		// viewmodel (BG_GetViewModel of ps.weapon). Nothing is invented.
		bool g_vm_request_enabled = true;
		int g_vm_requests = 0;
		int g_vm_request_last = 0;
		const void* g_vm_request_model = nullptr;

		void request_viewmodel_stream(void* vm, const int hide, const int now)
		{
			if (!g_vm_request_enabled || !vm || !hide) return;

			// Rate-limit BEFORE validating: readable() is a VirtualQuery syscall,
			// and running two of them per frame purely to reach a 500 ms gate was
			// costing real frames.
			if (vm == g_vm_request_model && now - g_vm_request_last < 500) return;

			// The model's first qword is its name -- sub_1A4240 does
			// I_stricmp(*a1, "tag_origin") -- so a bad pointer would fault there.
			if (!readable(vm, 40)) return;
			const auto name = *reinterpret_cast<const char* const*>(vm);
			if (!name || !readable(name, 8)) return;
			g_vm_request_model = vm;
			g_vm_request_last = now;

			using StreamModel_fn = std::int64_t(*)(void*, int, int, unsigned, unsigned);
			const auto req = reinterpret_cast<StreamModel_fn>(0x195220_b);  // IDA 0x196220
			req(vm, 8, 0, 0u, 5u);
			++g_vm_requests;
			if (g_vm_requests <= 3 || (g_vm_requests % 20) == 0)
			{
				Console::printf("[vmreq] #%d requested viewmodel stream: '%.48s' "
					"(list 8, priority 127, tier 0, fallback allowed)",
					g_vm_requests, name);
			}
		}


		// =====================================================================
		//  REMOTE PLAYERS -- world models
		// =====================================================================
		// The local fix covers BG_GetViewModel(ps.weapon) only. Remote players
		// draw WORLD models: a different XModel set from the same WeaponDef.
		// Proven from the accessors themselves --
		//     BG_GetViewModel  @0x3BD8F0 -> sub_3B6E40(weapon, alt, slot 16/24, i)
		//     BG_GetWorldModel @0x3BDD30 -> sub_3B6E40(weapon, alt, slot 0x388/0x390, i)
		// sub_3B6E40 indexes the WeaponDef by BYTE OFFSET, so the two differ only
		// in which slot they read. Requesting the viewmodel therefore does nothing
		// for anyone else's gun.
		//
		// Rather than enumerate entities and re-derive each player's weapon, hook
		// the accessor: whenever the engine resolves a world model it is telling
		// us exactly which model it wants, for every remote player and every
		// attachment, at the moment it wants it. Each distinct model is requested
		// once (and retried a few times, since there is no per-model `hide` byte
		// to tell us whether it landed).
		//
		// RULE A3.1: DevPatches.cpp DEFINES BG_GetWorldModel_hookfunc but never
		// calls Hook::create for it -- it is dead code, so there is no duplicate.
		using WorldModel_fn = void*(*)(void*, char, int);
		WorldModel_fn BG_GetWorldModel_orig = nullptr;
		bool g_world_request_enabled = true;
		int g_world_requests = 0;

		struct model_req { const void* model; int last_ms; int count; };
		model_req g_world_seen[128]{};
		int g_world_seen_n = 0;

		// True when this model should be requested now.
		bool should_request_model(const void* m, const int now)
		{
			for (int i = 0; i < g_world_seen_n; ++i)
			{
				if (g_world_seen[i].model != m) continue;
				// Retry a few times: unlike the viewmodel there is no `hide` byte
				// to confirm arrival, so one request is not obviously enough.
				if (g_world_seen[i].count >= 5) return false;
				if (now - g_world_seen[i].last_ms < 2000) return false;
				g_world_seen[i].last_ms = now;
				++g_world_seen[i].count;
				return true;
			}
			if (g_world_seen_n >= 128) return false;
			g_world_seen[g_world_seen_n++] = {m, now, 1};
			return true;
		}

		void* bg_get_world_model_stub(void* weapon, char alt, int variation)
		{
			void* model = BG_GetWorldModel_orig(weapon, alt, variation);
			if (!playback_active() || !g_world_request_enabled || !model)
			{
				return model;
			}
			// HOT PATH: this runs per player per frame. The cache probe must come
			// FIRST -- readable() is a VirtualQuery syscall, and putting it ahead
			// of the cache cost real frames. Validation now happens at most a few
			// times per distinct model instead of thousands of times per second.
			const int now = demo_game::cls_realtime();
			if (!should_request_model(model, now)) return model;

			if (!readable(model, 40)) return model;
			const auto name = *reinterpret_cast<const char* const*>(model);
			if (!name || !readable(name, 8)) return model;

			using StreamModel_fn = std::int64_t(*)(void*, int, int, unsigned, unsigned);
			const auto req = reinterpret_cast<StreamModel_fn>(0x195220_b);  // IDA 0x196220
			req(model, 8, 0, 0u, 5u);
			++g_world_requests;
			if (g_world_requests <= 12 || (g_world_requests % 25) == 0)
			{
				Console::printf("[vmreq] world model #%d: '%.48s' (remote player / attachment)",
					g_world_requests, name);
			}
			return model;
		}

		void vm_log(const char* why, const int t, const int hide,
			const unsigned held, void* vm, const char* name)
		{
			const int resident = vm_resident(vm);
			// Sampled EVERY heartbeat, not just at t=0: the six clientinfo parses
			// that measured customizationFlag=0 all happened inside the first
			// second, and the viewmodel pops at 40-68 s. If the flag turns on
			// later, that alone resolves the ParseServerLoadRequest contradiction.
			const auto* cflag = reinterpret_cast<const std::uint32_t*>(0x6CEAB84_b);
			const long long cust = readable(cflag, 4)
				? static_cast<long long>(*cflag) : -1;
			const int res_pop = bitmap_population(0x79DA100_b);   // IDA 0x79DB100
			const int req_pop = bitmap_population(0x1505700_b);   // IDA 0x1506700
			Console::printf(
				"[vmnative] t=%+7dms %-16s hide=%d held=%-4u resident=%-2d "
				"tier0 resident=%-6d requested=%-6d custFlag=%lld ssParse=%d "
				"ssCommit=%d vm=%p name=%.48s",
				t, why, hide, held, resident, res_pop, req_pop, cust,
				g_ss_parse_calls, g_ss_commit_calls, vm, name);
		}

		// =====================================================================
		//  ASSET POOL CENSUS + ALLOCATION TALLY
		// =====================================================================
		// The image-limit blocker ("Exceeded limit of 40192 'image' assets.") is
		// a REGISTRATION-COUNT problem, not memory: at the moment of failure the
		// pool was full while the residency bitmap was only ~12% set. So the
		// question is "what registers 40192 images", and until 2026-08-08 it was
		// unanswerable because the error string has zero xrefs (Arxan-computed).
		//
		// PROVEN 2026-08-08 from a minidump stack (RULE A13) and the decompile:
		//
		//   DB_AllocXAssetEntry_Checked @ IDA 0x9E640
		//     result = sub_AF9B70(*(QWORD*)(8*type + 0xB2F780), type, a2, a4);
		//     if (!result)
		//       DB_RaiseAssetLimitError(byte_B36D70,                  // fmt
		//                               *(DWORD*)(4*type + 0xD90990), // pool size
		//                               *(QWORD*)(8*type + 0xF89A60));// type name
		//
		//   DB_RaiseAssetLimitError @ IDA 0x78AC90 is __noreturn and ends in
		//   exit(-1). It does NOT route through Com_Error @ 0x90750, which is
		//   why the Com_Error watch has never caught an asset-limit error.
		//
		// Address arithmetic, written out per RULE A1 (`_b` literal = IDA - 0x1000):
		//   0x9E640  - 0x1000 = 0x9D640    DB_AllocXAssetEntry_Checked
		//   0x78AC90 - 0x1000 = 0x789C90   DB_RaiseAssetLimitError
		//   0xD90990 - 0x1000 = 0xD8F990   pool-size table, dword per type
		//   0xF89A60 - 0x1000 = 0xF88A60   type-name table, qword per type
		//   0xB36D70 - 0x1000 = 0xB35D70   "\x1FExceeded limit of %d '%s' assets.\n"
		//
		// RULE A3.1 checked: nothing else in src/ hooks either target.

		// sub_A2F80 walks the pool-size table as `while (p < &dword_D90ADC)`, and
		// 0xD90ADC - 0xD90990 = 0x14C = 83 * 4. So there are exactly 83 asset
		// types, which matches structs.h (ASSET_TYPE_DLOGROUTES == 0x52 == 82).
		// VERIFIED: type 21 is "image" with pool size 40192 â€” the number in the
		// fatal error AND the per-tier stride of the residency bitmap.
		constexpr int XASSET_TYPE_COUNT = 83;


		[[nodiscard]] const char* xasset_type_name(const int t)
		{
			if (t < 0 || t >= XASSET_TYPE_COUNT)
			{
				return "?";
			}
			auto** names = reinterpret_cast<const char**>(0xF88A60_b); // IDA 0xF89A60
			const char* n = names[t];
			return (n && readable(n, 1)) ? n : "?";
		}

		[[nodiscard]] std::uint32_t xasset_pool_size(const int t)
		{
			if (t < 0 || t >= XASSET_TYPE_COUNT)
			{
				return 0;
			}
			return reinterpret_cast<std::uint32_t*>(0xD8F990_b)[t]; // IDA 0xD90990
		}








		// Optional single argument, used only as a filename tag so a live-match
		// control dump does not overwrite a demo dump.
		[[nodiscard]] const char* command_tag()
		{
			auto* args = GameUtil::getCmdArgs();
			if (!args)
			{
				return nullptr;
			}
			const int nest = args->nesting;
			if (args->argc[nest] < 2)
			{
				return nullptr;
			}
			return args->argv[nest][1];
		}

		// ---- loaded-zone list, and the level-load bracket -------------------
		// USER OBSERVATION 2026-08-08: egyptbots plays once, then a SECOND
		// cl_demo_play of the SAME demo in the SAME process hits the image
		// limit. That is a leak across plays and it is independent of the
		// frontend-baseline question â€” it says the demo path does not release
		// the previous level.
		//
		// PROVEN from the decompile of DB_LoadXAssets @ IDA 0xA4F60: the unload
		// is inside it. It ORs together the flags of every LOADED zone whose
		// flag word overlaps the flags of the requested entries, builds a list
		// of those zone indices, frees them (sub_ADE50) and removes them from
		// the loaded list. So a zone is released only when its flag group is
		// named by the incoming load.
		//
		// PROVEN from DB_LoadLevelXAssets @ IDA 0xA4840: it issues that unload
		// itself as `sub_A4F60(&{name=NULL, flags=396|512}, 1, 3)` before
		// loading the map's own zones â€” a null name matches every name-tagged
		// zone, so this is the "release the current level" call.
		//
		// âš  AND IT HAS AN EARLY RETURN, BEFORE ANY OF THAT:
		//     for (i = 0; i < loadedZoneCount; ++i)
		//         if (!I_stricmp(zoneName[i], mapName)) return;   // no unload, no load
		// i.e. if a zone with the requested map's name is ALREADY loaded, the
		// whole function is skipped. That is exactly the shape of "second play
		// of the same demo behaves differently", so this bracket reports it.
		//
		// Zone table, corroborated independently by demo_playback.cpp's existing
		// soft-gate probe (same two literals) and by the decompile:
		//   IDA 0x290B124 - 0x1000 = 0x290A124   loaded zone count
		//   IDA 0x59ADC90 - 0x1000 = 0x59ACC90   u16 index per loaded zone
		//   IDA 0x58360E0 - 0x1000 = 0x58350E0   zone records, stride 1456
		//     +832 name (I_stricmp'd against the map name), +896 flags
		constexpr std::size_t ZONE_STRIDE = 1456;
		constexpr std::size_t ZONE_NAME_OFF = 832;
		constexpr std::size_t ZONE_FLAGS_OFF = 896;

		[[nodiscard]] int loaded_zone_count()
		{
			auto* p = reinterpret_cast<int*>(0x290A124_b);
			return readable(p, sizeof(int)) ? *p : -1;
		}

		[[nodiscard]] const std::uint8_t* zone_record(const unsigned index)
		{
			auto* base = reinterpret_cast<std::uint8_t*>(0x58350E0_b);
			auto* rec = base + ZONE_STRIDE * static_cast<std::size_t>(index);
			return readable(rec, ZONE_STRIDE) ? rec : nullptr;
		}

		// Calls `fn(name, flags)` for each loaded zone. Returns the count walked.
		template <typename Fn>
		int for_each_loaded_zone(Fn&& fn)
		{
			const int n = loaded_zone_count();
			if (n <= 0)
			{
				return n < 0 ? 0 : 0;
			}
			const auto* ids = reinterpret_cast<const std::uint16_t*>(0x59ACC90_b);
			if (!readable(ids, sizeof(std::uint16_t)))
			{
				return 0;
			}
			const int lim = n > 1024 ? 1024 : n;
			int walked = 0;
			for (int i = 0; i < lim; ++i)
			{
				const auto* rec = zone_record(ids[i]);
				if (!rec)
				{
					continue;
				}
				fn(reinterpret_cast<const char*>(rec + ZONE_NAME_OFF),
					*reinterpret_cast<const std::uint32_t*>(rec + ZONE_FLAGS_OFF));
				++walked;
			}
			return walked;
		}

		void dump_zone_list(const char* why)
		{
			std::string line;
			int n = 0;
			for_each_loaded_zone([&](const char* name, const std::uint32_t flags)
			{
				++n;
				char buf[96];
				std::snprintf(buf, sizeof(buf), " %s(%X)",
					(name && *name) ? name : "?", flags);
				line += buf;
				if (line.size() > 900)
				{
					Console::printf("[assets] zones %s:%s", why, line.c_str());
					line.clear();
				}
			});
			if (!line.empty())
			{
				Console::printf("[assets] zones %s:%s", why, line.c_str());
			}
			Console::printf("[assets] %d zone(s) loaded %s", n, why);
		}



		// =====================================================================
		//  THE FIX â€” give the demo path the releases the live paths perform
		// =====================================================================
		// PROVEN OFFLINE (tools/s2_asset_log.py over a full session):
		//   cold-menu baseline 36890/40192 images, so only 3302 slots remain;
		//   a map costs ~2600 (egypt) to 3200+ (dday); and across an entire
		//   session including a MAP CHANGE, REMOVED == 0 â€” not one image asset
		//   is ever released. Zones only accumulate: 10 -> 17.
		//
		// The engine releases in two places, for two different purposes:
		//   sub_48C0C0 (live map spawn)  sub_196110(); flags = 388;
		//                                if (!sub_B8C60()) flags |= 0x200;
		//                                DB_LoadXAssets({NULL,0,flags}, 1, 0)
		//                                -> drops ui_mp, i.e. THE FRONTEND
		//   sub_837E0  (map change)      sub_1906B0();
		//                                DB_LoadXAssets({NULL,0,136}, 1, 0)
		//                                -> drops the flag-8 set, i.e. THE LEVEL
		//
		// CL_Demo_Play_f calls DB_LoadLevelXAssets DIRECTLY (IDA 0x910B2F) and
		// goes through neither helper. DB_LoadLevelXAssets does have its own
		// release, but it is
		//     flags = 396; if (!sub_38F590()) flags = 512;
		// and 512 matches NO loaded zone, so it is a guaranteed no-op.
		//
		// âš  AND THE GATE BEING FALSE IS NORMAL, NOT A BUG: sub_B8C60 is a thunk
		// to sub_38F590, so sub_48C0C0 tests the SAME boolean and simply ORs
		// 0x200 when it is false. The 396 branch is DB_LoadLevelXAssets' "I was
		// called directly, so I will do it myself" path; otherwise the engine
		// expects the CALLER to have released. CL_Demo_Play_f is a direct caller
		// that releases nothing, which is the whole defect. So this fix does not
		// depend on why the gate reads false.
		//
		// Therefore: perform the two engine calls, with the ENGINE'S OWN masks
		// and the engine's own precursors, at the same point in the sequence
		// (before DB_LoadLevelXAssets does its work). Nothing is invented.
		//
		// Literals (RULE A1):
		//   DB_LoadXAssets  IDA 0xA4F60  - 0x1000 = 0xA3F60
		//   sub_1906B0      IDA 0x1906B0 - 0x1000 = 0x18F6B0
		//   sub_196110      IDA 0x196110 - 0x1000 = 0x195110
		//   CL_Demo_Play_f  IDA 0x910650, size 0x5A0 -> ends 0x910BF0
		bool g_release_level = true;      // sub_837E0's  mask 136 â€” the level
		bool g_release_frontend = true;   // sub_48C0C0's mask 388 â€” ui_mp

		// One entry of the zone-request array DB_LoadXAssets walks: +0 name,
		// +8 flagsA, +12 flagsB. Confirmed against sub_837E0's locals (v19 is an
		// __int64 name, v20 an int at +8, v21 an int at +12) and against
		// DB_LoadXAssets itself, which reads flagsB via `(_DWORD *)(a1 + 12)`.
		struct zone_request
		{
			const char* name;
			std::uint32_t flags_a;
			std::uint32_t flags_b;
		};
		static_assert(sizeof(zone_request) == 16, "zone request must be 16 bytes");

		using DB_LoadXAssets_fn = std::int64_t(*)(void*, unsigned int, int);
		using engine_void_fn = void(*)();

		// A NULL name matches every name-tagged zone, which is how the engine
		// releases a whole flag group at once (proven in DB_LoadXAssets: the name
		// comparison is guarded by `if (entry.name)`).
		void engine_release(const char* what, const std::uint32_t flags_b,
			const std::uintptr_t precursor)
		{
			const int zones_before = loaded_zone_count();
			reinterpret_cast<engine_void_fn>(precursor)();

			zone_request req{};
			req.name = nullptr;
			req.flags_a = 0;
			req.flags_b = flags_b;
			reinterpret_cast<DB_LoadXAssets_fn>(0xA3F60_b)(&req, 1u, 0);

			const int zones_after = loaded_zone_count();
			Console::printf(
				"[assets] RELEASE %s: mask %u (0x%X), zones %d -> %d (%+d)",
				what, flags_b, flags_b, zones_before, zones_after,
				zones_after - zones_before);
		}

		// sub_48C0C0's map spawn: drop the FRONTEND. This is the half that matters
		// for heavy maps â€” the menu baseline is 36890 of 40192 images, so without
		// it only 3302 slots remain while mp_gibraltar_02 needs 3495 and mp_d_day
		// 3514. Measured: it returns 2669 slots.
		//
		// âš  TIMING IS EVERYTHING, AND GETTING IT WRONG CRASHED THE GAME.
		// The first version did this from inside DB_LoadLevelXAssets â€” the LAST
		// call of CL_Demo_Play_f's sequence. By then the loading screen has already
		// restarted LUI ("LUI: Starting up... LUI.mp_menus.main"), so LUI is holding
		// luafiles that live in ui_mp, and freeing them is a use-after-free.
		// Gibraltar then reached connstate 10 and crashed to desktop inside the Lua
		// VM (sub_B0F000 â€” identified by its own "C stack overflow: too many (%d)
		// nested C function calls." literal) with UI_UpdateTime on the stack, and
		// the census showed luafile 1773 -> 941. RULE A13 (read the minidump first)
		// found this in one pass.
		//
		// sub_48C0C0 issues this release EARLY â€” before its teardown and before the
		// map's zones load â€” so nothing is bound to ui_mp when it goes. Doing it at
		// CL_Demo_Play_f's entry puts us at the same point: ahead of the loading
		// screen and ahead of the LUI restart, which then comes up in a world
		// without ui_mp exactly as it does for a live map spawn.
		void release_frontend_zones()
		{
			if (!g_release_frontend)
			{
				return;
			}
			std::uint32_t flags = 388u;
			if (!reinterpret_cast<bool(*)()>(0x38E590_b)())   // sub_38F590
			{
				flags |= 0x200u;   // exactly what sub_48C0C0 does
			}
			engine_release("frontend/ui_mp (as sub_48C0C0 does on a map spawn)",
				flags, 0x195110_b /* sub_196110 */);
		}

		// sub_837E0's map change: drop the previous LEVEL. Safe at the level-load
		// point â€” these are flag-8 map zones, not UI â€” and doing it there also
		// clears the map's own zone, so DB_LoadLevelXAssets' already-loaded-name
		// early return can no longer skip a reload of the same map.
		void release_level_zones()
		{
			if (!g_release_level)
			{
				return;
			}
			engine_release("level (as sub_837E0 does on a map change)",
				136u, 0x18F6B0_b /* sub_1906B0 */);
		}

		// Shared with force_host's map change. A `map <name>` issued while the
		// frontend is resident overflows the image pool: ui_mp holds ~36,890 of
		// the 40,192 image slots, leaving ~3,300 headroom, and a map needs
		// ~2,600-3,500. The engine's own map-spawn path releases the frontend
		// first; a bare `map` command does not, so we do it for it.
		//
		// TIMING IS PART OF THE CONTRACT: this must run BEFORE the load starts,
		// like sub_48C0C0 does. Releasing ui_mp after the loading screen has
		// restarted LUI is what crashed gibraltar (a Lua use-after-free).
		// ⛔ LEVEL ONLY. Releasing the frontend here CRASHED THE RENDERER, every time.
		//
		// This used to call release_frontend_zones() as well, and that is why
		// "change map always crashed the game". Crash dumps (RULE A13), three of
		// the four most recent, all agree:
		//
		//     fault IDA_0x88E307   READ from 0x58   (a NULL struct + 0x58)
		//     stack: SCR_DrawScreen -> sub_F25A0 -> sub_8BBB10 -> sub_88E680
		//
		// i.e. the fault is in the DRAW path, not the load path. A map change is
		// issued while the menu is up and RENDERING, so freeing ui_mp pulls
		// material/image data out from under the very next frame. It is the same
		// class of mistake as the gibraltar crash recorded above -- release the
		// frontend at the wrong lifecycle point and something already bound to it
		// dies -- and the lesson there was the same: WHERE you call it is part of
		// the contract.
		//
		// The engine's own map change (sub_837E0) releases the LEVEL only,
		// {NULL, 0, 136}. It never drops ui_mp, because you are not leaving the
		// frontend. Matching that keeps the original benefit -- the previous
		// level's zones are freed, so we do not overflow the 40192 image pool the
		// way a bare `map` did -- with none of the use-after-free.
		void release_for_external_map_change_impl()
		{
			release_level_zones();
		}

		// CL_Demo_Play_f @ IDA 0x910650 - 0x1000 = 0x90F650.
		// RULE A3.1 checked: nothing else in src/ hooks this target.
		using CL_Demo_Play_f_fn = void(*)();
		CL_Demo_Play_f_fn CL_Demo_Play_f_orig = nullptr;

		void cl_demo_play_f_stub()
		{
			// Before ANYTHING else the command does: before the loading-screen zone
			// is queued and before LUI restarts. See the timing note above.
			if (g_release_frontend)
			{
				Console::printf("[assets] cl_demo_play: releasing the frontend up "
					"front, as sub_48C0C0 does on a live map spawn (doing it later "
					"frees luafiles LUI has already bound)");
				release_frontend_zones();
			}
			CL_Demo_Play_f_orig();
		}

		using DB_LoadLevelXAssets_fn = std::int64_t(*)(const char*, char);
		DB_LoadLevelXAssets_fn DB_LoadLevelXAssets_orig = nullptr;

		std::int64_t db_load_level_xassets_stub(const char* map, const char flags)
		{
			// Act ONLY when CL_Demo_Play_f is the caller. That is the one direct
			// caller which performs no release of its own; every other caller
			// (sub_48C0C0, sub_837E0, CL_InitCGame, sub_6DC350) either releases
			// first or is mid-session, where releasing would be destructive.
			// Identifying the caller by return address is exact and needs no
			// state flag, so it works however playback was started â€” our play()
			// or a bare `cl_demo_play` typed at the console.
			// CL_Demo_Play_f: IDA 0x910650, size 0x5A0 -> [0x910650, 0x910BF0).
			const std::uint64_t caller = to_ida(_ReturnAddress());
			const bool from_demo_play = caller >= 0x910650 && caller < 0x910BF0;

			const char* name = (map && readable(map, 1)) ? map : "<null>";
			// Only the LEVEL release belongs here. The frontend release is done
			// far earlier, at CL_Demo_Play_f's entry â€” see the timing note on
			// release_frontend_zones().
			//
			// â­ THE ZONE LEAK (measured 2026-08-10, and it is not demo-specific)
			//
			// DB_LoadLevelXAssets' own release is
			//     v11 = 396; if (!sub_38F590()) v11 = 512;
			//     DB_LoadXAssets({NULL, 0, v11}, 1, 3);
			// and sub_38F590() is normally FALSE, so the mask is 512 â€” which
			// matches NO loaded zone and frees nothing. The engine therefore
			// relies on the CALLER having released first.
			//
			// tools/s2_asset_log.py on a live session log measured the result:
			//     mp_shipment_s2   zones 9 -> 12
			//     +3 zones (PURE ADDITION - nothing released), +1285 images
			//     ACCUMULATED: common_core_mp eng_common_core_mp mp_shipment_s2_path
			//     RELEASED:    <none>
			// and of the engine's own masks only 136/396 (bit 3 = 0x8) would have
			// freed them â€” 388 and 512 match nothing in that set.
			//
			// Zones are hundreds of MB each, so this accumulates into gigabytes
			// across a session. It is the leak.
			//
			// So issue the level release for EVERY caller, not just the demo
			// path. This is safe in a way the frontend release is not:
			//   * mask 136 touches only flag-8 MAP zones, never UI, so there is
			//     no LUI use-after-free hazard (that is what crashed gibraltar);
			//   * a caller that already released (sub_48C0C0, sub_837E0) simply
			//     finds nothing left to match, making it a no-op;
			//   * it also clears the map's own zone, so the already-loaded-name
			//     early return can no longer skip a genuine reload.
			if (g_release_level)
			{
				Console::printf("[assets] DB_LoadLevelXAssets('%s') from IDA_0x%llX%s: "
					"releasing the previous level (the engine's own release is a "
					"no-op here â€” mask 512 matches nothing)",
					name, static_cast<unsigned long long>(caller),
					from_demo_play ? " (CL_Demo_Play_f)" : "");
				release_level_zones();
			}

			// One line per LEVEL LOAD (a rare event, not a per-frame probe). The zone
			// count either side is the only evidence that the release above actually
			// freed anything, so a silent regression to the documented "zones only
			// ever accumulate" leak stays visible.
			const int zones_before = loaded_zone_count();
			const auto r = DB_LoadLevelXAssets_orig(map, flags);
			Console::printf("[assets] level '%s' loaded: zones %d -> %d",
				name, zones_before, loaded_zone_count());
			return r;
		}

		void cmd_zone_list()
		{
			dump_zone_list("on demand");
		}




		// ---- the priming-loop runaway -----------------------------------------
		// PROVEN 2026-08-08. CL_SetCGameTime @ 0x86D30 primes the connection from
		// the demo itself:
		//
		//     if (!CL_Demo_IsCompleted())
		//         for (i = connstate; connstate >= 5; i = connstate) {
		//             if (i >= 9) break;
		//             CL_Demo_ReadDemoMessage(client);   // return value IGNORED
		//         }                                      // IsCompleted NOT re-checked
		//
		// IsCompleted is tested ONCE, before the loop. Inside, neither the flag nor
		// the 0 return is honoured, so a demo that never advances connstate out of
		// [5,9) makes this consume the whole packet stream IN ONE FRAME and then
		// read one byte past the type-0 terminator â€” landing on the footer's
		// version dword (0x1D == 29), which is not a valid packet type:
		//     Com_Error(1, "EXE_ERR_PROCESS_DEMO_FILE_FAILED")
		// Measured with publicmatch.demo: FILEPOS 0x2D1EC6 == terminator(0x2D1EC4)+2.
		//
		// MW3's twin has the same shape, so this is inherited engine behaviour, not
		// an S2 regression. The engine's OTHER feed loop (0x87011) does it right â€”
		// it checks IsCompleted and the return value every iteration.
		//
		// This guard makes the priming loop honour what the engine already knows,
		// WITHOUT touching any MSG_ read position (see CLAUDE.md: cursor nudging is
		// the symptom patch we are explicitly not repeating).
		constexpr std::size_t CONNSTATE_STRIDE = 494 * 4; // int array, [494 * client]

		[[nodiscard]] int* connstate_ptr(const int client)
		{
			if (client < 0 || client >= 4)
			{
				return nullptr;
			}
			auto* base = reinterpret_cast<int*>(0x1BAE4E4_b); // IDA clientConnectionState 0x1BAF4E4
			return reinterpret_cast<int*>(
				reinterpret_cast<char*>(base) + CONNSTATE_STRIDE * static_cast<std::size_t>(client));
		}

		[[nodiscard]] bool demo_completed()
		{
			const auto g = demo_playback_data();
			return g && *reinterpret_cast<std::uint8_t*>(g + 8) != 0;
		}

		std::int64_t g_reads = 0;       // total ReadDemoMessage calls this playback
		std::int64_t g_prime_reads = 0; // calls made while connstate < 9 (the priming burn)
		int g_last_connstate = -1;
		bool g_eof_seen = false;        // set ONLY when the ORIGINAL returned 0
		bool g_runaway_reported = false;
		bool g_first_call_logged = false;
		// (definition moved to the top of this namespace â€” the viewmodel and
		//  StreamSync code above needs it)

		// Set once the zeroed stand-in is installed (see below). The null-guards key off
		// THIS, not g_native_playing, so they are inert during CL_Demo_Play_f's
		// multithreaded level load.
		// Diagnostic brackets. They did their job -- they located CL_InitCGame, the
		// cgame-init gate and the entity loop -- but they hook functions the
		// MULTITHREADED zone loader also calls, and an intermittent deadlock inside
		// DB_LoadLevelXAssets was measured on 2026-08-08 (main thread blocked in ntdll
		// with IDA_0x910B34 on its stack, cls_realtime frozen). Keeping ~17 extra
		// trampolines on hot cross-thread functions is not worth that risk once the
		// answers are in, so tracing is OPT-IN AT BUILD TIME. Flip to true and rebuild
		// when a new bracket is genuinely needed.

		// ---- NetConstStrings: the demo path never loads the name->index tables -----
		//
		// PROVEN 2026-08-08 (Com_Error "420 OmnvarChanged" @ IDA 0x183879):
		//
		//     if (!sub_DED60(23, "OmnvarChanged", &idx)) Com_Error(1, "420 %s", ...);
		//
		// sub_DED60(type, name, &out) searches the netconststring BLOCK LISTS at
		// qword_5A00C30[2*type] by name. Those lists are loaded by
		// NetConstStrings_Load @ 0xDEF90, which pulls 24 per-level assets named
		// "ncs_<name>_level" (this is the 'ncs_mdl_level' asset the log has always
		// mentioned) and sets the loaded-flag dword_5098948.
		//
		// CL_ParseGamestate calls it ONLY when NOT playing a demo:
		//     if (!CL_IsDemoPlaying(client)) { if (!sub_DEE80()) { sub_DEF90(); ... } }
		// because CL_Demo_ReadNetConstStringTable is supposed to have installed the
		// demo's own recorded table instead -- and that function calls sub_DFA70 (the
		// UNLOAD) first.
		//
		// But those are TWO DIFFERENT STORAGES:
		//     qword_A60D180 + 0x20000 + 8*index   pointer table   index -> string  (demo DOES fill this)
		//     qword_5A00C30[2*type]               block lists     name  -> index   (left NULL)
		// sub_DED60 needs the second. MEASURED live: all 26 block-list entries are 0
		// and dword_5098948 == 0.
		//
		// So we run the load the engine skipped, AFTER CL_ParseGamestate returns (by
		// which point CL_InitCGame has loaded the level, so the ncs_*_level assets are
		// present). Gated on the engine's own loaded-flag so we never double-load, and
		// on native playback so live play is untouched.
		//
		// This restores an engine call on a path where the engine omits it. It does not
		// fabricate data -- the strings come from the level's own assets.
		// NOTE: do NOT cache _b() addresses at namespace scope -- `base` is assigned in
		// init(), so a file-scope initialiser runs first and yields the bare literal.
		// A build that did exactly that faulted with "EXECUTE at 0xDDF90, outside any
		// module" (crash dump 13:57). Resolve inside the function instead.
		[[nodiscard]] std::int32_t netconststrings_loaded()
		{
			return *reinterpret_cast<std::int32_t*>(0x5097948_b);   // IDA 0x5098948
		}

		// qword_5A00C30[2*type] -- the block list sub_DED60 searches by NAME.
		[[nodiscard]] std::uintptr_t netconststrings_block_list(const int type)
		{
			auto* const table = reinterpret_cast<std::uintptr_t*>(0x59FFC30_b);  // IDA 0x5A00C30
			return table[2 * static_cast<std::size_t>(type)];
		}

		bool g_absent_installed = false;

		// ---- qword_2537508: the engine's own missing null checks -------------------
		//
		// PROVEN 2026-08-08 from five crash dumps (tools/s2_crash_dump.py, RULE A13).
		// This global is NULL for the whole of native demo playback. CL_Demo_Play_f
		// GUARDS it (`if (qword_2537508) ...`), so NULL is a legal, expected state --
		// but three tiny accessors on the demo path deref it with no check at all:
		//
		//   IDA 0x47D1C0  return *(QWORD*)(a1 + 8);        -> READ  0x8
		//   IDA 0x1FEB80  *(BYTE*)(a1 + 171) = a2;         -> WRITE 0xAB
		//   IDA 0x47E670  if (!a1[399636] || !a1[399618])  -> READ  +0x186450
		//
		// WHY NOT A FAKE OBJECT. The first attempt pointed the global at a zeroed 8 MB
		// block. That fixed 0x47D1C0 (crash moved 0x465211 -> 0x465228) and then broke
		// in a NEW way: sub_47D1C0 returned *(q+8) == 0 straight from our own zeros, and
		// CL_ParseSnapshot handed that 0 to sub_1FEB80, which wrote to 0+171. The object
		// holds real POINTERS, so zero-filling only relocates the fault, and filling in
		// fake sub-objects would cascade without end. Fabricating state also violates
		// this project's doctrine.
		//
		// BOTH MECHANISMS ARE NEEDED -- proven by removing the block on its own
		// (dump 13:04, back to IDA 0x462DA3 READ 0x10). CL_ParseGamestate derefs
		// *(q + 16) INLINE while building sub_2A2600's argument, so no accessor hook
		// can guard it; that one needs a READABLE object. So:
		//
		//   * the zeroed block gives inline derefs something to read -> they yield 0
		//   * these guards stop that 0 being used as a pointer further down
		//
		// Neither invents meaningful data: the block is all zeros ("everything absent")
		// and the guards make absent behave as absent.
		// Guards are active ONLY while a native demo is playing, so live play keeps the
		// stock code paths exactly.
		std::int64_t(*sub_47D1C0_orig)(std::uintptr_t) = nullptr;
		std::int64_t sub_47D1C0_stub(const std::uintptr_t a1)
		{
			if (!a1 && g_absent_installed)
			{
				return 0;   // absent object -> absent sub-object
			}
			return sub_47D1C0_orig(a1);
		}

		void(*sub_1FEB80_orig)(std::uintptr_t, char) = nullptr;
		void sub_1FEB80_stub(const std::uintptr_t a1, const char a2)
		{
			if (!a1 && g_absent_installed)
			{
				return;     // nothing to store the flag in
			}
			sub_1FEB80_orig(a1, a2);
		}

		std::int64_t(*sub_47E670_orig)(std::uintptr_t, void*) = nullptr;
		std::int64_t sub_47E670_stub(const std::uintptr_t a1, void* out)
		{
			if (!a1 && g_absent_installed)
			{
				return 0;   // its own "not available" return
			}
			return sub_47E670_orig(a1, out);
		}

		// ---- qword_2537508: a stand-in for an object the demo path never creates ----
		//
		// PROVEN 2026-08-08 from four crash dumps (tools/s2_crash_dump.py, RULE A13).
		// This global is NULL for the whole of native demo playback, and several
		// consumers deref it WITHOUT a null check:
		//
		//   IDA 0x462DA3  CL_ParseGamestate  *(QWORD*)(q + 16)      -> read 0x10
		//   IDA 0x47E670  via CL_InitCGame   a1[399636] (byte +1598544)
		//   IDA 0x47D1C0  via CL_ParseSnapshot  return *(QWORD*)(a1 + 8) -> read 0x8
		//
		// sub_47D1C0 is a bare accessor with no guard at all, so there is nothing to
		// satisfy except a readable object. CL_Demo_Play_f guards this SAME global
		// (`if (qword_2537508)`), which is what makes "absent" a legal state.
		//
		// SIZE: sub_47E670 indexes a _DWORD* at 399636 == byte +1,598,544. An earlier
		// 64-byte stand-in merely MOVED the crash there (dumps 12:20-12:39). 8 MB,
		// zero-filled, so every consumer reads 0 = "nothing here" rather than garbage.
		//
		// LIFETIME: installed for the WHOLE playback session. Scoping it to
		// CL_ParseGamestate alone was not enough â€” CL_ParseSnapshot faulted on the very
		// first snapshot after priming (dump 12:55, IDA 0x47D1C0).
		//
		// âš  MANUFACTURED STATE. Why the demo path never creates this object is UNPROVEN
		// (48 xrefs, incl. CL_FirstSnapshot / CL_ParseSnapshot / CL_SetClientState).
		// Not a fix until its producer is found.
		constexpr std::size_t ABSENT_OBJECT_BYTES = 8ull * 1024 * 1024;
		std::uint8_t* g_absent_object = nullptr;
		std::uintptr_t g_absent_saved = 0;

		[[nodiscard]] std::uintptr_t* absent_object_slot()
		{
			return reinterpret_cast<std::uintptr_t*>(0x2536508_b);   // IDA 0x2537508
		}

		void install_absent_client_object()
		{
			if (g_absent_installed)
			{
				return;
			}
			auto* const slot = absent_object_slot();
			if (*slot != 0)
			{
				return;   // the engine has a real one; never displace it
			}
			if (!g_absent_object)
			{
				g_absent_object = static_cast<std::uint8_t*>(VirtualAlloc(
					nullptr, ABSENT_OBJECT_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
			}
			if (!g_absent_object)
			{
				Console::printf("[native] WARNING: could not allocate the qword_2537508 stand-in");
				return;
			}
			// Give the ONE field that is used as a pointer somewhere valid to point.
			//
			// CL_ParseSnapshot does p = sub_47D1C0(q) (== *(q + 8)) and then hands p to a
			// family of one-line setters -- sub_1FEB80 writes p[171], sub_1FEC20 writes
			// p[172], and there are almost certainly more. With +8 left at zero every one
			// of them faults in turn, and guarding them individually is whack-a-mole
			// (measured: 0x465211 -> 0x465228 -> 0x46523A, one setter per rebuild).
			//
			// So +8 points at a scratch area inside our own block. Those setters then
			// write into memory we own, harmlessly, and the whole family is handled at
			// once. ONLY +8 is filled in: everything else stays zero, because fields like
			// +16 are null-CHECKED by their consumers (sub_2A2600 returns immediately on
			// 0), and making those non-null would push the engine down paths that expect
			// a real object.
			constexpr std::size_t ABSENT_SCRATCH_OFF = ABSENT_OBJECT_BYTES / 2;
			*reinterpret_cast<std::uintptr_t*>(g_absent_object + 8) =
				reinterpret_cast<std::uintptr_t>(g_absent_object + ABSENT_SCRATCH_OFF);

			g_absent_saved = *slot;
			*slot = reinterpret_cast<std::uintptr_t>(g_absent_object);
			g_absent_installed = true;
			Console::printf(
				"[native] qword_2537508 was NULL -> installed a zeroed %zu MB stand-in for the "
				"whole session (unguarded consumers: 0x462DA3 +0x10, 0x47E670 +0x%X, 0x47D1C0 +0x8)",
				ABSENT_OBJECT_BYTES / (1024 * 1024), 399636u * 4u);
		}

		void remove_absent_client_object()
		{
			if (!g_absent_installed)
			{
				return;
			}
			auto* const slot = absent_object_slot();
			// Only take it back if it is still OURS -- if the engine has since installed
			// a real object, stomping it would be far worse than leaking the stand-in.
			if (*slot == reinterpret_cast<std::uintptr_t>(g_absent_object))
			{
				*slot = g_absent_saved;
			}
			g_absent_installed = false;
			Console::printf("[native] qword_2537508 stand-in removed");
		}
		bool g_repaired_reported = false;
		std::int64_t(*CL_ParseGamestate_orig)(unsigned int, std::int64_t) = nullptr;
		std::int64_t g_gamestates = 0;

		// ---- svc opcode census ------------------------------------------------
		// CL_ParseServerMessage_Internal @ IDA 0x4636E0 reads a 4-bit opcode
		// (MSG_ReadBits(msg,4)) and dispatches:
		//     0 gamestate (rejected if connstate >= 8)   4 table upload
		//     2 server command, text                     5 matchdata
		//     3 server command, binary                   6 snapshot (rejected if connstate < 9)
		//                                                7 EOF
		// It is entered with the DECOMPRESSED message, readcount 0, bit 0, so the
		// first opcode is simply the low nibble of data[0] (LSB-first bit order,
		// inferred from a text command's string landing byte-aligned at +5).
		//
		// msg layout PROVEN from MSG_Init @ 0xDC750: +8 data, +28 cursize, +36
		// readcount, +40 bit position.
		std::int64_t(*CL_ParseServerMessage_Internal_orig)(unsigned int, std::uintptr_t, std::uintptr_t) = nullptr;
		std::int64_t g_internal_calls = 0;
		std::int64_t g_opcode_hist[16]{};

		// ---- EXPERIMENT: skip the first N server messages after the gamestate ---
		// publicmatch dies with 4780 in the entity block of svc message #1, which
		// offline analysis identifies as the FIRST of three retransmits (demo
		// packets 1/2/3, msgNum 11/12/13, all deltaNum=0 flags=0x0C, identical
		// command sets and identical header-end at readcount 727). Packet 8 is
		// structurally DIFFERENT â€” msgNum=14, flags=0x04, headerEnd=11, a
		// standalone snapshot rather than one bundled behind 25 commands â€” and it
		// is also FULL, so it needs no earlier baseline.
		//
		// So: skipping svc messages 1..3 should land playback on msgNum=14. This
		// tests whether the corruption is confined to the retransmit group.
		//
		// Skipping is safe for the file cursor: CL_Demo_ProcessPacket_Type2_3 has
		// already read the bytes into its scratch buffer and written the sequence
		// fields to clc BEFORE calling the parser, so declining to parse consumes
		// nothing extra and desynchronises nothing.
		//
		// EXPERIMENT ONLY. If it works it is not a fix â€” it discards real gameplay.
		// It answers "is the damage confined to those three packets".
		int g_skip_msgs = 0;

		std::int64_t cl_parse_server_message_internal_stub(
			const unsigned int client, const std::uintptr_t msg, const std::uintptr_t src)
		{
			// Index 0 is the gamestate and must always be parsed.
			if (g_native_playing && g_skip_msgs > 0
				&& g_internal_calls >= 1
				&& g_internal_calls <= static_cast<long long>(g_skip_msgs))
			{
				Console::printf("[native] SKIPPING svc msg #%lld of %d "
					"(experiment: is the damage confined to the retransmit group?)",
					static_cast<long long>(g_internal_calls), g_skip_msgs);
				++g_internal_calls;
				return 0;
			}

			if (msg && readable(reinterpret_cast<const void*>(msg), 56))
			{
				const auto data = *reinterpret_cast<std::uint8_t**>(msg + 8);
				const auto cursize = *reinterpret_cast<std::int32_t*>(msg + 28);

				// UNCONDITIONAL raw dump of the first few. Message #0 â€” the gamestate
				// slot â€” produced no histogram entry and no log line on 2026-08-08,
				// meaning data was null or cursize <= 0. sub_DCAB0 (the Huffman
				// decoder) returns SUCCESS with zero output when its length argument
				// is 0, so an empty message is silent. This shows which field is bad
				// instead of skipping the message.
				if (g_internal_calls < 4)
				{
					Console::printf(
						"[native] svc RAW #%lld: msg=%p data=%p cursize=%d readcount=%d "
						"useZlib=%d f20=%d f24=%d readableData=%d",
						static_cast<long long>(g_internal_calls),
						reinterpret_cast<void*>(msg), reinterpret_cast<void*>(data),
						cursize,
						*reinterpret_cast<std::int32_t*>(msg + 36),
						*reinterpret_cast<std::int32_t*>(msg + 52),
						*reinterpret_cast<std::int32_t*>(msg + 20),
						*reinterpret_cast<std::int32_t*>(msg + 24),
						data ? static_cast<int>(readable(data, 1)) : -1);
				}

				// ---- EXPERIMENT (demo_native_gs_install, default ON) ----------
				// PROVEN offline for all five demos: the decoded gamestate's byte0
				// is 0x20 â€” opcode 0 in the low nibble, and CL_ParseGamestate's two
				// MSG_ReadBit calls land on bits 4 and 5 of that SAME byte, because
				// MSG_ReadBits(4) leaves bitpos at 4 while the following byte reads
				// advance readcount independently.
				//     v9  = bit4 = 0
				//     v10 = bit5 = 1  ->  if (v10) { dword_8BDA060 = 1; }  and the
				//                         ENTIRE install is skipped: no configstring
				//                         install, no sub_70830(client, map, gametype),
				//                         no cl_paused reset. connstate stays 5.
				// Measured in game: "CL_ParseGamestate #1: connstate 5 -> 5".
				//
				// Clearing bit5 makes the engine take the install branch. This is an
				// EXPERIMENT, not a proven fix: it tests whether the install path is
				// what the demo needs. If it primes the connection, the next job is
				// to establish WHY the recorder writes v10 = 1 and what the correct
				// engine-faithful handling is.
				// (The old bit5 "install branch" experiment lived here. REMOVED
				// 2026-08-08: it was falsified in game, and the real cause turned
				// out to be the one-bit writer/reader mismatch handled in
				// msg_read_bit_stub. Clearing bit5 was treating a symptom of the
				// shift, not the shift itself.)
				if (data && cursize > 0 && readable(data, 1))
				{
					const unsigned lo = data[0] & 0x0F;
					++g_opcode_hist[lo];
					// First handful only â€” 1735 snapshots would drown the console.
					if (g_internal_calls < 12)
					{
						Console::printf(
							"[native] svc msg #%lld: firstNibble=%u (hi=%u) size=%d byte0=0x%02X",
							static_cast<long long>(g_internal_calls), lo,
							static_cast<unsigned>(data[0] >> 4), cursize,
							static_cast<unsigned>(data[0]));
					}
				}
			}
			++g_internal_calls;
			return CL_ParseServerMessage_Internal_orig(client, msg, src);
		}

		void dump_opcode_histogram()
		{
			std::string line;
			for (int i = 0; i < 16; ++i)
			{
				if (g_opcode_hist[i])
				{
					line += std::format("{}={} ", i, g_opcode_hist[i]);
				}
			}
			Console::printf("[native] svc opcode census over %lld messages: %s",
				static_cast<long long>(g_internal_calls),
				line.empty() ? "(none)" : line.c_str());
		}

		// ---- THE ONE-BIT FIX ---------------------------------------------------
		// ROOT CAUSE, proven 2026-08-08 by reading BOTH sides of the wire:
		//
		//   CL_Demo_WriteGameState @ 0x91D190 writes, after the gametype string:
		//       MSG_WriteBit(sub_B8C70());     <-- ONE bit
		//       MSG_WriteLong(0);
		//       CL_Demo_WriteConfigStrings();  <-- writes v12 = 1 as 4 bits
		//
		//   CL_ParseGamestate @ 0x462B00 reads, at the same point:
		//       MSG_ReadBit  -> v9
		//       MSG_ReadBit  -> v10            <-- SECOND BIT, NEVER WRITTEN
		//       MSG_ReadLong -> v11
		//       MSG_ReadBits(4) -> v12
		//
		// The extra read shifts the bit stream by one, so the engine sees
		//     v10 = written v12's low bit    = 1
		//     v12 = (1 >> 1) | (delta0 << 3) = 8
		// v12 must be 1 or 7, so CL_ParseGamestate takes its error exit: no
		// configstrings installed, connstate never leaves 5, and CL_SetCGameTime's
		// priming loop consumes the whole demo. This is a RECORDING defect â€” every
		// shipped .demo is affected identically, which is why native theater has
		// never worked in this build.
		//
		// VERIFIED OFFLINE on all five demos (tools/s2_huffman.py): consuming ONE
		// bit instead of two yields v12 = 1, count 108/119, 100% monotonic indices
		// [0,1,3,6,8,9,139,...] and cs[3] = the serverId â€” matching a known-good
		// LIVE gamestate exactly.
		//
		// Live gamestates DO carry both bits (measured v9=0 v10=0 from our own
		// .dm_s2 capture), so this must NEVER apply outside native demo playback.
		//
		// Mechanism: CL_ParseGamestate calls MSG_ReadBit exactly twice (v9, v10).
		// CL_ParseConfigStrings_Internal INLINES its bit read and does not come
		// through here, so "the 2nd call while inside CL_ParseGamestate" is exactly
		// v10; returning 0 without consuming a bit realigns the stream.
		thread_local int g_gs_bit_calls = -1;   // -1 = not inside CL_ParseGamestate

		// Toggle: `demo_native_onebit`. The fix is CORRECT (verified offline on all
		// five demos) but it takes the engine down the configstring-install path
		// that has never executed in this build, and that path crashed with
		// STATUS_STACK_BUFFER_OVERRUN (0xC0000409) on the first test. Keep it
		// switchable so a crashing build is never forced on the user.
		bool g_onebit_fix = true;


		std::int64_t(*MSG_ReadBit_orig)(std::uintptr_t) = nullptr;

		std::int64_t msg_read_bit_stub(const std::uintptr_t msg)
		{
			if (g_gs_bit_calls >= 0 && ++g_gs_bit_calls == 2)
			{
				return 0;   // v10 â€” never written by the demo writer; consume nothing
			}
			return MSG_ReadBit_orig(msg);
		}

		// CL_ParseConfigStrings_Internal @ IDA 0x462930 -> literal 0x462930 - 0x1000
		// = 0x461930. Bracketing it tells us whether the 0xC0000409 stack-buffer
		// overrun happens INSIDE the configstring install (bad string data) or
		// AFTER it (sub_70830 / sub_91E310 / sub_60A30 / sub_4612E0).
		std::int64_t(*CL_ParseConfigStrings_Internal_orig)(unsigned int, std::uintptr_t) = nullptr;
		std::int64_t g_cs_calls = 0;


		// ---- install-tail brackets --------------------------------------------
		// CL_ParseConfigStrings_Internal now completes (dataCount=1168 matched the
		// offline decode exactly), so the 0xC0000409 stack-buffer overrun is in
		// CL_ParseGamestate's install tail, which v10 == 0 newly selects:
		//     sub_AC9C0 -> ... -> sub_70830(client, mapname, gametype)
		//               -> sub_91E310 (DEMO branch) -> sub_60A30 -> sub_4612E0
		// None of these have ever run on this path. Bracket each to name the one.
		// Literals per RULE A1:
		//     0x70830  - 0x1000 = 0x6F830        0x91E310 - 0x1000 = 0x91D310
		//     0x60A30  - 0x1000 = 0x5FA30        0x4612E0 - 0x1000 = 0x4602E0
		bool g_in_gamestate = false;

		// Nothing between CL_ParseConfigStrings_Internal returning and sub_70830 had
		// a bracket, and the crash landed in that gap. The calls there are:
		//   CL_GetConfigString(8) + sub_76C7F0 (sscanf 3 floats â€” verified safe)
		//   MSG_ReadBits(4)=7, MSG_ReadLong x2 (clientNum=0, checksumFeed â€” verified)
		//   sub_AC9C0()
		//   CL_GetConfigString(3) + atoi, sub_287110/sub_287260 scratch
		//   if (!client) { sub_7AEA0(); sub_2A2600(*(QWORD*)(qword_2537508+16), v); }
		//        ^ UNGUARDED deref. CL_Demo_Play_f guards the SAME global:
		//          `if (qword_2537508) { ... sub_2A2960(*(qword_2537508+16), ...) }`
		//   sub_756A60(clc+304) / sub_756E20
		// Literals per RULE A1:
		//   0xAC9C0  - 0x1000 = 0xAB9C0     0x7AEA0  - 0x1000 = 0x79EA0
		//   0x2A2600 - 0x1000 = 0x2A1600    0x756A60 - 0x1000 = 0x755A60
		//   qword_2537508 -> 0x2536508
		std::int64_t(*sub_AC9C0_orig)() = nullptr;

		std::int64_t(*sub_7AEA0_orig)() = nullptr;

		std::int64_t(*sub_2A2600_orig)(std::uintptr_t, std::uintptr_t) = nullptr;

		std::int64_t(*sub_756A60_orig)(unsigned int) = nullptr;

		// sub_70830 is NOT map/systeminfo â€” it is the CONNSTATE ADVANCER:
		//   CL_SetClientState(client, 8)                       // LOADING
		//   if (com_sv_running || CL_IsDemoPlaying || ...) {
		//       if (byte_1BAF450[1976*client + 2]) {           // <-- GATE
		//           2048x { sub_13A00; sub_43D270 }            // entity init
		//           sub_4375D0 .. sub_3CC390                   // cgame init
		//           CL_SetClientState(client, 9)               // PRIMED
		//           sub_4A0B80; sub_276A70; sub_91C710(demo)
		//       }
		//   }
		// If the gate byte is 0 the demo NEVER reaches state 9, which is exactly the
		// symptom we started with. CL_SetClientState @ IDA 0x87070 -> literal 0x86070.
		std::int64_t(*CL_SetClientState_orig)(unsigned int, int) = nullptr;

		// We reach CL_SetClientState(client,8) then die before sub_4375D0. The window
		// is: sub_8555C0 -> sub_62F630 -> CG_GetServerCommandsState ->
		//     2048x { v31 = sub_13A00(client, i); *(DWORD*)(v31+1696) &= ~1;
		//             sub_43D270(client, v31); }
		// That loop writes THROUGH sub_13A00's return value, so a null/garbage entity
		// array faults. Log the first few returns rather than all 2048.
		// Literals: 0x8555C0-0x1000=0x8545C0  0x62F630-0x1000=0x62E630
		//           0x13A00 -0x1000=0x12A00   0x43D270-0x1000=0x43C270
		std::int64_t(*sub_62F630_orig)() = nullptr;

		std::int64_t g_ent_calls = 0;
		std::int64_t(*sub_13A00_orig)(unsigned int, int) = nullptr;
		std::int64_t(*sub_4375D0_orig)(unsigned int, int) = nullptr;

		std::int64_t(*sub_3CC390_orig)(unsigned int) = nullptr;

		// =====================================================================
		//  sub_91C710 IS CL_Demo_StartRecord -- AND IT RUNS ON LIVE CONNECTS
		// =====================================================================
		// CLAUDE.md recorded for weeks that "S2 has NO separate CL_Demo_StartRecord;
		// the only caller of CL_Demo_WriteGameState is the clip path". That is WRONG
		// and is corrected here. sub_91C710 @0x91C710:
		//
		//   * is called from CL_InitCGame @0x7BFD0 and sub_70830 -- the LIVE connect
		//     path, not playback;
		//   * opens a file via sub_912230(client, name, "GAME_REPLAY_FILE");
		//   * fills the native header IN clc: +262760 = 29 (version),
		//     +262764 = 79384 (header size), +262768 = client num, +262773 =
		//     (sub_197140(...) == 1)  <- the +0x0D byte that is 1 in every shipped
		//     private demo and 0 in publicmatch, +262784 = exe mode,
		//     and memcpy(clc+262788, sub_D0210(client), 0x135FC) -- the 79,356-byte
		//     state blob our transcoder has to borrow from a shipped demo;
		//   * then writes svc_gamestate + CL_Demo_WriteConfigStrings and emits it as
		//     a type-2 packet (type byte 2, sequence, length, body in 1024 B chunks).
		//
		// It also contains the ROOT-CAUSE BUG, confirming it is not clip-specific:
		//       MSG_WriteBit(v40, sub_B8C70());     <- ONE bit
		//       MSG_WriteLong(v40, 0);
		// where CL_ParseGamestate reads TWO bits (v9, v10).
		//
		// So the native recorder EXISTS and is wired in. It simply never fires,
		// because of the gate below.
		//
		// ---- THE GATE, decompiled ------------------------------------------------
		//   sub_9103D0(client) : clc[+262752] == 0        "not already recording"
		//   sub_90FBF0(client) : all five of
		//        1. !off_A60E5E0 || *(DWORD*)(off_A60E5E0 + 16)
		//        2. sub_856280() == Dvar_GetBool(off_1BD3750)
		//        3. *(BYTE*)(off_10F1B038 + 16)      dvar "1766", registered
		//                                            sub_B05E0("1766", 1, 0) -> bool,
		//                                            DEFAULT TRUE
		//        4. byte_10F23F68 && sub_4A0B80(client) == 0   (demo memory
		//                                            allocated, primary client)
		//        5. !byte_1BD36F8
		//
		// This probe reports EVERY one of them, UNCONDITIONALLY (RULE A15 -- a
		// diagnostic gated on the condition it measures is worthless), then reports
		// whether the original actually opened a file. One live match answers which
		// term is false, and whether it is a dvar we can simply set.
		//
		// Dvar value lives at +16 (proven from Dvar_SetBool @0xB1FD0 reading +12 as
		// the type). Every pointer is validated before use (RULE A6): globals of an
		// idle subsystem hold junk, not zero.
		int g_startrec_logs = 0;

		// Declared here because report_record_gate() below calls through it to get
		// the engine's own answer rather than re-deriving the gate from offsets.
		// RETURN TYPE MATTERS: sub_90FBF0 is `bool __fastcall(__int64)`, so only AL
		// is defined. A previous build declared this as returning int64, read the
		// undefined upper bits of RAX, got a truthy value, and therefore never
		// applied the force -- while also reporting a meaningless '=1'. Same class
		// of error as an offset guess: never widen a narrow return.
		bool(*CL_Demo_IsRecordingAllowed_orig)(std::int64_t) = nullptr;

		// Read a dvar pointer global and report its bool/int value at +16.
		// Returns -1 for "pointer unusable", -2 for "global itself unreadable".
		int dvar_value_at(const std::uintptr_t global, const bool as_byte)
		{
			if (!readable(reinterpret_cast<const void*>(global), 8)) return -2;
			const auto p = *reinterpret_cast<const std::uintptr_t*>(global);
			if (!p || !readable(reinterpret_cast<const void*>(p), 24)) return -1;
			return as_byte
				? static_cast<int>(*reinterpret_cast<const std::uint8_t*>(p + 16))
				: *reinterpret_cast<const std::int32_t*>(p + 16);
		}

		void report_record_gate(const unsigned int client, const char* when)
		{
			// clc is read from IDA 0x1BD3D00 DIRECTLY. demo_game::clc_for() used to
			// read 0x1BD3C00 -- an unrelated variable (3 xrefs, none client code, vs
			// 122 on the real clc). FIXED 2026-08-17; the two now agree, and this
			// direct read is retained only because it is a working probe.
			std::uintptr_t clc_base = 0;
			if (readable(reinterpret_cast<const void*>(0x1BD2D00_b), 8))
			{
				clc_base = *reinterpret_cast<const std::uintptr_t*>(0x1BD2D00_b);
			}
			const std::uintptr_t v3 = clc_base ? clc_base + 482656ull * client : 0;

			int gate2 = -2, handle_nonnull = -2, recflag = -2;
			if (v3 && readable(reinterpret_cast<const void*>(v3 + 262744), 16))
			{
				gate2 = *reinterpret_cast<const std::int32_t*>(v3 + 262752);
				handle_nonnull = *reinterpret_cast<const std::uintptr_t*>(v3 + 262744) ? 1 : 0;
			}
			if (v3 && readable(reinterpret_cast<const void*>(v3 + 346132), 1))
			{
				recflag = *reinterpret_cast<const std::uint8_t*>(v3 + 346132);
			}

			const int d_A60E5E0 = dvar_value_at(0xA60D5E0_b, false);
			const int d_1BD3750 = dvar_value_at(0x1BD2750_b, true);
			const int d_1766    = dvar_value_at(0x10F1A038_b, true);

			int memflag = -2;
			if (readable(reinterpret_cast<const void*>(0x10F22F68_b), 1))
			{
				memflag = *reinterpret_cast<const std::uint8_t*>(0x10F22F68_b);
			}
			int modeflag = -2;
			if (readable(reinterpret_cast<const void*>(0x1BD26F8_b), 1))
			{
				modeflag = *reinterpret_cast<const std::uint8_t*>(0x1BD26F8_b);
			}
			int localnum = -2;
			{
				const auto f = reinterpret_cast<std::int32_t(*)(unsigned int)>(0x49FB80_b);
				localnum = f(client);
			}

			// AUTHORITATIVE ANSWERS -- call the engine's own predicates instead of
			// re-deriving them from offsets. The first version of this probe read
			// dvar pointers at +16 and reported dvar1BD3750 = 143, then 22, on
			// successive runs. A bool cannot be 143. That offset reading is WRONG
			// for these globals and must not be built on (Absolute Rule: never
			// assign meaning to an offset because the value looks plausible -- and
			// certainly not when it does not).
			//
			// sub_856280 @0x856280 IS term 2 in full (`Dvar_GetBool(off_1BD3750)`),
			// so calling it gives that term exactly, with no offset assumption.
			const int term2 = reinterpret_cast<unsigned char(*)()>(0x855280_b)() ? 1 : 0;
			const int allowed = CL_Demo_IsRecordingAllowed_orig
				? (CL_Demo_IsRecordingAllowed_orig(client) ? 1 : 0) : -1;
			const int idle = reinterpret_cast<bool(*)(int)>(0x90F3D0_b)(
				static_cast<int>(client)) ? 1 : 0;

			Console::printf(
				"[rec] CL_Demo_StartRecord %s client=%u  demoPlaying=%d  || ENGINE SAYS: "
				"IsRecordingAllowed=%d  IsDemoStateIdle=%d  term2(sub_856280)=%d",
				when, client, static_cast<int>(g_native_playing),
				allowed, idle, term2);
			Console::printf(
				"[rec]   gate2 clc+262752=%d (needs 0)  |  RAW+16 reads, UNVERIFIED: "
				"A60E5E0=%d 1BD3750=%d \"1766\"=%d",
				gate2, d_A60E5E0, d_1BD3750, d_1766);
			unsigned long long freespace = 0ull;
			if (readable(reinterpret_cast<const void*>(0x10F1A120_b), 8))
			{
				freespace = *reinterpret_cast<const unsigned long long*>(0x10F1A120_b);
			}
			Console::printf(
				"[rec]   demoMemAllocated=%d (needs !=0)  localClientNum=%d (needs 0)  "
				"byte_1BD36F8=%d (needs 0)  |  demoMemFree=%llu (capacity check after the "
				"gates: needs > (nClients+1) * slotSize)",
				memflag, localnum, modeflag, freespace);
			Console::printf(
				"[rec]   -> %s   [fileHandle=%s recordingFlag=%d]",
				(gate2 == 0 && d_A60E5E0 != 0 && d_1BD3750 > 0 && d_1766 > 0
					&& memflag > 0 && localnum == 0 && modeflag == 0)
					? "ALL CONDITIONS MET, recording should start"
					: "BLOCKED -- see which term above fails its 'needs'",
				handle_nonnull < 0 ? "?" : (handle_nonnull ? "OPEN" : "null"),
				recflag);
		}

		// =====================================================================
		//  ENABLING THE ENGINE'S OWN RECORDER
		// =====================================================================
		// MEASURED (6 samples, 2026-08-08): CL_Demo_StartRecord runs on every
		// connect and is refused by CL_Demo_IsRecordingAllowed. Of its five terms,
		// three were measured PASSING with readings that are corroborated
		// elsewhere -- demoMemAllocated=1, localClientNum=0, and byte_1BD36F8=0 on
		// a real map (the same value the asset bracket independently reports in the
		// same run). Gate 2 (clc+262752 == 0) also passes. So the refusal comes
		// from the dvar terms.
		//
		// We do NOT know those dvars' names, and their values could not be read
		// reliably (see the note above about 143/22). Rather than guess a name or an
		// offset, hook the PREDICATE. Its only caller is CL_Demo_StartRecord, so the
		// entire blast radius is "let recording start" -- nothing else in the engine
		// consults it.
		//
		// This is enabling a shipped engine capability that config disables, in the
		// same spirit as the mod's unlock-all. It is NOT manufacturing state: every
		// byte of the demo is still written by the engine's own recorder from live
		// data.
		//
		// Two known writer defects will be present in whatever it records, and BOTH
		// are already fixed on our read side, which is strong corroboration that they
		// were always writer bugs:
		//   * CL_Demo_StartRecord writes the gamestate as the FIRST record in its
		//     message, so bitpos ends at 4 and our framing auto-detect applies the
		//     one-bit fix -- exactly right, because the recorder does
		//     MSG_WriteBit(msg, sub_B8C70()) where CL_ParseGamestate reads two bits.
		//   * it emits [4-byte sequence][compressed body] with NO int16 length
		//     prefix, which is precisely what repair_gamestate_message() inserts.
		bool g_force_record = true;
		int g_gate_logs = 0;

		// ⭐ Recording a NATIVE demo while playing a CUSTOM one back is OFF by default.
		//
		// It happens for a real reason, not by accident: demo_utils'
		// commit_live_message_state() replicates sub_99960's post-parse commit edge,
		// and part of that edge IS the native recorder's per-message append chain
		// (CL_Demo_RecordMessage_Begin/Data/End). So replaying a .dm_s2 also writes a
		// native .demo -- with the engine's own header, footer and NCS table, i.e. a
		// free .dm_s2 -> .demo transcode that needs no same-map template.
		//
		// That is genuinely useful, but it is not what someone asked for when they
		// pressed play, and it silently writes a multi-megabyte file every time. So it
		// is opt-in: `demo_record_theater 1`.
		//
		// Blocking it at the GATE (recording never starts) rather than at the append is
		// deliberate. Suppressing only the append would still open the file and write a
		// header and footer, leaving a packet-less .demo that looks valid and is not.
		bool g_record_in_theater = false;

		bool cl_demo_is_recording_allowed_stub(const std::int64_t client)
		{
			const bool real = CL_Demo_IsRecordingAllowed_orig(client);

			// FORCE ONLY THE DVAR TERMS. CL_Demo_IsRecordingAllowed is a conjunction
			// of five things; two are dvars that config disables (which is what we
			// want to override) and the rest are CONTEXTUAL -- they encode when the
			// engine considers recording meaningful at all. Forcing the whole
			// predicate threw those away too, and the first enabled run duly recorded
			// the hub / virtual lobby as well as the match.
			//
			// MEASURED across every run so far: byte_1BD36F8 is 1 in
			// mp_hub_allies_slim and 0 on mp_shipment_s2 / mp_canon_farm /
			// mp_sandbox_01 -- and the asset bracket independently prints the same
			// value in the same runs. The engine's own gate requires !byte_1BD36F8,
			// so the hub exclusion was ALREADY THERE and my force removed it.
			// Honouring it is restoring engine behaviour, not adding a special case.
			//
			// Both reads below are plain bytes -- no ABI or offset assumption (see
			// RULE A17: the dvar terms could not be read reliably, which is exactly
			// why we override rather than evaluate them).
			bool context_ok = true;
			if (readable(reinterpret_cast<const void*>(0x1BD26F8_b), 1))
			{
				// term 5: !byte_1BD36F8  -- 1 means hub/frontend, do not record
				context_ok = (*reinterpret_cast<const std::uint8_t*>(0x1BD26F8_b) == 0);
			}
			if (context_ok && readable(reinterpret_cast<const void*>(0x10F22F68_b), 1))
			{
				// term 4a: demo record memory must be allocated
				context_ok = (*reinterpret_cast<const std::uint8_t*>(0x10F22F68_b) != 0);
			}

			// A custom-theater demo being OPEN is enough to decline: start_file() opens
			// it before bootstrap_map(), so this is already true by the time
			// CL_InitCGame reaches CL_Demo_StartRecord.
			const bool theater = demo_playback::is_playing();
			const bool theater_blocks = theater && !g_record_in_theater;

			const bool force = g_force_record && !real && !g_native_playing
				&& !theater_blocks && context_ok;
			if (g_gate_logs < 4)
			{
				++g_gate_logs;
				const char* why =
					force ? "  [forced: demo_record is ON]"
					: theater_blocks
						? "  [not forced: custom demo playback -- `demo_record_theater 1` "
						  "to also write a native .demo while replaying]"
					: (g_force_record && !real && !g_native_playing && !context_ok)
						? "  [not forced: hub/frontend or demo memory unavailable]"
						: "";
				Console::printf(
					"[rec] CL_Demo_IsRecordingAllowed(%lld) engine=%d -> returning %d%s",
					static_cast<long long>(client), real ? 1 : 0,
					force ? 1 : (real ? 1 : 0), why);
			}
			// Never record while playing a demo back. Gate 2 already blocks that
			// (clc+262752 is non-zero during playback), but do not rely on one guard
			// for something that would recurse into the demo system.
			return force ? true : real;
		}


		// =====================================================================
		//  PUBLIC-MATCH DEMOS: splice ncs type 21 into the footer after recording
		// =====================================================================
		// PROVEN 2026-08-09 by a same-map controlled pair on mp_forest_01 and
		// confirmed in game three times (publicmatch_fixed, x03ae_fixed,
		// x01a2_fixed): a public-match recording's footer carries NO netconststring
		// type 21 (the GSC script-string table), and playback cannot resolve the
		// type-21 indices its own snapshots contain. Adding those 396 strings and
		// nothing else makes the demo play.
		//
		// WHY the engine omits them: sub_5DC650 writes the footer from a REGISTRY of
		// registered {type,index} records (unk_A608570+3084, count dword_A609178).
		// A local match registers script strings because the client is the server and
		// runs GSC; a public match never does.
		//
		// WHY NOT fix it at record time: that needs the right registrar for type 21
		// and its valid index range, neither of which is established. An earlier
		// attempt gated on netconststrings_block_list(21) -- a DIFFERENT table, which
		// is populated live -- so it never fired and would not have helped. Three
		// separate storages are involved; see CLAUDE.md. Splicing the finished file
		// is byte-identical to the repair that is verified working, so it is what
		// ships until the registration path is properly understood.
		//
		// Non-destructive by construction: the file is only rewritten after a full
		// successful parse, and any inconsistency aborts the repair.
		bool g_autofix_ncs = true;
		// Rename a finished recording to <map>_<date>_<time>. On by default:
		// the engine's own x<4hex>_<8hex> name tells the user nothing.
		bool g_autorename = true;

		// NetConstStrings block, from CL_Demo_ReadNetConstStringTable @0x5DC0E0:
		//   u16 magic 0xD311 | u16 version 1 | u32 total
		//   26 x u32 per-type counts
		//   total x { u16 index, u16 length, length bytes }   grouped in type order
		struct ncs_block
		{
			std::size_t at = 0;                 // offset of the magic within the file
			std::size_t entries_at = 0;         // first entry
			std::size_t end = 0;                // one past the last entry
			std::uint32_t counts[26]{};
			std::size_t group_start[26]{};
			std::size_t group_end[26]{};
		};

		bool parse_ncs(const std::vector<std::uint8_t>& f, std::size_t footer_start,
			std::size_t footer_end, ncs_block& out)
		{
			static const std::uint8_t magic[4] = { 0x11, 0xD3, 0x01, 0x00 };
			std::size_t at = std::string::npos;
			for (std::size_t i = footer_start; i + 4 <= footer_end; ++i)
			{
				if (std::memcmp(f.data() + i, magic, 4) == 0) { at = i; break; }
			}
			if (at == std::string::npos || at + 8 + 26 * 4 > footer_end) return false;
			out.at = at;
			for (int t = 0; t < 26; ++t)
			{
				std::memcpy(&out.counts[t], f.data() + at + 8 + 4 * t, 4);
				if (out.counts[t] > 100000u) return false;      // obvious garbage
			}
			std::size_t pos = at + 8 + 26 * 4;
			out.entries_at = pos;
			for (int t = 0; t < 26; ++t)
			{
				out.group_start[t] = pos;
				for (std::uint32_t n = 0; n < out.counts[t]; ++n)
				{
					if (pos + 4 > footer_end) return false;
					std::uint16_t len = 0;
					std::memcpy(&len, f.data() + pos + 2, 2);
					pos += 4u + len;
					if (pos > footer_end) return false;
				}
				out.group_end[t] = pos;
			}
			out.end = pos;
			return true;
		}

		bool read_file(const std::filesystem::path& p, std::vector<std::uint8_t>& out)
		{
			std::ifstream in(p, std::ios::binary);
			if (!in) return false;
			in.seekg(0, std::ios::end);
			const auto n = static_cast<std::size_t>(in.tellg());
			if (n < 79384 + 8) return false;
			in.seekg(0);
			out.resize(n);
			in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
			return static_cast<bool>(in);
		}

		// The type-21 table is EMBEDDED (demo_ncs21.inc), so no donor file is needed
		// and a fresh install with only public recordings still works.
		//
		// Justification for a single baked-in copy: type 21 is a GSC string table,
		// not map content. The block was extracted from dday, airshipdemo, egyptbots,
		// gibraltar and a locally recorded mp_forest_01 demo -- all five BYTE-IDENTICAL,
		// 396 entries, 8141 bytes. One copy therefore repairs a public demo from any
		// map.
		bool type21_donor(const std::uint8_t*& data, std::size_t& size,
			std::uint32_t& count, std::string& source)
		{
			data = NCS_TYPE21_BLOCK;
			size = sizeof(NCS_TYPE21_BLOCK);
			count = NCS_TYPE21_COUNT;
			source = "built-in table";
			return size != 0;
		}

		std::string repair_demo_type21(const std::filesystem::path& path, bool& ok)
		{
			ok = false;
			std::vector<std::uint8_t> f;
			if (!read_file(path, f)) return "could not read the file";
			std::uint32_t fsz = 0;
			std::memcpy(&fsz, f.data() + f.size() - 8, 4);
			if (fsz + 8u > f.size())
			{
				return "no valid footer -- the game was probably killed while recording, "
				       "and a demo with no footer cannot be repaired from outside";
			}
			const std::size_t fstart = f.size() - 8 - fsz;
			ncs_block b{};
			if (!parse_ncs(f, fstart, f.size() - 8, b))
			{
				return "the footer's NetConstStrings block could not be parsed";
			}
			if (b.counts[21])
			{
				ok = true;   // nothing wrong with it
				return "already has script strings (ncs type 21) -- nothing to do";
			}

			const std::uint8_t* donor = nullptr;
			std::size_t donor_size = 0;
			std::uint32_t n21 = 0;
			std::string donor_name;
			if (!type21_donor(donor, donor_size, n21, donor_name))
			{
				return "the built-in script-string table is unavailable";
			}

			// Rebuild: same bytes, with type 21's group inserted in type order and the
			// counts/total/footer-size updated to match.
			std::vector<std::uint8_t> out;
			out.reserve(f.size() + donor_size + 16);
			out.insert(out.end(), f.begin(), f.begin() + b.at);          // up to the magic
			const std::uint8_t magic[4] = { 0x11, 0xD3, 0x01, 0x00 };
			out.insert(out.end(), magic, magic + 4);
			std::uint32_t total = 0;
			std::uint32_t counts[26];
			for (int t = 0; t < 26; ++t)
			{
				counts[t] = (t == 21) ? n21 : b.counts[t];
				total += counts[t];
			}
			const auto put32 = [&out](std::uint32_t v)
			{
				out.insert(out.end(), reinterpret_cast<std::uint8_t*>(&v),
					reinterpret_cast<std::uint8_t*>(&v) + 4);
			};
			put32(total);
			for (int t = 0; t < 26; ++t) put32(counts[t]);
			for (int t = 0; t < 26; ++t)
			{
				if (t == 21)
				{
					out.insert(out.end(), donor, donor + donor_size);
				}
				else
				{
					out.insert(out.end(), f.begin() + b.group_start[t],
						f.begin() + b.group_end[t]);
				}
			}
			// everything between the last entry and the trailing [size][magic]
			out.insert(out.end(), f.begin() + b.end, f.begin() + (f.size() - 8));
			const std::uint32_t new_fsz = static_cast<std::uint32_t>(out.size() - fstart);
			put32(new_fsz);
			put32(29u);

			std::ofstream o(path, std::ios::binary | std::ios::trunc);
			if (!o)
			{
				Console::printf("[rec] could not rewrite %s", path.filename().string().c_str());
				return "could not open the file for writing";
			}
			o.write(reinterpret_cast<const char*>(out.data()),
				static_cast<std::streamsize>(out.size()));
			o.close();
			Console::printf(
				"[rec] %s was a public-match recording with no script strings -- spliced "
				"ncs type 21 (%u entries, donor %s). It will now play.",
				path.filename().string().c_str(), n21, donor_name.c_str());
			ok = true;
			return "repaired: spliced " + std::to_string(n21) + " script strings from "
				+ donor_name;
		}

		// CL_Demo_StopRecord closes the file, so repair after it returns.
		void(*CL_Demo_StopRecord_orig)(unsigned int) = nullptr;
		void cl_demo_stop_record_stub(const unsigned int client)
		{
			CL_Demo_StopRecord_orig(client);
			if (!g_autofix_ncs || g_native_playing) return;

			// Repair the most recently written demo. The engine names files
			// x<4hex>_<8hex>.demo and we cannot easily recover the path from clc, so
			// take the newest .demo touched in the last few seconds.
			const auto dir = native_demos_directory();
			if (!dir) return;
			std::error_code ec;
			std::filesystem::path newest;
			std::filesystem::file_time_type best{};
			for (const auto& e : std::filesystem::directory_iterator(*dir, ec))
			{
				if (ec) break;
				if (!e.is_regular_file() || e.path().extension() != ".demo") continue;
				const auto t = e.last_write_time(ec);
				if (ec) { ec.clear(); continue; }
				if (newest.empty() || t > best) { best = t; newest = e.path(); }
			}
			if (!newest.empty())
			{
				bool ok = false;
				repair_demo_type21(newest, ok);

				// Then give it a name a human can read. The engine writes
				// x<4hex>_<8hex>.demo; the map name is in the demo's own footer,
				// so this becomes <map>_<date>_<time>.
				//
				// ORDER MATTERS: repair first. The repair rewrites the file, and
				// renaming beforehand would only mean it rewrites a file under a
				// different name -- harmless, but it also means a failed repair
				// would leave a nicely-named demo that does not play.
				if (g_autorename)
				{
					std::string why;
					if (const auto renamed = demo_library::auto_rename(newest, why))
					{
						if (*renamed != newest)
						{
							Console::printf("[rec] saved as %s",
								renamed->filename().string().c_str());
						}
					}
					else if (!why.empty())
					{
						Console::printf("[rec] left the generated name in place (%s)",
							why.c_str());
					}
				}
			}
		}


		// =====================================================================
		//  CL_Demo_HandleAction @0x915E40 -- the native demo's own key actions
		// =====================================================================
		// void CL_Demo_HandleAction(int client, int action, int down)
		//
		// `action` is an ACTION ID, not a raw key, so we do not need the key map.
		// Decoded from the switch:
		//     1 / 32     toggle cl_demo_pause                    (spacebar)
		//     3 / 114    queue sub_917C10 or CL_Demo_StartClipRecord
		//     4 / 168    cycle camera mode 0->1->2 via sub_91AB50 (F2: 1st/3rd/free)
		//     5 / 202    sub_91AC80(client, 1)   gated !CG_IsTheaterOrbitCamera
		//     6 / 201    sub_91AC80(client, 0)   gated !CG_IsTheaterOrbitCamera
		//    16 / 167    toggle the byte at PlaybackData+32
		//    17 / 169    SET PAUSE and queue sub_916740   <-- prime suspect for F3:
		//                "freezes the demo and does weird camera movement"
		//    18 / 156    queue sub_916FC0            (seek back)
		//    19 / 157    queue CL_Demo_SeekToNextKeyFrame (seek forward)
		//    20 / 154    timescale += 0.1, clamped to 4.0     <-- up arrow
		//    21 / 155    timescale -= 0.1, clamped to 0.1     <-- down arrow
		//    22 / 170    timescale = sub_93C5F2() if >= 1.0
		//    23 / 171    timescale += 1.0, clamped to 4.0
		//
		// â­ Cases 20/21/23 write *(float*)(PlaybackData + 28) -- which PROVES that
		// field is the demo timescale. It had only been INFERRED when the HUD
		// scaling was wired to it.
		bool g_log_actions = true;
		int g_blocked_action_a = 17;    // see above; user-verifiable, and toggleable
		int g_blocked_action_b = 169;
		int g_action_logs = 0;

		void(*CL_Demo_HandleAction_orig)(int, int, int) = nullptr;

		// End of the packet stream for the playing demo (start of the footer),
		// used by playback_progress(). 0 = unknown.
		std::int64_t g_playing_body_end = 0;

		// =================================================================
		//  KEYFRAME GENERATION â€” why rewind/forward do nothing
		// =================================================================
		//
		// PROVEN 2026-08-10 by cross-referencing MW3.
		//
		// The seek functions themselves are CORRECT. S2's JumpBack (0x916FC0)
		// and JumpForward (0x917010) are structurally identical to MW3's
		// CL_Demo_JumpBack / CL_Demo_JumpForward, including the subtle part:
		// jump to the keyframe's BASELINE first, then to the keyframe, because
		// keyframes are delta-coded.
		//
		//     kf = GetKeyFrameForJump*();            S2 sub_915A10 / inlined
		//     if (kf >= 0) {
		//         b = GetBaselineForKeyframe(kf);    S2 sub_9158C0
		//         if (b != -1 && b != kf) ProcessKeyFrameJump(b);   S2 sub_917ED0
		//         ProcessKeyFrameJump(kf);
		//     }
		//
		// Both scan a 250-slot ring (48-byte stride) for a keyframe time on the
		// right side of "now". If the ring is EMPTY they return -1 and do
		// nothing at all -- silently. That is exactly the reported symptom.
		//
		// The ring is empty because generation is gated:
		//
		//     CL_Demo_ReadDemoMessage:
		//         if (*(BYTE*)(clc + 262772))            <- isClipPlaying
		//             if (sub_91A3A0(client))            <- ShouldGenerateKeyFrame
		//                 sub_914790(client, 0, 0);      <- generate
		//
		// clc+262772 is the isClipPlaying flag (CL_Demo_IsClipPlaying @0x916CB0
		// reads exactly it, and MW3's CL_Demo_IsClipPlaying reads the same byte
		// its own keyframe gate uses). Scanning 0x90F000-0x922000, that byte has
		// FORTY reads and exactly ONE write -- `mov byte ptr [r15+40274h], 0` in
		// CL_Demo_StartRecord. Nothing in the demo region ever sets it.
		//
		// So during ordinary playback no keyframes are ever cached, and seeking
		// has nothing to seek to.
		//
		// THE FIX: call the engine's own keyframe writer ourselves, on the
		// engine's own schedule, during native playback. We deliberately do NOT
		// set clc+262772 -- that flag also drives the clip branches in
		// CL_SetCGameTime and several CL_Demo_HandleAction cases, and forcing it
		// would change far more than keyframe caching.
		//
		//   sub_914790  IDA 0x914790 - 0x1000 = 0x913790   the keyframe writer
		//   interval    matches the engine's own cadence; dvar "5756" is the
		//               keyframe rate (MW3 calls it cl_demo_keyframerate)
		bool  g_gen_keyframes = true;
		DWORD g_last_keyframe_ms = 0;
		int   g_keyframes_made = 0;
		constexpr DWORD KEYFRAME_INTERVAL_MS = 5000;
		void cl_demo_handle_action_stub(const int client, const int action, const int down)
		{
			if (g_log_actions && down && g_action_logs < 40)
			{
				++g_action_logs;
				Console::printf("[demo] demo action %d (down)%s", action,
					(action == g_blocked_action_a || action == g_blocked_action_b)
						? "  <- BLOCKED by demo_block_action" : "");
			}
			// Swallow the unwanted action entirely. Everything else passes through, so
			// pause / camera / seek / speed keep working exactly as the engine intends.
			if (down && (action == g_blocked_action_a || action == g_blocked_action_b))
			{
				return;
			}
			CL_Demo_HandleAction_orig(client, action, down);
		}

		// =====================================================================
		//  THE STRANDED DIVISION WIDGETS -- ONE LUI MODEL, NOT THE WHOLE HUD
		// =====================================================================
		// Symptom: during native playback a checkerboard icon plus division names
		// (Blitzkrieg / Clandestine) sit on the right of the screen and never
		// clear. The console names the culprit:
		//
		//   ui/s2/mphud_uc.lua:35 -> equippedperks_uc.lua:10 -> :129 -> :80
		//     -> ui/utility/mp/divisions_utils.lua
		//   "Error while handling model subscription"
		//
		// sub_36A550 is the LUI model-subscription dispatcher: it walks the dirty
		// models and lua_pcalls each subscriber (sub_2E40A0), reporting on failure.
		// The divisions callback throws, so the widget stops part-built and
		// whatever was already laid out stays on screen.
		//
		// PROVEN -- who feeds it. sub_357AD0 republishes the whole LUI HUD model
		// set every frame via
		//     sub_3573B0(client, descIndex, "cg.hud.<name>")
		// a table-driven publisher over the descriptor array off_F7D220. Model
		// **86 is "cg.hud.currentDivision"** (0x35807A), the exact model the
		// divisions helper subscribes to, and the string has ONE code xref -- that
		// call site -- so there is no ambiguity about which id it is.
		//
		// So instead of cg_draw2D, which gates two ENTIRE native draw functions
		// (hitmarkers, killfeed, score popups) and does not touch LUI at all, we
		// decline to publish this ONE model while a native demo is playing. Every
		// other HUD model, and the whole native HUD, is untouched.
		//
		// A demo does not carry the local player's division/loadout data -- the
		// same gap that leaves the viewmodel's customization images non-resident
		// -- so the value being published here is not meaningful during playback.
		//
		// ⚠ UNPROVEN that this clears the widget. Suppressing a publish stops the
		// model CHANGING; if the widget was already built from an earlier publish
		// it may persist. That is why the id list is runtime-settable: one build
		// can test several ids instead of one build per guess.
		//
		//   sub_3573B0  IDA 0x3573B0 - 0x1000 = 0x3563B0   (nothing else hooks it)
		//   ids 49..57 are the perk models, also drawn by equippedperks_uc.lua
		using CG_PublishHudModel_t = std::int64_t(__fastcall*)(unsigned int, int, const char*);
		CG_PublishHudModel_t CG_PublishHudModel_orig = nullptr;

		constexpr int HUD_MODEL_CURRENT_DIVISION = 86;

		// ⛔ CRASH FIX 2026-08-12. These were a std::vector<int> and a std::set<int>
		// read from THIS HOOK -- which runs on the engine thread ~70 TIMES PER FRAME
		// -- while `demo_hud_suppress` / `demo_hud_minimal` (console thread) and the
		// GUI checkbox (DXGI Present thread) REASSIGNED them:
		//
		//     console : g_suppressed_hud_models = next;   // frees the old buffer
		//     hook    : const auto& s = g_suppressed_hud_models;
		//               std::find(s.begin(), s.end(), desc);   // walks the freed one
		//
		// operator= frees and reallocates, so the hook could iterate freed memory ->
		// use-after-free -> crash. `demo_hud_models` had the same bug independently:
		// it called g_seen_hud_models.clear() while the hook called .insert() on it.
		// Both commands could therefore crash the game, which matches the report.
		//
		// FIX: a lock-free atomic BITMASK. Every measured id is 1..104 (73 distinct,
		// max 104 -- see the id table in CLAUDE.md), so two uint64_t cover 0..127
		// exactly. The hook now does one relaxed atomic load and a bit test: no
		// allocation, no iteration, no shared container, and materially faster on a
		// path that runs 70x/frame. Writers compose a value and store it.
		constexpr std::uint64_t hud_bit_lo(const int id)
		{
			return (id >= 0 && id < 64) ? (1ULL << id) : 0ULL;
		}
		constexpr std::uint64_t hud_bit_hi(const int id)
		{
			return (id >= 64 && id < 128) ? (1ULL << (id - 64)) : 0ULL;
		}

		std::atomic<std::uint64_t> g_hud_mask[2]{
			hud_bit_lo(HUD_MODEL_CURRENT_DIVISION), hud_bit_hi(HUD_MODEL_CURRENT_DIVISION) };
		std::atomic<std::uint64_t> g_hud_seen[2]{ 0ULL, 0ULL };
		bool g_log_hud_models = false;
		std::atomic<int> g_hud_suppressed_hits{ 0 };

		// Withhold LUI model 100 (cg.Shared.connectionStateActive). Set by the custom
		// theater across a rewind, where connstate deliberately leaves CA_ACTIVE and
		// would otherwise make LUI open the in-game menu. See the comment at the
		// desc == 100 branch in cg_publish_hud_model_stub.
		std::atomic<bool> g_hold_conn_active{ false };
		std::atomic<int> g_hold_conn_hits{ 0 };

		[[nodiscard]] bool hud_mask_has(const int id)
		{
			if (id < 0 || id > 127)
			{
				return false;
			}
			return ((g_hud_mask[id >> 6].load(std::memory_order_relaxed) >> (id & 63)) & 1ULL) != 0;
		}

		[[nodiscard]] bool hud_mask_empty()
		{
			return g_hud_mask[0].load(std::memory_order_relaxed) == 0
				&& g_hud_mask[1].load(std::memory_order_relaxed) == 0;
		}

		// Writers only. Composes locally, then two stores -- the hook can never see
		// a torn container because there is no container.
		void hud_mask_set(const std::vector<int>& ids)
		{
			std::uint64_t lo = 0, hi = 0;
			for (const int id : ids)
			{
				lo |= hud_bit_lo(id);
				hi |= hud_bit_hi(id);
			}
			g_hud_mask[0].store(lo, std::memory_order_relaxed);
			g_hud_mask[1].store(hi, std::memory_order_relaxed);
			g_hud_suppressed_hits.store(0, std::memory_order_relaxed);
		}

		// ⭐ THE INVERSE OF hud_mask_set: suppress EVERY model-driven LUI HUD id
		// except the ones named. This is what "only keep X" actually requires --
		// a suppress LIST is open-ended, so anything we forgot to list survives,
		// which is why every previous attempt left something on screen.
		//
		// Why this leaves obituaries / score popups / hitmarkers alone (the mapping
		// is from the MWR PS4 named build, recorded in CLAUDE.md):
		//     hitmarkers   NATIVE, in the crosshair path -- not a LUI model at all
		//     killfeed     LUI, but EVENT-driven (LUI_Obituary), not model-driven
		//     score popup  LUI, EVENT-driven
		//     everything else on screen (ammo, division, perks, health, morale,
		//     combat roles, killCounter, clan tag, vehicle, netPerf, hints)
		//                  LUI, MODEL-driven through CG_PublishHudModel
		// So withholding the whole model-driven set should leave exactly the three
		// you want. If something you wanted to keep disappears, it was model-driven
		// after all -- name its id to keep it, which is what the argument is for.
		void hud_mask_suppress_all_except(const std::vector<int>& keep)
		{
			std::uint64_t lo = ~0ULL, hi = ~0ULL;
			for (const int id : keep)
			{
				lo &= ~hud_bit_lo(id);
				hi &= ~hud_bit_hi(id);
			}
			// id 100 = cg.Shared.connectionStateActive. It is published OUTSIDE the
			// connstate gate, at the top of the publisher, and is not HUD furniture
			// -- never blanket-suppress it.
			lo &= ~hud_bit_lo(100);
			hi &= ~hud_bit_hi(100);
			g_hud_mask[0].store(lo, std::memory_order_relaxed);
			g_hud_mask[1].store(hi, std::memory_order_relaxed);
			g_hud_suppressed_hits.store(0, std::memory_order_relaxed);
		}

		[[nodiscard]] std::string hud_mask_list(const std::atomic<std::uint64_t>* m)
		{
			std::string out;
			for (int id = 0; id < 128; ++id)
			{
				if (((m[id >> 6].load(std::memory_order_relaxed) >> (id & 63)) & 1ULL) != 0)
				{
					if (!out.empty()) { out += " "; }
					out += std::to_string(id);
				}
			}
			return out;
		}

		// ---- MINIMAL HUD ------------------------------------------------------
		// "Keep only hitmarkers, killfeed and score popups" cannot be done with one
		// engine switch, because those three do NOT share a system:
		//
		//   hitmarkers   NATIVE, drawn inside the crosshair path
		//                (MWR twin: CG_DrawCrosshair, gated by CG_ShouldDrawHud)
		//   killfeed     LUI, EVENT-driven  (MWR twin: LUI_Obituary @0x1857E0)
		//   score popup  LUI, EVENT-driven
		//   everything else on screen (ammo, division, perks, health, morale,
		//                combat roles, killCounter, clan tag, vehicle, netPerf)
		//                LUI, MODEL-driven through sub_3573B0
		//
		// That is what makes the split possible: the clutter is precisely the
		// model-driven half, and the three keepers are not model-driven at all.
		// So we withhold the model-driven models and leave both event-driven LUI
		// and the native HUD completely alone.
		//
		// Deliberately NOT used, with reasons:
		//   cg_draw2D           gates two ENTIRE native draw functions — it takes
		//                       the hitmarkers with it (user-confirmed).
		//   CG_ShouldDrawHud    MWR shows it also gates CG_DrawCrosshair, so the
		//                       S2 twin would kill hitmarkers too.
		//
		// ⚠ HONEST LIMIT: withholding a publish stops a model CHANGING; it does not
		// tear down an element already built from an earlier publish. Enable it
		// BEFORE starting playback for the cleanest result. And "killfeed and score
		// popups survive" is STRONG INFERENCE from the id->name list below
		// containing nothing resembling an obituary or points feed — not a
		// measurement. If either disappears, `demo_hud_suppress` narrows the list
		// without a rebuild.
		//
		// Ids are from sub_357AD0's own call sites, so each is exact, not guessed.
		const std::vector<int> HUD_MODELS_MINIMAL = {
			// weapon + ammo block
			30, 31, 32, 27, 28, 29, 1, 2, 3, 5, 4, 33, 34, 104,
			// lethals / tacticals
			47, 48, 8, 9, 10, 11,
			// ADS, breath, reload
			12, 13, 14, 15, 16, 17, 62,
			// morale, perks, combat roles
			35, 49, 50, 51, 52, 53, 54, 55, 56, 57, 36, 37, 38, 39, 40,
			// health / state
			41, 26, 25, 67,
			// interaction hints + hub kiosks
			68, 69, 72, 71, 70, 103, 73, 74, 75, 76,
			// identity / match furniture
			78, 79, 80, 83, 84, 85, 86,
			// netPerf + vehicle
			93, 94, 95, 96, 97, 98, 99, 101, 102,
		};
		bool g_hud_minimal = false;

		// =====================================================================
		//  ⭐ BLANKING -- the mechanism that can actually REMOVE an element
		//
		//  MEASURED 2026-08-12: withholding the publish only FREEZES a widget.
		//  User report: "it stops the kill counter from updating but it's still
		//  drawn, and the weapon ammo counter and minimap still drawn too."
		//  That is the documented limitation seen from the other side -- an
		//  element already built keeps its last value forever, because nothing
		//  tells it to go away.
		//
		//  So do NOT skip the publish. Let it happen, with an EMPTY value. A LUI
		//  widget fed 0 / "" hides itself the same way it does when the thing it
		//  displays genuinely goes away.
		//
		//  The value comes from CG_GetHudModelValue @0x356560, an out-param getter
		//  the publisher calls just before pushing:
		//      sub_356560(client, sourceId, idx, int*, float*, bool*, strMax, char*)
		//  and the descriptor table at off_F7D220 (32-byte stride) has
		//  srcId == descIndex for the whole valid range 0..104, VERIFIED -- so the
		//  source id IS the HUD model id and the same numbers apply.
		//
		//  RULE A1:  CG_GetHudModelValue  0x356560 - 0x1000 = 0x355560
		//  RULE A3.1: checked, nothing else in src/ hooks it. Six call sites, all
		//             inside the HUD publish family.
		//  RULE A17: the return is `char`, not int or bool.
		//
		//  ⚠ Blanking and withholding are MUTUALLY EXCLUSIVE: if the publish hook
		//  returns early the getter is never reached, so an id must be in one mask
		//  or the other, never both. demo_hud_only clears the withhold mask.
		std::atomic<std::uint64_t> g_hud_blank[2]{ 0ULL, 0ULL };

		[[nodiscard]] bool hud_blank_has(const int id)
		{
			if (id < 0 || id > 127)
			{
				return false;
			}
			return ((g_hud_blank[id >> 6].load(std::memory_order_relaxed) >> (id & 63)) & 1ULL) != 0;
		}

		void hud_blank_all_except(const std::vector<int>& keep)
		{
			std::uint64_t lo = ~0ULL, hi = ~0ULL;
			for (const int id : keep)
			{
				lo &= ~hud_bit_lo(id);
				hi &= ~hud_bit_hi(id);
			}
			// Never blank id 100 (cg.Shared.connectionStateActive) -- not HUD
			// furniture, and it is published outside the connstate gate.
			lo &= ~hud_bit_lo(100);
			hi &= ~hud_bit_hi(100);
			g_hud_blank[0].store(lo, std::memory_order_relaxed);
			g_hud_blank[1].store(hi, std::memory_order_relaxed);
		}

		void hud_blank_clear()
		{
			g_hud_blank[0].store(0, std::memory_order_relaxed);
			g_hud_blank[1].store(0, std::memory_order_relaxed);
		}

		using CG_GetHudModelValue_t = char(__fastcall*)(std::int64_t, int, std::int64_t,
			int*, float*, bool*, unsigned int, char*);
		CG_GetHudModelValue_t CG_GetHudModelValue_orig = nullptr;
		std::atomic<int> g_hud_blanked_hits{ 0 };

		char __fastcall cg_get_hud_model_value_stub(const std::int64_t client, const int src_id,
			const std::int64_t idx, int* i_out, float* f_out, bool* b_out,
			const unsigned int str_max, char* str_buf)
		{
			const char r = CG_GetHudModelValue_orig(client, src_id, idx,
				i_out, f_out, b_out, str_max, str_buf);
			if (!g_native_playing || !hud_blank_has(src_id))
			{
				return r;
			}
			// Publish an EMPTY value rather than the real one. Return success even
			// if the real getter failed, so the push definitely happens -- a widget
			// only hides when it is TOLD there is nothing, not when it is starved.
			if (i_out) { *i_out = 0; }
			if (f_out) { *f_out = 0.0f; }
			if (b_out) { *b_out = false; }
			if (str_buf && str_max) { str_buf[0] = '\0'; }
			g_hud_blanked_hits.fetch_add(1, std::memory_order_relaxed);
			return 1;
		}

		std::int64_t __fastcall cg_publish_hud_model_stub(
			const unsigned int client, const int desc, const char* name)
		{
			// ---- once-per-frame call-out for the minimal HUD -----------------
			// id 100 (cg.Shared.connectionStateActive) is published at the TOP of
			// sub_357AD0, OUTSIDE the connstate gate, so it fires exactly once per
			// frame — the cheapest safe per-frame hook point we have, on the
			// client thread with cg known valid. broadcaster::on_frame() is one
			// relaxed atomic load when the mode is off.
			// RULE A3.1: this is a call-out, NOT a second hook.
			if (desc == 100)
			{
				broadcaster::on_frame();
				// Bone cam keeps its bone list current here rather than inside its
				// camera write, so the list exists BEFORE the lock is switched on
				// and regardless of the camera mode. Self-throttled to ~4 Hz.
				bonecam::tick();

				// ⭐ THE MENU-ON-REWIND FIX.
				//
				// sub_357AD0 opens with
				//     v5 = clientConnectionState[494 * client];
				//     CG_PublishHudModel(client, 100, "cg.Shared.connectionStateActive");
				//     ...
				//     if (v5 >= 10) { ...the entire rest of the HUD... }
				// so id 100 is the ONE model published outside the connstate gate, and
				// LUI drives the in-game menu off it.
				//
				// The theater's rewind drops connstate ACTIVE -> PRIMED so
				// CL_SetCGameTime will re-promote through CL_FirstSnapshot. It writes
				// the field DIRECTLY, bypassing CL_SetClientState, so nothing inside
				// the engine's own setter fires -- but this publisher POLLS connstate
				// every frame and duly tells LUI the connection went away, which is
				// what opens the menu mid-rewind.
				//
				// Withholding the publish leaves LUI's model at its previous value
				// (active), so the menu never opens. Nothing is fabricated: we are not
				// telling LUI the connection is up, we are declining to tell it about a
				// transition the theater is making on its own behalf and will undo
				// within a few frames.
				if (g_hold_conn_active.load(std::memory_order_relaxed))
				{
					if (g_hold_conn_hits.fetch_add(1, std::memory_order_relaxed) == 0)
					{
						Console::printf("[demo] holding cg.Shared.connectionStateActive "
							"across the rewind so LUI does not open the in-game menu");
					}
					return 0;
				}
			}

			// Lock-free: one relaxed atomic load and a bit test. See the bitmask
			// block above for why this is not a container any more.
			if (g_native_playing && hud_mask_has(desc))
			{
				// Report the FIRST suppression only, unconditionally, so a run
				// where this does nothing is still distinguishable from a run
				// where the hook never fired (RULE A15).
				if (g_hud_suppressed_hits.fetch_add(1, std::memory_order_relaxed) == 0)
				{
					Console::printf("[demo] suppressing LUI HUD model %d (\"%s\") "
						"during native playback -- candidate fix for the stranded "
						"division widgets. demo_hud_suppress to change the list.",
						desc, name ? name : "?");
				}
				return 0;
			}
			// Log each id ONCE, not once per publish. This is a discovery tool --
			// it exists to answer "what ids are there" -- and the publisher runs
			// ~70 times PER FRAME, so streaming it wrote 187,096 lines (99.7% of a
			// 10 MB log) in a single session and buried everything else.
			// RULE: a discovery probe reports each distinct thing once.
			if (g_log_hud_models && g_native_playing && desc >= 0 && desc < 128)
			{
				// fetch_or returns the PREVIOUS value, so this is an atomic
				// test-and-set: the id is reported by exactly one caller, once,
				// with no shared container to race on.
				const std::uint64_t bit = 1ULL << (desc & 63);
				const std::uint64_t was = g_hud_seen[desc >> 6].fetch_or(
					bit, std::memory_order_relaxed);
				if ((was & bit) == 0)
				{
					Console::printf("[demo] hud model %3d = %s", desc, name ? name : "?");
				}
			}
			return CG_PublishHudModel_orig(client, desc, name);
		}

		// =====================================================================
		//  FREE CAMERA SPEED -- make the engine's baked-in constant adjustable
		// =====================================================================
		// CG_PredictPlayerState @0x4EFA0 routes the theater cameras:
		//     if (CG_IsTheaterFreeCamera || CG_IsTheaterOrbitCamera) {
		//         if (orbit) { sub_913AE0(c, cmd); return; }   <- the FLY camera
		//         sub_9135D0(c, cmd); return;                  <- follow/3rd person
		//     }
		// and sub_913AE0's wish-speed is
		//     v18 = ((maxAbs * 190.0) / (magnitude * 127.0)) * 3.0;
		//
		//     913d65  mulss xmm11, cs:dword_B639A8   ; 190.0   <- the speed
		//     913d72  divss xmm11, xmm0
		//     913d77  mulss xmm11, cs:dword_B37828   ; 3.0
		//
		// Scaling the INPUT cannot work: maxAbs/magnitude is scale-invariant
		// (analog-stick normalisation), so the constants are the only lever.
		//
		// dword_B37828 (3.0) has 15+ xrefs across the whole game -- untouchable.
		// dword_B639A8 (190.0) has only TWO, one of which is this instruction. Rather
		// than patch even that shared constant, we repoint THIS ONE INSTRUCTION's
		// rip-relative displacement at a float we own. Nothing else in the game is
		// affected, and the value becomes live-adjustable with no re-patching.
		float g_freecam_speed_storage = 190.0f;      // stock
		// The float the patched instruction actually reads. It normally points
		// at g_freecam_speed_storage, but that lives in OUR DLL, which Windows
		// may load many GB from the game module -- and a rip-relative operand
		// only reaches +/-2GB. When that happens we allocate a page NEAR the
		// instruction and use it instead (see alloc_float_near).
		float* g_freecam_speed = &g_freecam_speed_storage;
		bool g_freecam_patched = false;

		void patch_freecam_speed()
		{
			// IDA 0x913D65 -> literal 0x912D65, and _b adds base + 0x1000, giving
			// base + 0x913D65. Written out per RULE A1 rather than done in my head.
			auto* instr = reinterpret_cast<std::uint8_t*>(0x912D65_b);
			if (!readable(instr, 9))
			{
				Console::printf("[demo] freecam speed: instruction not readable, left stock");
				return;
			}
			// Verify the exact opcode before touching anything: F3 44 0F 59 1D disp32
			// (mulss xmm11, [rip+disp32]). If this build differs, do nothing.
			static const std::uint8_t expect[5] = { 0xF3, 0x44, 0x0F, 0x59, 0x1D };
			if (std::memcmp(instr, expect, sizeof(expect)) != 0)
			{
				Console::printf(
					"[demo] freecam speed: unexpected opcode at IDA_0x913D65, left stock "
					"(%02X %02X %02X %02X %02X)",
					instr[0], instr[1], instr[2], instr[3], instr[4]);
				return;
			}
			const auto rip = reinterpret_cast<std::uintptr_t>(instr) + 9;
			auto tgt = reinterpret_cast<std::uintptr_t>(g_freecam_speed);
			std::intptr_t delta = static_cast<std::intptr_t>(tgt) -
				static_cast<std::intptr_t>(rip);

			// Our DLL can land many GB from the game module (measured: ~10 GB),
			// and a rip-relative operand only reaches +/-2GB. Rather than give
			// up, allocate a page WITHIN range of the instruction and keep the
			// float there. Walk outwards from the instruction and take the
			// first free page VirtualAlloc will give us at that address.
			if (delta > INT32_MAX || delta < INT32_MIN)
			{
				constexpr std::uintptr_t STEP = 0x10000;          // allocation granularity
				constexpr std::uintptr_t REACH = 0x60000000ull;   // stay well inside 2GB
				float* near_slot = nullptr;
				for (std::uintptr_t off = STEP; off < REACH && !near_slot; off += STEP)
				{
					// Below the instruction first, then above.
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
					Console::printf(
						"[demo] freecam speed: our float is %lld bytes from the instruction "
						"and no page could be reserved within rip range -- left stock",
						static_cast<long long>(delta));
					return;
				}
				*near_slot = *g_freecam_speed;   // carry the current value over
				g_freecam_speed = near_slot;     // the GUI slider now edits this
				tgt = reinterpret_cast<std::uintptr_t>(near_slot);
				delta = static_cast<std::intptr_t>(tgt) - static_cast<std::intptr_t>(rip);
				Console::printf(
					"[demo] freecam speed: allocated a float %lld bytes from the "
					"instruction (our DLL was out of rip range)",
					static_cast<long long>(delta));
				if (delta > INT32_MAX || delta < INT32_MIN)
				{
					Console::printf("[demo] freecam speed: still out of range, left stock");
					return;
				}
			}
			DWORD old = 0;
			if (!VirtualProtect(instr, 9, PAGE_EXECUTE_READWRITE, &old))
			{
				Console::printf("[demo] freecam speed: VirtualProtect failed, left stock");
				return;
			}
			const std::int32_t d32 = static_cast<std::int32_t>(delta);
			std::memcpy(instr + 5, &d32, 4);
			VirtualProtect(instr, 9, old, &old);
			FlushInstructionCache(GetCurrentProcess(), instr, 9);
			g_freecam_patched = true;
			Console::printf(
				"[demo] freecam speed is now adjustable (stock %.0f) -- repointed one "
				"instruction at our own float, nothing else in the game is affected",
				*g_freecam_speed);
		}

		std::int64_t(*sub_91C710_orig)(unsigned int) = nullptr;
		std::int64_t sub_91C710_stub(unsigned int c)
		{
			// Report the first few calls whatever the context. This function runs on
			// LIVE connects (CL_InitCGame) as well as during demo prime, and the live
			// case is the one that matters.
			const bool report = (g_startrec_logs < 6);
			if (report)
			{
				++g_startrec_logs;
				report_record_gate(c, "ENTER");
			}
			const auto rr = sub_91C710_orig(c);

			if (report)
			{
				report_record_gate(c, "LEAVE");
			}
			return rr;
		}


		std::int64_t(*sub_70830_orig)(unsigned int, std::uintptr_t, std::uintptr_t) = nullptr;

		// The two calls that run when the gate is CLOSED, i.e. the path the demo is
		// actually taking. One of these is where it dies.
		std::int64_t(*sub_7BFD0_orig)(unsigned int) = nullptr;

		std::int64_t(*sub_85ED0_orig)(unsigned int) = nullptr;

		std::int64_t(*sub_91E310_orig)() = nullptr;

		std::int64_t(*sub_60A30_orig)(unsigned int) = nullptr;

		std::int64_t(*sub_4612E0_orig)(unsigned int) = nullptr;

		std::int64_t cl_parse_gamestate_stub(const unsigned int client, const std::int64_t msg)
		{
			auto* cs = connstate_ptr(static_cast<int>(client));
			const int before = cs ? *cs : -1;

			// ---- TWO INDEPENDENT DECISIONS, previously (wrongly) one ----------
			//
			// `scaffold` = everything the demo path needs regardless of who wrote
			// the gamestate: the qword_2537508 stand-in, the asset census, the
			// NetConstStrings reload. Gated on native playback ONLY.
			//
			// `onebit`   = compensation for S2's demo WRITER emitting one bit where
			// CL_ParseGamestate reads two. Applies only to demos the shipped
			// recorder produced.
			//
			// BUG THIS FIXES: both used to hang off `g_onebit_fix`, so turning the
			// one-bit toggle off ALSO disabled the qword_2537508 stand-in â€” and
			// CL_ParseGamestate then faulted at IDA 0x462DA3 reading *(NULL+16),
			// exactly as it did before that stand-in existed (crash dump
			// 2026-08-08, READ from 0x10). A toggle must never silently disable an
			// unrelated fix.
			const bool scaffold = g_native_playing;

			// AUTO-DETECT the framing rather than making the user toggle it.
			// Derived from the PROVEN msg_t layout (+40 = bitpos) and the proven
			// reader model:
			//   * gamestate is the FIRST record in its message (every shipped
			//     .demo): its 4-bit opcode came from byte0's LOW nibble, so bitpos
			//     is left at 4 â€” mid-byte. The writer bug applies.
			//   * gamestate is a LATER record (live capture, and our transcoded
			//     .demo where an svc 2 text command leads): the opcode came from a
			//     HIGH nibble, so bitpos landed on 8 â€” byte-aligned. Both bits are
			//     genuinely present and the fix must NOT run, or it would skip a
			//     bit that exists and desync the parse.
			bool first_record = true;
			const auto* bitpos = reinterpret_cast<const std::uint32_t*>(msg + 40);
			if (msg && readable(bitpos, 4))
			{
				first_record = ((*bitpos & 7u) != 0u);
			}

			const bool onebit = g_onebit_fix && g_native_playing
				&& MSG_ReadBit_orig != nullptr && first_record;
			const bool apply = scaffold;
			if (onebit)
			{
				g_gs_bit_calls = 0;
			}
			if (scaffold)
			{
				Console::printf(
					"[native] gamestate framing: bitpos&7=%u -> %s record in its message "
					"-> one-bit writer fix %s",
					msg && readable(bitpos, 4) ? (*bitpos & 7u) : 0u,
					first_record ? "FIRST" : "a LATER",
					onebit ? "APPLIED"
					: (first_record ? "disabled by demo_native_onebit"
					   : "NOT needed (both bits present, as in a live gamestate)"));
			}
			// ---- null-object for qword_2537508 ---------------------------------
			// PROVEN 2026-08-08 by bracketing: with the one-bit fix the gamestate
			// installs (dataCount=1168), then CL_ParseGamestate reaches
			//     if (!client) { v = sub_7AEA0(); sub_2A2600(*(QWORD*)(qword_2537508+16), v); }
			// qword_2537508 measured NULL during native playback, so *(NULL+16)
			// faults while EVALUATING the argument â€” which is why sub_2A2600's own
			// bracket never printed. CL_Demo_Play_f guards this SAME global
			// (`if (qword_2537508) ...`); CL_ParseGamestate does not, because on the
			// live path it is always populated.
			//
			// sub_2A2600 @ 0x2A2600 opens with `if (a1) { ... }`, so 0 is the
			// engine's own "absent" value and passing it is a complete no-op. We
			// therefore point the global at a zeroed block for the duration of the
			// call: the deref reads 0, sub_2A2600 returns immediately, and nothing
			// is fabricated. Restored afterwards so live play is untouched.
			// Install here, NOT in play(). CL_Demo_Play_f must run with the global at its
			// honest NULL: it READS this global itself (0x910892, 0x9108C0) and its level
			// load is heavily multithreaded. Installing before that made the game hang
			// intermittently inside DB_LoadLevelXAssets -- main thread blocked in ntdll
			// with IDA_0x910B34 on its stack, cls_realtime frozen (measured 2026-08-08).
			// From the gamestate onward the level is loaded and the load threads are done.
			if (apply)
			{
				install_absent_client_object();
			}
			// Heavy maps (dday, gibraltar) die on the image limit INSIDE this
			// call, before "Loading Map:" is printed. Bracketing the census here
			// is what makes airship (works) and dday (fails) comparable at the
			// same lifecycle point. The failure case is covered separately by the
			// DB_RaiseAssetLimitError hook, which dumps before the process exits.
			if (apply)
			{
			}

			g_in_gamestate = scaffold;
			const std::int64_t r = CL_ParseGamestate_orig(client, msg);
			g_in_gamestate = false;

			if (apply)
			{
			}

			if (apply)
			{
				// Report UNCONDITIONALLY. The first version of this only printed inside
				// the `if`, so when the gate was shut it printed nothing at all and the
				// run could not distinguish "already loaded" from "my code never ran".
				// Never gate a diagnostic on the condition it is meant to diagnose.
				const auto loaded = netconststrings_loaded();
				// Dump ALL 26 block lists, not just type 23. The live control (measured in
				// a real match 2026-08-08) is:
				//   populated: every type except 13 and 22  -> 24 types, exactly matching
				//              the 24 ncs_*_level assets sub_DEF90 loads
				//   type 23 = 129 entries
				// The demo footer carries only 0,1,2,3,4,6,8,12,14,16,20,21,24,25, so ten
				// types (5,7,9,10,11,15,17,18,19,23) are populated live and absent from
				// the demo. Comparing this dump against that list says whether the demo
				// path never loaded them, or loaded and then lost them.
				char line[512];
				int n = std::snprintf(line, sizeof(line),
					"[native] NCS blockLists (dword_5098948=%d):", loaded);
				for (int t = 0; t < 26 && n > 0 && n < static_cast<int>(sizeof(line)); ++t)
				{
					n += std::snprintf(line + n, sizeof(line) - n, " %d=%c",
						t, netconststrings_block_list(t) ? 'Y' : '.');
				}
				Console::printf("%s", line);
				Console::printf(
					"[native]   live control: all present except 13,22.  MISSING-vs-live "
					"means the demo path lost them (23 = 'OmnvarChanged', 129 entries live)");

				// PROVEN 2026-08-08: the Y set above is EXACTLY the 14 types the demo
				// footer carries. CL_Demo_ReadNetConstStringTable unloads every block list
				// and re-registers only those, so the 10 types the level provides
				// (5,7,9,10,11,15,17,18,19,23) are destroyed. dword_5098948 stays 1, so the
				// flag is STALE and must not be used as "everything is loaded" -- that
				// false reading is what made me discard this fix earlier.
				//
				// NetConstStrings_Load @0xDEF90 is the engine's own loader for the 24
				// ncs_*_level assets. Re-running it restores the missing tables. It opens
				// with `if (dword_5098948) sub_DFA70();`, so it cleans up before reloading.
				//
				// Address resolved HERE, inside the function: `base` is assigned in init(),
				// so a namespace-scope `= 0xDDF90_b` initialiser runs first and yields the
				// bare literal. A build that did that faulted with
				// "EXECUTE at 0xDDF90, OUTSIDE any module".
				bool missing = false;
				for (const int t : {5, 7, 9, 10, 11, 15, 17, 18, 19, 23})
				{
					if (!netconststrings_block_list(t)) { missing = true; break; }
				}
				if (missing)
				{
					Console::printf(
						"[native] tables the level provides are missing -- re-running "
						"NetConstStrings_Load @0xDEF90 (the demo install wiped them)");
					reinterpret_cast<void(*)()>(0xDDF90_b)();

					int m = std::snprintf(line, sizeof(line), "[native] after reload:");
					for (int t = 0; t < 26 && m > 0 && m < static_cast<int>(sizeof(line)); ++t)
					{
						m += std::snprintf(line + m, sizeof(line) - m, " %d=%c",
							t, netconststrings_block_list(t) ? 'Y' : '.');
					}
					Console::printf("%s", line);
				}
			}
			if (onebit)
			{
				g_gs_bit_calls = -1;
			}

			const int after = cs ? *cs : -1;
			++g_gamestates;
			Console::printf(
				"[native] CL_ParseGamestate #%lld: connstate %d -> %d (after %lld demo "
				"packets)%s",
				static_cast<long long>(g_gamestates), before, after,
				static_cast<long long>(g_reads),
				onebit ? "  [one-bit writer fix applied]" : "");
			return r;
		}

		// Shared by intercept_read / note_read_result below. There is deliberately
		// no Hook::create here â€” demo_playback.cpp already owns this hook.
		void log_first_call(const int before)
		{
			if (!g_first_call_logged)
			{
				g_first_call_logged = true;
				// Print BEFORE touching any engine pointer, so this line appears even
				// if every read below is rejected. Its absence means the hook never
				// ran at all, which is a different problem from a bad pointer.
				Console::printf("[native] demo read hook is live (connstate=%d)", before);
				const auto g = demo_playback_data();
				// playbackData+8 is what CL_Demo_IsCompleted returns. NOTHING in
				// CL_Demo_Play_f initialises it â€” the only clear found is inside
				// CL_Demo_ReadDemoMessage's mode==3 branch, which does not run when
				// mode is 0. So log it once: if it is already non-zero here, the
				// engine believes the demo is finished before it has read a byte.
				Console::printf(
					"[native] first demo read: connstate=%d completedFlag=%u mode=%d",
					before,
					g ? static_cast<unsigned>(*reinterpret_cast<std::uint8_t*>(g + 8)) : 0xFFu,
					g ? *reinterpret_cast<std::int32_t*>(g + 3336304) : -1);
			}

			if (before != g_last_connstate)
			{
				Console::printf("[native] connstate %d -> %d after %lld demo packets",
					g_last_connstate, before, static_cast<long long>(g_reads));
				g_last_connstate = before;
			}
		}

		// =====================================================================
		//  PROBE ONLY -- "4780" call site + the delta-index history
		// =====================================================================
		// publicmatch dies with Com_Error "4780" from MSG_ReadMonotonicDeltaIndex
		// @ IDA 0x66B9E0 ("the delta field/entity index failed to increase").
		// The Com_Error hook only sees the immediate caller, which is always that
		// shared helper, so it names nothing useful.
		//
		// Two additions, both read-only:
		//   1. a bounded scan of the stack for a return address inside one of the
		//      three delta readers, which says WHICH one was running;
		//   2. a ring of the last delta-index reads (bits, previous index, result)
		//      recorded by hooking the helper, dumped when the error fires.
		// `bits` is the index width, == count.bit_length() of the netfield list in
		// use (MW3's MSG_ReadDeltaFields computes it as 32 - CLZ(numFields)), so
		// the ring shows both the width and how the indices were progressing when
		// the stream desynced.
		//
		// Literal (RULE A1): 0x66B9E0 - 0x1000 = 0x66A9E0.
		// RULE A3.1 checked: nothing else in src/ hooks this target.
		struct delta_reader_range
		{
			std::uint64_t lo;
			std::uint64_t hi;
			const char* what;
		};

		// Ranges are start + size, both read from the IDB.
		constexpr delta_reader_range DELTA_READERS[] = {
			{ 0x4642C0, 0x4642C0 + 0x704, "sub_4642C0  entities        (own id 452)" },
			{ 0x463E90, 0x463E90 + 0x42E, "sub_463E90                  (own id 453)" },
			{ 0x463A20, 0x463A20 + 0x46F, "sub_463A20                  (own id 454)" },
			{ 0x463000, 0x463000 + 0x6AA, "sub_463000  archived path   (not expected)" },
			{ 0x464C00, 0x464C00 + 0x97C, "CL_ParseSnapshot" },
			// Sizes are read from the IDB, not guessed. The first version of this
			// used 0x3000 for CG_InterpolatePlayerState_S2, which overlapped the
			// helper at 0x66B9E0 and mislabelled Com_Error's own return address
			// as a playerstate frame. Real size is 0x1DFB, ending at 0x66A88B.
			{ 0x668A90, 0x668A90 + 0x1DFB, "CG_InterpolatePlayerState_S2 (playerstate)" },
			{ 0x66B9E0, 0x66B9E0 + 0x0B7, "MSG_ReadMonotonicDeltaIndex (the helper itself)" },
		};

		struct delta_sample
		{
			std::uint32_t bits;
			std::uint32_t last;     // msg+44 BEFORE the call
			std::uint32_t result;   // 0xFFFFFFFF if the call never returned
			// The msg cursor at entry, so a failing read can be located in the
			// decoded message and cross-checked offline with tools/s2_svc_walk.py.
			// +36 readcount (bytes), +40 bitpos. Proven from MSG_Init @0xDC750.
			std::uint32_t readcount;
			std::uint32_t bitpos;
		};

		constexpr int DELTA_RING = 48;
		delta_sample g_delta_ring[DELTA_RING]{};
		std::atomic<std::uint32_t> g_delta_pos{0};
		std::atomic<bool> g_delta_auto_dumped{false};
		// Set inside the hot stub, serviced from the Present hook path: printing
		// dozens of console lines from inside a per-field parse callback is a good
		// way to change the timing of the very thing being measured.
		std::atomic<bool> g_delta_want_dump{false};

		using MSG_ReadDeltaIndex_fn = std::int64_t(*)(std::int64_t, int);
		MSG_ReadDeltaIndex_fn MSG_ReadMonotonicDeltaIndex_orig = nullptr;


		void dump_delta_ring();

		void dump_delta_diagnostics()
		{
			// ---- who was running -------------------------------------------
			// A bounded, read-only stack scan. HEURISTIC, exactly like
			// tools/dump_stacks.py: stale slots survive, so treat the SET of
			// matches as evidence and their order as unreliable. It is reliable
			// enough here because we only look for six specific ranges.
			Console::printf("[native] 4780 call-site scan (heuristic, set not order):");
			auto** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());
			int hits = 0;
			for (int i = 0; i < 768 && hits < 12; ++i)
			{
				if (!readable(sp + i, sizeof(void*)))
				{
					break;
				}
				const auto ida = to_ida(sp[i]);
				for (const auto& r : DELTA_READERS)
				{
					if (ida >= r.lo && ida < r.hi)
					{
						Console::printf("[native]    IDA_0x%llX  %s",
							static_cast<unsigned long long>(ida), r.what);
						++hits;
						break;
					}
				}
			}
			if (!hits)
			{
				Console::printf("[native]    (no delta reader found on the stack)");
			}

			dump_delta_ring();
		}

		void dump_delta_ring()
		{
			// ---- how the indices were progressing --------------------------
			const auto total = g_delta_pos.load(std::memory_order_relaxed);
			if (!total)
			{
				Console::printf("[native] delta-index ring is EMPTY -- the helper was "
					"never called, so 4780 came from somewhere unexpected");
				return;
			}
			Console::printf("[native] last %d delta-index reads of %u total "
				"(bits = index width = netfield count.bit_length()):",
				total < DELTA_RING ? total : DELTA_RING, total);
			const auto shown = total < static_cast<std::uint32_t>(DELTA_RING)
				? total : static_cast<std::uint32_t>(DELTA_RING);
			for (std::uint32_t i = 0; i < shown; ++i)
			{
				const auto idx = (total - shown + i) % DELTA_RING;
				const auto& s = g_delta_ring[idx];
				char res[32];
				if (s.result == 0xFFFFFFFFu)
				{
					std::snprintf(res, sizeof(res), "DID NOT RETURN <<<");
				}
				else
				{
					std::snprintf(res, sizeof(res), "%d", static_cast<int>(s.result));
				}
				Console::printf("[native]    #%-4u bits=%-3u last=%-6d -> %-18s "
					"readcount=%-6u bitpos=%u (byte %u.%u)",
					total - shown + i, s.bits, static_cast<int>(s.last), res,
					s.readcount, s.bitpos, s.bitpos >> 3, s.bitpos & 7);
			}
		}

		void com_error_stub(const int code, const char* fmt, ...)
		{
			// Capture BEFORE calling the original â€” Com_Error does not return.
			const std::uint64_t ida = to_ida(_ReturnAddress());

			char msg[512]{};
			if (fmt)
			{
				va_list args;
				va_start(args, fmt);
				_vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
				va_end(args);
			}

			{
				std::lock_guard<std::mutex> lock(g_err_lock);
				if (g_errors.size() < MAX_ERRORS)
				{
					g_errors.push_back(error_record{code, msg, ida});
				}
			}

			// Printed immediately: Com_Error usually tears the session down, so a
			// deferred read of the vector may never happen.
			Console::printf("[comerr] code=%d caller=IDA_0x%llX msg=\"%s\"",
				code, static_cast<unsigned long long>(ida), msg);

			// The demo reader's errors all come out of CL_Demo_* (IDA 0x90F000 ->
			// 0x91F000). Dump the playback cursor for those: it names the exact
			// file offset the engine died on, which no post-mortem can recover.
			if (ida >= 0x90F000 && ida < 0x920000)
			{
				dump_playback_state("at Com_Error");
			}

			// "4780"/"4781" both come out of MSG_ReadMonotonicDeltaIndex, whose own
			// address tells us nothing. Report the caller and the index history.
			if (msg[0] == '4' && msg[1] == '7' && msg[2] == '8'
				&& (msg[3] == '0' || msg[3] == '1'))
			{
				dump_delta_diagnostics();
			}

			Com_Error_orig(code, "%s", msg);
		}
	}

	bool auto_record()
	{
		return g_force_record;
	}

	void set_auto_record(bool on)
	{
		g_force_record = on;
	}

	// sub_910440 (IDA 0x910440 - 0x1000 = 0x90F440):
	//     return clc && &clc[client] && clc[client].demoState == 1;
	// the exact gate sub_99960's per-message append chain checks, so "true" means
	// packets are being written to a .demo right now. RULE A17: returns bool.
	bool native_recording()
	{
		using fn = bool(__fastcall*)(int);
		return reinterpret_cast<fn>(_b(0x90F440))(0);
	}

	// CL_Demo_StopRecord (IDA 0x90FCA0 - 0x1000 = 0x90ECA0), decompiled 2026-09-15:
	// gated on connstate >= 5; reads demoState, ZEROES it, writes the type-0
	// terminator + footer + NetConstStrings table, closes the file and sets
	// demoFileHandle = 0. Once demoState is 0 the append gate above is false, so
	// no further packets are written. It is the teardown a disconnect runs.
	//
	// Called through the ENGINE address, not our trampoline, so the existing
	// cl_demo_stop_record_stub still runs the public-match repair and rename.
	// No return-address check in its body. RULE A17: returns char.
	bool stop_native_recording()
	{
		if (!native_recording())
		{
			return false;
		}
		using fn = char(__fastcall*)(int);
		reinterpret_cast<fn>(_b(0x90ECA0))(0);
		return !native_recording();
	}

	std::optional<std::filesystem::path> native_demos_directory()
	{
		// Our own demos live in <base>/demos; the ENGINE's live in <base>/main/demo.
		const auto ours = demo_utils::demos_directory();
		if (!ours)
		{
			return std::nullopt;
		}
		auto dir = ours->parent_path() / "main" / "demo";
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec))
		{
			return std::nullopt;
		}
		return dir;
	}

	void refresh()
	{
		g_files.clear();
		g_selected = 0;
		const auto dir = native_demos_directory();
		if (!dir)
		{
			return;
		}
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(*dir, ec))
		{
			if (e.is_regular_file() && e.path().extension() == ".demo")
			{
				g_files.push_back(e.path());
			}
		}
		std::sort(g_files.begin(), g_files.end());
	}

	const std::vector<std::filesystem::path>& files() { return g_files; }
	int selected() { return g_selected; }

	void set_selected(const int index)
	{
		if (index >= 0 && index < static_cast<int>(g_files.size()))
		{
			g_selected = index;
		}
	}

	bool server_running()
	{
		void* dvar = dvar_com_sv_running();
		if (!dvar)
		{
			return false;
		}
		return *(reinterpret_cast<unsigned char*>(dvar) + DVAR_VALUE_OFFSET) != 0;
	}

	bool play(const std::filesystem::path& path)
	{
		if (server_running())
		{
			Console::printf(
				"[native] refusing: a server is running. CL_Demo_Play_f is wrapped in "
				"!com_sv_running, so cl_demo_play would do nothing silently. "
				"Disconnect to the main menu first.");
			return false;
		}

		// Fresh counters per playback so the priming measurement is per-demo.
		g_reads = 0;
		g_prime_reads = 0;
		g_last_connstate = -1;
		g_eof_seen = false;
		g_runaway_reported = false;
		g_first_call_logged = false;
		g_repaired_reported = false;
		g_ent_calls = 0;
		g_native_playing = true;   // gates repair_gamestate_message; live play untouched

		// Where the PACKET STREAM ends, for the progress bar. The body runs from
		// the header size (79384, proven) to the start of the footer, and the
		// footer is located by the last 8 bytes: [u32 footerSize][u32 magic==29]
		// â€” exactly what CL_Demo_ReadFooter does with seek(-8, SEEK_END).
		// Falls back to the file size if that does not read cleanly; the footer
		// is only ~1-2% of a demo, so the bar stays honest either way.
		// Fresh keyframe ring per playback.
		g_last_keyframe_ms = 0;
		g_keyframes_made = 0;

		g_playing_body_end = 0;
		try
		{
			const auto sz = static_cast<std::int64_t>(std::filesystem::file_size(path));
			g_playing_body_end = sz;
			std::ifstream f(path, std::ios::binary);
			if (f && sz > 8)
			{
				f.seekg(sz - 8, std::ios::beg);
				std::uint32_t footer_size = 0, magic = 0;
				f.read(reinterpret_cast<char*>(&footer_size), 4);
				f.read(reinterpret_cast<char*>(&magic), 4);
				if (magic == 29 && footer_size > 0
					&& static_cast<std::int64_t>(footer_size) + 8 < sz)
				{
					g_playing_body_end = sz - 8 - static_cast<std::int64_t>(footer_size);
				}
			}
		}
		catch (const std::exception&)
		{
			g_playing_body_end = 0;
		}

		// The engine appends ".demo" and prefixes "demo/", so hand it the stem.
		const auto stem = path.stem().string();
		// Mark the allocation tally here so the census attributes registrations
		// to THIS playback rather than to boot plus the frontend.
		g_ss_gate_logged = false;
		g_ci_calls = 0;
		// Reconstruct the connect-time StreamSync request from the demo's own
		// later requests. Offline-proven: the gun waits for the first command
		// that adds to list 8, which is 40-68 s in.
		// MEASURED 2026-08-08 (publicmatch): the image pool was already 95% full
		// BEFORE CL_ParseGamestate, with only 1296 registrations attributable to
		// the demo. So the interesting number is the FRONTEND baseline, captured
		// here â€” and whether the demo's teardown frees any of it. If `live` does
		// not drop between this census and the next, nothing was unloaded.
		Console::printf("[native] cl_demo_play %s", stem.c_str());
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("cl_demo_play {}", stem));
		return true;
	}

	std::string repair_selected(bool& ok)
	{
		ok = false;
		const auto& list = files();
		const int sel = selected();
		if (sel < 0 || sel >= static_cast<int>(list.size()))
		{
			return "no demo selected";
		}
		return repair_demo_type21(list[static_cast<std::size_t>(sel)], ok);
	}

	bool play_selected()
	{
		if (g_files.empty() || g_selected < 0 || g_selected >= static_cast<int>(g_files.size()))
		{
			Console::printf("[native] no native demo selected (main/demo empty?)");
			return false;
		}
		return play(g_files[g_selected]);
	}

	float* freecam_speed() { return g_freecam_speed; }
	bool freecam_speed_patched() { return g_freecam_patched; }

	// Called from dolly's shared CL_Demo_FreeCameraMove stub, wrapped tightly
	// around the ORIGINAL call -- that is the only place that actually reads
	// *g_freecam_speed each frame. Shift = sprint, Alt = slow/precise, matching
	// the same modifier keys already used for other flying controls in this
	// project's history. Returns the base value unmodified so the caller can
	// restore it exactly, whether or not the patch is even live.
	float begin_speed_modifier()
	{
		const float base = *g_freecam_speed;
		if (!g_freecam_patched)
		{
			return base; // instruction was never repointed -- nothing to scale
		}
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
		{
			*g_freecam_speed = base * 4.0f;
		}
		else if (GetAsyncKeyState(VK_MENU) & 0x8000)
		{
			*g_freecam_speed = base * 0.25f;
		}
		return base;
	}

	void end_speed_modifier(const float base)
	{
		if (g_freecam_patched)
		{
			*g_freecam_speed = base;
		}
	}

	bool native_playing() { return g_native_playing; }

	void hold_connection_state_active(const bool on)
	{
		const bool was = g_hold_conn_active.exchange(on, std::memory_order_relaxed);
		if (was && !on)
		{
			// Report on RELEASE, once per rewind, with the count -- so a hold that is
			// never released (which would suppress the menu forever) is visible as a
			// missing line rather than as silence.
			Console::printf("[demo] released cg.Shared.connectionStateActive (held %d "
				"publish(es) across the rewind)",
				g_hold_conn_hits.exchange(0, std::memory_order_relaxed));
		}
	}

	bool connection_state_held()
	{
		return g_hold_conn_active.load(std::memory_order_relaxed);
	}

	// cl.snap.serverTime — the clock every seek in this file already uses, and
	// the one ProcessKeyFrameJump's tail copies across the whole clock. Exported
	// so the dolly can stamp its points on the SAME timebase the keyframe ring
	// uses, which is what makes seeking move the dolly camera correctly.
	int demo_time() { return current_demo_time(); }

	// ⭐ THE SMOOTH DEMO CLOCK — cl.serverTime, clientActive dword 6368.
	//
	// demo_time() above returns cl.snap.serverTime (dword 6345), which is a STEP
	// function: it changes only when a new snapshot is consumed, so anything that
	// samples it per frame sees the same value for a whole snapshot interval and
	// then a jump. That is correct for SEEKING — keyframe slot times are stored
	// from the same field (slot+8 == clientActive+25380 == dword 6345) — and
	// WRONG for anything that animates.
	//
	// CL_SetCGameTime recomputes this one every frame:
	//     cl.serverTime = cl.serverTimeDelta + cls_realtime;
	//     if (cl.serverTime < cl.oldFrameServerTime) cl.serverTime = cl.oldFrameServerTime;
	// so it interpolates continuously between snapshots and is what the engine's
	// own camera and view code run on.
	//
	// Using the snapshot clock for the dolly made the camera jump once per
	// snapshot instead of moving — exactly the defect CLAUDE.md records for view
	// angles, where writing one discrete sample per frame replaced the engine's
	// smooth interpolation. Same mistake, different subsystem.
	int demo_time_smooth()
	{
		const auto ca = local_client_active(LOCAL_CLIENT_0);
		return ca ? *reinterpret_cast<const std::int32_t*>(ca + 6368 * 4) : -1;
	}

	// CA_ACTIVE (10) is the only state in which cgame is fully up. It matters to
	// anything that reads cg from OUTSIDE the engine's own call graph: a seek
	// (ProcessKeyFrameJump) memsets the configstrings and reparses the whole
	// gamestate, and during that window CG_GetLocalClientGlobals returns NULL
	// while the renderer back-pointer demo_game::cg_globals_for derives from is
	// still stale-but-set. That disagreement crashed the game on 2026-08-11.
	bool cgame_active()
	{
		const auto* cs = connstate_ptr(LOCAL_CLIENT_0);
		return cs && *cs >= 10;
	}


	// 0 first person / 1 third person / 2 free. PROVEN: CL_Demo_SetCameraMode
	// writes it (`*(DWORD*)(playbackData + 6297228) = mode`) and
	// CG_IsTheaterOrbitCamera tests it == 2 — and mode 2 is the only mode in
	// which CG_PredictPlayerState routes to CL_Demo_FreeCameraMove at all.
	int camera_mode()
	{
		const auto g = demo_playback_data();
		if (!g || !readable(reinterpret_cast<const void*>(g + 6297228), 4))
		{
			return -1;
		}
		return *reinterpret_cast<const std::int32_t*>(g + 6297228);
	}

	// The engine's own setter, CL_Demo_SetCameraMode @ IDA 0x91AB50.
	//   0x91AB50 - 0x1000 = 0x919B50
	// It early-outs when the mode is unchanged, and on entering free camera it
	// seeds the freecam origin from the refdef view origin — so the camera does
	// not jump when you switch. Same call CL_Demo_HandleAction's F2 makes.
	//
	// Declared `double` to match the decompile exactly (RULE A17): the tail is
	// `return Dvar_SetInt(...)`, so the value lands in xmm0 and is meaningless
	// either way. We discard it.
	bool set_camera_mode(const int mode)
	{
		if (!g_native_playing || mode < 0 || mode > 2)
		{
			return false;
		}
		if (!demo_playback_data())
		{
			return false;
		}
		reinterpret_cast<double(__fastcall*)(int, int)>(_b(0x919B50))(LOCAL_CLIENT_0, mode);
		return camera_mode() == mode;
	}


	// =====================================================================
	//  THEATER TRANSPORT â€” drive the engine's OWN action handler
	// =====================================================================
	//
	// S2 ships a complete theater engine and an incomplete front-end. PROVEN
	// 2026-08-10:
	//   * the uiScript dispatcher sub_746A50 handles PlayDemo, PreviewSegment,
	//     MoveSegment, DeleteSegment, SwitchSegmentTransition, ... and PlayDemo
	//     (sub_91E110) is fully implemented â€” it reads the selected demo and
	//     issues `cl_demo_play <name>`;
	//   * every PLATFORM_DEMO_DVR_* / MENU_DEMO_DVR_* prompt resolves into
	//     CG_OwnerDraw, so the control HUD is real;
	//   * but the timeline materials demo_timeline_faded / _solid / _arrow /
	//     _bookmark and demo_play have NO references â€” nothing draws the scrub
	//     bar â€” and whatever LUI screen would call the uiScript commands is Lua
	//     inside the fastfiles.
	//
	// So we do not need to revive the menus: we drive CL_Demo_HandleAction
	// directly. These are the engine's own transport actions, not a
	// reimplementation. Action ids are from the switch in that function.
	// ---- CLIPS ---------------------------------------------------------
	// Decoded from CL_Demo_SaveSegment_f @0x911060 on 2026-08-31: the argument
	// is the whole interface -- non-zero records the current time as the
	// segment START, zero closes the segment and appends it. The segment COUNT
	// lives at playbackData + 6096400 (CL_Demo_DeleteClip_f zeroes exactly that
	// field, which is what identifies it).
	//
	// Issued as console commands so they run on the client thread and go
	// through the engine's own gate, rather than us reproducing its logic.
	int clip_count()
	{
		const auto g = demo_playback_data();
		if (!g)
		{
			return -1;
		}
		const auto* p = reinterpret_cast<const int*>(g + 6096400);
		if (!readable(p, sizeof(int)))
		{
			return -1;
		}
		const int n = *p;
		// A plausibility bound: a wild value means we are reading a torn-down
		// playback object, not that the user marked two billion clips.
		return (n >= 0 && n < 4096) ? n : -1;
	}

	void clip_mark_in()
	{
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "cl_demo_savesegment 1");
		Console::printf("[clip] mark IN at the current position");
	}

	void clip_mark_out()
	{
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "cl_demo_savesegment 0");
		Console::printf("[clip] mark OUT - segment saved");
	}

	void clip_preview()
	{
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "cl_demo_previewclip");
		Console::printf("[clip] previewing the marked clip");
	}

	void clip_clear()
	{
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "cl_demo_deleteclip");
		Console::printf("[clip] all segments cleared");
	}

	void transport(const int action)
	{
		if (!CL_Demo_HandleAction_orig)
		{
			Console::printf("[demo] transport: CL_Demo_HandleAction is not hooked");
			return;
		}
		// down = 1 then 0: the handler acts on key-down and several cases latch
		// on the release, exactly as a real button press delivers them.
		CL_Demo_HandleAction_orig(LOCAL_CLIENT_0, action, 1);
		CL_Demo_HandleAction_orig(LOCAL_CLIENT_0, action, 0);
	}

	// ---- pause and speed: do what the engine's own cases do -----------------
	//
	// These used to go through CL_Demo_HandleAction (cases 1 and 20/21). That
	// handler opens with
	//     if (!CL_IsDemoPlaying(c) || !PlaybackData || !sub_7DFD0(c)
	//         || CG_GetLocalClientGlobals(c)[22924]) return;
	// plus a second ladder (sub_91D920 / sub_91DA40 / sub_91D910 / connstate
	// != 10). Any one of those silently swallows the action, which is why the
	// GUI speed buttons appeared to do nothing and why space stopped pausing
	// once the GUI took the key from the engine's own binding.
	//
	// The cases themselves are two lines each, so perform them directly:
	//
	//   case 1/32:   Dvar_SetBool(dvar_cl_demo_pause, !cl_demo_pause);
	//                PlaybackData[36] = Sys_Milliseconds();
	//   case 20/21:  *(float*)(PlaybackData + 28) += ±0.1, clamped 0.1 .. 4.0
	//
	// PlaybackData+28 as the timescale is PROVEN — cases 20/21/23 are the only
	// writers and they apply exactly that step and clamp.
	//   dvar_cl_demo_pause  IDA 0x10F1AFE8 - 0x1000 = 0x10F19FE8
	//   Dvar_SetBool        IDA 0xB1FD0    - 0x1000 = 0xB0FD0
	void toggle_pause()
	{
		auto* slot = reinterpret_cast<std::uintptr_t*>(_b(0x10F19FE8));
		if (!readable(slot, sizeof(void*)) || !*slot)
		{
			transport(1);   // fall back to the engine's action
			return;
		}
		auto* val = reinterpret_cast<std::uint8_t*>(*slot + 16);   // dvar value at +16
		if (!readable(val, 1))
		{
			transport(1);
			return;
		}
		const auto set = reinterpret_cast<void(__fastcall*)(std::uintptr_t, bool)>(_b(0xB0FD0));
		set(*slot, *val == 0);
		if (const auto g = demo_playback_data())
		{
			auto* stamp = reinterpret_cast<std::int32_t*>(g + 36);
			if (readable(stamp, 4)) { *stamp = static_cast<std::int32_t>(GetTickCount()); }
		}
	}

	void set_timescale(float v)
	{
		const auto g = demo_playback_data();
		if (!g) { return; }
		auto* p = reinterpret_cast<float*>(g + 28);
		if (!readable(p, sizeof(float))) { return; }
		// 0.05, USER-MEASURED 2026-09-21. The old 0.1 was labelled "the engine's own
		// clamp" but never proven; lowering it showed the engine DOES accept less
		// than 0.1, and that playback goes visibly laggy below 0.05. So the floor is
		// now where the footage holds up, which is a different and better reason
		// than the one it had.
		// The read-back below catches an INSTANT rejection (a mirrored or
		// write-protected field). A clamp applied where the engine CONSUMES this
		// value would not show here -- only the picture would tell.
		if (v < 0.05f) { v = 0.05f; }
		if (v > 4.0f) { v = 4.0f; }     // the engine's own clamp
		*p = v;
		if (const float got = *p; got != v)
		{
			Console::printf("[demo] speed %.2fx: the engine held it at %.2fx", v, got);
		}
	}

	// ---- hide the broken game HUD ------------------------------------------
	//
	// During demo playback the MP HUD half-builds and strands widgets on screen
	// (the checkerboard division icon and the division names). The console log
	// names the exact path:
	//
	//   ui/s2/mphud_uc.lua:35
	//     ui/s2/equippedperks_uc.lua:10 -> :129 -> :80
	//       ui/utility/mp/divisions_utils.lua
	//   "Error while handling model subscription: Name unavailable in ship builds"
	//
	// The division model does not resolve during playback, the subscription
	// handler throws part-way through building the widget, and what has already
	// been laid out stays on screen. It is a LUI failure inside fastfile Lua, so
	// there is nothing to fix in native code.
	//
	// cg_draw2D is dvar "2562" ("Draw 2D screen elements") in S2's numeric dvar
	// scheme — the engine's own switch for the whole 2D HUD. Our ImGui overlay
	// is drawn from the DXGI Present hook, so it is unaffected.
	// ⛔ cg_draw2D IS THE WRONG LEVER — this is a CORRECTION.
	//
	// I first hid the stranded division widgets with cg_draw2D (dvar "2562").
	// That was lazy and wrong. Mapping every reader of its global
	// (off_8B0DA20, registered in sub_41D690) shows exactly three:
	//
	//   CG_DrawActiveFrame @0x69CF8   NOT a real read — Arxan integrity noise
	//                                 (XOR chains using off_8B0DA20+16 as an int)
	//   sub_E7980 @0xE79D4            if (globals[5729] || !Dvar_GetBool(cg_draw2D))
	//                                     return;
	//                                 gates a 0x108D-byte draw function — the whole
	//                                 native 2D HUD: hitmarkers, killfeed, score
	//   sub_EB030 @0xEB04D            same shape, gates another draw path
	//
	// So it is two blanket gates over the native HUD. Worse, the stranded
	// widgets are LUI, drawn by a different system, so it very likely did not
	// even remove them — it only deleted the HUD that was working.
	//
	// The real cause, from the console log:
	//   ui/s2/mphud_uc.lua -> equippedperks_uc.lua -> ui/utility/mp/divisions_utils.lua
	//   "Error while handling model subscription: Name unavailable in ship builds"
	// sub_36A550 is the LUI model-subscription dispatcher: it walks dirty models
	// and lua_pcalls each subscriber (sub_2E40A0). The divisions callback throws,
	// so the widget stops mid-build and whatever was laid out stays on screen.
	// The checkerboard is its unresolved division icon — a missing-material
	// placeholder, the same customization-streaming family as the viewmodel.
	//
	// TARGETED CANDIDATE: `divisionsGlobalOverhaul` is a plain-named bool dvar
	// (default 1) registered in Perks_RegisterDvars. Its dvar_t* at off_8A99838
	// has exactly ONE reference — the registration itself — so nothing native
	// reads it: it is consumed from Lua, by the very divisions code that throws.
	// Turning it off should send divisions_utils down its pre-overhaul path.
	//
	// ⚠ NOT PROVEN to fix it, and it is a gameplay-data flag, so it is an
	// explicit opt-in rather than something applied automatically.
	bool hud_minimal() { return g_hud_minimal; }

	void set_hud_minimal(const bool on)
	{
		g_hud_minimal = on;
		hud_mask_set(on ? HUD_MODELS_MINIMAL
			: std::vector<int>{ HUD_MODEL_CURRENT_DIVISION });
		Console::printf("[demo] minimal HUD: %s -- %zu LUI HUD model(s) withheld. "
			"Hitmarkers stay (native), killfeed and score popups stay (LUI but "
			"event-driven, not model-driven). demo_hud_suppress to tune.",
			on ? "ON" : "off",
			on ? HUD_MODELS_MINIMAL.size() : std::size_t{ 1 });

		// Say plainly when the toggle cannot do its job yet, instead of letting it
		// look broken. Withholding stops a model CHANGING; an element already
		// built from an earlier publish stays on screen until playback restarts.
		if (on && g_native_playing)
		{
			Console::printf("[demo] NOTE: playback is already running, so HUD elements "
				"built before now will REMAIN. Restart the demo for the full effect.");
		}
		else if (on)
		{
			Console::printf("[demo] armed before playback -- this is the case it works "
				"best in.");
		}
	}

	void suppress_division_model(const bool on)
	{
		g_hud_minimal = false;
		hud_mask_set(on ? std::vector<int>{ HUD_MODEL_CURRENT_DIVISION }
			: std::vector<int>{});
		Console::printf("[demo] LUI HUD model %d (cg.hud.currentDivision) is now %s "
			"during native playback. The native HUD is NOT affected.",
			HUD_MODEL_CURRENT_DIVISION, on ? "WITHHELD" : "published normally");
	}


	void cycle_camera()   { transport(4);  }   // case 4 / 168 -> 1st / 3rd / free

	// â›” NOT the engine's actions 18/19 any more.
	//
	// Those queue GetKeyFrameForJumpBack / SeekToNextKeyFrame, whose SELECTION
	// is the bug: sub_915A10 only scans backwards when the current keyframe is
	// newer than a fixed 800 ms cutoff or its index sits in the 32-entry
	// baseline list, and otherwise returns the current index â€” a jump to where
	// you already are, which is the "tiny nudge instead of a rewind" symptom.
	//
	// We pick the keyframe by TIME ourselves and drive the engine's own
	// ProcessKeyFrameJump, which is a complete seek (file reposition, gamestate
	// reparse, state block, message replay, clock resync).
	// ⛔ 2026-09-15: this used to queue `demo_seek prev`. `demo_seek` now belongs to
	// demo_player and means "relative SECONDS", so the native keyframe jump lives
	// under `demo_seek_kf`. Never queue a bare `demo_seek` from here again.
	void seek_back()    { GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_seek_kf prev"); }

	// =====================================================================
	//  FAST FORWARD — ported from IWXMVM (reallyluckyy/IWXMVM), 2026-08-11
	// =====================================================================
	//
	//  IWXMVM's whole fast-forward is one line:
	//
	//      void SkipDemoForward(int32_t ticks) { *cls.realtime += ticks; }
	//
	//  and it needs NO keyframes, which is exactly the gap S2 had: keyframes
	//  are written as playback passes, so seeking forward past where you have
	//  already been had nothing to seek to.
	//
	//  ⭐ THE MECHANISM IS PRESENT IN S2, VERBATIM. CL_SetCGameTime @0x86D30:
	//
	//      v7 = cl.serverTimeDelta;                       // clientActive[6370]
	//      cl.serverTime = v7 + cls_realtime;             // [6368]
	//      if (cl.serverTime < cl.oldFrameServerTime)     // [6365]
	//          cl.serverTime = cl.oldFrameServerTime;     //   <- CLAMPED
	//      cl.oldFrameServerTime = cl.serverTime;
	//      ...
	//      do {
	//          if (CL_Demo_IsCompleted()) break;
	//          if (cl.serverTime < cl.snap.serverTime          // [6345]
	//              && !CL_Demo_ShouldReadMultipleNonDeltaSnapshots(c)) break;
	//      } while (CL_Demo_ReadDemoMessage(c));
	//
	//  So cl.serverTime is DERIVED from cls_realtime, and the feed loop then
	//  reads demo packets until the demo has caught up. Raising cls_realtime by
	//  N ms therefore advances playback by N ms, through the engine's own pump.
	//
	//  ⚠ AND IT EXPLAINS WHY THIS ONLY GOES FORWARD. That clamp against
	//  oldFrameServerTime means LOWERING cls_realtime does nothing at all — the
	//  clamp pins serverTime at its old value. That is precisely why IWXMVM
	//  needs a full gamestate restore to rewind but one line to fast-forward,
	//  and why S2 still uses keyframes for the backward direction.
	//
	//  This loop is the NORMAL-play one at 0x87011, which re-checks
	//  CL_Demo_IsCompleted and the return value every iteration — NOT the
	//  priming loop at 0x86D97 that caused the runaway documented in CLAUDE.md.
	//  So it terminates correctly at end of stream.
	//
	//      cls_realtime  IDA 0x1C7E1F0 - 0x1000 = 0x1C7D1F0
	void skip_forward_ms(const int ms)
	{
		if (ms <= 0)
		{
			return;
		}
		// Backstop against a unit error upstream. A skip this large reads every
		// packet in one frame and leaves the clock hours ahead, and the FX system
		// then steps through that much time and never returns -- measured
		// 2026-09-15, a 287-million-ms skip spun a worker in the FX update
		// (IDA 0x51B502) for minutes. No genuine demo seek is this big.
		constexpr int MAX_SKIP_MS = 60 * 60 * 1000;
		if (ms > MAX_SKIP_MS)
		{
			Console::printf("[demo] skip: refusing a %d ms jump (over an hour) -- "
				"almost certainly a units bug in whatever asked for it", ms);
			return;
		}
		auto* rt = reinterpret_cast<std::int32_t*>(_b(0x1C7D1F0));
		if (!readable(rt, sizeof(std::int32_t)))
		{
			Console::printf("[demo] skip: cls_realtime not readable");
			return;
		}
		*rt += ms;
	}

	void seek_forward() { GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_skip 5000"); }

	// Arbitrary seek, both directions — IWXMVM's rewind-then-skip pattern with
	// S2's keyframes standing in for their gamestate restore:
	//   * forward  -> pure cls_realtime skip, no keyframe needed
	//   * backward -> keyframe jump to a point BEFORE the target, then skip
	//                 forward the remainder, which lands exactly on it
	//
	// ⛔ CORRECTED 2026-08-11. The comment above described the intent; the code
	// only ever did the first half. `demo_seek <ms>` picks the nearest usable
	// keyframe AT OR BEFORE the target and jumps to it, full stop — so a
	// backward seek landed on the keyframe, up to the keyframe cadence (~1 s at
	// best, a full snapshot interval at worst) earlier than asked.
	//
	// The remainder is issued here rather than inside demo_seek because the two
	// commands are queued on the SAME Cbuf and drain in order on the client
	// thread: the jump completes (it calls CL_SetCGameTime itself) before the
	// skip runs, which is exactly the ordering the skip needs.
	// Queues the absolute seek so it runs on the CLIENT thread. Safe to call
	// from the GUI. `demo_seek_kf`, never `demo_seek` (that is demo_player's
	// relative-seconds command -- see seek_back above).
	void seek_to_time(const int ms)
	{
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("demo_seek_kf {}", ms));
	}

	namespace
	{
		// Run CL_SetCGameTime once, NOW, even if the demo is paused -- the
		// engine's own way. sub_919CE0 (the engine's reset-and-replay) does
		// exactly this:
		//     *(BYTE*)(PlaybackData + 10) = 1;
		//     CL_SetCGameTime(client);
		//     *(BYTE*)(PlaybackData + 10) = 0;
		// because CL_Demo_IsPaused @0x916E30 is
		//     PlaybackData && cl_demo_pause && !PlaybackData[10]
		// so the flag makes the one call behave as unpaused.
		//
		// WHY IT IS NEEDED (PROVEN from the CL_SetCGameTime decompile @0x86D30):
		// while paused OR completed it does
		//     cls_realtime = cl.serverTime - cl.serverTimeDelta - 5; return;
		// before its feed loop. So a forward skip (cls_realtime += gap) made while
		// paused was thrown away on the next frame and no packet was read: a
		// paused native seek either did nothing (forward) or stopped on the
		// keyframe instead of the target (backward). Calling it here, once, reads
		// the packets up to the target and leaves the pause exactly as it was.
		//
		//   CL_SetCGameTime  IDA 0x86D30 - 0x1000 = 0x85D30  (demo_game::
		//   addr_CL_SetCGameTime, hooked by demo_playback -- its stub passes
		//   straight through when the custom theater is not playing). The engine
		//   itself calls it from command context in ProcessKeyFrameJump.
		bool pump_cgame_time_now()
		{
			const auto g = demo_playback_data();
			auto* force = g ? reinterpret_cast<std::uint8_t*>(g + 10) : nullptr;
			if (!force || !readable(force, 1))
			{
				return false;
			}
			const std::uint8_t was = *force;
			*force = 1;
			reinterpret_cast<void(__fastcall*)(unsigned int)>(
				demo_game::addr_CL_SetCGameTime())(static_cast<unsigned int>(LOCAL_CLIENT_0));
			*force = was;
			return true;
		}

		bool g_saw_demo_state = false;
	}

	// ONE absolute seek, the same contract as the custom theater's seek_to:
	// land on `target` and keep the current pause state.
	//   backward: jump to the nearest usable keyframe at or before the target
	//             (the earliest one if the target is before all of them), then
	//   forward:  skip the remainder through the engine's own feed, now.
	// CLIENT THREAD ONLY (it drives ProcessKeyFrameJump and CL_SetCGameTime).
	bool seek_absolute_now(int target)
	{
		if (!g_native_playing)
		{
			Console::printf("[demo] seek: no native demo playing");
			return false;
		}
		target = (std::max)(0, target);
		int now = demo_time_smooth();
		if (now < 0)
		{
			now = current_demo_time();
		}
		if (now < 0)
		{
			Console::printf("[demo] seek: demo clock not readable yet");
			return false;
		}
		const int from = now;

		// Snapshots are 50 ms apart, so anything closer than that is "here".
		if (target < now - 50)
		{
			const auto slots = usable_slots();
			if (slots.empty())
			{
				Console::printf("[demo] seek -> %d ms: nothing to rewind to yet -- let the "
					"demo play a few more seconds so the engine writes keyframes.", target);
				return false;
			}
			int pick = -1, pick_time = -1;
			for (const auto& s : slots)
			{
				if (s.time <= target && s.time > pick_time) { pick = s.index; pick_time = s.time; }
			}
			if (pick < 0)
			{
				for (const auto& s : slots)
				{
					if (pick_time < 0 || s.time < pick_time) { pick = s.index; pick_time = s.time; }
				}
				Console::printf("[demo] seek -> %d ms is before the earliest keyframe; "
					"landing on %d ms", target, pick_time);
				target = pick_time;
			}
			jump_to_slot(pick);
			// ProcessKeyFrameJump repositioned the file and cleared the engine's
			// completed flag (PlaybackData[8] = 0), so reading is legitimate again.
			g_eof_seen = false;
			now = demo_time_smooth();
			if (now < 0)
			{
				now = pick_time;
			}
		}

		const int gap = target - now;
		if (gap > 0)
		{
			skip_forward_ms(gap);
			pump_cgame_time_now();
		}
		Console::printf("[demo] seek %d -> %d ms (landed %d%s)", from, target,
			demo_time_smooth(), engine_paused() ? ", still paused" : "");
		return true;
	}

	// The native session ends when the ENGINE says so: clc.demoState leaves 2
	// (a disconnect zeroes it through CL_Demo_StopRecord). Polled once a frame
	// from the Present hook. It only ever ends a session it has seen running,
	// because demoState is still 0 for the frames between play() and
	// CL_Demo_Play_f actually executing.
	void poll_session()
	{
		if (!g_native_playing)
		{
			g_saw_demo_state = false;
			return;
		}
		const char* c = clc_native(LOCAL_CLIENT_0);
		const auto* st = c ? reinterpret_cast<const std::int32_t*>(c + 262752) : nullptr;
		if (!st || !readable(st, 4))
		{
			return;
		}
		if (*st == 2)
		{
			g_saw_demo_state = true;
			return;
		}
		if (g_saw_demo_state)
		{
			g_saw_demo_state = false;
			g_native_playing = false;
			g_eof_seen = false;
			Console::printf("[native] demo session ended");
		}
	}

	SeekRange seek_range()
	{
		SeekRange r;
		const auto g = demo_playback_data();
		if (!g)
		{
			return r;
		}
		const auto* rec = reinterpret_cast<const std::uint8_t*>(g + 2176584);
		if (!readable(rec, 250 * 48))
		{
			return r;
		}
		for (int i = 0; i < 250; ++i)
		{
			const auto* s = reinterpret_cast<const std::int32_t*>(
				rec + static_cast<std::size_t>(i) * 48);
			if (s[7] > 0 && s[2] >= 0)
			{
				++r.keyframes;
				if (r.first_ms < 0 || s[2] < r.first_ms) { r.first_ms = s[2]; }
				if (s[2] > r.last_ms) { r.last_ms = s[2]; }
			}
		}
		r.now_ms = current_demo_time();
		r.valid = (r.keyframes > 0);
		return r;
	}
	void speed_up()   { set_timescale(engine_timescale() + 0.1f); }
	void speed_down() { set_timescale(engine_timescale() - 0.1f); }

	// Playback progress, derived from the FILE cursor rather than any clock:
	// CL_Demo_Read accumulates bytes consumed into PlaybackData+24, the body
	// starts at the header size (79384) and ends at the footer. That gives a
	// genuine 0..1 without needing to model demo time.
	//
	// Returns -1 when it cannot be determined.
	float playback_progress()
	{
		if (!g_native_playing) { return -1.0f; }
		const auto g = demo_playback_data();
		if (!g) { return -1.0f; }
		const auto* pos = reinterpret_cast<const std::uint64_t*>(g + 24);
		if (!readable(pos, sizeof(std::uint64_t))) { return -1.0f; }
		const auto cur = static_cast<std::int64_t>(*pos);
		const std::int64_t body_start = 79384;                  // proven header size
		const std::int64_t body_end = g_playing_body_end;       // set when we opened it
		if (body_end <= body_start || cur < body_start) { return -1.0f; }
		const double f = static_cast<double>(cur - body_start)
			/ static_cast<double>(body_end - body_start);
		return static_cast<float>(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
	}

	// ---- the keyframe ring, read directly -------------------------------
	//
	// Layout proven from the two seek functions (they both scan it):
	//     ring  = PlaybackData + 2176592, stride 48, 250 slots
	//     slot+0 = dword time; a slot is EMPTY when that is < 0
	//     cur   = PlaybackData + 2188584   (the index the scan starts from)
	//
	// Both GetKeyFrameForJumpBack and GetKeyFrameForJumpForward return -1 when
	// nothing matches, and their callers then do nothing AT ALL -- silently.
	// So "seek does nothing" and "the ring is empty" look identical from the
	// outside, and this is how we tell them apart.
	KeyframeRing keyframe_ring()
	{
		KeyframeRing r;
		const auto g = demo_playback_data();
		if (!g)
		{
			return r;
		}
		const auto* cur = reinterpret_cast<const std::int32_t*>(g + 2188584);
		if (readable(cur, sizeof(std::int32_t)))
		{
			r.current_index = *cur;
		}
		const auto* base = reinterpret_cast<const std::int32_t*>(g + 2176592);
		if (!readable(base, 250 * 48))
		{
			return r;
		}
		r.valid = true;
		const auto* raw = reinterpret_cast<const std::uint8_t*>(base);
		for (int i = 0; i < 250; ++i)
		{
			const auto* slot = raw + static_cast<std::size_t>(i) * 48;
			const std::int32_t t = *reinterpret_cast<const std::int32_t*>(slot);
			// â­ THE ENGINE'S OWN VALIDITY TEST IS slot+20 > 0, NOT the time.
			// sub_915A10 (GetKeyFrameForJumpBack) steps back through the ring and
			// only considers a slot when
			//     *(int*)(ring + 48*i + 20) > 0
			// before it ever looks at the time. An earlier version of this reader
			// counted slots by `time >= 0`, which would report keyframes the
			// engine skips entirely â€” making a broken ring look healthy.
			const std::int32_t ok = *reinterpret_cast<const std::int32_t*>(slot + 20);
			if (t >= 0)
			{
				++r.timed;                       // has a plausible timestamp
				if (r.min_time < 0 || t < r.min_time) { r.min_time = t; }
				if (t > r.max_time) { r.max_time = t; }
			}
			if (ok > 0)
			{
				++r.used;                        // the engine will actually consider it
			}
		}
		return r;
	}


	bool engine_paused()
	{
		if (!g_native_playing)
		{
			return false;
		}
		// CL_Demo_IsPaused @ IDA 0x916E30 -> literal 0x916E30 - 0x1000 = 0x915E30.
		// Declared `bool()` to match exactly: it returns AL only, and widening a
		// narrow return is what made an earlier probe read register residue as true
		// (RULE A17). Resolved inside the function, never at namespace scope, because
		// `base` is assigned in init() (RULE A14).
		return reinterpret_cast<bool(*)()>(0x915E30_b)();
	}

	float engine_timescale()
	{
		if (!g_native_playing)
		{
			return 1.0f;
		}
		const auto g = demo_playback_data();
		if (!g || !readable(reinterpret_cast<const void*>(g + 28), 4))
		{
			return 1.0f;
		}
		const float v = *reinterpret_cast<const float*>(g + 28);
		// The field is only ASSUMED to be the timescale -- it is 1.0f at play and
		// reset to 1.0f at end-of-stream, which is the shape of a speed. So a wrong
		// guess must degrade to "normal speed", never to a broken HUD.
		if (!(v > 0.01f) || !(v < 100.0f))
		{
			return 1.0f;
		}
		// Report each change once, so a single run confirms this really is the value
		// the engine's up/down-arrow speed control drives.
		static float last = 1.0f;
		if (v < last - 0.001f || v > last + 0.001f)
		{
			last = v;
			Console::printf("[native] demo timescale now %.3fx (PlaybackData+28)", v);
		}
		return v;
	}

	// Called every rendered frame from the Present hook. One byte read in the
	// common case; everything else only runs when there is something to say.
	void watch_viewmodel()
	{
		if (!playback_active())
		{
			g_vm_last_hide = -2;
			g_vm_last_held = 0;
			g_vm_last_vm = nullptr;
			g_vm_t0 = 0;
			g_vm_last_beat = -100000;
			g_vm_reported_pop = false;
			g_str_last_res_pop = -1;
			g_str_last_req_pop = -1;
			g_str_pending_res = 0;
			g_str_req_set_changes = 0;
			g_str_last_emit = -100000;
			return;
		}

		auto* cs = connstate_ptr(0);
		if (!cs || *cs < 10)          // CA_ACTIVE; nothing to watch before then
		{
			return;
		}

		// FAST PATH -- diagnostics off. Everything below this block (bitmap scans,
		// per-frame readable() syscalls, the material/image walk) exists only to
		// measure, so with demo_vm_debug off we do the FIX and nothing else, at
		// 2 Hz rather than per frame.
		if (!g_vm_debug)
		{
			const int now_fast = demo_game::cls_realtime();
			if (now_fast - g_vm_fix_last < 500)
			{
				return;
			}
			g_vm_fix_last = now_fast;
			void* cg_fast = demo_game::cg_globals_for(0);
			if (!cg_fast || !readable(static_cast<char*>(cg_fast) + 2452523, 1))
			{
				return;
			}
			const int hide_fast = *reinterpret_cast<const unsigned char*>(
				static_cast<char*>(cg_fast) + 2452523);
			if (!hide_fast)
			{
				return;     // gun already visible; nothing to request
			}
			unsigned held_fast = 0; void* vm_fast = nullptr; const char* nm = "<none>";
			vm_describe_cheap(cg_fast, held_fast, vm_fast, nm);

			// CG_UpdateViewModel @0x5D0C0 has TWO early returns BEFORE it ever
			// calls CG_BuildViewmodelDObj, and neither has been measured:
			//     if (cg[2] >= 7u)                 -> LABEL_5, touches the DObj
			//                                         but never builds it
			//     if (*(DWORD*)(cg+104) & 0x3800)  -> return
			// If either fires the gun XModel can never be appended, no matter how
			// resident its images are. Bounded to 6 lines.
			static int gate_logs = 0;
			if (gate_logs < 6)
			{
				++gate_logs;
				const auto b2 = *reinterpret_cast<const unsigned char*>(
					static_cast<char*>(cg_fast) + 2);
				const auto f104 = *reinterpret_cast<const std::uint32_t*>(
					static_cast<char*>(cg_fast) + 104);
				Console::printf(
					"[vmgate] cg[2]=%u (>=7 skips the build) cg[104]&0x3800=0x%X "
					"(non-zero returns) held=%u hide=%d -> %s",
					static_cast<unsigned>(b2), f104 & 0x3800u, held_fast, hide_fast,
					(b2 >= 7u) ? "BLOCKED: cg[2] branch, viewmodel never built"
					: ((f104 & 0x3800u) ? "BLOCKED: cg+104 flags"
					: (held_fast == 0 ? "BLOCKED: held == 0"
					: "reaches CG_BuildViewmodelDObj")));
			}

			request_viewmodel_stream(vm_fast, hide_fast, now_fast);
			return;
		}
		// First ACTIVE frame: hand the engine the StreamSync requests the demo
		// recorded later. cgame is up by now, which is what CommitNewSyncData's
		// per-client work needs.
		replay_streamsync(0);

		void* cg = demo_game::cg_globals_for(0);
		if (!cg || !readable(static_cast<char*>(cg) + 2452523, 1))
		{
			return;
		}
		// cg+0x256C2B == 2452523. Sole writer is CG_UpdateViewModel's store at
		// IDA 0x5D40E.
		const int hide = *reinterpret_cast<const unsigned char*>(
			static_cast<char*>(cg) + 2452523);

		const int now = demo_game::cls_realtime();
		if (!g_vm_t0)
		{
			g_vm_t0 = now;
			Console::printf("[vmnative] watching cg+0x256C2B from CA_ACTIVE "
				"(t=0 is the first ACTIVE frame, not cl_demo_play)");
		}
		const int t = now - g_vm_t0;
		streamer_tick(t);

		// Sample the weapon and its viewmodel every frame too. A bipod deploy (or
		// any attachment change) can swap the XModel WITHOUT changing `hide`, and
		// the user saw the gun appear at exactly such a moment on gibraltar.
		unsigned held = 0; void* vm = nullptr; const char* name = "<none>";
		vm_describe_cheap(cg, held, vm, name);

		// THE FIX: ask the engine's own customization streamer for this model
		// while `hide` is set. Rate-limited internally; a no-op once hide == 0.
		request_viewmodel_stream(vm, hide, now);

		const bool hide_changed = (hide != g_vm_last_hide);
		const bool weapon_changed = (g_vm_last_hide != -2)
			&& (held != g_vm_last_held || vm != g_vm_last_vm);

		if (hide_changed || weapon_changed)
		{
			const bool pop = (g_vm_last_hide == 1 && hide == 0);
			const char* why = pop ? "HIDE 1->0 POP"
				: (hide_changed && weapon_changed) ? "hide+weapon"
				: hide_changed ? "hide changed" : "WEAPON/VM CHANGED";
			vm_log(why, t, hide, held, vm, name);
			// The decisive per-image state: requested-but-unserviced, or not
			// requested at all? Only on transitions, so it stays cheap.
			dump_vm_image_state(why, vm);
			if (weapon_changed)
			{
				Console::printf("[vmnative]    weapon %u -> %u, vm %p -> %p",
					g_vm_last_held, held, g_vm_last_vm, vm);
			}
			if (pop && !g_vm_reported_pop)
			{
				g_vm_reported_pop = true;
				Console::printf("[vmnative] ^^^ THE GUN APPEARS HERE: %d ms after "
					"the connection went ACTIVE%s", t,
					weapon_changed ? "  -- AND THE WEAPON/VM CHANGED IN THE SAME "
					"FRAME, so this is a model swap, not the streamer catching up"
					: "  -- with NO weapon change, so residency simply arrived");
			}
			g_vm_last_hide = hide;
			g_vm_last_held = held;
			g_vm_last_vm = vm;
			g_vm_last_beat = t;
			return;
		}

		// Heartbeat so a run that never pops still shows how long it waited.
		if (t - g_vm_last_beat >= 2000)
		{
			g_vm_last_beat = t;
			vm_log("heartbeat", t, hide, held, vm, name);
			// Only walk the image list when something is actually wrong; the walk
			// touches every material and image and is far too costly to run on a
			// 2 s heartbeat once the gun is already visible.
			if (hide)
			{
				dump_vm_image_state("heartbeat", vm);
			}
		}
	}

	bool repair_gamestate_message(void* msg_ptr)
	{
		// Live play must never be touched: only act while OUR play() has a native
		// demo in flight.
		if (!g_native_playing || !msg_ptr || !readable(msg_ptr, 56))
		{
			return false;
		}

		// msg_t offsets PROVEN from MSG_Init @ 0xDC750 and confirmed by the live
		// probe: +8 data, +28 cursize, +36 readcount, +52 useZlib.
		auto* m = reinterpret_cast<std::uint8_t*>(msg_ptr);
		auto* data = *reinterpret_cast<std::uint8_t**>(m + 8);
		auto& cursize = *reinterpret_cast<std::int32_t*>(m + 28);
		const std::int32_t rc = *reinterpret_cast<std::int32_t*>(m + 36);
		const std::int32_t use_zlib = *reinterpret_cast<std::int32_t*>(m + 52);

		if (use_zlib || !data || cursize <= 0 || rc < 0 || rc + 2 > cursize)
		{
			return false;   // zlib messages carry a correct prefix
		}
		if (!readable(data, static_cast<std::size_t>(cursize) + 2))
		{
			return false;
		}

		const std::int32_t body = cursize - rc;   // bytes after the sequence long
		if (body <= 2)
		{
			return false;
		}

		const auto declared = static_cast<std::int16_t>(
			static_cast<std::uint16_t>(data[rc]) |
			(static_cast<std::uint16_t>(data[rc + 1]) << 8));

		// THE DISCRIMINATOR, validated on every packet of all five shipped demos:
		// a Huffman message has a length prefix iff 0 < int16 <= body - 2.
		// Exactly one packet per demo fails it â€” always packet 0, msgSeq 1, the
		// gamestate. Note `<=`, not `==`: publicmatch pads packets to 1244 bytes,
		// so a shorter-than-payload length is legitimate and must NOT be repaired.
		if (declared > 0 && declared <= body - 2)
		{
			return false;   // well-formed, leave alone
		}

		// Insert the missing prefix so the engine's own parser reads it correctly:
		// shift the body up 2 bytes and write the true length. The engine's
		// scratch buffer is 0x20000 against a ~1.2-2.4 KB message, so there is
		// ample room for the 2 extra bytes.
		std::memmove(data + rc + 2, data + rc, static_cast<std::size_t>(body));
		data[rc] = static_cast<std::uint8_t>(body & 0xFF);
		data[rc + 1] = static_cast<std::uint8_t>((body >> 8) & 0xFF);
		cursize += 2;

		if (!g_repaired_reported)
		{
			g_repaired_reported = true;
			Console::printf(
				"[native] REPAIRED prefix-less gamestate message: declared=%d "
				"(bogus) -> inserted length=%d at +%d, cursize %d -> %d",
				static_cast<int>(declared), body, rc, cursize - 2, cursize);
		}
		return true;
	}

	bool intercept_read(const unsigned int local_client_num, int* result)
	{
		auto* cs = connstate_ptr(static_cast<int>(local_client_num));
		log_first_call(cs ? *cs : -1);

		// Suppress reads ONLY after the engine itself returned 0 (its own
		// end-of-stream signal). Deliberately NOT gated on CL_Demo_IsCompleted /
		// playbackData+8: that byte is never initialised on this path, and gating
		// on it regressed airshipdemo into a permanent loading screen (CLAUDE.md
		// RULE A7). This latch cannot be set by stale memory â€” only by a real 0
		// return recorded in note_read_result().
		if (g_eof_seen)
		{
			if (result)
			{
				*result = 0;
			}
			return true;
		}
		return false;
	}

	void note_read_result(const unsigned int local_client_num, const int result)
	{
		auto* cs = connstate_ptr(static_cast<int>(local_client_num));
		const int state = cs ? *cs : -1;
		++g_reads;

		// ---- keep the keyframe ring fed so seeking works --------------------
		// See the long note at g_gen_keyframes. The engine gates its own call on
		// isClipPlaying (clc+262772), which nothing ever sets, so during ordinary
		// playback no keyframes are cached and rewind/forward have nothing to
		// find. We call the engine's own writer on the same schedule instead.
		//
		// This runs on the CLIENT thread, inside the demo pump -- the exact place
		// the engine would have called it.
		if (g_gen_keyframes && g_native_playing && state >= 10)
		{
			// â­ USE THE ENGINE'S OWN POLICY, NOT A TIMER. This is a CORRECTION.
			//
			// The first version generated every 5 s. The ring filled with
			// genuinely valid keyframes (measured: length ~115 KB each, times
			// 5 s apart, all USABLE) â€” and rewind STILL did not work.
			//
			// Why: sub_915A10 (GetKeyFrameForJumpBack) only scans backwards when
			// the CURRENT keyframe is newer than the cutoff, or when its index
			// appears in a 32-entry list at PlaybackData+2188596. Otherwise it
			// returns the current index unchanged â€” "jump to where you already
			// are", which is the tiny nudge instead of a rewind.
			//
			// And sub_914790 only registers an index in that list when
			//     (clientActive[25376] & 8) != 0
			// i.e. on a FULL (non-delta) snapshot â€” the only kind that can serve
			// as a jump baseline. A blind timer almost never coincides with one,
			// so nothing was ever registered.
			//
			// sub_91A3A0 IS the engine's ShouldGenerateKeyFrame and it already
			// encodes the right policy:
			//     ((clientActive[25376] & 8) && now > last)   // full snapshot
			//  || last == -1                                  // first ever
			//  || now - last >= 1000 * cl_demo_keyframerate   // periodic
			// It reads normal demo time when not clip-playing, so it is correct
			// to call here. Only the engine's OUTER isClipPlaying gate is what
			// we are bypassing.
			//   sub_91A3A0  IDA 0x91A3A0 - 0x1000 = 0x9193A0
			const auto should = reinterpret_cast<bool(*)(unsigned int)>(0x9193A0_b);
			const DWORD now = GetTickCount();
			// Belt-and-braces rate limit: sub_91A3A0's periodic branch compares
			// against PlaybackData+16 (last keyframe time). If that field is not
			// maintained on this path it would return true every call, and each
			// keyframe costs ~115 KB â€” so never write them faster than 1 Hz.
			const bool not_too_soon =
				(g_last_keyframe_ms == 0) || ((now - g_last_keyframe_ms) >= 1000);
			if (not_too_soon && should(local_client_num))
			{
				g_last_keyframe_ms = now;
				const auto gen = reinterpret_cast<void(*)(unsigned int, int, int)>(0x913790_b);
				gen(local_client_num, 0, 0);
				++g_keyframes_made;
				if (g_keyframes_made <= 3)
				{
					Console::printf("[demo] keyframe %d written on the engine's own "
						"ShouldGenerateKeyFrame (full-snapshot or rate). The engine skips "
						"this entirely outside clip playback, which is why seeking had "
						"nothing usable to jump to.", g_keyframes_made);
				}
			}
		}

		// Service the one-shot healthy delta-index sample here, on the demo pump
		// path, rather than from inside the per-field parse callback that set it.
		if (g_delta_want_dump.exchange(false, std::memory_order_relaxed))
		{
			Console::printf("[native] delta-index HEALTHY sample (a working demo's "
				"first %d entity index reads) -- compare against the 4780 dump:",
				DELTA_RING);
			dump_delta_ring();
		}

		// THE SEQUENCE GATE. CL_Demo_ProcessPacket_Type2_3 @ 0x919629:
		//     mov ecx,[rsi+134h] / lea eax,[rcx-80h] / cmp [rsi+138h],eax / jge
		// i.e. parse only if messageSequence >= clc[0x134] - 128 (SIGNED).
		// Every demo's first packet has messageSequence == 1, so any stale
		// clc+0x134 above 129 silently DROPS it â€” bytes consumed, no error, and
		// that packet is the gamestate. Log both fields for the first few reads.
		if (g_reads <= 6)
		{
			if (auto* slot = clc_native(static_cast<int>(local_client_num)))
			{
				Console::printf(
					"[native] read #%lld: clc+0x134=%d clc+0x138=%d clc+0x2013C=%d "
					"(gate: parse if 0x138 >= 0x134-128)",
					static_cast<long long>(g_reads),
					*reinterpret_cast<std::int32_t*>(slot + 0x134),
					*reinterpret_cast<std::int32_t*>(slot + 0x138),
					*reinterpret_cast<std::int32_t*>(slot + 0x2013C));
			}
		}

		if (state >= 5 && state < 9)
		{
			++g_prime_reads;
			// A healthy demo primes in a handful of packets. Warn long before the
			// whole file is gone.
			if (g_prime_reads == 2000)
			{
				Console::printf(
					"[native] WARNING: %lld packets consumed and connstate is still %d "
					"(<9). This is the priming runaway; every snapshot parsed here "
					"registers assets, which is the suspected image-pool exhaustion.",
					static_cast<long long>(g_prime_reads), state);
			}
		}

		if (result == 0)
		{
			// The engine hit the type-0 terminator: it set the completed flag and
			// returned 0. CL_SetCGameTime's priming loop (0x86D97) discards both,
			// so without this the NEXT call reads one byte past the terminator,
			// lands on the footer's version dword (0x1D) and Com_Errors.
			g_eof_seen = true;
			// ⛔ 2026-09-15: this used to also clear g_native_playing. That ended
			// the SESSION at end-of-stream while the demo was still loaded and
			// frozen -- so from then on demo_skip / demo_seek_kf refused ("no
			// native demo playing"), demo_player lost the transport, and Stop
			// could not even disconnect. Rewinding from the end was impossible.
			// The session now lives until the engine's demoState leaves 2
			// (poll_session), and a seek clears g_eof_seen after the jump.
			remove_absent_client_object();

			if (cs && state >= 5 && state < 9 && !g_runaway_reported)
			{
				g_runaway_reported = true;
				Console::printf(
					"[native] ABORT: demo ended while still priming (connstate=%d) "
					"after %lld packets (%lld during priming). The stream never "
					"advanced the connection to PRIMED(9), so the priming loop "
					"consumed the whole file.",
					state, static_cast<long long>(g_reads),
					static_cast<long long>(g_prime_reads));
				dump_playback_state("at runaway");
				dump_opcode_histogram();
				*cs = 0; // CA_DISCONNECTED â€” breaks `for (; connstate >= 5;)`
			}
		}
	}

	const std::vector<error_record>& errors() { return g_errors; }

	void clear_errors()
	{
		std::lock_guard<std::mutex> lock(g_err_lock);
		g_errors.clear();
	}

	void init()
	{
		refresh();

		Hook::create("Com_Error", reinterpret_cast<void*>(addr_Com_Error()),
			reinterpret_cast<void*>(com_error_stub),
			reinterpret_cast<void**>(&Com_Error_orig));

		// DB_LoadLevelXAssets @ IDA 0xA4840 - 0x1000 = 0xA3840. Hooked for BOTH
		// live map loads and demo playback (CL_Demo_Play_f and the ordinary
		// map-load path sub_48C0C0 use the identical five-call idiom), so this
		// gives the live-vs-demo A/B for free. RULE A3.1: demo_playback.cpp has a
		// map_zone_load_orchestrator_stub for this address but NEVER installs it
		// â€” no Hook::create references it â€” so there is no duplicate here.
		const bool level_hooked = Hook::create("DB_LoadLevelXAssets",
			reinterpret_cast<void*>(0xA3840_b),
			reinterpret_cast<void*>(db_load_level_xassets_stub),
			reinterpret_cast<void**>(&DB_LoadLevelXAssets_orig));
		Console::printf("[assets] DB_LoadLevelXAssets hook: %s (orig=%p)",
			(level_hooked && DB_LoadLevelXAssets_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(DB_LoadLevelXAssets_orig));

		// CL_Demo_Play_f @ IDA 0x910650 - 0x1000 = 0x90F650. Hooked purely so the
		// frontend release lands BEFORE the loading screen restarts LUI; doing it
		// later frees luafiles LUI has already bound and crashes the Lua VM.
		const bool play_hooked = Hook::create("CL_Demo_Play_f",
			reinterpret_cast<void*>(0x90F650_b),
			reinterpret_cast<void*>(cl_demo_play_f_stub),
			reinterpret_cast<void**>(&CL_Demo_Play_f_orig));
		Console::printf("[assets] CL_Demo_Play_f hook: %s (orig=%p)",
			(play_hooked && CL_Demo_Play_f_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(CL_Demo_Play_f_orig));


		// THE VIEWMODEL FIX. One code caller, so this is surgical; the real global
		// is left alone and the server-driven StreamSync path is unaffected.
		const bool ss_hooked = Hook::create("CL_StreamSync_IsCustomizationEnabled",
			reinterpret_cast<void*>(0x192630_b),   // IDA 0x193630 - 0x1000
			reinterpret_cast<void*>(streamsync_gate_stub),
			reinterpret_cast<void**>(&CL_StreamSync_IsCustomizationEnabled_orig));
		const bool wm_hooked = Hook::create("BG_GetWorldModel",
			reinterpret_cast<void*>(0x3BCD30_b),   // IDA 0x3BDD30
			reinterpret_cast<void*>(bg_get_world_model_stub),
			reinterpret_cast<void**>(&BG_GetWorldModel_orig));
		Console::printf("[vmreq] BG_GetWorldModel hook (remote player weapons): %s (orig=%p)",
			(wm_hooked && BG_GetWorldModel_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(BG_GetWorldModel_orig));

		const bool ssp_hooked = Hook::create("CL_StreamSync_ParseServerLoadRequest",
			reinterpret_cast<void*>(0x192B20_b),   // IDA 0x193B20 - 0x1000
			reinterpret_cast<void*>(streamsync_parse_stub),
			reinterpret_cast<void**>(&CL_StreamSync_ParseServerLoadRequest_orig));
		const bool ssc_hooked = Hook::create("CL_StreamSync_DataList_CommitNewSyncData",
			reinterpret_cast<void*>(0x192100_b),   // IDA 0x193100 - 0x1000
			reinterpret_cast<void*>(streamsync_commit_stub),
			reinterpret_cast<void**>(&CL_StreamSync_DataList_CommitNewSyncData_orig));
		Console::printf("[ss] StreamSync trace: parse=%s (orig=%p) commit=%s (orig=%p)",
			(ssp_hooked && CL_StreamSync_ParseServerLoadRequest_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(CL_StreamSync_ParseServerLoadRequest_orig),
			(ssc_hooked && CL_StreamSync_DataList_CommitNewSyncData_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(CL_StreamSync_DataList_CommitNewSyncData_orig));

		const bool ci_hooked = Hook::create("sub_439EA0 (clientinfo parse)",
			reinterpret_cast<void*>(0x438EA0_b),   // IDA 0x439EA0 - 0x1000
			reinterpret_cast<void*>(client_info_parse_stub),
			reinterpret_cast<void**>(&sub_439EA0_orig));
		Console::printf(
			"[vmfix] StreamSync local-package fix: gate=%s (orig=%p) clientinfo=%s (orig=%p)",
			(ss_hooked && CL_StreamSync_IsCustomizationEnabled_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(CL_StreamSync_IsCustomizationEnabled_orig),
			(ci_hooked && sub_439EA0_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(sub_439EA0_orig));

		// EXPERIMENT: `demo_skip_msgs 3` then `cl_demo_play publicmatch`.
		// 3 lands playback on demo packet 8 (msgNum=14), the first snapshot that is
		// NOT part of the retransmit group. 0 disables.
		dev_mode::add_command("demo_skip_msgs", []
		{
			const char* arg = command_tag();
			g_skip_msgs = arg ? std::atoi(arg) : 0;
			if (g_skip_msgs < 0) g_skip_msgs = 0;
			Console::printf("[native] will skip the first %d svc message(s) after the "
				"gamestate on the next cl_demo_play%s", g_skip_msgs,
				g_skip_msgs == 3 ? "  (3 = the publicmatch retransmit group -> lands on msgNum=14)"
				                 : "");
		});


		dev_mode::add_command("demo_worldmodel_request", []
		{
			g_world_request_enabled = !g_world_request_enabled;
			Console::printf("[vmreq] remote-player world model requests: %s (%d issued, %d distinct models)",
				g_world_request_enabled ? "ON" : "OFF", g_world_requests, g_world_seen_n);
		});

		// Which LUI HUD models to withhold during native playback.
		//   demo_hud_suppress            -> report the current list
		//   demo_hud_suppress 86         -> suppress only cg.hud.currentDivision
		//   demo_hud_suppress 86 49 50   -> division + two perk models
		//   demo_hud_suppress none       -> suppress nothing (stock behaviour)
		// Runtime-settable so one build can test several ids; see the comment on
		// cg_publish_hud_model_stub for why the id list is the uncertain part.
		dev_mode::add_command("demo_hud_suppress", []
		{
			const auto* args = GameUtil::getCmdArgs();
			const int argc = args ? args->argc[args->nesting] : 0;
			if (args && argc > 1)
			{
				std::vector<int> next;
				bool clear = false;
				for (int i = 1; i < argc && next.size() < 16; ++i)
				{
					const char* a = args->argv[args->nesting][i];
					if (!a || !*a)
					{
						continue;
					}
					if (_stricmp(a, "none") == 0 || _stricmp(a, "off") == 0)
					{
						clear = true;
						break;
					}
					const int id = std::atoi(a);
					if (id > 0)
					{
						next.push_back(id);
					}
				}
				hud_mask_set(clear ? std::vector<int>{} : next);
			}

			const std::string list = hud_mask_list(g_hud_mask);
			Console::printf("[demo] suppressed LUI HUD models: %s  (%d withheld so far). "
				"86 = cg.hud.currentDivision, 49-57 = the perk models. "
				"'demo_hud_suppress none' restores stock behaviour.",
				list.empty() ? "<none>" : list.c_str(),
				g_hud_suppressed_hits.load(std::memory_order_relaxed));
		});

		// Dump every model id -> name the engine publishes, once, so an id can be
		// picked from evidence instead of guessed.
		// The requested mode: keep hitmarkers + killfeed + score popups, drop the rest.
		// ⭐ demo_hud_only -- "keep ONLY obituaries, score popups and hitmarkers".
		//
		//   demo_hud_only              suppress every model-driven LUI HUD element
		//   demo_hud_only 79 80        ...except ids 79 and 80 (the scores)
		//   demo_hud_only off          stock behaviour, nothing suppressed
		//
		// ⚠ WHY NOT cg_draw2D, which is the obvious-looking lever: that dvar gates
		// the ENTIRE native 2D HUD in one branch --
		//     sub_E7980 @0xE79D4:  if (globals[5729] || !Dvar_GetBool(cg_draw2D)) return;
		// and that single function is 4237 bytes with 61 callees, drawing the
		// hitmarkers among everything else. There is no partial setting: turning it
		// off takes the hitmarkers, the killfeed and the score popups with it, which
		// is exactly what was observed the last time it was tried. It is the wrong
		// lever for this, and PROVEN so -- see CLAUDE.md.
		// =====================================================================
		//  ⭐⭐ REGISTER THE HUD-HIDE DVARS THE SHIPPED LUI ALREADY READS
		//
		//  From the dumped LUI bytecode (941 HKS files, ui/ + clientscript/),
		//  ui/s2/mphud_uc.lua and ui/s2/raidhud_uc.lua read these BY NAME:
		//      ui_hide_hud   ui_hide_minimap   ui_hide_hints_hud   ui_hide_1v1scores
		//  through the LUI binding GetDvarBool (sub_10AE90), which is:
		//      v5 = Dvar_FindVar(name);            // sub_AF9D0
		//      if (v5) push(value) else push(false);
		//
		//  ⭐ AND NONE OF THOSE NAMES EXISTS IN THE EXE -- checked .rdata, all six
		//  absent. So nothing registers them: the Lua reads them DEFENSIVELY and
		//  gets false because Dvar_FindVar fails. They are not hashed and not
		//  numeric; they simply do not exist yet. That is why typing them in the
		//  console did nothing -- there was no dvar to set.
		//
		//  So we register them. The shipped HUD Lua then picks them up on its own,
		//  through its own visibility system (MPHudVisibility : HudVisibilityBase,
		//  GetWidgetsToToggle, per-widget setAlpha). Nothing is hooked, nothing is
		//  patched, and the game hides its own widgets the way it already knows how.
		//
		//  ⚠ NOT YET TESTED. What is PROVEN: the Lua reads these names, the lookup
		//  is by string, and the names are absent from the binary. What is NOT
		//  proven is which widgets each one covers -- that is what the test shows.
		// =====================================================================
		//  ⭐⭐⭐ OMNVARS — the actual mechanism the HUD Lua uses
		//
		//  ⛔ THE DVAR ATTEMPT BELOW WAS WRONG. ui_hide_hud / ui_hide_minimap /
		//  ui_hide_hints_hud / ui_hide_1v1scores are NOT dvars. The dumped LUI
		//  bytecode shows them going to GetOmnvar / registerOmnvarHandler, while
		//  the DVAR handlers next to them use NUMERIC names (2454, 2562 = cg_draw2D,
		//  2523) exactly as S2 dvars always do:
		//
		//      #295  GetLuiRoot / Game / GetOmnvar / ui_hide_hud / GetDvarBool / 2562
		//      #600  registerOmnvarHandler / ui_session_state / ui_hide_hud /
		//            ui_killstreak_remote / registerDvarHandler / 2454 / 2562 / 2523
		//
		//  Omnvars are SERVER-REPLICATED variables; their names live in the
		//  NetConstStrings TYPE 23 table (129 entries live), not in the exe -- which
		//  is why none of those names appears in .rdata and why registering dvars
		//  with those names changed nothing.
		//
		//  The engine's own SetOmnvar (LUI binding, sub_185250) is:
		//      idx  = OmnvarIndexFromName(name)     sub_768FC0, -1 == not registered
		//      slot = OmnvarSlot(client, idx)       sub_768D40
		//      def  = OmnvarDef(idx)                sub_768F80
		//      if (def[8] & 4) {                    <- must be CLIENT-SCOPE
		//          switch (def[12]) {               <- type
		//            1 bool  : *(BYTE *)(slot+4) = v
		//            2 float : *(float*)(slot+4) = v
		//            3/4 int : *(DWORD*)(slot+4) = v - def[32]     (biased)
		//            5 string: sub_7698A0(def, slot, s)
		//          }
		//          OmnvarNotifyChanged(slot)        sub_769520  <- fires LUI handlers
		//      } else "'%s' is not a client-scope Omnvar"
		//
		//  RULE A1: 0x768FC0-0x1000=0x767FC0  0x768D40->0x767D40
		//           0x768F80->0x767F80        0x769520->0x768520
		// =====================================================================
		{
			using OmnvarIndex_t  = unsigned int(__fastcall*)(const char*);
			using OmnvarSlot_t   = std::int64_t(__fastcall*)(int, unsigned int);
			using OmnvarDef_t    = std::int64_t(__fastcall*)(unsigned int);
			using OmnvarNotify_t = void(__fastcall*)(std::int64_t);

			dev_mode::add_command("omnvar", []
			{
				const auto idx_of = reinterpret_cast<OmnvarIndex_t>(_b(0x767FC0));
				const auto slot_of = reinterpret_cast<OmnvarSlot_t>(_b(0x767D40));
				const auto def_of = reinterpret_cast<OmnvarDef_t>(_b(0x767F80));
				const auto notify = reinterpret_cast<OmnvarNotify_t>(_b(0x768520));

				const auto* args = GameUtil::getCmdArgs();
				const int argc = args ? args->argc[args->nesting] : 0;
				if (argc < 2)
				{
					Console::printf("[omni] usage: omnvar <name> [value]   e.g. "
						"'omnvar ui_hide_hud 1'.  These are what the SHIPPED HUD Lua "
						"reads -- ui_hide_hud, ui_hide_minimap, ui_hide_hints_hud, "
						"ui_hide_1v1scores. Omnvars, not dvars.");
					return;
				}
				const char* name = args->argv[args->nesting][1];
				const unsigned int idx = idx_of(name);
				if (idx == 0xFFFFFFFFu)
				{
					Console::printf("[omni] '%s' is not a registered omnvar. The name table "
						"is NetConstStrings type 23 and only exists once a map/gamestate has "
						"loaded -- try again in a match or during demo playback.", name);
					return;
				}
				const std::int64_t def = def_of(idx);
				const std::int64_t slot = slot_of(0, idx);
				if (!readable(reinterpret_cast<void*>(def), 36)
					|| !readable(reinterpret_cast<void*>(slot), 8))
				{
					Console::printf("[omni] '%s' idx=%u but its storage is not readable yet.",
						name, idx);
					return;
				}
				const int flags = *reinterpret_cast<const int*>(def + 8);
				const int type = *reinterpret_cast<const int*>(def + 12);
				if (argc < 3)
				{
					Console::printf("[omni] %s : idx=%u type=%d clientScope=%s  current(raw)=%d",
						name, idx, type, (flags & 4) ? "yes" : "NO",
						*reinterpret_cast<const int*>(slot + 4));
					return;
				}
				if ((flags & 4) == 0)
				{
					Console::printf("[omni] '%s' is not a client-scope omnvar (flags=0x%X) -- "
						"the engine refuses to set it locally, same as its own SetOmnvar does.",
						name, flags);
					return;
				}
				const int v = GameUtil::safeStringToInt(args->argv[args->nesting][2]);
				switch (type)
				{
				case 1:
					*reinterpret_cast<unsigned char*>(slot + 4) = v ? 1u : 0u;
					break;
				case 2:
					*reinterpret_cast<float*>(slot + 4) = static_cast<float>(v);
					break;
				case 3:
				case 4:
					*reinterpret_cast<int*>(slot + 4) = v - *reinterpret_cast<const int*>(def + 32);
					break;
				default:
					Console::printf("[omni] '%s' has type %d, which this command does not "
						"write (only bool/float/int).", name, type);
					return;
				}
				notify(slot);          // fires the LUI omnvar handlers
				Console::printf("[omni] %s = %d  (idx=%u type=%d) -- notified LUI.",
					name, v, idx, type);
			});
			Console::printf("[omni] `omnvar <name> [value]` ready. The HUD-hide switches are "
				"OMNVARS, not dvars: ui_hide_hud, ui_hide_minimap, ui_hide_hints_hud, "
				"ui_hide_1v1scores.");
		}

		{
			int made = 0;
			if (false && Functions::_Dvar_RegisterBool)   // ⛔ superseded: they are omnvars
			{
				// flags 0 == an ordinary settable dvar (same class Com_InitDvars
				// uses for the host-qualification thresholds), NOT the 0x2000
				// engine-owned class -- so these are settable from the console.
				static const char* const kHudDvars[] = {
					"ui_hide_hud",
					"ui_hide_minimap",
					"ui_hide_hints_hud",
					"ui_hide_1v1scores",
				};
				for (const char* n : kHudDvars)
				{
					// Do not stomp one that somehow already exists.
					if (Functions::_Dvar_FindVar && Functions::_Dvar_FindVar(n))
					{
						continue;
					}
					Functions::_Dvar_RegisterBool(n, false, 0);
					++made;
				}
			}
			(void)made;   // ⛔ dvar route superseded -- see the omnvar block above.
		}

		dev_mode::add_command("demo_hud_only", []
		{
			const auto* args = GameUtil::getCmdArgs();
			const int argc = args ? args->argc[args->nesting] : 0;
			std::vector<int> keep;
			bool off = false;
			for (int i = 1; i < argc && keep.size() < 32; ++i)
			{
				const char* a = args->argv[args->nesting][i];
				if (!a || !*a) { continue; }
				if (_stricmp(a, "off") == 0 || _stricmp(a, "none") == 0) { off = true; break; }
				const int id = std::atoi(a);
				if (id > 0 && id < 128) { keep.push_back(id); }
			}
			g_hud_minimal = false;
			// Withholding and blanking are mutually exclusive -- always clear the
			// withhold mask, or the getter would never be reached to blank.
			hud_mask_set({});
			if (off)
			{
				hud_blank_clear();
				Console::printf("[demo] HUD: stock. Nothing withheld, nothing blanked.");
				return;
			}
			hud_blank_all_except(keep);
			g_hud_blanked_hits.store(0, std::memory_order_relaxed);

			std::string kept;
			for (const int id : keep)
			{
				if (!kept.empty()) { kept += " "; }
				kept += std::to_string(id);
			}
			Console::printf("[demo] HUD: BLANKING every model-driven LUI element%s%s.",
				kept.empty() ? "" : " except id(s) ", kept.c_str());
			Console::printf("[demo]   Blanking, not withholding. Withholding only FROZE things "
				"(the kill counter stopped updating but stayed on screen); publishing an EMPTY "
				"value tells the widget there is nothing to show, which is what hides it.");
			Console::printf("[demo]   Kept by design: hitmarkers (native, crosshair path), "
				"obituaries/killfeed and score popups (LUI but EVENT-driven -- they do not go "
				"through this model publisher at all).");
			Console::printf("[demo]   Anything still on screen after this is NOT model-driven, "
				"so it is drawn natively and needs a different lever. Say which.");
		});

		dev_mode::add_command("demo_hud_minimal", []
		{
			set_hud_minimal(!g_hud_minimal);
		});



		dev_mode::add_command("demo_block_action", []
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2)
			{
				g_blocked_action_a = std::atoi(args->argv[args->nesting][1]);
				g_blocked_action_b = (args->argc[args->nesting] >= 3)
					? std::atoi(args->argv[args->nesting][2]) : -1;
			}
			else
			{
				g_blocked_action_a = -1;
				g_blocked_action_b = -1;
			}
			Console::printf("[demo] blocking demo actions %d / %d (no args = block none)",
				g_blocked_action_a, g_blocked_action_b);
		});

		GameUtil::addCommand("demo_autoname", []
		{
			g_autorename = !g_autorename;
			Console::printf(
				"[rec] rename finished recordings to <map>_<date>_<time>: %s",
				g_autorename ? "ON" : "off (the engine's x<hex> name is kept)");
		});

		dev_mode::add_command("demo_autofix", []
		{
			g_autofix_ncs = !g_autofix_ncs;
			Console::printf(
				"[rec] automatic script-string repair on stop: %s",
				g_autofix_ncs ? "ON" : "off (public-match demos will not play)");
		});

		dev_mode::add_command("demo_record_theater", []
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] > 1)
			{
				g_record_in_theater = args->argv[args->nesting][1][0] != '0';
			}
			else
			{
				g_record_in_theater = !g_record_in_theater;
			}
			Console::printf(
				"[rec] write a native .demo while replaying a custom one: %s",
				g_record_in_theater ? "ON" : "off");
			if (g_record_in_theater)
			{
				Console::printf(
					"[rec]   this is a free .dm_s2 -> .demo transcode: the engine writes "
					"its own header, footer and NetConstStrings table, so no same-map "
					"template is needed. Arm it BEFORE demo_play -- CL_Demo_StartRecord "
					"only runs at cgame init.");
			}
		});

		dev_mode::add_command("demo_viewmodel_request", []
		{
			g_vm_request_enabled = !g_vm_request_enabled;
			Console::printf("[vmreq] direct viewmodel stream request: %s (issued %d so far)",
				g_vm_request_enabled ? "ON" : "OFF", g_vm_requests);
		});


		dev_mode::add_command("demo_streamsync_gate", []
		{
			g_ss_force_gate = !g_ss_force_gate;
			Console::printf("[ss] force StreamSync gate open during native playback: %s",
				g_ss_force_gate ? "ON (the demo's own recorded requests are honoured)"
				                : "OFF (stock: requests are discarded while the gate is 0)");
		});


		dev_mode::add_command("demo_zones", cmd_zone_list);
		// =====================================================================
		// Dump the actual slot contents. The three fields that decide whether a
		// seek can use a slot, proven from sub_914790 (writer) and sub_915A10
		// (scan), with the record base at PlaybackData+2176584:
		//     [0] offset into keyframe memory
		//     [2] time  = clientActive[25380]   <- what the scan compares
		//     [7] length = writeCursor - [0]    <- scan requires > 0
		// A slot with a time but length 0 means the keyframe wrote NO PAYLOAD,
		// and every seek will skip it.
		// demo_seek_kf <ms>   absolute demo time   (was `demo_seek`; renamed, see seek_back)
		// demo_seek_kf prev | next
		// Runs on the CLIENT thread, which matters: ProcessKeyFrameJump reparses
		// the gamestate and calls CL_SetCGameTime.
		// demo_skip <ms> — IWXMVM's fast-forward. Runs on the CLIENT thread, which
		// matters: it feeds CL_SetCGameTime's packet loop, and a large skip makes
		// that loop consume many packets in one frame.
		// ⛔ ALWAYS registered, not dev-only: seek_forward() and seek_to_time()
		// queue this, and they are reached from the product transport and the
		// dolly Go button. As a dev_mode command it simply did not exist outside
		// developer mode, so those seeks silently did nothing.
		GameUtil::addCommand("demo_skip", []
		{
			if (!g_native_playing)
			{
				Console::printf("[demo] skip: no native demo playing");
				return;
			}
			auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[demo] usage: demo_skip <ms>   (forward only — the engine "
					"clamps cl.serverTime against oldFrameServerTime, so a negative skip "
					"does nothing. Use demo_seek_kf for backward.)");
				return;
			}
			const int ms = GameUtil::safeStringToInt(args->argv[args->nesting][1]);
			if (ms <= 0)
			{
				Console::printf("[demo] skip: forward only. Use `demo_seek_kf <ms>` to go back.");
				return;
			}
			// A very large skip is legal but reads every packet in between within
			// one frame, so say what is happening rather than appearing to hang.
			if (ms > 60000)
			{
				Console::printf("[demo] skipping %d ms — this reads every packet in "
					"between, so expect a pause.", ms);
			}
			// Through the one absolute seek, so a skip lands the same way whether
			// the demo is playing or paused.
			const int now = demo_time_smooth();
			seek_absolute_now((now >= 0 ? now : current_demo_time()) + ms);
		});

		// demo_skip_to <absolute ms> — the second half of seek_to_time's
		// rewind-then-skip. Queued AFTER demo_seek on the same Cbuf, so by the
		// time it runs the jump has completed and current_demo_time() reports
		// where the keyframe actually landed. Forward-only by nature: if the jump
		// overshot (or landed exactly) there is nothing to do.
		GameUtil::addCommand("demo_skip_to", []   // always: seek_to_time queues it
		{
			if (!g_native_playing)
			{
				return;
			}
			auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[demo] usage: demo_skip_to <absolute ms>");
				return;
			}
			const int target = GameUtil::safeStringToInt(args->argv[args->nesting][1]);
			const int now = current_demo_time();
			if (now < 0 || target <= now)
			{
				Console::printf("[demo] skip_to %d: already at %d ms, nothing to do",
					target, now);
				return;
			}
			Console::printf("[demo] skip_to %d: closing the %d ms the keyframe jump "
				"left short", target, target - now);
			seek_absolute_now(target);
		});

		dev_mode::add_command("demo_seek_forceclock", []
		{
			auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[demo] seek clock force is %s. usage: "
					"demo_seek_forceclock <0|1>", g_force_seek_clock ? "ON" : "OFF");
				return;
			}
			g_force_seek_clock = GameUtil::safeStringToInt(args->argv[args->nesting][1]) != 0;
			Console::printf("[demo] after a keyframe jump, force the clock onto the "
				"keyframe time: %s", g_force_seek_clock ? "ON" : "OFF");
		});

		GameUtil::addCommand("demo_seek_kf", []   // always: seek_to_time / seek_back queue it
		{
			if (!g_native_playing)
			{
				Console::printf("[demo] seek: no native demo playing");
				return;
			}
			auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[demo] usage: demo_seek_kf <ms> | prev | next");
				return;
			}
			const std::string a = args->argv[args->nesting][1];
			// An absolute time is the ONE seek (keyframe jump + exact landing,
			// pause-safe). Only prev/next remain raw keyframe steps below.
			if (a != "prev" && a != "next")
			{
				seek_absolute_now(GameUtil::safeStringToInt(a.c_str()));
				return;
			}
			auto slots = usable_slots();
			if (slots.empty())
			{
				Console::printf("[demo] seek: no usable keyframes yet â€” let the demo run a "
					"few more seconds. A slot counts only when it has a payload AND a "
					"NON-EMPTY replay range (slot+32 != slot+36); an empty one restores "
					"no snapshot, so seeking to it lands the clock but leaves the world "
					"stale. `demo_keyframe_dump` shows every slot.");
				return;
			}
			const int now = current_demo_time();
			int target = -1;

			if (a == "prev" || a == "next")
			{
				// Strictly past/future of NOW, nearest first. The 800 ms guard
				// mirrors the engine's own cutoff so a "previous" never picks the
				// keyframe we are effectively sitting on.
				int best = -1;
				for (const auto& s : slots)
				{
					if (a == "prev")
					{
						if (s.time < now - 800 && (best < 0 || s.time > best)) { best = s.time; }
					}
					else
					{
						if (s.time > now + 800 && (best < 0 || s.time < best)) { best = s.time; }
					}
				}
				if (best < 0)
				{
					Console::printf("[demo] seek %s: nothing %s %d ms%s", a.c_str(),
						a == "prev" ? "before" : "after", now,
						a == "next"
						? " â€” forward only works into ground already played, because "
						  "keyframes are written as playback passes"
						: "");
					return;
				}
				target = best;
			}
			else
			{
				target = GameUtil::safeStringToInt(a.c_str());
			}

			// Nearest usable keyframe at or before the target.
			int pick = -1, pick_time = -1;
			for (const auto& s : slots)
			{
				if (s.time <= target && s.time > pick_time) { pick = s.index; pick_time = s.time; }
			}
			if (pick < 0)
			{
				// Before every keyframe: take the earliest we have.
				for (const auto& s : slots)
				{
					if (pick_time < 0 || s.time < pick_time) { pick = s.index; pick_time = s.time; }
				}
			}
			Console::printf("[demo] seek -> %d ms: keyframe slot %d (t=%d), from %d ms",
				target, pick, pick_time, now);
			jump_to_slot(pick);
			g_eof_seen = false;   // the jump cleared the engine's completed flag
		});

		dev_mode::add_command("demo_keyframe_dump", []
		{
			const auto g = demo_playback_data();
			if (!g)
			{
				Console::printf("[demo] no playback state");
				return;
			}
			const auto* rec = reinterpret_cast<const std::uint8_t*>(g + 2176584);
			const auto* cur = reinterpret_cast<const std::int32_t*>(g + 2188584);
			if (!readable(rec, 250 * 48) || !readable(cur, 4))
			{
				Console::printf("[demo] keyframe ring not readable");
				return;
			}
			const int idx = *cur;
			Console::printf("[demo] keyframe ring: write index = %d   "
				"(slot fields: [0]=offset [2]=time [7]=length; a seek needs length > 0)",
				idx);
			int shown = 0;
			for (int k = 0; k < 250 && shown < 12; ++k)
			{
				const int i = ((idx - k) % 250 + 250) % 250;   // newest first
				const auto* s = reinterpret_cast<const std::int32_t*>(rec + static_cast<std::size_t>(i) * 48);
				if (s[0] == 0 && s[2] == 0 && s[7] == 0)
				{
					continue;   // untouched slot
				}
				Console::printf("[demo]   slot %3d  offset=%-10d time=%-10d length=%-8d %s",
					i, s[0], s[2], s[7],
					s[7] > 0 ? "USABLE" : "<- length 0, seeks SKIP this");
				++shown;
			}
			if (shown == 0)
			{
				Console::printf("[demo]   ring is entirely empty - the writer is not running");
			}

			// The 32-entry baseline list at +2188596. sub_915A10 only scans
			// BACKWARDS when the current keyframe's index appears here;
			// otherwise it returns that index unchanged and the "rewind" is a
			// no-op. sub_914790 registers an index only on a full snapshot
			// (clientActive[25376] & 8), so this list being empty is precisely
			// why seeking did nothing.
			const auto* list = reinterpret_cast<const std::int32_t*>(g + 2188596);
			if (readable(list, 32 * 4))
			{
				std::string s;
				int n = 0;
				for (int i = 0; i < 32; ++i)
				{
					if (list[i] == -1) { continue; }
					if (!s.empty()) { s += ' '; }
					s += std::to_string(list[i]);
					++n;
				}
				Console::printf("[demo] baseline list (+2188596): %d entr%s%s%s",
					n, n == 1 ? "y" : "ies", n ? " -> " : "", s.c_str());
				if (n == 0)
				{
					Console::printf("[demo]   EMPTY -> GetKeyFrameForJumpBack returns the "
						"current index instead of scanning back. This is the rewind bug.");
				}
			}
		});
		dev_mode::add_command("demo_keyframes", []
		{
			g_gen_keyframes = !g_gen_keyframes;
			Console::printf("[demo] playback keyframe generation %s (%d made so far). "
				"Rewind/forward seek to keyframes; the engine only generates them while "
				"isClipPlaying, which nothing ever sets, so without this they do nothing.",
				g_gen_keyframes ? "ON" : "OFF", g_keyframes_made);
		});
		dev_mode::add_command("demo_release_level", []
		{
			g_release_level = !g_release_level;
			Console::printf("[assets] demo level release (sub_837E0's mask 136): %s",
				g_release_level ? "ON" : "OFF (the previous level will leak)");
		});
		dev_mode::add_command("demo_release_frontend", []
		{
			g_release_frontend = !g_release_frontend;
			Console::printf("[assets] demo frontend release (sub_48C0C0's mask 388): %s",
				g_release_frontend ? "ON"
					: "OFF (baseline stays ~36890/40192, heavy maps cannot fit)");
		});

		dev_mode::add_command("demo_native_state", cmd_native_state);

		dev_mode::add_command("demo_native_onebit", []
		{
			g_onebit_fix = !g_onebit_fix;
			Console::printf("[native] one-bit gamestate fix: %s",
				g_onebit_fix
					? "ON (auto-detected per gamestate: applied only when the gamestate "
					  "is the FIRST record in its message, i.e. shipped .demo framing)"
					: "OFF -- forced off. Shipped demos will fail to prime; transcoded "
					  "and live-framed demos are unaffected (they never needed it)");
		});



		// CL_SetClientState @ IDA 0x87070 -> 0x87070 - 0x1000 = 0x86070

		// CL_Demo_StartRecord @ IDA 0x91C710 -> literal 0x91C710 - 0x1000 = 0x91B710.
		//
		// Deliberately NOT inside TRACE_HOOKS. That block is disabled because 17
		// bracket hooks on functions the MULTITHREADED zone loader also calls made
		// the demo level load hang intermittently. CL_Demo_StartRecord is cold --
		// once per connect, from CL_InitCGame -- and touches nothing the loader
		// threads touch, so it carries none of that risk.
		//
		// RULE A3: report the result. A hook that silently failed to install is
		// indistinguishable from one that installed and never fired, and those need
		// completely different investigations. This exact confusion already cost a
		// live test run: the probe was extended while the hook sat behind
		// TRACE_HOOKS=false, so it produced no output and looked like "the engine
		// never calls it".
		{
			// CL_Demo_IsRecordingAllowed @ IDA 0x90FBF0 -> 0x90FBF0 - 0x1000 = 0x90EBF0.
			// Sole caller is CL_Demo_StartRecord (checked), so forcing it cannot
			// affect anything else.
			const bool ok = Hook::create("CL_Demo_IsRecordingAllowed",
				reinterpret_cast<void*>(0x90EBF0_b),
				reinterpret_cast<void*>(cl_demo_is_recording_allowed_stub),
				reinterpret_cast<void**>(&CL_Demo_IsRecordingAllowed_orig));
			Console::printf(
				"[rec] CL_Demo_IsRecordingAllowed hook: %s (orig=%p) -- native recording "
				"is %s (demo_record)",
				(ok && CL_Demo_IsRecordingAllowed_orig) ? "OK" : "FAILED/DUPLICATE",
				reinterpret_cast<void*>(CL_Demo_IsRecordingAllowed_orig),
				g_force_record ? "ENABLED" : "disabled");
		}

		patch_freecam_speed();

		{
			// CL_Demo_HandleAction @ IDA 0x915E40 -> 0x915E40 - 0x1000 = 0x914E40.
			// Not hooked elsewhere (checked). Cold: only on demo key actions.
			const bool ok = Hook::create("CL_Demo_HandleAction",
				reinterpret_cast<void*>(0x914E40_b),
				reinterpret_cast<void*>(cl_demo_handle_action_stub),
				reinterpret_cast<void**>(&CL_Demo_HandleAction_orig));
			Console::printf(
				"[demo] CL_Demo_HandleAction hook: %s (orig=%p) -- logs demo key actions; "
				"blocking %d/%d (demo_block_action)",
				(ok && CL_Demo_HandleAction_orig) ? "OK" : "FAILED/DUPLICATE",
				reinterpret_cast<void*>(CL_Demo_HandleAction_orig),
				g_blocked_action_a, g_blocked_action_b);
		}

		{
			// CG_PublishHudModel @ IDA 0x3573B0 -> 0x3573B0 - 0x1000 = 0x3563B0.
			// Not hooked anywhere else (grepped src/ for both forms).
			//
			// This one IS hot -- sub_357AD0 calls it ~70 times per frame -- so the
			// stub early-outs on !g_native_playing before touching anything, and
			// live play pays only a trampoline.
			const bool ok = Hook::create("CG_PublishHudModel",
				reinterpret_cast<void*>(0x3563B0_b),
				reinterpret_cast<void*>(cg_publish_hud_model_stub),
				reinterpret_cast<void**>(&CG_PublishHudModel_orig));
			Console::printf(
				"[demo] CG_PublishHudModel hook: %s (orig=%p) -- suppressing LUI HUD "
				"model 86 (cg.hud.currentDivision) during native playback",
				(ok && CG_PublishHudModel_orig) ? "OK" : "FAILED/DUPLICATE",
				reinterpret_cast<void*>(CG_PublishHudModel_orig));
		}

		{
			// CG_GetHudModelValue @ IDA 0x356560 -> 0x356560 - 0x1000 = 0x355560.
			// RULE A3.1: grepped src/ for both forms, not hooked anywhere else.
			//
			// This is the BLANKING lever (demo_hud_only). Withholding a publish only
			// freezes a widget -- measured. Publishing an EMPTY value is what makes
			// it hide. Same hot-path discipline: early-out on !g_native_playing.
			const bool ok = Hook::create("CG_GetHudModelValue",
				reinterpret_cast<void*>(0x355560_b),
				reinterpret_cast<void*>(cg_get_hud_model_value_stub),
				reinterpret_cast<void**>(&CG_GetHudModelValue_orig));
			Console::printf(
				"[demo] CG_GetHudModelValue hook: %s (orig=%p) -- demo_hud_only blanks "
				"model values through this",
				(ok && CG_GetHudModelValue_orig) ? "OK" : "FAILED/DUPLICATE",
				reinterpret_cast<void*>(CG_GetHudModelValue_orig));
		}

		{
			// CL_Demo_StopRecord @ IDA 0x90FCA0 -> 0x90FCA0 - 0x1000 = 0x90ECA0.
			// Not hooked anywhere else (checked). Cold: once per disconnect.
			const bool ok = Hook::create("CL_Demo_StopRecord",
				reinterpret_cast<void*>(0x90ECA0_b),
				reinterpret_cast<void*>(cl_demo_stop_record_stub),
				reinterpret_cast<void**>(&CL_Demo_StopRecord_orig));
			Console::printf(
				"[rec] CL_Demo_StopRecord hook: %s (orig=%p) -- public-match demos get "
				"ncs type 21 spliced in automatically (demo_autofix)",
				(ok && CL_Demo_StopRecord_orig) ? "OK" : "FAILED/DUPLICATE",
				reinterpret_cast<void*>(CL_Demo_StopRecord_orig));
		}

		{
			const bool ok = Hook::create("CL_Demo_StartRecord",
				reinterpret_cast<void*>(0x91B710_b),
				reinterpret_cast<void*>(sub_91C710_stub),
				reinterpret_cast<void**>(&sub_91C710_orig));
			Console::printf(
				"[rec] CL_Demo_StartRecord hook: %s (orig=%p) -- reports why native "
				"recording does or does not start, on every connect",
				(ok && sub_91C710_orig) ? "OK" : "FAILED/DUPLICATE",
				reinterpret_cast<void*>(sub_91C710_orig));
		}

		// The engine's missing null checks on qword_2537508 (see the stubs above).
		Hook::create("sub_47D1C0", reinterpret_cast<void*>(0x47C1C0_b),
			reinterpret_cast<void*>(sub_47D1C0_stub),
			reinterpret_cast<void**>(&sub_47D1C0_orig));
		Hook::create("sub_1FEB80", reinterpret_cast<void*>(0x1FDB80_b),
			reinterpret_cast<void*>(sub_1FEB80_stub),
			reinterpret_cast<void**>(&sub_1FEB80_orig));
		Hook::create("sub_47E670", reinterpret_cast<void*>(0x47D670_b),
			reinterpret_cast<void*>(sub_47E670_stub),
			reinterpret_cast<void**>(&sub_47E670_orig));
		Console::printf(
			"[native] qword_2537508 null-guards: 47D1C0=%s 1FEB80=%s 47E670=%s "
			"(the global stays NULL, which is its honest value)",
			sub_47D1C0_orig ? "OK" : "FAILED",
			sub_1FEB80_orig ? "OK" : "FAILED",
			sub_47E670_orig ? "OK" : "FAILED");


		const bool int_hooked = Hook::create("CL_ParseServerMessage_Internal",
			reinterpret_cast<void*>(demo_game::addr_CL_ParseServerMessage_Internal()),
			reinterpret_cast<void*>(cl_parse_server_message_internal_stub),
			reinterpret_cast<void**>(&CL_ParseServerMessage_Internal_orig));
		Console::printf("[native] CL_ParseServerMessage_Internal hook: %s (orig=%p)",
			(int_hooked && CL_ParseServerMessage_Internal_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(CL_ParseServerMessage_Internal_orig));

		// MSG_ReadBit @ IDA 0xDC870 -> literal 0xDC870 - 0x1000 = 0xDB870.
		// Verified per RULE A3.1 that nothing else in src/ hooks this target.
		// The stub early-outs on a thread_local unless we are inside
		// CL_ParseGamestate during native playback, so live play is unaffected.
		const bool bit_hooked = Hook::create("MSG_ReadBit",
			reinterpret_cast<void*>(0xDB870_b),
			reinterpret_cast<void*>(msg_read_bit_stub),
			reinterpret_cast<void**>(&MSG_ReadBit_orig));
		Console::printf("[native] MSG_ReadBit hook: %s (orig=%p) - the one-bit gamestate fix",
			(bit_hooked && MSG_ReadBit_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(MSG_ReadBit_orig));

		const bool gs_hooked = Hook::create("CL_ParseGamestate",
			reinterpret_cast<void*>(demo_game::addr_CL_ParseGamestate()),
			reinterpret_cast<void*>(cl_parse_gamestate_stub),
			reinterpret_cast<void**>(&CL_ParseGamestate_orig));
		// orig != nullptr is the real proof of install â€” Hook::create also returns
		// true for MH_ERROR_ALREADY_CREATED, which leaves orig null (RULE A3.1).
		Console::printf("[native] CL_ParseGamestate hook: %s (orig=%p)",
			(gs_hooked && CL_ParseGamestate_orig) ? "OK" : "FAILED/DUPLICATE",
			reinterpret_cast<void*>(CL_ParseGamestate_orig));

		// NO Hook::create for CL_Demo_ReadDemoMessage here â€” demo_playback.cpp
		// already hooks IDA 0x9188C0 (cl_demo_read_message_stub). A duplicate
		// returns MH_ERROR_ALREADY_CREATED, which Hook::create counts as success
		// while MinHook leaves `original` null, so the second detour is silently
		// discarded. That is exactly what happened on 2026-08-08 and it cost a full
		// test cycle. The theater's stub calls intercept_read/note_read_result on
		// its native-playback path instead. See CLAUDE.md RULE A3.

		Console::printf("[native] %zu engine demo(s) in main/demo; Com_Error watch armed",
			g_files.size());
	}
}
