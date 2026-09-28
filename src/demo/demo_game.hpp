#pragma once
// =============================================================================
//  demo/demo_game.hpp — S2 (s2_mp64_ship / s2x_dump.exe) engine bridge
//
//  Address convention: X_b  =>  module_base + (IDA_abs - 0x1000). Lazy accessors
//  only (never static-init pointers — base may not exist yet).
//
//  IDA naming policy: rename proven twins as they land (see comments below).
//  Session 2026-08-05: Parse / Snapshot / Gamestate / ConfigStrings / clc / gs /
//  snap / WritePacket / ExecuteNewServerCommands / CalcViewValues / LUI clocks.
//
//  Clock: CL_SetCGameTime IS present @ IDA 0x86D30 (was mislabelled CL_Demo_RunFrame;
//    identified by Com_Error IDs "439"/"440" = !cl.snap.valid / serverTime regression).
//    It owns PRIMED -> ACTIVE via CL_FirstSnapshot @ 0x74910 and the serverTimeDelta
//    lock, so replay must let it run. The global cl_serverTime @ IDA 0xC5FBA44 is
//    separate: CL_ClientFrame @ 0x6E0940 does cl_serverTime += 50 and theater
//    overwrites it after ClientFrame — that one drives the demo feed gate only.
//  Do NOT borrow CL_Demo mode=2 / AllocPlaybackState.
// =============================================================================

#include "game.h"
#include "structs.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// client_active_for() validates the engine-owned pointer it caches (see the comment
// there -- a null check alone let a freed pointer through and crashed the game), so
// this header now needs VirtualQuery and a way to say so.
#include <Windows.h>
#include "Console.hpp"

namespace demo_game
{
	using vec3_t = float[3];

	constexpr int CA_DISCONNECTED = 0;
	constexpr int CA_CONNECTED = 5;
	constexpr int CA_PRIMED = 9;
	constexpr int CA_ACTIVE = 10;

	// ---- clientActive_t live predicted sources (CL_SavePredicted @ 0x465580) ---
	constexpr std::size_t CA_ORIGIN = 25704;
	constexpr std::size_t CA_VELOCITY = 25716;
	constexpr std::size_t CA_VIEWANGLES = 25728;
	constexpr std::size_t CA_EXTRA0 = 25740;   // dword + 3 words @ 25744
	// Live mouse / CreateCmd source angles (CL_CreateCmd @ 0x9DC60 → sub_74770).
	// Packed into usercmd +0x10/+0x14/+0x18 as ANGLE2SHORT. Distinct from CA_VIEWANGLES.
	constexpr std::size_t CA_CMD_VIEWANGLES = 25900; // 0x652C pitch, +4 yaw, +8 roll
	constexpr std::size_t CA_BOB_A = 25880;
	constexpr std::size_t CA_BOB_B = 25884;
	constexpr std::size_t CA_ARCHIVE = 42344;       // ClientArchiveEntry[256]
	constexpr std::size_t CA_ARCHIVE_STRIDE = 188;  // HIGH: SavePredicted
	constexpr std::size_t CA_ARCHIVE_INDEX = 90472;

	// Snap pin fields (CL_ParseSnapshot @ 0x464C00 renamed):
	//   snap starts at clientActive+8, size 0x6368 (25448).
	//   temp[6343] serverTime → absolute +25380
	//   temp[6338] valid      → absolute +25360
	//   end: *(clientActive+25484)=1 → newSnapshots
	constexpr std::size_t CA_SNAP_BASE = 8;
	constexpr std::size_t CA_SNAP_VALID = 25360;       // HIGH
	constexpr std::size_t CA_SNAP_FLAGS = 25376;       // HIGH: bit1 = SNAPFLAG_NOT_ACTIVE
	constexpr std::size_t CA_SNAP_SERVERTIME = 25380;  // HIGH
	constexpr std::size_t CA_NEW_SNAPSHOTS = 25484;    // HIGH

	// CL_SetCGameTime @ 0x86D30 / CL_FirstSnapshot @ 0x74910 clock fields.
	//
	// CONFIRMED 2026-08-07 by decompiling 0x86D30 (S2 IDB). Function tail is:
	//     serverTime = serverTimeDelta + cls_realtime;
	//     if (serverTime < oldFrameServerTime) serverTime = oldFrameServerTime; // ratchet
	//     oldFrameServerTime = serverTime;
	//     if (newSnapshots) CL_AdjustTimeDelta();
	// Two consequences the theater clock depends on:
	//   1. The clock is anchored to cls_realtime, which always runs at 1.0x.  A
	//      timescale anchor written OUTSIDE this call therefore comes back with a
	//      full frame of UNSCALED real time added on top, and the oldFrameServerTime
	//      ratchet stops it falling back — which is why demo_playback re-stamps the
	//      theater clock immediately AFTER this function returns, not at frame end.
	//   2. CL_AdjustTimeDelta is gated on newSnapshots, so zeroing that field is a
	//      sufficient and correct way to stop it fighting the theater clock.
	// Every offset below was verified against that same decompile (idx = byte/4):
	// 6345 snap.serverTime, 6365 oldFrameServerTime, 6366 extrapolatedSnapshot,
	// 6368 serverTime, 6369 oldServerTime, 6370 serverTimeDelta, 6371 newSnapshots.
	constexpr std::size_t CA_OLD_FRAME_SERVER_TIME = 25460;
	constexpr std::size_t CA_EXTRAPOLATED_SNAPSHOT = 25464;
	constexpr std::size_t CA_SERVER_TIME = 25472;
	constexpr std::size_t CA_OLD_SERVER_TIME = 25476;
	constexpr std::size_t CA_SERVER_TIME_DELTA = 25480;

	constexpr int SNAPFLAG_NOT_ACTIVE = 2;

	// clientActive stride via CL_GetLocalClientActive @ 0x795D0 → base+91552*i
	constexpr std::size_t CA_STRIDE = 91552;

	// playerState inject (CL_GetPredicted @ 0x462650)
	constexpr std::size_t PS_BOB_BYTE = 15;
	constexpr std::size_t PS_ORIGIN = 132;
	constexpr std::size_t PS_VELOCITY = 144;
	constexpr std::size_t PS_MOVEMENT_DIR = 248;
	// HIGH: sub_4E620 copies cg.ps fields into clientActive each frame —
	//   clientActive+25728 (viewangles) <- ps+576, so ps is the only durable inject point.
	//   Writing clientActive directly is pointless; it is overwritten from ps every frame.
	constexpr std::size_t PS_VIEWANGLES = 576;
	// CL_GetDemoViewAnglesFromArchive writes archive.extra0 → ps dword index 204 (= +816).
	constexpr std::size_t PS_ARCHIVE_EXTRA0 = 816;

	static_assert(offsetof(msg_t, useZlib) == 0x34, "msg_t.useZlib");

	// Archive slot layout (relative to CA_ARCHIVE + i*188) — CL_SavePredicted @ 0x465580
	// writes through +56 (w2). CL_GetPredicted restores origin/vel/bob only;
	// CL_GetDemoViewAnglesFromArchive restores viewangles + extra0 + w0..w2.
#pragma pack(push, 1)
	struct ClientArchiveEntry
	{
		int serverTime;          // +0
		vec3_t origin;           // +4
		vec3_t velocity;         // +16
		int bobA;                // +28  from CA_BOB_A → ps+15 (byte)
		int bobB;                // +32  from CA_BOB_B → ps+248
		vec3_t viewangles;       // +36  from live CA_VIEWANGLES → ps+576
		int extra0;              // +48  → ps+816
		std::uint16_t w0, w1, w2;// +52  → cg kick/angle words
		std::uint8_t _pad[188 - 58];
	};
#pragma pack(pop)
	static_assert(sizeof(ClientArchiveEntry) == 188, "archive stride");
	static_assert(offsetof(ClientArchiveEntry, extra0) == 48, "archive.extra0");
	static_assert(offsetof(ClientArchiveEntry, w0) == 52, "archive.w0");

	// Lightweight ps view for inject — only fields GetPredicted / theater touch.
	struct demo_playerState_t
	{
		std::uint8_t _pad0[PS_BOB_BYTE];
		std::uint8_t bobByte;            // +15
		std::uint8_t _pad1[PS_ORIGIN - (PS_BOB_BYTE + 1)];
		vec3_t origin;                   // +132
		vec3_t velocity;                 // +144
		std::uint8_t _pad2[PS_MOVEMENT_DIR - (PS_VELOCITY + 12)];
		int movementDir;                 // +248
	};

	// ---- lazy RVAs (IDA abs → _b). Named in IDB. --------------------------------
	inline uintptr_t addr_CL_ParseServerMessage() { return 0x4639D0_b; }              // 0x4649D0
	inline uintptr_t addr_CL_ParseServerMessage_Internal() { return 0x4626E0_b; }     // 0x4636E0 renamed
	inline uintptr_t addr_CL_ParseSnapshot() { return 0x463C00_b; }                   // 0x464C00
	inline uintptr_t addr_CL_ParseGamestate() { return 0x461B00_b; }                  // 0x462B00
	inline uintptr_t addr_CL_ParseConfigStrings_Internal() { return 0x461930_b; }     // 0x462930
	inline uintptr_t addr_CL_GetConfigString() { return 0x78160_b; }                  // 0x79160
	inline uintptr_t addr_CL_GetLocalClientActive() { return 0x785D0_b; }             // 0x795D0
	inline uintptr_t addr_CL_SavePredicted() { return 0x464580_b; }                   // 0x465580
	inline uintptr_t addr_CL_GetPredicted() { return 0x461650_b; }                    // 0x462650
	// NOT a weapon restore — this is CL_GetPredictedVehicleForServerTime. It refills
	// ps.vehicleState (ps+0x10C..0x188) from the predicted archive, and its caller feeds
	// ps+0x10C straight into CG_GetEntity. The archive is dead during replay, so let it
	// fail (H1 never installs its equivalent either). Firing is unrelated to this hook.
	inline uintptr_t addr_CL_GetPredictedWeapon() { return 0x461720_b; }              // 0x462720
	inline uintptr_t addr_CL_ClientFrame() { return 0x6DF940_b; }                     // 0x6E0940
	// HIGH: CL_WritePacket — IW3 CL_SendCmd ends here. IsDemoPlaying early-out;
	// calls CL_SavePredicted (archive fill). Theater suppress = must seed archive
	// ourselves so GetDemoViewAnglesFromArchive still works.
	inline uintptr_t addr_CL_WritePacket() { return 0x83C10_b; }                      // 0x84C10
	// HIGH: not the engine demo runner — this is CL_SetCGameTime (errors "439"/"440" =
	// !cl.snap.valid / snap.serverTime < oldServerTime). Promotes PRIMED -> ACTIVE via
	// CL_FirstSnapshot @ 0x74910 when cl.newSnapshots is set. Must keep running in replay.
	inline uintptr_t addr_CL_SetCGameTime() { return 0x85D30_b; }                     // 0x86D30
	inline uintptr_t addr_CL_FirstSnapshot() { return 0x73910_b; }                    // 0x74910
	// MISLEADING IDA NAME — this parses a text command and tests for "keepalive"; it is an
	// out-of-band packet handler, NOT the demo reader. Same mislabelling as CL_SetCGameTime.
	inline uintptr_t addr_CL_Demo_GetNetMsgInfo() { return 0x6C280_b; }               // 0x6D280
	// The REAL native demo reader step. Called from CL_SetCGameTime in a feed loop
	// (while serverTime < snap.serverTime) and once in the pre-ACTIVE block. It reads the
	// ENGINE's own demo file handle, which custom playback does not own, so it is
	// neutralised while armed. Returning 0 also terminates that feed loop immediately,
	// which is what lets the native CLOCK path run without the native READ path.
	inline uintptr_t addr_CL_Demo_ReadMessage() { return 0x9178C0_b; }                // 0x9188C0
	inline uintptr_t addr_Sys_Milliseconds() { return 0x7B0290_b; }                   // 0x7B1290

	// Frame / view (HIGH — named in IDB this session)
	inline uintptr_t addr_SCR_DrawScreen() { return 0xF10D0_b; }                     // 0xF20D0
	inline uintptr_t addr_CG_DrawActiveFrame() { return 0x67DC0_b; }                  // 0x68DC0
	inline uintptr_t addr_CG_CalcViewValues() { return 0x8B9C0_b; }                   // 0x8C9C0 (after ApplyFov)
	inline uintptr_t addr_CG_GetLocalClientGlobals() { return 0x14330_b; }            // 0x15330
	// Returns clc demo-mode dword == 2. Theater spoofs this when armed+ACTIVE so
	// DrawActiveFrame writes cg.demoType (0x69014) and CL_CreateCmd (0x9DC60) skips
	// live input — but NEVER while CL_SetCGameTime runs (native demo reader).
	inline uintptr_t addr_CL_IsDemoPlaying() { return 0x90F400_b; }                   // 0x910400
	inline uintptr_t addr_CG_AfterDemoTypeWrite() { return 0x169570_b; }              // 0x17A570
	// Builds usercmd from CA mouse; gated by CL_IsDemoPlaying @ 0x9DE04.
	// On S2, IW3's CL_SendCmd is inlined into CG_DrawActiveFrame as:
	//   CL_CreateCmd (0x696F2) → … → Predict → … → CL_WritePacket (0x699DC).
	inline uintptr_t addr_CL_CreateCmd() { return 0x9CC60_b; }                        // 0x9DC60

	// cg_t starts with predictedPlayerState. demoType @ +0x5980 = dword index 5728.
	// Adjacent +0x5984 cleared each frame; +0x5990 must be non-zero or DrawActiveFrame
	// early-returns 0 (skips world). Do NOT nop the demoType store (Arxan).
	constexpr std::size_t CG_DEMO_TYPE = 0x5980;
	constexpr std::size_t CG_DEMO_TYPE_ADJ = 0x5984;
	constexpr std::size_t CG_DRAW_GATE = 0x5990; // must be != 0 to draw
	constexpr int DEMO_TYPE_NONE = 0;
	constexpr int DEMO_TYPE_CLIENT = 1;
	constexpr std::size_t CG_TIME = 1993596;
	// Final camera angles consumed by AnglesToAxis in CG_CalcViewValues. The IW6/H1
	// implementation records refdefViewAngles, not the client archive's raw angles.
	constexpr std::size_t CG_REFDEF_VIEWANGLES = 2355472;
	// CG_BuildViewmodelDObj. CONFIRMED 2026-08-07 by decompiling IDA 0x5E980 — it has
	// THREE gates, and the first two are hard early-returns before anything is built:
	//   1. if (!*a3) return;                  a3 = Weapon*, first u16 is the weapon index.
	//                                         A zero held-weapon kills the viewmodel outright.
	//   2. if (!BG_GetViewModel(a3)) return;  IDA 0x3BD8F0, named in the IDB.
	//   3. the gun XModel is appended only when the hide byte at cg+0x256C2B == 0;
	//      cg+0x256C2A is a separate byte compared against (ps+1524 & 0x4000) = left-hand.
	// Gate 1 is the prime suspect for "local viewmodel never appears" — remote players
	// render through a different (world-model) path, which is why they can differ.
	inline uintptr_t addr_CG_BuildViewmodelDObj() { return 0x5D980_b; }               // 0x5E980
	// CONFIRMED 2026-08-07: the hide byte is NOT an intent flag — CG_UpdateViewModel is
	// its only writer (IDA 0x5D40E) and computes it as:
	//     hide = (XModel_AreImagesResident(BG_GetViewModel(weapon)) == 0)
	// So "no gun" means the weapon's TEXTURES were never streamed in, not that the engine
	// decided to hide it. Explains the local viewmodel never appearing AND remote players
	// mostly lacking third-person guns: only weapons whose images happen to be resident draw.
	inline uintptr_t addr_CG_UpdateViewModel() { return 0x5C0C0_b; }                  // 0x5D0C0
	// VERIFIED 2026-08-07 by decompiling 0x3BD8F0: BG_GetViewModel(Weapon*, char leftHand,
	// int slot). Resolves off_8A9C660[weaponIndex] -> +16 (or +24 for the dual/alt path)
	// -> [8*slot]. A null return is CG_BuildViewmodelDObj's gate 2 and returns before the
	// hide byte is ever consulted.
	inline uintptr_t addr_BG_GetViewModel() { return 0x3BC8F0_b; }                    // 0x3BD8F0
	// The viewmodel DRAW path, downstream of every CG_BuildViewmodelDObj gate. Reads
	// cg_t::blockDrawViewmodel at 0x2FE8E. MEASURED: blockDraw is always 0, so if the gun
	// is still missing while all gates are open, the question is whether this runs at all.
	inline uintptr_t addr_CG_DrawViewWeapon() { return 0x2EC80_b; }                   // 0x2FC80
	// CG_DrawViewWeapon's hard gates, in order (all must pass to reach CG_AddPlayerWeapon):
	//   (pm_type - 5) > 1        i.e. pm_type NOT 5 or 6      cg+2
	//   cg+1993628 == 0
	//   !BG_IsThirdPersonMode(cg)                             <-- prime suspect for theater
	//   (ps.eFlags & 0x203800) == 0                           cg+104
	//   held weapon index != 0
	// NOTE cg+2495992 (0x2615F8, blockDrawViewmodel) is NOT a hard gate — it only selects
	// an argument passed to CG_AddPlayerWeapon, which is why blockDraw==0 changed nothing.
	inline uintptr_t addr_BG_IsThirdPersonMode() { return 0x38A810_b; }               // 0x38B810
	constexpr std::size_t CG_DRAWVIEW_FLAG = 1993628;
	inline uintptr_t addr_XModel_AreImagesResident() { return 0x1951F0_b; }           // 0x1961F0
	constexpr std::size_t CG_VIEWMODEL_HIDE = 2452523;   // cg+0x256C2B, gate 3

	// ---- The gates BELOW CG_DrawViewWeapon (decompiled 2026-08-07) -------------------
	// CG_DrawViewWeapon's tail is:
	//     drawGun = 1;
	//     if (cg->cubemapShot || !Dvar_GetBool("1762")) drawGun = 0;
	//     v21 = cg->blockDrawViewmodel ? 0 : drawGun;
	//     CG_AddPlayerWeapon(client, &axis, cg, cg->..., v21);
	// and CG_AddPlayerWeapon @ 0x2F1B0 submits the viewmodel ONLY when
	//     v21 != 0  &&  CG_ViewWeaponHideTransition(cg, 0) == 0
	// then, per DObj slot,
	//     dobj != 0  &&  DObj_MaterialsReady(dobj) != 0.
	// `!a5 || hideTransition` takes a silent no-op branch — no log, no gun, every gate we
	// had probed still reading "open". That is exactly the observed symptom.
	inline uintptr_t addr_CG_AddPlayerWeapon() { return 0x2E1B0_b; }                  // 0x2F1B0
	// Named in the IDB. Non-zero only when the held weapon is mid put-away/deploy AND the
	// returned fraction > 0.01. Suppresses the whole viewmodel submission.
	inline uintptr_t addr_CG_ViewWeaponHideTransition() { return 0x3E2250_b; }        // 0x3E3250
	// Named in the IDB. The DObj-level material check, SEPARATE from the XModel image
	// residency that `demo_force_images` fakes. Forcing XModel_AreImagesResident only opens
	// the `hide` byte so the gun XModel gets appended — it does NOT satisfy this. The
	// "image residency is DISPROVEN" result was measured with a probe blind to this gate.
	inline uintptr_t addr_DObj_MaterialsReady() { return 0xAACC0_b; }                 // 0xABCC0
	// IDA qword_8B0CBD0 — viewmodel DObj array. Per local client stride 268 qwords;
	// two DObj slots per client, 134 qwords (1072 bytes) apart. The DObj pointer is the
	// first qword of each slot.
	inline std::uintptr_t* viewmodel_dobjs(const int client_num = 0)
	{
		return reinterpret_cast<std::uintptr_t*>(0x8B0BBD0_b) + 268 * client_num;
	}
	constexpr std::size_t VIEWMODEL_DOBJ_STRIDE = 134;   // in qwords
	// cg->cubemapShot. Same dword this header already calls CG_DEMO_TYPE_ADJ ("cleared each
	// frame") — the IW cg_s layout puts cubemapShot directly after demoType, and
	// CG_DrawViewWeapon reads it at cg+22916 as a hard drawGun kill.
	constexpr std::size_t CG_CUBEMAP_SHOT = CG_DEMO_TYPE_ADJ;
	// Arxan-obfuscated bool read: Dvar_GetBool(off_8B0DA90). The dvar is registered in
	// sub_41D690 as name "1762", default true, flags 4 (S2 uses numeric dvar names).
	// This is the engine's cg_drawGun equivalent.
	inline uintptr_t addr_Dvar_GetBool_obf() { return 0xAE180_b; }                    // 0xAF180
	inline void* dvar_draw_gun() { return *reinterpret_cast<void**>(0x8B0CA90_b); }   // off_8B0DA90

	// CG_DrawViewWeapon asks for the viewmodel with the SOFT (advisory) request every draw,
	// but uses the HARD request for the config-string models immediately above it. Soft
	// requests are serviced at the streamer's discretion; hard ones are not. Both take an
	// XModel*, both named in the IDB.
	inline uintptr_t addr_XModel_StreamHard() { return 0x4DC3A0_b; }                  // 0x4DD3A0
	inline uintptr_t addr_XModel_StreamSoft() { return 0x4DC3F0_b; }                  // 0x4DD3F0
	inline uintptr_t addr_BG_GetHeldWeapon() { return 0x3B50F0_b; }                   // 0x3B60F0
	// Viewmodel reads the 12-byte Weapon via BG_GetHeldWeapon (sub_3B60F0): primary at
	// ps+1512, alternate at ps+1464 when (ps+1524)&2.
	//
	// ps+0x10C IS NOT THE WEAPON. sub_4E620 feeds it straight into CG_GetEntity
	// (sub_13A00) and then memcpys ~100 bytes off the result — it is vehicleState.entity
	// (2047 = none), and 0x462720 is CL_GetPredictedVehicleForServerTime, not a weapon
	// restore. Writing `held` there = CG_GetEntity(218) on a dead centity = spawn crash.
	// Leave it at 2047; that is correct and expected during theater.
	constexpr std::size_t PS_PM_TYPE = 2;                // byte; sub_5D0C0 skips create at >= 7
	constexpr std::size_t PS_VEHICLE_ENTITY = 0x10C;     // ushort, 2047 = none. NEVER write.
	// CORRECTED 2026-08-17: this is ps.otherFlags, NOT eFlags. Offset unchanged and
	// still the right field to test -- only the NAME was wrong.
	//   sub_5D0C0 tests three consecutive flag dwords: +96 & 0x800, +100 & 0x100,
	//   +104 & 0x3800. AW has pm_flags@84 / eFlags@88 / otherFlags@92.
	//   The AW->S2 shift can only INCREASE (fields were inserted, never removed) and
	//   is PROVEN +12 at origin (CL_GetPredicted writes archive+4 -> ps+132, and AW's
	//   origin is at 120). So +104 must map to AW >= 92, i.e. otherFlags. eFlags is
	//   at +100. See re/playerState_s.h.
	//   NB this is the same misnaming the MWR netfield work already hit once, where
	//   ps+0x5C turned out to be otherFlags rather than eFlags/pm_flags.
	constexpr std::size_t PS_OTHERFLAGS = 104;           // sub_5D0C0 returns if & 0x3800
	constexpr std::size_t PS_EFLAGS = 100;               // sub_5D0C0 tests & 0x100
	constexpr std::size_t PS_WEAPONS = 0x430;            // Weapon[15], stride 12
	constexpr std::size_t PS_WEAPON_SLOT_META = 0x4E4;   // 14 bytes * 15; byte[1] = equipped
	constexpr std::size_t PS_WEAPON_SLOT_META_STRIDE = 14;
	constexpr int PS_WEAPON_SLOT_COUNT = 15;
	constexpr std::size_t PS_WEAPON_HELD_ALT = 1464;     // Weapon[12]
	constexpr std::size_t PS_WEAPON_HELD = 1512;         // Weapon[12]
	constexpr std::size_t PS_WEAPON_FLAGS = 1524;
	constexpr std::size_t CG_WEAPON_SELECT = 2452280;    // Weapon[12] (cg+0x256B38)
	constexpr std::size_t CG_WEAPON_SELECT_ALT = 2452292;
	constexpr std::size_t CG_BLOCK_DRAW_VIEWMODEL = 0x2615F8;
	constexpr std::uint16_t WEAPON_NONE = 2047;
	constexpr std::size_t WEAPON_SIZE = 12;

	// IDA off_8A9C660 — WeaponDef* table indexed by weapon index (sub_3B6E40).
	inline void** weapon_def_table()
	{
		return reinterpret_cast<void**>(0x8A9B660_b); // IDA 0x8A9C660
	}

	inline void* weapon_def(const std::uint16_t weapon)
	{
		if (!weapon || weapon == WEAPON_NONE)
		{
			return nullptr;
		}
		return weapon_def_table()[weapon];
	}

	// Engine key-catcher bitmask (GameUtil::blockGameInput toggles bit 0 for the console;
	// LUI/menus set their own bits). Non-zero = something else owns the keyboard.
	inline int& key_catchers()
	{
		return *reinterpret_cast<int*>(0x1BAE4E0_b);
	}

	// CG_SelectWeapon (sub_51540): weaponSelectTime=cg.time, copies Weapon into
	// cg->weaponSelect, notify bit, clears cl latch. Takes Weapon*, not a packed uint.
	inline uintptr_t addr_CG_SelectWeapon() { return 0x50540_b; }                     // 0x51540
	inline uintptr_t addr_CG_UpdateLocalPlayerState() { return 0x4D620_b; }            // 0x4E620
	inline uintptr_t addr_CG_PredictPlayerState() { return 0x4DFA0_b; }                // 0x4EFA0
	inline uintptr_t addr_CL_GetDemoViewAnglesFromArchive() { return 0x464A70_b; }     // 0x465A70
	// S2 CG_CalcViewValues copies ps+576 into refdef angles (cg+2355472). H1 used
	// ps.delta_angles @ 0x12C — different engine. Theater overlays THIS field.
	constexpr std::size_t PS_DELTA_ANGLES = 300; // 0x12C — present but NOT the view driver on S2
	// PS_VIEWANGLES = 576 already defined above.
	inline int angle_to_short(const float angle)
	{
		return static_cast<std::uint16_t>(static_cast<int>(
			std::floor(static_cast<double>(angle) * (65536.0 / 360.0) + 0.5)));
	}

	// cg stride from CG_GetLocalClientGlobals (returns base + 4441600*client).
	constexpr std::ptrdiff_t CG_STRIDE = 4441600;
	// Renderer back-pointer → cg: *(0x8AFBB38) - 0x1E6C10 (GameUtil::CG_GetLocalClientGlobals).
	// NEVER call addr_CG_GetLocalClientGlobals() from the mod — Arxan retaddr checks
	// infinite-loop when the caller is outside the game image (ACTIVE DrawActiveFrame freeze).
	// Note: the renderer back-ptr is often unset at DrawActiveFrame *enter*; read after orig.
	inline void* cg_globals_for(const int client_num = 0)
	{
		const auto* renderer_data = reinterpret_cast<const std::uintptr_t*>(0x8AFBB38_b);
		const auto rd = *renderer_data;
		if (!rd || rd < 0x1E6C10)
		{
			return nullptr;
		}
		return reinterpret_cast<char*>(rd - 0x1E6C10) + CG_STRIDE * client_num;
	}
	inline uintptr_t addr_CG_ApplyFov() { return 0x49850_b; }                         // 0x4A850
	inline uintptr_t addr_AnglesToAxis() { return 0x75DE10_b; }                       // 0x75EE10 SIMD

	// Server-command walker (HIGH — twin of H1 FC3D0 / 11A410)
	//   wrapper loops cgs+24 → target, calling per-cmd executor
	//   executor: clc ring @ +0x2014C, lastExecuted dword @ +0x20148
	inline uintptr_t addr_CG_ExecuteNewServerCommands() { return 0x4340D0_b; }        // 0x4350D0
	inline uintptr_t addr_CG_ExecuteServerCommand() { return 0x4363F0_b; }            // 0x4373F0
	inline uintptr_t addr_CG_GetServerCommandsState() { return 0x451E0_b; }           // 0x461E0 Arxan; stride 94008

	// LUI / UI clocks (HIGH — "root"+"deltaTime"; UI_UpdateTime feeds LUI_Layout)
	// S2 signatures differ from H1 (extra args) — check demos before hooking.
	inline uintptr_t addr_LUI_Layout() { return 0xC4DD0_b; }                          // 0xC5DD0
	inline uintptr_t addr_UI_UpdateTime() { return 0x3185B0_b; }                      // 0x3195B0
	inline uintptr_t addr_LUI_ObituaryHandler() { return 0xC90F0_b; }                 // 0xCA0F0

	using CL_ParseServerMessage_fn = void (*)(int, msg_t*);
	inline void call_CL_ParseServerMessage(int client, msg_t* msg)
	{
		reinterpret_cast<CL_ParseServerMessage_fn>(addr_CL_ParseServerMessage())(client, msg);
	}

	using Sys_Milliseconds_fn = int (*)();
	inline int now_ms()
	{
		auto* fn = reinterpret_cast<Sys_Milliseconds_fn>(addr_Sys_Milliseconds());
		return fn ? fn() : 0;
	}

	inline int& server_time()
	{
		// Named cl_serverTime in IDB
		return *reinterpret_cast<int*>(0xC5FAA44_b); // IDA 0xC5FBA44
	}

	// Render thread (sub_6E1A00) Sleep-spins while latch!=0 until
	// cl_serverTime == this target (or sub_91440). Theater must keep them in sync
	// or SCR/DrawActiveFrame never runs (draw_probe enter=0, fence stuck).
	inline int& render_server_time_target()
	{
		return *reinterpret_cast<int*>(0xBC4FB1C_b); // IDA 0xBC50B1C
	}

	inline int& render_server_time_wait_latch()
	{
		return *reinterpret_cast<int*>(0xC60C8E0_b); // IDA 0xC60D8E0
	}

	// Refcount between render worker and DrawActiveFrame. Worker does ++ then
	// WaitForSingleObject(hEvent); DrawActiveFrame Sleeps while != 0.
	inline int& render_fence()
	{
		return *reinterpret_cast<int*>(0xFFC80E4_b); // IDA 0xFFC90E4
	}

	// Write cl_serverTime for theater and keep the render-thread waiter unblocked.
	inline void set_theater_server_time(const int t)
	{
		server_time() = t;
		render_server_time_target() = t;
		render_server_time_wait_latch() = 0;
		const int fence = render_fence();
		if (fence != 0 && fence != 1)
		{
			// Corrupted / underflowed fence leaves DrawActiveFrame in Sleep forever.
			render_fence() = 0;
		}
	}

	// Named clientConnectionState — dword array, client0 at this address.
	// Access pattern: clientConnectionState[494 * localClientNum]
	inline int& connstate()
	{
		return *reinterpret_cast<int*>(0x1BAE4E4_b); // IDA 0x1BAF4E4
	}

	inline int& sv_migrate()
	{
		return *reinterpret_cast<int*>(0xBB4EE68_b);
	}

	inline bool virtual_lobby_loaded()
	{
		return *reinterpret_cast<bool*>(0x1BD26F8_b);
	}

	// ---- clc (pointer named `clc` @ IDA 0x1BD3D00) -------------------------------
	// (char*)clc + 482656 * clientNum
	//
	// FULLY MAPPED 2026-08-17 -> re/clientConnection.h. The struct is built purely
	// from field offsets and comes to EXACTLY 482656 bytes -- the stride below,
	// derived independently -- which is the layout's own proof. Every VALUE in this
	// block was re-verified against it; several NAMES were wrong and are corrected
	// here. The values did not change, so this is a documentation fix only.
	constexpr std::size_t CLC_STRIDE = 482656;             // == sizeof(clientConnection_t)

	// PROVEN: reliableCommands[128][1024] spans 316..131388, so 0x2013C is the
	// first field past it.
	constexpr std::size_t CLC_SERVER_MESSAGE_SEQUENCE = 0x2013C;   // serverMessageSequence
	constexpr std::size_t CLC_INCOMING_MESSAGE_SEQUENCE = CLC_SERVER_MESSAGE_SEQUENCE;

	// ⭐ 0x20140 IS AN S2-ONLY FIELD WITH NO REFERENCE-BUILD COUNTERPART.
	// The AW->S2 offset shift is +8 immediately below it and +12 immediately above,
	// and the shift is monotonic (insertions only), so exactly one 4-byte field is
	// inserted here. That is why it could never be identified by analogy, and it is
	// the field the old implementation corrupted.
	// Its MEANING IS UNKNOWN. Never write it. The old name
	// (CLC_LAST_SNAPSHOT_SERVER_TIME) was an invented guess and is retired.
	constexpr std::size_t CLC_S2_ONLY_0x20140 = 0x20140;

	// CORRECTED: 0x20144 is serverCommandSequence, NOT reliableAcknowledge.
	// PROVEN in S2: svc 2 and svc 3 both do `if (x < seq) x = seq` on it, and the
	// reliable ring at 0x2014C is indexed `seq & 0x7F`.
	constexpr std::size_t CLC_SERVER_COMMAND_SEQUENCE = 0x20144;
	constexpr std::size_t CLC_LAST_EXECUTED_SERVER_COMMAND = 0x20148;
	constexpr std::size_t CLC_RELIABLE_COMMANDS = 0x2014C;         // serverCommands[128][1024]

	// ⛔ CORRECTED: 308 and 312 were SWAPPED. CL_Demo_ProcessPacket_Type2_3 is Q3's
	// server-message preamble verbatim:
	//     clc.reliableAcknowledge = MSG_ReadLong(msg);            <- 312
	//     if (reliableAcknowledge < reliableSequence - 128)        <- 308, and 128 IS
	//         reliableAcknowledge = reliableSequence;                 MAX_RELIABLE_COMMANDS
	constexpr std::size_t CLC_RELIABLE_SEQUENCE = 308;
	constexpr std::size_t CLC_RELIABLE_ACKNOWLEDGE = 312;

	// Back-compat aliases for existing call sites. The VALUES were always right;
	// only the labels were wrong, so nothing downstream changes behaviour.
	constexpr std::size_t CLC_CLIENT_RELIABLE_ACKNOWLEDGE = CLC_RELIABLE_SEQUENCE;
	constexpr std::size_t CLC_CLIENT_RELIABLE_SEQUENCE = CLC_RELIABLE_ACKNOWLEDGE;
	constexpr std::size_t CLC_SERVER_RELIABLE_SEQUENCE = CLC_RELIABLE_ACKNOWLEDGE;

	// cgs via CG_GetServerCommandsState — Arxan decrypt; do NOT read raw qword.
	constexpr std::size_t CGS_STRIDE = 94008;                    // HIGH GetServerCommandsState
	constexpr std::size_t CGS_SERVER_COMMAND_SEQUENCE = 24;      // HIGH ExecuteNew walker

	// ⭐ FIXED 2026-08-17 -- this read the WRONG GLOBAL for the life of the project.
	//
	//   0x1BD2C00_b == IDA 0x1BD3C00 == `off_1BD3C00`   3 xrefs, none client code
	//                                                   (sub_303D0, sub_3D6030, and
	//                                                    sub_E7980, the 2D HUD drawer)
	//   0x1BD2D00_b == IDA 0x1BD3D00 == `clc`           122 xrefs: CL_ParseSnapshot,
	//                                                   CL_ParseGamestate, CL_Disconnect,
	//                                                   CL_WritePacket, every CL_Demo_*
	//
	// The comment on the old line already SAID 0x1BD3D00; only the literal was wrong,
	// and it had been 0x100 too low since it was written. Consequences, both measured:
	//
	//   * demo_recording's pack_network() read the message sequence through it, so
	//     EVERY .dm_s2 record stored seq = 0. That is why the transcoder had to
	//     renumber packets, and why it got the numbering wrong until it was changed
	//     to recover the real sequence from the payload instead.
	//   * commit_live_message_state() and the feed in demo_utils.cpp restore
	//     serverMessageSequence and reliableAcknowledge around every replayed
	//     message -- a faithful transcription of sub_99960's live commit edge -- and
	//     those WRITES were landing in an unrelated object. CL_ParseServerMessage
	//     then read the real clc, which still held whatever it had. So the theater's
	//     sequence state never actually reached the engine.
	//
	// The field choices in that feed were correct all along (verified against
	// re/clientConnection.h); only the base and some constant NAMES were wrong.
	//
	// Store build: BuildMap.Store.inc already maps 0x1BD2D00 -> 0x187BF38, so the
	// port needs no change.
	inline char* clc_for(const int client_num = 0)
	{
		auto* base = *reinterpret_cast<char**>(0x1BD2D00_b); // IDA 0x1BD3D00 `clc`
		return base ? base + CLC_STRIDE * client_num : nullptr;
	}

	inline int& clc_server_message_sequence(const int client_num = 0)
	{
		return *reinterpret_cast<int*>(clc_for(client_num) + CLC_SERVER_MESSAGE_SEQUENCE);
	}

	inline int& clc_reliable_acknowledge(const int client_num = 0)
	{
		return *reinterpret_cast<int*>(clc_for(client_num) + CLC_RELIABLE_ACKNOWLEDGE);
	}

	inline int& clc_client_reliable_acknowledge(const int client_num = 0)
	{
		return *reinterpret_cast<int*>(clc_for(client_num) + CLC_CLIENT_RELIABLE_ACKNOWLEDGE);
	}

	inline int& clc_client_reliable_sequence(const int client_num = 0)
	{
		return *reinterpret_cast<int*>(clc_for(client_num) + CLC_CLIENT_RELIABLE_SEQUENCE);
	}

	// ---- gameState (contiguous) -------------------------------------------------
	// stringOffsets[7010] @ 0x2489350  (0x1B62 entries)
	// stringData[0x20000] @ 0x24900D8  (+0x6D88)
	// dataCount           @ 0x24B00D8  (+0x26D88)  — named cls_gameState_*
	constexpr int MAX_CONFIGSTRINGS = 0x1B62;
	constexpr std::size_t GS_STRING_DATA_OFF = 0x6D88;
	constexpr std::size_t GS_DATA_COUNT_OFF = 0x26D88;

	inline int* gs_string_offsets()
	{
		return reinterpret_cast<int*>(0x2488350_b); // IDA 0x2489350
	}

	inline char* gs_string_data()
	{
		return reinterpret_cast<char*>(0x248F0D8_b); // IDA 0x24900D8
	}

	inline int& gs_data_count()
	{
		return *reinterpret_cast<int*>(0x24AF0D8_b); // IDA 0x24B00D8
	}

	// Named cls_realtime @ IDA 0x1C7E1F0 — used by ParseSnapshot ping math
	inline int& cls_realtime()
	{
		return *reinterpret_cast<int*>(0x1C7D1F0_b);
	}

	// ---- helpers ----------------------------------------------------------------
	inline float read_f(const void* base, const std::size_t off)
	{
		float v{};
		std::memcpy(&v, static_cast<const char*>(base) + off, sizeof(v));
		return v;
	}

	inline void write_f(void* base, const std::size_t off, const float v)
	{
		std::memcpy(static_cast<char*>(base) + off, &v, sizeof(v));
	}

	inline void write_f3(void* base, const std::size_t off, const float v[3])
	{
		std::memcpy(static_cast<char*>(base) + off, v, sizeof(float) * 3);
	}

	inline int read_i(const void* base, const std::size_t off)
	{
		int v{};
		std::memcpy(&v, static_cast<const char*>(base) + off, sizeof(v));
		return v;
	}

	inline void write_i(void* base, const std::size_t off, const int v)
	{
		std::memcpy(static_cast<char*>(base) + off, &v, sizeof(v));
	}

	// WARNING: CL_GetLocalClientActive is Arxan retaddr/call-site gated. Calling it
	// from this DLL — even via an E8 thunk — soft-freezes (live dump: RIP inside
	// 0x795D0). ONLY use a pointer the game already passed into our hooks
	// (GetPredicted / SavePredicted). Never invoke the Arxan getter.
	inline void*& client_active_cache(const int client_num = 0)
	{
		static void* cache[2]{};
		return cache[client_num & 1];
	}

	inline void note_client_active(void* client_active, const int client_num = 0)
	{
		if (client_active)
		{
			client_active_cache(client_num) = client_active;
		}
	}

	// ⭐ CRASH FIX 2026-08-17. The cache is a raw pointer the ENGINE handed us, with
	// no ownership and no invalidation, and a null check is NOT enough:
	//
	//     mov  rcx, [client_active_cache]
	//     test rcx,rcx
	//     je   skip                       ; passes -- the pointer is non-null
	//     mov  eax, [rcx+6320h]           ; <-- ACCESS_VIOLATION, unmapped heap
	//
	// (0x6320 == CA_SNAP_FLAGS; that is clear_snap_not_active().) The sequence that
	// produced it: a rewind made the engine raise a Com_Error, the engine tore the
	// connection down and freed clientActive, and the very next frame we dereferenced
	// the freed pointer. Any engine teardown -- error, disconnect, map change --
	// leaves this dangling, so this is not specific to rewind.
	//
	// Validated with ONE VirtualQuery per epoch rather than per call. The epoch is
	// bumped once per client frame and on close, so in steady state this is a compare
	// and a return; the probe only re-runs when the frame changes or the pointer
	// does. That matters because this accessor is on hot paths (the feed gate runs
	// per rendered frame).
	//
	// The span covers every offset this file uses (CA_ARCHIVE_INDEX at 90472 is the
	// highest), so a partially-unmapped structure is rejected too.
	inline unsigned& client_active_epoch()
	{
		static unsigned epoch = 1;
		return epoch;
	}

	// Call once per client frame, and whenever the engine may have torn down.
	inline void invalidate_client_active()
	{
		++client_active_epoch();
	}

	inline void* client_active_for(const int client_num = 0)
	{
		void* p = client_active_cache(client_num);
		if (!p)
		{
			return nullptr;
		}
		const int slot = client_num & 1;
		static void* validated[2]{};
		static void* probed[2]{};
		static unsigned checked[2]{};
		const unsigned epoch = client_active_epoch();
		if (checked[slot] != epoch || probed[slot] != p)
		{
			checked[slot] = epoch;
			probed[slot] = p;
			MEMORY_BASIC_INFORMATION mbi{};
			constexpr std::size_t SPAN = CA_ARCHIVE_INDEX + 4;
			const bool ok = VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)
				&& mbi.State == MEM_COMMIT
				&& (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0
				&& (reinterpret_cast<const char*>(p) + SPAN)
					<= (static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize);
			validated[slot] = ok ? p : nullptr;
			if (!ok)
			{
				// Report once per stale pointer, not once per frame: the engine tears
				// down for legitimate reasons and this would otherwise flood.
				client_active_cache(client_num) = nullptr;
				Console::printf("[demo] clientActive %p is no longer mapped (engine "
					"teardown) -- dropping the cached pointer", p);
			}
		}
		return validated[slot];
	}

	inline int snap_server_time(const int client_num = 0)
	{
		if (const auto* cl = client_active_for(client_num))
		{
			return read_i(cl, CA_SNAP_SERVERTIME);
		}
		return 0;
	}

	inline void reset_snap_clock(const int client_num = 0)
	{
		set_theater_server_time(0);
		if (auto* cl = client_active_for(client_num))
		{
			write_i(cl, CA_SNAP_SERVERTIME, 0);
			write_i(cl, CA_SNAP_VALID, 0);
			write_i(cl, CA_NEW_SNAPSHOTS, 0);
			// Stale loopback values here make CL_SetCGameTime treat the first demo snapshot
			// as time running backwards (error 440) and skip CL_FirstSnapshot.
			write_i(cl, CA_OLD_SERVER_TIME, 0);
			write_i(cl, CA_OLD_FRAME_SERVER_TIME, 0);
			write_i(cl, CA_SERVER_TIME, 0);
			write_i(cl, CA_SERVER_TIME_DELTA, 0);
			write_i(cl, CA_EXTRAPOLATED_SNAPSHOT, 0);
		}
	}

	// Rewind/repeat is not a fresh process bootstrap: cls.realtime is already far
	// ahead.  Match the established H1 repeat lifecycle by rebasing every snapshot
	// clock to the demo's first server time and deriving serverTimeDelta from the
	// current real clock.  This keeps CL_SetCGameTime monotonic after the restart.
	inline void reset_snap_clock_for_rewind(const int restart_time,
		const int client_num = 0)
	{
		set_theater_server_time(restart_time);
		if (auto* cl = client_active_for(client_num))
		{
			write_i(cl, CA_SNAP_SERVERTIME, restart_time);
			write_i(cl, CA_SNAP_VALID, 0);
			write_i(cl, CA_NEW_SNAPSHOTS, 0);
			write_i(cl, CA_OLD_SERVER_TIME, restart_time);
			write_i(cl, CA_OLD_FRAME_SERVER_TIME, restart_time);
			write_i(cl, CA_SERVER_TIME, restart_time);
			write_i(cl, CA_SERVER_TIME_DELTA, restart_time - cls_realtime());
			write_i(cl, CA_EXTRAPOLATED_SNAPSHOT, 0);
		}
	}

	// CL_FirstSnapshot bails on SNAPFLAG_NOT_ACTIVE, which recorded connect-time snapshots
	// carry, leaving the client stuck in CA_PRIMED forever during replay.
	inline bool clear_snap_not_active(const int client_num = 0)
	{
		if (auto* cl = client_active_for(client_num))
		{
			const int flags = read_i(cl, CA_SNAP_FLAGS);
			if (flags & SNAPFLAG_NOT_ACTIVE)
			{
				write_i(cl, CA_SNAP_FLAGS, flags & ~SNAPFLAG_NOT_ACTIVE);
				return true;
			}
		}
		return false;
	}

	inline int archive_index(const void* client_active)
	{
		return read_i(client_active, CA_ARCHIVE_INDEX);
	}

	inline ClientArchiveEntry* archive_slot(void* client_active, const int idx)
	{
		if (!client_active)
		{
			return nullptr;
		}
		const auto i = static_cast<unsigned>(idx) & 255u;
		return reinterpret_cast<ClientArchiveEntry*>(
			static_cast<char*>(client_active) + CA_ARCHIVE + i * CA_ARCHIVE_STRIDE);
	}

	inline const ClientArchiveEntry* archive_slot(const void* client_active, const int idx)
	{
		return archive_slot(const_cast<void*>(client_active), idx);
	}

	// FOV cluster on cg (CG_ApplyFov): +4431560..+4431600
	// View block used by CG_CalcViewValues: refdef-ish @ cg+1993744, org @ +1993796/+1993852
	constexpr std::size_t CG_VIEW_ORG = 1993796;                 // HIGH CalcViewValues
	constexpr std::size_t CG_VIEW_ORG_LERP = 1993852;            // HIGH CalcEntityLerpOrigins
	constexpr std::size_t CG_FOV = 4431564;                      // HIGH ApplyFov

	// TODO(s2-re): IN_MouseMove — freecam mouse accumulator twin still open
	// TODO(s2-re): S2 has no Com_TimeScaleMsec string/symbol. Theater speed uses
	// demo_clock_ms * demotimescale, optional engine `timescale` dvar mirror, LUI deltas,
	// and clearing CA_NEW_SNAPSHOTS so AdjustTimeDelta cannot fight. Named CL_ClientFrame
	// hardcodes cl_serverTime += 50 — theater overwrites via set_theater_server_time.
	// NOTE: LUI_Layout / UI_UpdateTime wired in demo_timescale.cpp.
}
