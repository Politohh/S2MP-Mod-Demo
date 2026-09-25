#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace s2cinebot
{
	inline constexpr wchar_t kGameExe[] = L"s2_mp64_ship.exe";
	// Verified executable from the source handoff. The integrated client DLL
	// changes each package, so only the game executable is pinned here.
	inline constexpr char kGameSha256[] = "6E785BA25E0133BD3ADE87B74F4269116BEA93B21D861EFE9CDD6B37FA941734";

	inline constexpr std::uint32_t kGameTimeDateStamp = 0x6963A4D9;
	inline constexpr std::uint32_t kGameSizeOfImage = 0x12073200;

	namespace rva
	{
		inline constexpr std::uintptr_t RtlEnterCriticalSection = 0x7729B0;
		inline constexpr std::uintptr_t RtlLeaveCriticalSection = 0x772A20;
		inline constexpr std::uintptr_t CL_IsLocalClientInGame = 0x7E110;
		inline constexpr std::uintptr_t CL_GetViewPos = 0x7B8B0;
		inline constexpr std::uintptr_t CL_GetViewForward = 0x7B880;
		inline constexpr std::uintptr_t SV_Loaded = 0x6DB810;
		inline constexpr std::uintptr_t SV_AddBot = 0xF2650;
		inline constexpr std::uintptr_t SV_SpawnTestClient = 0xF6AA0;
		inline constexpr std::uintptr_t SV_SetAssignedTeam = 0x6E1410;
		inline constexpr std::uintptr_t G_SetPlayerOrigin = 0x548820;
		inline constexpr std::uintptr_t G_SetPlayerAngles = 0x548930;
		// Final native bot usercmd writer (verified in the matching runtime image).
		inline constexpr std::uintptr_t SV_BotBuildUsercmd = 0x69E970;
		inline constexpr std::uintptr_t SV_LinkEntity = 0x6F97E0;
		inline constexpr std::uintptr_t SV_IsTestClient = 0x6C4160;
		inline constexpr std::uintptr_t SV_RefreshTestClient = 0x6C1E00;
		inline constexpr std::uintptr_t G_LocationalTrace = 0x5609B0;
		inline constexpr std::uintptr_t Scr_AddInt = 0x68FB50;
		inline constexpr std::uintptr_t Scr_AddString = 0x68FC30;
		inline constexpr std::uintptr_t Scr_Notify = 0x5B9D90;
		inline constexpr std::uintptr_t SL_GetString = 0x6891F0;

		inline constexpr std::uintptr_t cmd_funcCount = 0xAA764C8;
		inline constexpr std::uintptr_t cmd_funcArray = 0xAA764D0;
		inline constexpr std::uintptr_t virtualLobby_Loaded = 0x1BD36F8;
		inline constexpr std::uintptr_t sv_maxclients = 0xC5FBA50;
		inline constexpr std::uintptr_t svs_clients = 0xC5FBA58;
		inline constexpr std::uintptr_t g_entities = 0x9ED4430;
		// Key_SetBinding uses this 16-byte-per-key binding table for local client 0.
		inline constexpr std::uintptr_t key_bindings = 0x8B90FA0;
	}

	inline constexpr std::size_t kClientStride = 0x11E870;
	inline constexpr std::size_t kClientStateOffset = 0x0;
	inline constexpr std::size_t kClientEntityOffset = 0x41DF0;
	inline constexpr std::size_t kClientTestClientOffset = 0x42158;
	inline constexpr std::size_t kEntityStride = 0x418;
	inline constexpr std::size_t kEntityClientOffset = 0x258;
	inline constexpr std::size_t kEntityHealthOffset = 0x2DC;
	inline constexpr std::size_t kEntityOriginOffset = 0x234;
	inline constexpr std::size_t kGameClientTeamOffset = 0x5AB8;
	inline constexpr std::size_t kGameClientFlagsOffset = 0x5DFC;
}
