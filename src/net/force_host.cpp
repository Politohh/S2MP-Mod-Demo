#include "pch.h"
#include "force_host.hpp"

#include "Console.hpp"
#include "DevMode.hpp"
#include "GameUtil.hpp"
#include "Hook.hpp"
#include "ModPaths.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace force_host
{
	namespace
	{
		// ---- addresses -----------------------------------------------------
		// Arithmetic written out per RULE A1. `_b(x)` == module_base + 0x1000 + x,
		// so every literal below is (IDA address - 0x1000).
		//
		//   lobby 0                 IDA 0x8BE6040 - 0x1000 = 0x8BE5040
		//   g_hostingEnabled        IDA 0x0F9E9D8 - 0x1000 = 0x0F9D9D8
		//   playlist table row 0    IDA 0xABDE010 - 0x1000 = 0xABDD010
		//   map name ptr table      IDA 0xABF1B50 - 0x1000 = 0xABF0B50
		//   map name count (byte)   IDA 0xABF1F50 - 0x1000 = 0xABF0F50
		//   gametype record table   IDA 0xABF1F60 - 0x1000 = 0xABF0F60
		//   sub_6563D0 apply pl.    IDA 0x06563D0 - 0x1000 = 0x06553D0
		//   Bots_SetTeamLimit       IDA 0x038E350 - 0x1000 = 0x038D350
		//   Bots_GetTeamLimit       IDA 0x03882C0 - 0x1000 = 0x03872C0
		//   dvar_sv_maxclients      IDA 0x0C60D8E8 - 0x1000 = 0x0C60C8E8
		//   dvar_com_sv_running     IDA 0x01BD3778 - 0x1000 = 0x01BD2778
		//   svs.clientCount         IDA 0x0C5FBA50 - 0x1000 = 0x0C5FAA50
		constexpr std::size_t ADDR_LOBBY0        = 0x8BE5040;
		// sub_470CB0 "which lobby is active"  IDA 0x470CB0 - 0x1000 = 0x46FCB0
		constexpr std::size_t ADDR_LOBBY_ACTIVE  = 0x46FCB0;
		// PartyHost_KickPlayer                 IDA 0x48CD70 - 0x1000 = 0x48BD70
		constexpr std::size_t ADDR_PARTY_KICK    = 0x48BD70;
		// GetLoadoutStatsGroupForGameMode      IDA 0x654100 - 0x1000 = 0x653100
		constexpr std::size_t ADDR_STATS_GROUP   = 0x653100;
		// clientConnectionState                IDA 0x1BAF4E4 - 0x1000 = 0x1BAE4E4
		constexpr std::size_t ADDR_CONNSTATE     = 0x1BAE4E4;
		// UsingRankedStatsGroup                IDA 0x654630 - 0x1000 = 0x653630
		constexpr std::size_t ADDR_USING_RANKED  = 0x653630;
		constexpr std::size_t LOBBY_STRIDE       = 1640240;   // 205030 qwords
		// 0 = game lobby, 1 = the sub_470CB0 alternate, 2 = private party.
		constexpr int         LOBBY_SLOTS        = 3;
		// The engine's real per-match client count. See set_max_players.
		constexpr int         MATCH_MAX_CLIENTS  = 18;
		constexpr std::size_t ADDR_HOSTING_EN    = 0xF9D9D8;
		constexpr std::size_t ADDR_PL_TABLE      = 0xABDD010;
		constexpr std::size_t ADDR_MAPNAME_TABLE = 0xABF0B50;
		constexpr std::size_t ADDR_MAPNAME_COUNT = 0xABF0F50;
		constexpr std::size_t ADDR_GT_TABLE      = 0xABF0F60;
		constexpr std::size_t ADDR_APPLY_PL      = 0x6553D0;
		constexpr std::size_t ADDR_BOTS_SET      = 0x38D350;
		constexpr std::size_t ADDR_BOTS_GET      = 0x3872C0;
		constexpr std::size_t ADDR_DVAR_SVMAX    = 0xC60C8E8;
		constexpr std::size_t ADDR_DVAR_SVRUN    = 0x1BD2778;
		constexpr std::size_t ADDR_SVS_CLIENTCNT = 0xC5FAA50;

		// ---- lobby field offsets (bytes from the lobby base) ---------------
		constexpr std::size_t OFF_MINPLAYERS   = 592;
		constexpr std::size_t OFF_MAXPLAYERS   = 596;
		constexpr std::size_t OFF_UI_MAPNAME   = 616;
		constexpr std::size_t OFF_UI_GAMETYPE  = 624;
		constexpr std::size_t OFF_PRIVATE      = 632;
		constexpr std::size_t OFF_PLAYLIST     = 636;
		constexpr std::size_t OFF_REQ_OPEN_NAT = 644;   // params[13]
		constexpr std::size_t OFF_ALLOW_LISTEN = 652;
		constexpr std::size_t OFF_HAVE_SESSION = 1598512;
		constexpr std::size_t OFF_WE_HOST      = 1598540;
		constexpr std::size_t OFF_DEDICATED    = 1598544;

		// ---- playlist table layout -----------------------------------------
		constexpr std::size_t PL_STRIDE     = 568;
		constexpr std::size_t PL_COUNT      = 100;
		constexpr std::size_t PL_OFF_NAME   = 0;
		constexpr std::size_t PL_OFF_ICON   = 480;
		constexpr std::size_t PL_OFF_VALID  = 542;   // u16, non-zero == real row

		constexpr std::size_t GT_STRIDE = 216;
		constexpr std::size_t GT_COUNT  = 38;

		// ---- the MASTER map registry -----------------------------------
		//
		// Found 2026-09-15 chasing a report that "mp_house" (Groesten
		// House) is missing from the private-match map list. It exists
		// as a fully compiled base-game map -- mp_house.ff / _load /
		// _path are all present on disk, same as any other core MP map --
		// but it is not one of the ~46 entries in ADDR_MAPNAME_TABLE
		// above, which is the playlist-facing list.
		//
		// This is a SEPARATE, bigger table the engine also carries:
		//
		//   map registry table   IDA 0xBA25D0 - 0x1000 = 0xBA15D0
		//
		// {const char* name; u64 tag} pairs, stride 16. Confirmed in the
		// static IDB as a clean, contiguous run of 52 entries running from
		// mp_vlobby_room (tag 0x1...01) to mp_zombie_descent (tag
		// 0x6...37) -- every map the engine has ever registered, not just
		// the ones a playlist can select. The low dword of `tag` is the
		// map's numeric ID (the same ID the playlist rotation tables
		// reference by index); the high byte varies 1-6 and tracks
		// release wave, not game mode. Neither is needed here -- only
		// the name is read, walked live until an entry stops looking
		// like a real map (unreadable, or the string does not start
		// "mp_"), capped generously in case a live read ever disagrees
		// with the static one.
		constexpr std::size_t ADDR_MAP_REGISTRY = 0xBA15D0;
		constexpr std::size_t MAPREG_STRIDE     = 16;
		constexpr int         MAPREG_MAX        = 128;

		// ---- config ---------------------------------------------------------
		bool        g_enabled = false;
		int         g_play_pl = -1;
		int         g_search_pl = -1;
		std::string g_map;
		std::string g_gametype;
		int         g_max = 18;
		int         g_min = 1;

		// ---- runtime bookkeeping -------------------------------------------
		std::string g_last_display;      // what we last pushed to playlist_name
		DWORD       g_last_display_ms = 0;
		int         g_last_applied_pl = -1;
		bool        g_warned_lobby = false;
		// The playlist the ENGINE path was last run for, so a change triggers it
		// exactly once. -1 == never / re-arm.
		int         g_engine_applied_pl = -1;
		bool        g_auto_apply = true;

		// -------------------------------------------------------------------
		// RULE A6: a global belonging to an idle subsystem can hold junk, so a
		// null check is not enough before dereferencing.
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
			return reinterpret_cast<std::uintptr_t>(p) + n <= start + mbi.RegionSize;
		}

		[[nodiscard]] std::string read_cstr(const char* p, const std::size_t cap)
		{
			std::string out;
			if (!readable(p, 1))
			{
				return out;
			}
			for (std::size_t i = 0; i < cap; ++i)
			{
				if (!readable(p + i, 1))
				{
					break;
				}
				const auto c = static_cast<unsigned char>(p[i]);
				if (c == 0)
				{
					break;
				}
				if (c >= 0x20 && c < 0x7F)
				{
					out.push_back(static_cast<char>(c));
				}
			}
			return out;
		}

		// The master registry genuinely mixes real competitive maps in with
		// hub/lobby zones, training ranges, and Zombies/Raid content that
		// needs its own dedicated gametype to even load correctly. Exclude
		// those by name so a private-match "pick any map" list stays to
		// maps that work with an ordinary gametype.
		[[nodiscard]] bool looks_like_a_real_map(const std::string& name)
		{
			static const char* const excluded[] =
			{
				"hub", "vlobby", "training", "zombie", "raid", "_srv",
			};
			for (const auto* bad : excluded)
			{
				if (name.find(bad) != std::string::npos)
				{
					return false;
				}
			}
			return true;
		}

		// Everything the ADDR_MAPNAME_TABLE above does not carry -- see the
		// ADDR_MAP_REGISTRY comment for what this is and how it was found.
		[[nodiscard]] std::vector<std::string> read_map_registry()
		{
			std::vector<std::string> out;
			const auto* base = reinterpret_cast<const std::uint8_t*>(_b(ADDR_MAP_REGISTRY));
			for (int i = 0; i < MAPREG_MAX; ++i)
			{
				const auto* entry = base + static_cast<std::size_t>(i) * MAPREG_STRIDE;
				if (!readable(entry, MAPREG_STRIDE))
				{
					break;
				}
				const auto name_ptr = *reinterpret_cast<const char* const*>(entry);
				auto s = read_cstr(name_ptr, 48);
				// The table runs clean and contiguous; the first entry that
				// does not start "mp_" means we have walked off its end.
				if (s.compare(0, 3, "mp_") != 0)
				{
					break;
				}
				out.push_back(std::move(s));
			}
			return out;
		}

		[[nodiscard]] std::uint8_t* lobby(const int slot = 0)
		{
			auto* p = reinterpret_cast<std::uint8_t*>(
				_b(ADDR_LOBBY0) + static_cast<std::size_t>(slot) * LOBBY_STRIDE);
			// Validate the FAR offsets too: this object spans 1.6 MB and the
			// session block we read lives right at the top of it.
			return readable(p, OFF_DEDICATED + 4) ? p : nullptr;
		}

		// ⭐ THE ACTIVE GAME LOBBY IS NOT ALWAYS LOBBY 0.
		//
		//   sub_470CB0():
		//       if (!dword_8EFCBC0 || (dword_8D6C490 && sub_481690(&lobby0)))
		//            return &lobby0;
		//       else return &s_lobby1;
		//
		// MEASURED IN GAME 2026-08-13 with the game sitting in the hub:
		//       lobby0 + 1598540 (AreWeHost) = 0
		//       lobby1 + 1598540 (AreWeHost) = 1
		// i.e. the live session was on LOBBY 1 while everything here read
		// LOBBY 0. Every param this module latches -- hosting, min/max players,
		// allowJoiningListenServer, requireOpenNat, playlist -- was therefore
		// being written to an object the engine was not using.
		//
		// Call the engine's own selector rather than reimplementing it, so the
		// choice cannot drift from the engine's. It returns a POINTER TO THE
		// SLOT (&off_8BE6040), so it must be dereferenced once.
		using LobbyGetActive_t = void** (__fastcall*)();

		[[nodiscard]] std::uint8_t* active_lobby()
		{
			static LobbyGetActive_t fn = nullptr;
			if (!fn)
			{
				fn = reinterpret_cast<LobbyGetActive_t>(_b(ADDR_LOBBY_ACTIVE));
			}
			auto* slot = reinterpret_cast<std::uint8_t*>(fn());
			if (!slot || !readable(slot, OFF_DEDICATED + 4))
			{
				return lobby(0);              // fall back rather than fail
			}
			return slot;
		}

		// Which index did the engine pick? Report-only, for diagnostics.
		[[nodiscard]] int active_lobby_index()
		{
			const auto* a = active_lobby();
			const auto* l0 = reinterpret_cast<const std::uint8_t*>(_b(ADDR_LOBBY0));
			if (!a || !l0)
			{
				return -1;
			}
			return static_cast<int>((a - l0) / LOBBY_STRIDE);
		}

		[[nodiscard]] std::string interned(const std::uint8_t* L, const std::size_t off)
		{
			if (!L)
			{
				return {};
			}
			const auto* s = *reinterpret_cast<const char* const*>(L + off);
			return read_cstr(s, 96);
		}

		// The engine accepts these from the console, and Cmd_ExecuteString feeds
		// EVERY command to Party_SetLobbyParamFromCmd before dispatch -- so this
		// is the engine's own path onto the engine's own thread. It is also the
		// only safe way to touch the interned-string pool from our tick, which
		// runs on the Present thread.
		void cbuf(const std::string& cmd)
		{
			GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, cmd);
		}

		// ===================================================================
		//  ⭐ THE HOST GATE — Session_CanHostServer @ IDA 0x8534B0
		//
		//  Decompiled in full 2026-08-11. This is what actually decides whether
		//  the GSC lobby state machine is ALLOWED to host, and every earlier
		//  force-host attempt latched things DOWNSTREAM of it:
		//
		//    1. systemlink                  -> return 1   (takes us offline; unusable)
		//    2. !g_hostingEnabled           -> 0          (we re-assert this)
		//    3. requireOpenNat && nat == 3  -> 0          <- params[13], OURS TO CLEAR
		//    4. !bandwidthTestDone          -> CLEARS g_hostingEnabled, return 0
		//    5. !private && !dvar"393":
		//         requiredUpload(maxplayers) > measured -> 0
		//         requiredPing  (maxplayers) > measured -> 0
		//    6. return 1
		//
		//  ⚠ STEP 5 IS KEYED ON maxplayers, AND THE BAR RISES WITH IT:
		//
		//      <=2   "357"   64000     ping: none (0.0f)
		//      <=4   "2237"  128000    ping: "1925"
		//      <=8   "2871"  256000    ping: "1925"
		//      <=12  "3829"  576000    ping: "2737"
		//      <=18  "889"   725000    ping: "3819"     <- WHERE OUR OWN LATCH PUT US
		//      >18   "4342"  900000    ping: "134"
		//
		//  So force_host latching maxplayers to 18 was RAISING the host bar to
		//  725 kbps measured upload. If the engine's own bandwidth test does not
		//  clear that, CanHostServer returns 0, the script never hosts, and the
		//  lobby search puts you in someone else's game -- exactly the symptom.
		//
		//  All ten thresholds are registered by Com_InitDvars with flags 0, i.e.
		//  ORDINARY SETTABLE DVARS (not the 0x2000 engine-owned class). Setting
		//  them is using the engine's own tunables through the engine's own
		//  console path -- nothing is fabricated and no state is manufactured.
		//
		//  dvar "393" (off_1BD3708) also waives step 5, but it is written FROM THE
		//  GAMESTATE in CL_ParseGamestate and cleared by UI_SetActiveMenu, so it
		//  is server-controlled and NOT ours to use.
		//
		//  RULE A1 — arithmetic written out. `_b` literal = IDA - 0x1000.
		//    Session_CanHostServer  0x8534B0  - 0x1000 = 0x8524B0
		//    measured upload        0x789860  - 0x1000 = 0x788860
		//    measured ping          0x7890B0  - 0x1000 = 0x7880B0
		//    bandwidth-test object  0x10DC800 - 0x1000 = 0x10DB800
		//      done byte = object + 100264 (0x187A8) -> IDA 0x10F4FA8
		//    threshold dvar pointers 0x14DBC40..0x14DBC88 - 0x1000
		// ===================================================================
		struct Threshold
		{
			const char*   dvar;      // S2 names dvars numerically
			std::size_t   ptr;       // _b address of the dvar_t*
			bool          is_float;
			const char*   what;
		};

		constexpr Threshold QUAL[] = {
			{ "357",  0x14DAC40, false, "upload <=2"  },
			{ "2237", 0x14DAC48, false, "upload <=4"  },
			{ "2871", 0x14DAC50, false, "upload <=8"  },
			{ "3829", 0x14DAC58, false, "upload <=12" },
			{ "889",  0x14DAC60, false, "upload <=18" },
			{ "4342", 0x14DAC68, false, "upload >18"  },
			{ "1925", 0x14DAC70, true,  "ping <=8"    },
			{ "2737", 0x14DAC78, true,  "ping <=12"   },
			{ "3819", 0x14DAC80, true,  "ping <=18"   },
			{ "134",  0x14DAC88, true,  "ping >18"    },
		};

		// A dvar_t*: value lives at +16 (proven from Dvar_SetBool @0xB1FD0,
		// which reads dvar+12 as the type).
		[[nodiscard]] const void* dvar_at(const std::size_t slot)
		{
			auto* pp = reinterpret_cast<void**>(_b(slot));
			if (!readable(pp, 8))
			{
				return nullptr;
			}
			auto* d = *pp;
			return readable(d, 24) ? d : nullptr;
		}

		[[nodiscard]] bool qual_value(const Threshold& t, double& out)
		{
			const auto* d = dvar_at(t.ptr);
			if (!d)
			{
				return false;
			}
			const auto* v = static_cast<const unsigned char*>(d) + 16;
			out = t.is_float ? static_cast<double>(*reinterpret_cast<const float*>(v))
				: static_cast<double>(*reinterpret_cast<const std::int32_t*>(v));
			return true;
		}

		bool  g_relax_qual = true;
		DWORD g_last_qual_ms = 0;
		std::uint64_t g_hosting_reasserts = 0;
		DWORD g_last_reassert_report = 0;

		// Compare-then-write, so in steady state this issues nothing at all.
		void relax_qualification()
		{
			for (const auto& t : QUAL)
			{
				double v = 0.0;
				if (qual_value(t, v) && v != 0.0)
				{
					cbuf(std::string(t.dvar) + " 0");
				}
			}
		}

		[[nodiscard]] bool platform_session_ready()
		{
			const auto* p = reinterpret_cast<const unsigned char*>(_b(0x10F3FA8));
			return readable(p, 1) && *p != 0;
		}

		// ===================================================================
		//  ⭐ THE JOIN GATE — Lobby_AcceptSearchResult @ IDA 0x29BFF0
		//
		//  This is the piece that makes you the LOBBY HOST, and it is what the
		//  user's own earlier tool relied on: refuse every search result, the
		//  matchmaker runs out of candidates, and the engine makes you host
		//  instead. Relaxing Session_CanHostServer only grants PERMISSION to
		//  host; nothing in that path stops matchmaking placing you in someone
		//  else's lobby first. This does.
		//
		//  RULE A17 — the exact signature, from the S2 decompile:
		//      bool __fastcall sub_29BFF0(__int64 a1, unsigned int a2, char a3)
		//  Return is BOOL. Do not widen it.
		//
		//  RULE A19 — a hook is never truly passive, so this target was chosen
		//  deliberately: the function is a PURE PREDICATE with no side effects.
		//  Its whole body reads the result status at +84, a per-client byte and
		//  a dvar, and returns `v9 >= 5 && v10 && v8 == 1`. Returning false is
		//  exactly "do not accept this candidate" and nothing else unwinds.
		//
		//  RULE A1 — arithmetic written out:
		//      Lobby_AcceptSearchResult  0x29BFF0 - 0x1000 = 0x29AFF0
		//  RULE A3.1 — checked: nothing else in src/ hooks this address.
		//
		//  ⚠ SAFETY. Hooks on the matchmaking path once broke matchmaking
		//  outright, and the culprit was never isolated. So this one:
		//    * installs only behind the force-host toggle being ON,
		//    * DISARMS ITSELF the moment it succeeds (we become host, or a
		//      session exists),
		//    * DISARMS ITSELF after a timeout so it can never leave the user
		//      unable to find any game at all, and says so in plain language,
		//    * can be killed entirely with s2mp_nojoingate.txt next to the exe.
		//  Worst case is therefore "you get a normal match", never "stuck".
		// ===================================================================
		constexpr std::size_t ADDR_ACCEPT_RESULT = 0x29AFF0;
		constexpr DWORD       JOINGATE_TIMEOUT_MS = 90000;   // 90 s then give up

		using AcceptResult_fn = bool(__fastcall*)(std::int64_t, unsigned int, char);
		AcceptResult_fn AcceptResult_orig = nullptr;

		bool                  g_joingate_installed = false;
		std::atomic<bool>     g_joingate_armed{ false };
		std::atomic<uint32_t> g_joingate_refused{ 0 };
		DWORD                 g_joingate_armed_ms = 0;
		bool                  g_joingate_reported_off = false;
		bool                  g_joingate_gave_up = false;

		bool __fastcall accept_result_stub(const std::int64_t a1, const unsigned int a2,
			const char a3)
		{
			const bool real = AcceptResult_orig(a1, a2, a3);
			if (!real || !g_joingate_armed.load(std::memory_order_relaxed))
			{
				return real;      // not a candidate anyway, or we are not arming
			}
			g_joingate_refused.fetch_add(1, std::memory_order_relaxed);
			return false;         // refuse: do not join this lobby
		}

		void install_join_gate()
		{
			// Per-build: the Store package directory is not writable, so markers
			// live with the rest of the mod data there. Steam is unchanged.
			if (mod_paths::marker_present("s2mp_nojoingate.txt"))
			{
				Console::printf("[host] join gate DISABLED by s2mp_nojoingate.txt — "
					"force host will not be able to make you the lobby host.");
				return;
			}
			// RULE A3 — a hook is not installed until it says so, and a non-null
			// original is the only real proof (Hook::create reports duplicates now).
			g_joingate_installed = Hook::create("Lobby_AcceptSearchResult",
				_b(ADDR_ACCEPT_RESULT), &accept_result_stub, &AcceptResult_orig)
				&& AcceptResult_orig != nullptr;
			Console::printf("[host] join gate hook: %s (orig=%p)",
				g_joingate_installed ? "OK" : "FAILED",
				reinterpret_cast<void*>(AcceptResult_orig));
		}

		// -------------------------------------------------------------------
		//  The playlist table, read live, using the ENGINE's own walk.
		//
		//  sub_6554E0(client, ordinal):
		//      v3 = 0; v15 = 0; v14 = &row0.valid;
		//      while (!*v14) { ++v15; v14 += 284 words; if (past end) fail; }
		//      if (v3 != ordinal) { ++v3; continue; }
		//      return v15;                       // the table ROW
		//
		//  i.e. the id is the ordinal over rows whose validity word is set.
		[[nodiscard]] std::vector<Playlist> read_playlists()
		{
			std::vector<Playlist> out;
			const auto* base = reinterpret_cast<const std::uint8_t*>(_b(ADDR_PL_TABLE));
			if (!readable(base, PL_STRIDE * PL_COUNT))
			{
				return out;
			}
			for (std::size_t i = 0; i < PL_COUNT; ++i)
			{
				const auto* row = base + PL_STRIDE * i;
				if (*reinterpret_cast<const std::uint16_t*>(row + PL_OFF_VALID) == 0)
				{
					continue;
				}
				auto name = read_cstr(reinterpret_cast<const char*>(row + PL_OFF_NAME), 64);
				if (name.empty())
				{
					// Valid but unnamed: still occupies an ordinal, so it must be
					// listed or every later id would shift. Say so rather than
					// silently dropping it.
					name = "(unnamed)";
				}
				out.push_back({ static_cast<int>(out.size()), static_cast<int>(i), std::move(name) });
			}
			return out;
		}

		[[nodiscard]] const std::uint8_t* playlist_row(const int id)
		{
			const auto* base = reinterpret_cast<const std::uint8_t*>(_b(ADDR_PL_TABLE));
			if (id < 0 || !readable(base, PL_STRIDE * PL_COUNT))
			{
				return nullptr;
			}
			int ordinal = 0;
			for (std::size_t i = 0; i < PL_COUNT; ++i)
			{
				const auto* row = base + PL_STRIDE * i;
				if (*reinterpret_cast<const std::uint16_t*>(row + PL_OFF_VALID) == 0)
				{
					continue;
				}
				if (ordinal == id)
				{
					return row;
				}
				++ordinal;
			}
			return nullptr;
		}

		// sv_maxclients, live. S2 dvar layout: +12 type, +16 value.
		[[nodiscard]] int sv_maxclients()
		{
			const auto* slot = reinterpret_cast<const std::uint8_t* const*>(_b(ADDR_DVAR_SVMAX));
			if (!readable(slot, sizeof(void*)) || !*slot || !readable(*slot, 20))
			{
				return -1;
			}
			return *reinterpret_cast<const std::int32_t*>(*slot + 16);
		}

		// svs.clientCount -- the ALLOCATED server slot count, sized once at
		// SV_Init and never grown afterwards except through SV_ChangeMaxClients.
		[[nodiscard]] int svs_client_count()
		{
			const auto* p = reinterpret_cast<const std::int32_t*>(_b(ADDR_SVS_CLIENTCNT));
			return readable(p, 4) ? *p : -1;
		}

		[[nodiscard]] bool server_running()
		{
			const auto* slot = reinterpret_cast<const std::uint8_t* const*>(_b(ADDR_DVAR_SVRUN));
			if (!readable(slot, sizeof(void*)) || !*slot || !readable(*slot, 17))
			{
				return false;
			}
			return *(*slot + 16) != 0;   // S2 dvar layout: +12 type, +16 value
		}

		// -------------------------------------------------------------------
		[[nodiscard]] bool have_session(const std::uint8_t* L)
		{
			return L && *reinterpret_cast<const std::int32_t*>(L + OFF_HAVE_SESSION) != 0;
		}

		// "search" while the lobby has no session yet -- that is exactly the test
		// the LUI binding IsSearching uses (sub_335270: !lobby[1598512] && !private).
		[[nodiscard]] const char* phase_of(const std::uint8_t* L)
		{
			if (!g_enabled)
			{
				return "off";
			}
			if (g_search_pl < 0)
			{
				return "deploy";   // one playlist for both; there is no phase
			}
			return have_session(L) ? "deploy" : "search";
		}

		void push_display(const int id)
		{
			const auto* row = playlist_row(id);
			if (!row)
			{
				return;
			}
			const auto name = read_cstr(reinterpret_cast<const char*>(row + PL_OFF_NAME), 64);
			const auto icon = read_cstr(reinterpret_cast<const char*>(row + PL_OFF_ICON), 64);
			if (name.empty())
			{
				return;
			}
			// Both are Dvar_SetString targets in sub_6563D0's tail:
			//   sub_B3070("playlist_name", row + 0)
			//   sub_B3070("1676",          row + 480)
			cbuf("playlist_name \"" + name + "\"");
			if (!icon.empty())
			{
				cbuf("1676 \"" + icon + "\"");
			}
			g_last_display = name;
			g_last_display_ms = GetTickCount();
		}

		// -------------------------------------------------------------------
		// -------------------------------------------------------------------
		//  THE LOBBY MEMBER ARRAY -- every offset proven, not inferred.
		//
		//  sub_49E3C0 returns  lobby + 1016 + 33216 * memberIndex
		//      -> member base 1016, stride 33216.
		//  sub_481260 gates on *(BYTE*)(lobby + 1016 + 33216*i) and reads the
		//      id at *(QWORD*)(lobby + 21568 + 33216*i).
		//  sub_1912F0 walks i in 0..47 requiring state >= 5, matches that id,
		//      and returns  lobby + 21634 + 33216*i  -- the per-member NAME.
		//      Its `if (++v2 >= 48)` is where the 48 comes from.
		// -------------------------------------------------------------------
		constexpr std::size_t MEMBER_STRIDE = 33216;
		constexpr std::size_t MEMBER_STATE  = 1016;
		constexpr std::size_t MEMBER_XUID   = 21568;
		constexpr std::size_t MEMBER_NAME   = 21634;
		constexpr int         MEMBER_MAX    = 48;
		constexpr int         MEMBER_CONNECTED = 5;   // state >= 5

		// PartyHost_KickPlayer  IDA 0x48CD70 - 0x1000 = 0x48BD70
		// RULE A17: __int64 __fastcall(_QWORD* lobby, unsigned __int8 member).
		using PartyHostKick_t = std::int64_t(__fastcall*)(void*, unsigned char);

		// -------------------------------------------------------------------
		//  OUR OWN ACCOUNT ID -- read from the engine, NOT assumed to be member 0.
		//
		//  ⛔ THE FIRST VERSION HARDCODED MEMBER 0:
		//        const auto* p = L + 21568;   // "the local client is member 0
		//                                     //  when we are the host"
		//     That assumption is exactly what this project keeps getting burned
		//     by. If we are NOT member 0 it returns SOMEONE ELSE'S id, so is_me
		//     marks the wrong player as "you" -- the tool then REFUSES to kick
		//     that innocent person and HAPPILY LETS YOU KICK YOURSELF. Backwards
		//     in both directions.
		//
		//  THE ENGINE'S OWN ANSWER:
		//     sub_856250(localClient)  is a thunk to
		//     sub_789870(localClient) = *((_QWORD*)&unk_D8ACED8 + 13 * client)
		//     i.e. a plain global array, 13 qwords (104 bytes) per local client.
		//     No retaddr check, no Arxan (RULE A22 checked) -- so we read the
		//     global directly rather than calling it.
		//
		//  SAME ID SPACE, PROVEN: sub_4813D0 takes sub_856250()'s return and
		//  searches the member array for `id@member+21568 == id`. The engine
		//  itself compares these two fields, so they are the same value.
		//
		//  Corroborated independently: CL_Demo_StartRecord writes
		//  sub_789870(client) into the demo header at +0x10, which is the field
		//  that differs between the private demos and publicmatch.
		//
		//  RULE A1:  unk_D8ACED8  IDA 0xD8ACED8 - 0x1000 = 0xD8ABED8
		// -------------------------------------------------------------------
		constexpr std::uintptr_t ADDR_LOCAL_XUID = 0xD8ABED8;
		constexpr std::size_t    LOCAL_XUID_STRIDE = 104;   // 13 qwords

		// 0 means "cannot identify us", which DISABLES the self-kick guard
		// rather than guessing at a slot. Callers report that plainly.
		[[nodiscard]] std::uint64_t my_xuid()
		{
			// Local client 0 -- the primary, as everywhere else in this codebase.
			const auto* p = reinterpret_cast<const std::uint64_t*>(
				_b(ADDR_LOCAL_XUID) + LOCAL_XUID_STRIDE * 0);
			return readable(p, 8) ? *p : 0;
		}

		void cmd_lobby_list();
		void cmd_lobby_kick();
		void cmd_status();

		// -------------------------------------------------------------------
		//  WHICH STATS GROUP AM I IN?  -- read-only, no writes.
		//
		//  This is what decides whether a match counts toward your public rank,
		//  and it is NOT the dedicated-server question. Decompiled:
		//
		//    GetLoadoutStatsGroupForGameMode  sub_654100
		//        if (sub_B8B60())                       return 3;
		//        if (sub_28D5C0() || sub_28D5D0())      return 2;
		//        if (sub_151720())                      return 9;
		//        online = Com_IsOnlineGame();
		//        if ( dedicatedFlag(lobby) && private(lobby) ) online = 0;
		//        if ((v3 & online) == 0)                return 2;   // PRIVATE
		//        return 1;                                          // RANKED
		//
		//    UsingRankedStatsGroup            sub_654630   -- same shape, bool
		//
		//  ⭐ The forced-offline term is an AND of TWO flags:
		//        lobby + 1598544  ("dedicated")
		//        lobby params[10] (private)
		//  so PRIVATE ALONE DOES NOT MOVE YOU OUT OF THE RANKED GROUP. That
		//  matters because BG_BotsAreAllowed needs private=1 for bots to move,
		//  and these are different flags -- the two requirements are not in
		//  conflict the way they appear to be.
		//
		//  RULE A17: both return __int64 in the decompile; do not narrow.
		using GetStatsGroup_t     = std::int64_t(__fastcall*)();
		using UsingRankedGroup_t  = std::int64_t(__fastcall*)();

		void cmd_lobby_list()
		{
			const auto ms = lobby_members();
			if (ms.empty())
			{
				Console::printf("[host] no lobby members readable (not in a lobby?)");
				return;
			}
			const auto* L = lobby(0);
			const int host = L ? *reinterpret_cast<const std::int32_t*>(L + OFF_WE_HOST) : 0;
			Console::printf("[host] %zu player(s) in the lobby  (AreWeHost=%d)",
				ms.size(), host);
			for (const auto& m : ms)
			{
				Console::printf("[host]   %2d  %-24s %llu%s", m.index,
					m.name.c_str(), static_cast<unsigned long long>(m.xuid),
					m.is_me ? "   <- you" : "");
			}
			if (my_xuid() == 0)
			{
				Console::printf("[host] ⚠ could not read your own account id, so nobody is "
					"marked '<- you' and the self-kick guard is OFF. Check the slot before "
					"kicking.");
			}
			if (!host)
			{
				Console::printf("[host] you are NOT the lobby host, so kicking will refuse.");
			}
			Console::printf("[host] usage: fh_kick <slot>   (list them with fh_who)");
		}

		void cmd_lobby_kick()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[host] usage: fh_kick <slot>   (see fh_who)");
				return;
			}
			const int slot = GameUtil::safeStringToInt(args->argv[args->nesting][1]);
			std::string name;
			for (const auto& m : lobby_members())
			{
				if (m.index == slot) { name = m.name; break; }
			}
			std::string why;
			if (kick_member(slot, why))
			{
				Console::printf("[host] kicked slot %d (%s) via PartyHost_KickPlayer.",
					slot, name.empty() ? "?" : name.c_str());
			}
			else
			{
				Console::printf("[host] kick refused: %s", why.c_str());
			}
		}

		void cmd_statsgroup()
		{
			const auto get_group = reinterpret_cast<GetStatsGroup_t>(_b(ADDR_STATS_GROUP));
			const auto using_ranked = reinterpret_cast<UsingRankedGroup_t>(_b(ADDR_USING_RANKED));

			const std::int64_t group = get_group();
			const std::int64_t ranked = using_ranked();

			const auto* L = active_lobby();
			const int ded = L ? *reinterpret_cast<const std::int32_t*>(L + OFF_DEDICATED) : -1;
			const int priv = L ? *reinterpret_cast<const std::int32_t*>(L + OFF_PRIVATE) : -1;

			const char* meaning =
				group == 1 ? "RANKED  -- counts toward your public rank" :
				group == 2 ? "PRIVATE -- separate bucket, does NOT count" :
				group == 3 ? "group 3 (sub_B8B60 mode)" :
				group == 9 ? "group 9 (sub_151720 mode)" : "unknown";

			Console::printf("[host] stats group = %lld  (%s)", group, meaning);
			Console::printf("[host] UsingRankedStatsGroup = %lld", ranked);
			Console::printf("[host] inputs: lobby+1598544 ('dedicated')=%d  private=%d  "
				"-> forced-offline term is (%d && %d) = %d",
				ded, priv, ded != 0, priv != 0, (ded != 0 && priv != 0));
			Console::printf("[host] so: bots need private=1; the RANKED group only needs "
				"NOT BOTH of those two set. They are different flags.");
		}

		// The honest "why can't I host" readout: it CALLS the engine's own
		// predicate rather than re-implementing it, then prints every term so a
		// refusal names itself instead of being inferred.
		//
		// ⚠ Runs on the CLIENT thread (console command), never the Present thread.
		// Safe to call because we clear requireOpenNat in tick(), so the branch
		// that does an interlocked release + an Arxan thunk is not taken.
		void cmd_canhost()
		{
			auto* L = lobby(0);
			if (!L)
			{
				Console::printf("[host] no lobby yet.");
				return;
			}
			const auto maxp = *reinterpret_cast<const std::int32_t*>(L + OFF_MAXPLAYERS);
			const auto nat = *reinterpret_cast<const std::int32_t*>(L + OFF_REQ_OPEN_NAT);
			const auto priv = *reinterpret_cast<const std::int32_t*>(L + OFF_PRIVATE);
			const auto* he = reinterpret_cast<const std::int32_t*>(_b(ADDR_HOSTING_EN));

			Console::printf("[host] === Session_CanHostServer terms ===");
			Console::printf("[host]   hostingEnabled = %d   requireOpenNat = %d   private = %d",
				readable(he, 4) ? *he : -1, nat, priv);

			// NAT. The engine's own getter (same body as the inline block inside
			// CanHostServer): acquires the refcounted platform object, reads +168,
			// releases. PROVEN: 3 is the value CanHostServer refuses on, and
			// Script_GetNatType returns 0 when unavailable or on system link.
			// The other values are NOT proven, so they are printed raw.
			using GetNat_fn = std::int64_t(__fastcall*)();
			const auto nat_type = reinterpret_cast<GetNat_fn>(_b(0x855190))();
			Console::printf("[host]   NAT type = %lld%s   (only consulted when "
				"requireOpenNat != 0, and 3 is the refusal value)",
				static_cast<long long>(nat_type),
				nat_type == 3 ? "  <-- STRICT" : "");
			Console::printf("[host]   platform session ready = %s   (byte at unk_10DC800+100264)",
				platform_session_ready() ? "yes" : "NO");
			if (!platform_session_ready())
			{
				Console::printf("[host]     ^ if NO, hosting is refused AND g_hostingEnabled is "
					"latched off by BOTH Session_CanHostServer and the LUI CanHost predicate.");
				Console::printf("[host]     The Xbox/Store build does not check this at all, so "
					"it is platform-specific, not a universal hosting requirement.");
			}
			Console::printf("[host]   g_hostingEnabled re-asserted %llu time(s)%s",
				static_cast<unsigned long long>(g_hosting_reasserts),
				g_hosting_reasserts ? "  <-- a host predicate IS running and failing" : "");
			Console::printf("[host]   party_maxplayers = %d  (the qualification bucket)", maxp);

			// Measured vs required, using the engine's own getters.
			using GetInt_fn = std::int64_t(__fastcall*)();
			using GetFlt_fn = float(__fastcall*)();
			const auto meas_up = reinterpret_cast<GetInt_fn>(_b(0x788860));
			const auto meas_pg = reinterpret_cast<GetFlt_fn>(_b(0x7880B0));
			Console::printf("[host]   measured upload = %lld   measured ping = %.1f",
				static_cast<long long>(meas_up()), static_cast<double>(meas_pg()));

			for (const auto& t : QUAL)
			{
				double v = 0.0;
				Console::printf("[host]   %-12s dvar \"%-5s\" = %s", t.what, t.dvar,
					qual_value(t, v) ? std::format("{:g}", v).c_str() : "<unreadable>");
			}

			// RULE A17 — Session_CanHostServer returns char, not int.
			using CanHost_fn = char(__fastcall*)(std::uint8_t*, unsigned int);
			const auto can = reinterpret_cast<CanHost_fn>(_b(0x8524B0));
			Console::printf("[host]   ENGINE VERDICT: Session_CanHostServer(lobby0, %d) = %d",
				maxp, static_cast<int>(can(L, static_cast<unsigned int>(maxp))));
		}

		void cmd_on()
		{
			set_enabled(true);
		}

		void cmd_off()
		{
			set_enabled(false);
		}

		void cmd_playlists()
		{
			const auto pls = playlists();
			Console::printf("[host] %zu playlist(s) live (id = what params[11] uses):", pls.size());
			for (const auto& p : pls)
			{
				Console::printf("[host]   %3d  row %-3d  %s", p.id, p.row, p.name.c_str());
			}
		}

		void cmd_maps()
		{
			const auto ms = maps();
			std::string line;
			Console::printf("[host] %zu map(s) from the engine table:", ms.size());
			for (std::size_t i = 0; i < ms.size(); ++i)
			{
				line += ms[i];
				line += "  ";
				if ((i % 4) == 3 || i + 1 == ms.size())
				{
					Console::printf("[host]   %s", line.c_str());
					line.clear();
				}
			}
			const auto gts = gametypes();
			line.clear();
			for (const auto& g : gts)
			{
				line += g;
				line += "  ";
			}
			Console::printf("[host] gametypes: %s", line.c_str());
		}

		void cmd_play()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[host] usage: fh_play <playlistId>   (fh_playlists to list)");
				return;
			}
			set_play_playlist(GameUtil::safeStringToInt(args->argv[args->nesting][1]));
			Console::printf("[host] play playlist = %d (%s)", g_play_pl,
				playlist_name(g_play_pl).c_str());
		}

		void cmd_search()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[host] usage: fh_search <playlistId|-1>   -1 = same as play");
				return;
			}
			set_search_playlist(GameUtil::safeStringToInt(args->argv[args->nesting][1]));
			Console::printf("[host] search playlist = %d", g_search_pl);
		}

		void cmd_map()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[host] usage: fh_map <mp_xxx>   (empty string = leave alone)");
				return;
			}
			set_map(args->argv[args->nesting][1]);
			Console::printf("[host] map = %s", g_map.c_str());
		}

		void cmd_gametype()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[host] usage: fh_gametype <war|dom|conf|sd|dm|hp|...>");
				return;
			}
			set_gametype(args->argv[args->nesting][1]);
			Console::printf("[host] gametype = %s", g_gametype.c_str());
		}

		void cmd_players()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 3)
			{
				Console::printf("[host] usage: fh_players <min> <max>   (now %d/%d)", g_min, g_max);
				return;
			}
			set_min_players(GameUtil::safeStringToInt(args->argv[args->nesting][1]));
			set_max_players(GameUtil::safeStringToInt(args->argv[args->nesting][2]));
			Console::printf("[host] players %d..%d", g_min, g_max);
		}

		void cmd_apply()
		{
			apply_playlist_now();
		}

		void cmd_bots()
		{
			const auto* args = GameUtil::getCmdArgs();
			const int argc = args ? args->argc[args->nesting] : 0;
			if (argc < 2)
			{
				Console::printf("[host] bots per team: axis=%d allies=%d  (total %d)",
					bots_team_limit(0), bots_team_limit(1),
					bots_team_limit(0) + bots_team_limit(1));
				Console::printf("[host] usage: fh_bots <perTeam>   |   fh_bots <team 0|1> <count>");
				return;
			}
			if (argc >= 3)
			{
				set_bots_team_limit(GameUtil::safeStringToInt(args->argv[args->nesting][1]),
					GameUtil::safeStringToInt(args->argv[args->nesting][2]));
			}
			else
			{
				const int n = GameUtil::safeStringToInt(args->argv[args->nesting][1]);
				set_bots_team_limit(0, n);
				set_bots_team_limit(1, n);
			}
			// Read back through the ENGINE's own getter. If the value does not
			// stick, that shows here instead of being discovered in a match.
			const int a = bots_team_limit(0);
			const int b = bots_team_limit(1);
			const int need = a + b + 1;                    // bots + you
			Console::printf("[host] bots now axis=%d allies=%d (total %d, +you = %d)",
				a, b, a + b, need);

			// ⭐ WHY YOU CAN ASK FOR 24v24 AND GET 9v9.
			//
			// sub_6C7050 (the GSC-side reader) hands the script our byte unclamped
			// in a private match, so the 24 IS delivered. What bounds the actual
			// spawn is how many CLIENT SLOTS exist -- and 9v9 is 18, which is
			// exactly party_maxplayers for the Ground War blobs (and this module's
			// own default). The script fills the slots it has and balances teams.
			//
			// So report the two real constraints next to the request instead of
			// leaving "why 9v9" as a mystery.
			int party_max = -1;
			char line[192] = {};
			int off = 0;
			static const char* kName[LOBBY_SLOTS] = { "game", "alt", "private" };
			for (int slot = 0; slot < LOBBY_SLOTS; ++slot)
			{
				const auto* Ls = lobby(slot);
				const int mx = Ls
					? *reinterpret_cast<const std::int32_t*>(Ls + OFF_MAXPLAYERS) : -1;
				off += std::snprintf(line + off, sizeof(line) - off, " %s=%d",
					kName[slot], mx);
				// The tightest of the three is what actually binds.
				if (mx >= 0 && (party_max < 0 || mx < party_max))
				{
					party_max = mx;
				}
			}
			Console::printf("[host] party_maxplayers:%s   sv_maxclients=%d  "
				"svs.clientCount=%d  serverRunning=%d",
				line, sv_maxclients(), svs_client_count(), server_running() ? 1 : 0);
			if (svs_client_count() >= 0 && svs_client_count() < need)
			{
				Console::printf("[host] svs.clientCount is %d -- that is the hard ceiling. "
					"It is allocated once at SV_Init and the server hunk is reserved for "
					"the same number in advance, so it cannot be raised. 17 bots + you is "
					"the most that will spawn.", svs_client_count());
			}

			if (party_max >= 0 && party_max < need)
			{
				if (g_enabled)
				{
					// Force host is holding this value, so raising it is ours to do
					// -- and doing it silently would be worse than saying so.
					set_max_players((std::min)(MATCH_MAX_CLIENTS, need));
					Console::printf("[host] party_maxplayers was %d, too few for %d "
						"-> raised to %d. It is latched, so it will stick.",
						party_max, need, g_max);
				}
				else
				{
					Console::printf("[host] ^3party_maxplayers is %d but you asked for "
						"%d. Enable force host (fh_on) and run `fh_players 1 %d`, or "
						"the match will fill only %d slots and balance them.",
						party_max, need, (std::min)(MATCH_MAX_CLIENTS, need), party_max);
				}
			}
		}
	}

	// -----------------------------------------------------------------------
	void init()
	{
		dev_mode::add_command("fh_on", cmd_on);
		dev_mode::add_command("fh_off", cmd_off);
		dev_mode::add_command("fh_status", cmd_status);
		dev_mode::add_command("fh_statsgroup", cmd_statsgroup);
		dev_mode::add_command("fh_who", cmd_lobby_list);
		dev_mode::add_command("fh_kick", cmd_lobby_kick);
		dev_mode::add_command("fh_playlists", cmd_playlists);
		dev_mode::add_command("fh_maps", cmd_maps);
		dev_mode::add_command("fh_play", cmd_play);
		dev_mode::add_command("fh_search", cmd_search);
		dev_mode::add_command("fh_map", cmd_map);
		dev_mode::add_command("fh_gametype", cmd_gametype);
		dev_mode::add_command("fh_players", cmd_players);
		dev_mode::add_command("fh_applyplaylist", cmd_apply);
		dev_mode::add_command("fh_bots", cmd_bots);
		dev_mode::add_command("fh_canhost", cmd_canhost);

		// The join gate is what actually makes you the lobby host. Installed here,
		// armed only while force host is on, and self-disarming (see tick step 2c).
		install_join_gate();

		Console::printf("[host] force host ready — ZERO HOOKS. Latched lobby params only.");
	}

	// -----------------------------------------------------------------------
	std::vector<Playlist> playlists() { return read_playlists(); }

	std::string playlist_name(const int id)
	{
		const auto* row = playlist_row(id);
		if (!row)
		{
			return {};
		}
		return read_cstr(reinterpret_cast<const char*>(row + PL_OFF_NAME), 64);
	}

	std::vector<std::string> maps()
	{
		std::vector<std::string> out;
		const auto* cnt = reinterpret_cast<const std::uint8_t*>(_b(ADDR_MAPNAME_COUNT));
		const auto* tbl = reinterpret_cast<const char* const*>(_b(ADDR_MAPNAME_TABLE));
		if (!readable(cnt, 1))
		{
			return out;
		}
		const int n = *cnt;
		if (n <= 0 || n > 256 || !readable(tbl, sizeof(char*) * static_cast<std::size_t>(n)))
		{
			return out;
		}
		for (int i = 0; i < n; ++i)
		{
			auto s = read_cstr(tbl[i], 64);
			if (!s.empty())
			{
				out.push_back(std::move(s));
			}
		}

		// Add anything the master registry knows about that the
		// playlist-facing table above does not -- maps like mp_house that
		// are fully compiled and loadable but were pulled from matchmaking
		// rotation. A private match set up through this tool is not bound
		// by the playlist system, so there is no reason to hide them here.
		for (auto& hidden : read_map_registry())
		{
			if (!looks_like_a_real_map(hidden))
			{
				continue;
			}
			if (std::find(out.begin(), out.end(), hidden) == out.end())
			{
				out.push_back(std::move(hidden));
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	std::vector<std::string> gametypes()
	{
		std::vector<std::string> out;
		const auto* base = reinterpret_cast<const std::uint8_t*>(_b(ADDR_GT_TABLE));
		if (!readable(base, GT_STRIDE * GT_COUNT))
		{
			return out;
		}
		for (std::size_t i = 0; i < GT_COUNT; ++i)
		{
			auto s = read_cstr(reinterpret_cast<const char*>(base + GT_STRIDE * i), 32);
			if (s.empty())
			{
				continue;
			}
			if (std::find(out.begin(), out.end(), s) == out.end())
			{
				out.push_back(std::move(s));
			}
		}
		return out;
	}

	// -----------------------------------------------------------------------
	bool enabled() { return g_enabled; }
	bool auto_apply() { return g_auto_apply; }
	void set_auto_apply(const bool on) { g_auto_apply = on; g_engine_applied_pl = -1; }
	void queue_apply_playlist() { cbuf("fh_applyplaylist"); }
	int  play_playlist() { return g_play_pl; }
	int  search_playlist() { return g_search_pl; }
	std::string map() { return g_map; }
	std::string gametype() { return g_gametype; }
	int  max_players() { return g_max; }
	int  min_players() { return g_min; }

	void set_play_playlist(const int id) { g_play_pl = id; g_last_applied_pl = -1; g_engine_applied_pl = -1; }
	void set_search_playlist(const int id) { g_search_pl = id; g_last_applied_pl = -1; g_engine_applied_pl = -1; }
	void set_map(const std::string& m) { g_map = m; }
	void set_gametype(const std::string& g) { g_gametype = g; }
	// ⛔ 18 IS THE REAL CEILING FOR A MATCH, and it cannot be raised from here.
	//
	// sv_maxclients registers with a max of 48 and the hub genuinely runs 48 --
	// but the hub takes SV_Init's OTHER branch (`svs_clientCount = 48` as a
	// constant). A match takes `svs_clientCount = sv_maxclients`, and raising that
	// is FATAL: SV_CreateServerHunk reserves g_serverHunkUser in advance from a
	// client count, QUADRATICALLY (~33 MB at 18, ~106 MB at 48), so SV_Init then
	// allocates past the reservation and Hunk_Alloc aborts with
	// "Memory Error: 6 161". Tested in game 2026-08-11. The reservation's caller is
	// behind an Arxan thunk, so it cannot be made to agree.
	//
	// So the UI must not offer a number the server can never honour: 18.
	void set_max_players(const int n) { g_max = (std::max)(0, (std::min)(MATCH_MAX_CLIENTS, n)); }
	void set_min_players(const int n) { g_min = (std::max)(0, (std::min)(MATCH_MAX_CLIENTS, n)); }

	void set_enabled(const bool on)
	{
		g_enabled = on;
		g_last_applied_pl = -1;
		g_engine_applied_pl = -1;
		g_last_display.clear();
		if (!on)
		{
			Console::printf("[host] force host OFF. Nothing is being re-asserted; the "
				"engine keeps whatever it last set.");
			return;
		}
		Console::printf("[host] force host ON — hosting enabled, listen servers allowed, "
			"players %d..%d, map '%s', gametype '%s', playlist %d",
			g_min, g_max,
			g_map.empty() ? "(engine's)" : g_map.c_str(),
			g_gametype.empty() ? "(engine's)" : g_gametype.c_str(),
			g_play_pl);
	}

	// -----------------------------------------------------------------------
	void apply_playlist_now()
	{
		auto* L = lobby(0);
		if (!L)
		{
			Console::printf("[host] no lobby object -- cannot apply.");
			return;
		}
		if (g_play_pl < 0)
		{
			Console::printf("[host] no play playlist chosen (fh_play <id>).");
			return;
		}
		// Set the ordinal first: sub_6563D0 reads params[11] and maps it itself.
		*reinterpret_cast<std::int32_t*>(L + OFF_PLAYLIST) = g_play_pl;

		// sub_6563D0(localClientNum, gametypeRotationSlot, lobby) -- the engine's
		// own applier. Signature transcribed verbatim (RULE A17).
		using apply_t = std::int64_t(__fastcall*)(std::uint32_t, std::int32_t, void*);
		const auto fn = reinterpret_cast<apply_t>(_b(ADDR_APPLY_PL));
		const auto r = fn(0, 0, L);
		Console::printf("[host] engine playlist apply: playlist %d (%s) -> %lld",
			g_play_pl, playlist_name(g_play_pl).c_str(), static_cast<long long>(r));

		// It rewrites ui_mapname/ui_gametype from the playlist's rotation, so put
		// ours back on the next tick.
		g_last_applied_pl = -1;
	}

	// -----------------------------------------------------------------------
	void tick()
	{
		if (!g_enabled)
		{
			return;
		}
		// SLOT 0, deliberately -- matching the ENGINE's own param writers:
		//   sub_1972B0  setgameprivatematch      -> Lobby_Get(0)
		//   sub_80290   create hosted session    -> Lobby_Get(0)
		//   sub_654100  stats-group decision     -> Lobby_Get(0)
		// sub_470CB0 (active_lobby) picks 0-or-1 and is used by the LUI-facing
		// getters, not by these. Latching the "active" one was a regression --
		// the writers the engine actually reads back are all on slot 0.
		// active_lobby() is kept for the STATUS readout, where seeing the two
		// disagree is the whole point.
		auto* L = lobby(0);
		if (!L)
		{
			if (!g_warned_lobby)
			{
				g_warned_lobby = true;
				Console::printf("[host] lobby object not readable yet -- waiting.");
			}
			return;
		}
		g_warned_lobby = false;

		// 1. Hosting. g_hostingEnabled defaults to 1 in the image and its only
		//    writer, Session_ClearHostingEnabled, can ONLY clear it -- there is no
		//    path that sets it back. Re-asserting is repairing a one-way latch,
		//    not inventing a capability.
		//    ⭐ CORRECTED 2026-08-12 after diffing the Xbox/Store build (IDB on port
		//    12346). TWO functions clear this byte, not one:
		//        Session_CanHostServer @0x8534B0   outer gate fails -> clears, returns 0
		//        sub_853400 (LUI CanHost)          same
		//    both on !Session_IsPlatformSessionReady(unk_10DC800). So re-asserting
		//    does NOT fix a refusal -- it only papers over the latch, and the next
		//    call re-clears it. COUNT the re-asserts: a climbing count is direct,
		//    inference-free proof that a host predicate is running and failing.
		if (auto* he = reinterpret_cast<std::int32_t*>(_b(ADDR_HOSTING_EN)); readable(he, 4))
		{
			if (*he == 0)
			{
				*he = 1;
				++g_hosting_reasserts;
				const DWORD now = GetTickCount();
				if (g_hosting_reasserts == 1 || now - g_last_reassert_report > 10000)
				{
					g_last_reassert_report = now;
					Console::printf("[host] ⚠ g_hostingEnabled was CLEARED (re-asserted %llu "
						"time(s)). Something is calling a host predicate and failing it — "
						"most likely Session_IsPlatformSessionReady. fh_canhost names the term.",
						static_cast<unsigned long long>(g_hosting_reasserts));
				}
			}
		}

		// 2. Let strangers be routed to a listen server. The public playlists ship
		//    this as 0 (see the var-rule blobs at IDA 0xABF5560 / 0xABF60F6), which
		//    is exactly why a client host is never offered.
		if (auto* al = reinterpret_cast<std::int32_t*>(L + OFF_ALLOW_LISTEN); *al == 0)
		{
			*al = 1;
		}

		// 2b. ⭐ THE HOST GATE. Session_CanHostServer is what decides whether the
		//     GSC lobby state machine may host at all, and everything force_host
		//     latched before now sat DOWNSTREAM of it. Two of its terms are ours:
		//
		//       requireOpenNat (params[13]) -- when set, a STRICT NAT (type 3) is
		//       an outright refusal. Clearing it skips that test entirely.
		//
		//       the bandwidth/ping qualification -- keyed on maxplayers, and our
		//       own 18-player latch puts us in the 725 kbps bucket. The ten
		//       thresholds are plain settable dvars (flags 0), so we zero them
		//       through the engine's own console path.
		//
		//     Neither fabricates state: one is a lobby param the playlist sets,
		//     the other is a shipped tunable. See the QUAL table above for the
		//     full decompile and the address arithmetic.
		if (auto* nat = reinterpret_cast<std::int32_t*>(L + OFF_REQ_OPEN_NAT); *nat != 0)
		{
			*nat = 0;
		}
		if (g_relax_qual)
		{
			// Cheap, but it is 10 pointer reads -- no reason to do it per frame.
			if (const DWORD now = GetTickCount(); now - g_last_qual_ms > 2000)
			{
				g_last_qual_ms = now;
				relax_qualification();
			}
		}

		// 2c. ⭐ THE JOIN GATE. Relaxing the gate above only grants PERMISSION to
		//     host. This is what actually makes you the lobby host: refuse other
		//     people's lobbies until the engine gives up searching and hosts.
		//
		//     It arms itself, and -- more importantly -- DISARMS itself on success
		//     or on a timeout, so it can never leave you unable to find any game.
		if (g_joingate_installed)
		{
			// ⛔ FIXED 2026-08-12. The first version disarmed on `we_host ||
			// have_session`, and the log proved that wrong in one line:
			//     "✔ YOU ARE THE HOST. Join gate released after refusing 0 lobbies"
			// You are ALREADY host of your own party lobby, with a session, before
			// matchmaking even starts -- so the success test was true on the very
			// first frame. The gate armed, immediately declared victory, disarmed,
			// re-armed, and was never actually up while the search ran.
			//
			// The correct signal is CONNECTION STATE: guard while we are at the
			// menu / searching, and stand down once we are genuinely in a game.
			//   clientConnectionState  IDA 0x1BAF4E4 - 0x1000 = 0x1BAE4E4
			//   (byte_1BAF450 + 148, dword, stride 1976 per client)
			//   >= 9 == PRIMED, 10 == ACTIVE.
			const auto* cs = reinterpret_cast<const std::int32_t*>(_b(0x1BAE4E4));
			const int connstate = readable(cs, 4) ? *cs : 0;
			const bool in_game = connstate >= 9;
			const bool armed = g_joingate_armed.load(std::memory_order_relaxed);
			const auto refused = g_joingate_refused.load(std::memory_order_relaxed);

			if (g_enabled && !in_game && !armed && !g_joingate_gave_up)
			{
				g_joingate_armed.store(true, std::memory_order_relaxed);
				g_joingate_refused.store(0, std::memory_order_relaxed);
				g_joingate_armed_ms = GetTickCount();
				Console::printf("[host] HOSTING — refusing other people's lobbies so the "
					"game makes YOU the host. This can take a little while.");
			}
			else if (armed && in_game)
			{
				// We are in a game. Stand down either way, and say honestly which
				// of the two happened rather than claiming a win we cannot see.
				g_joingate_armed.store(false, std::memory_order_relaxed);
				const bool we_host = *reinterpret_cast<const std::int32_t*>(L + OFF_WE_HOST) != 0;
				Console::printf("[host] in a game (connstate=%d) — join gate stood down "
					"after refusing %u lobb%s. AreWeHost=%d.",
					connstate, refused, refused == 1 ? "y" : "ies", we_host);
			}
			else if (armed && GetTickCount() - g_joingate_armed_ms > JOINGATE_TIMEOUT_MS)
			{
				g_joingate_armed.store(false, std::memory_order_relaxed);
				g_joingate_gave_up = true;
				Console::printf("[host] join gate gave up after %u s (refused %u lobb%s) "
					"— letting you join normally so you are not stuck. Untick and retick "
					"Host my own match to try again.",
					JOINGATE_TIMEOUT_MS / 1000, refused, refused == 1 ? "y" : "ies");
			}
			else if (!g_enabled && armed)
			{
				g_joingate_armed.store(false, std::memory_order_relaxed);
				Console::printf("[host] join gate released (force host turned off).");
			}
			// Re-arm cleanly on a fresh toggle, and once we leave a game.
			if (!g_enabled || in_game)
			{
				g_joingate_gave_up = false;
			}
		}
		else if (g_enabled && !g_joingate_reported_off)
		{
			// RULE A15 — say this out loud rather than silently doing nothing.
			g_joingate_reported_off = true;
			Console::printf("[host] ⚠ join gate is NOT installed, so nothing stops "
				"matchmaking putting you in someone else's lobby. Force host will only "
				"grant permission to host, not make it happen.");
		}

		// 3. Player counts, on ALL THREE LOBBIES.
		//
		//    ⭐ There are three lobby objects and the menus write DIFFERENT ones:
		//        SetGamePartyMaxPlayers    (LUI) -> Lobby_Get(0)      the GAME lobby
		//        SetPartyMaxPlayers        (LUI) -> sub_470CB0()      lobby 0 or 1
		//        SetPrivatePartyMaxPlayers (LUI) -> sub_47E350()
		//                                        = Lobby_Get(2)      the PRIVATE party
		//    (off_8D76770 - off_8BE6040 = 0x190730 = exactly one 1640240 stride, so
		//     sub_470CB0's alternate really is lobby 1.)
		//
		//    Latching only lobby 0 would leave a private match reading lobby 2's
		//    value -- which is a live suspect for asking 24v24 and getting 9v9.
		//    LobbyParams_SetMaxPlayers is `*(DWORD*)(a1+596) = n` with NO clamp, so
		//    writing all three is cheap and cannot corrupt anything.
		//
		//    ⚠ Deliberately NOT the five raw bytes the old tool poked: those feed
		//    SV_ChangeMaxClients without reallocating svs.clients and crash.
		for (int slot = 0; slot < LOBBY_SLOTS; ++slot)
		{
			auto* Ls = lobby(slot);
			if (!Ls)
			{
				continue;
			}
			if (g_min > 0)
			{
				auto* mn = reinterpret_cast<std::int32_t*>(Ls + OFF_MINPLAYERS);
				if (*mn != g_min)
				{
					*mn = g_min;
				}
			}
			if (g_max > 0)
			{
				auto* mx = reinterpret_cast<std::int32_t*>(Ls + OFF_MAXPLAYERS);
				if (*mx != g_max)
				{
					*mx = g_max;
				}
			}
		}

		// 4. The ENGINE's own playlist applier, once per change. CONFIRMED IN GAME
		//    2026-08-11, which is why it is the automatic path now instead of an
		//    Advanced button: it sets map, gametype, the var-rule blob, the display
		//    name and the icon exactly as the menu does, so nothing is reconstructed
		//    by hand. Queued as a console command so it runs on the CLIENT thread --
		//    it execs config files and touches the interned-string pool, neither of
		//    which may happen on the Present thread.
		//    Change-triggered only. In a latch loop this would exec cfg every frame.
		if (g_auto_apply && g_play_pl >= 0 && !have_session(L) && g_engine_applied_pl != g_play_pl)
		{
			g_engine_applied_pl = g_play_pl;
			*reinterpret_cast<std::int32_t*>(L + OFF_PLAYLIST) = g_play_pl;
			cbuf("fh_applyplaylist");
		}

		// 5. Map + gametype. Compare first: in steady state this issues nothing.
		//    The search never reads either field (sub_285DA0), so holding them is
		//    free -- that is what removes the timing.
		if (!g_map.empty() && interned(L, OFF_UI_MAPNAME) != g_map)
		{
			cbuf("ui_mapname " + g_map);
		}
		if (!g_gametype.empty() && interned(L, OFF_UI_GAMETYPE) != g_gametype)
		{
			cbuf("ui_gametype " + g_gametype);
		}

		// 6. Playlist number. This IS read by the search, so it is the only field
		//    with a phase -- and only when a separate search playlist is set.
		const int want = (g_search_pl >= 0 && !have_session(L)) ? g_search_pl : g_play_pl;
		if (want >= 0)
		{
			auto* pl = reinterpret_cast<std::int32_t*>(L + OFF_PLAYLIST);
			if (*pl != want)
			{
				*pl = want;
			}
			// 7. The display. GetGameLobbyPlaylistNum returns params[11] (now
			//    correct), and sub_6563D0's tail also publishes the row's name and
			//    icon as dvars -- so push those too, or the HUD keeps reporting
			//    whatever playlist was last applied.
			const DWORD now = GetTickCount();
			if (want != g_last_applied_pl || now - g_last_display_ms > 5000)
			{
				g_last_applied_pl = want;
				push_display(want);
			}
		}
	}

	// -----------------------------------------------------------------------
	//  BOTS — the per-team limit, and why there is no cap to fight
	//
	//  The LUI bindings are SetBotsTeamLimit (sub_335F00) / GetBotsTeamLimit
	//  (sub_335E20) / BotsAreAllowed (sub_335990). Underneath:
	//
	//    sub_388210()                = return 1        <- bots always allowed
	//    sub_38E350(team, count):      v6 = (char)count;
	//                                  if (count <= 0) v6 = 0;      <- the ONLY clamp
	//                                  *(BYTE*)(sub_924670(c) + team + 47) = v6;
	//    sub_3882C0(team):             if (gametype == "scorestreak_training")
	//                                      return dvar off_1BD3710[+16];
	//                                  return *(u8*)(sub_924650(c) + team + 47);
	//
	//  ⭐ THERE IS NO UPPER CLAMP IN NATIVE CODE. One byte per team, and the only
	//  guard rejects negatives. Whatever cap the private-match menu shows lives in
	//  the LUI slider, i.e. in Lua -- calling the native setter goes under it.
	//
	//  The "scorestreak_training" branch is the training mode, not private match,
	//  so in a private match the getter reads the byte we wrote.
	//
	//  ⚠ SetBotsTeamLimit's own gate is `!Com_IsOnlineGame() || we are the game
	//  host`. We call the setter directly, so that gate is not applied -- which is
	//  harmless (it is one byte in our own client's settings object) but means the
	//  value only MEANS anything when we are the one starting the match.
	void set_bots_team_limit(const int team, const int count)
	{
		if (team < 0 || team > 1)
		{
			Console::printf("[host] team must be 0 or 1");
			return;
		}
		// Clamp to what can actually spawn: 18 client slots, one of which is you,
		// so 17 bots is the most that will ever appear (the spawner balances them,
		// which is why "9v9" is 8 bots + you against 9 bots).
		const int n = (std::max)(0, (std::min)(MATCH_MAX_CLIENTS - 1, count));
		using set_t = std::int64_t(__fastcall*)(int, int);
		reinterpret_cast<set_t>(_b(ADDR_BOTS_SET))(team, n);
	}

	int bots_team_limit(const int team)
	{
		if (team < 0 || team > 1)
		{
			return 0;
		}
		using get_t = std::int64_t(__fastcall*)(int);
		return static_cast<int>(reinterpret_cast<get_t>(_b(ADDR_BOTS_GET))(team));
	}

	// -----------------------------------------------------------------------
	std::vector<member_t> lobby_members()
	{
		std::vector<member_t> out;
		const auto* L = lobby(0);
		if (!L)
		{
			return out;
		}
		// Our own xuid, so the list can mark us and kick() can refuse us.
		const std::uint64_t mine = my_xuid();

		for (int i = 0; i < MEMBER_MAX; ++i)
		{
			const std::size_t off = static_cast<std::size_t>(i) * MEMBER_STRIDE;
			const auto* st = L + MEMBER_STATE + off;
			if (!readable(st, 1))
			{
				break;                       // ran off the object; stop cleanly
			}
			const int state = *st;
			if (state < MEMBER_CONNECTED)
			{
				continue;                    // empty / not connected
			}

			member_t m{};
			m.index = i;
			m.state = state;

			const auto* xu = L + MEMBER_XUID + off;
			m.xuid = readable(xu, 8) ? *reinterpret_cast<const std::uint64_t*>(xu) : 0;

			const auto* nm = reinterpret_cast<const char*>(L + MEMBER_NAME + off);
			if (readable(nm, 32))
			{
				// Only accept printable ASCII -- a wrong offset must LOOK wrong
				// rather than render as convincing garbage.
				char buf[33]{};
				std::size_t n = 0;
				for (; n < 32 && nm[n]; ++n)
				{
					const unsigned char c = static_cast<unsigned char>(nm[n]);
					if (c < 0x20 || c > 0x7E)
					{
						break;
					}
					buf[n] = nm[n];
				}
				m.name.assign(buf, n);
			}
			if (m.name.empty())
			{
				m.name = "(slot " + std::to_string(i) + ")";
			}
			m.is_me = (mine != 0 && m.xuid == mine);
			out.push_back(m);
		}
		return out;
	}

	bool kick_member(const int index, std::string& why)
	{
		auto* L = lobby(0);
		if (!L)
		{
			why = "lobby object not readable";
			return false;
		}
		if (index < 0 || index >= MEMBER_MAX)
		{
			why = "slot out of range";
			return false;
		}
		// Only the lobby HOST can remove anyone. Say so rather than firing a
		// call that will be ignored.
		if (*reinterpret_cast<const std::int32_t*>(L + OFF_WE_HOST) == 0)
		{
			why = "you are not the lobby host -- only the host can kick";
			return false;
		}

		const std::size_t off = static_cast<std::size_t>(index) * MEMBER_STRIDE;
		const auto* st = L + MEMBER_STATE + off;
		if (!readable(st, 1) || *st < MEMBER_CONNECTED)
		{
			why = "that slot is empty";
			return false;
		}

		const auto* xu = L + MEMBER_XUID + off;
		const std::uint64_t x = readable(xu, 8)
			? *reinterpret_cast<const std::uint64_t*>(xu) : 0;
		if (x != 0 && x == my_xuid())
		{
			why = "that is you";
			return false;
		}

		// -------------------------------------------------------------------
		//  THE ENGINE'S OWN SILENT NO-OP, pre-checked so a failed kick reports a
		//  REASON instead of a false success.
		//
		//  PartyHost_KickPlayer @0x48CD70:
		//      if (sub_4811F0() || sub_481210(L, m) || !sub_6FE240(*L, m))
		//          return sub_48E3D0(L, m, 1, "kicked");     // alternate path,
		//                                                    // still a real kick
		//      result = sub_6FF6D0(*L, m);
		//      if (!result) { ...send "%ikickedFromParty"...; return sub_48AA00(L, m); }
		//      return result;                                // <-- DOES NOTHING
		//
		//  and sub_6FF6D0(rawSession, m) = *(DWORD*)(rawSession + 156 + 56*m) == 2
		//
		//  rawSession is `*L` -- ONE indirection (already proven when the
		//  republish latch was walking the wrong base).
		// -------------------------------------------------------------------
		const auto* raw = *reinterpret_cast<void* const*>(L);
		if (readable(raw, 1))
		{
			const auto* ss = reinterpret_cast<const char*>(raw) + 156 + 56 * index;
			if (readable(ss, 4) && *reinterpret_cast<const std::int32_t*>(ss) == 2)
			{
				why = "the engine refuses: that member's session state is 2 "
					"(already leaving, or not fully joined)";
				return false;
			}
		}

		const auto fn = reinterpret_cast<PartyHostKick_t>(_b(ADDR_PARTY_KICK));
		const std::int64_t rc = fn(L, static_cast<unsigned char>(index));

		// Report the engine's own return rather than asserting success. The
		// previous version discarded it and returned true unconditionally, so it
		// claimed a kick even when nothing happened.
		Console::printf("[host] PartyHost_KickPlayer(slot %d) returned %lld", index,
			static_cast<long long>(rc));
		why.clear();
		return true;
	}

	// -----------------------------------------------------------------------
	Status status()
	{
		Status s{};
		const auto* L = active_lobby();
		s.phase = phase_of(L);
		if (!L)
		{
			return s;
		}
		s.lobby_ok = true;
		s.we_host = *reinterpret_cast<const std::int32_t*>(L + OFF_WE_HOST) != 0;
		s.dedicated = *reinterpret_cast<const std::int32_t*>(L + OFF_DEDICATED) != 0;
		s.priv = *reinterpret_cast<const std::int32_t*>(L + OFF_PRIVATE) != 0;
		s.have_session = have_session(L);
		s.playlist = *reinterpret_cast<const std::int32_t*>(L + OFF_PLAYLIST);
		s.playlist_disp = playlist_name(s.playlist);
		s.cur_map = interned(L, OFF_UI_MAPNAME);
		s.cur_gametype = interned(L, OFF_UI_GAMETYPE);
		s.cur_min = *reinterpret_cast<const std::int32_t*>(L + OFF_MINPLAYERS);
		s.cur_max = *reinterpret_cast<const std::int32_t*>(L + OFF_MAXPLAYERS);
		s.allow_listen = *reinterpret_cast<const std::int32_t*>(L + OFF_ALLOW_LISTEN) != 0;
		const auto* he = reinterpret_cast<const std::int32_t*>(_b(ADDR_HOSTING_EN));
		s.hosting_enabled = readable(he, 4) && *he != 0;
		return s;
	}

	namespace
	{
		void cmd_status()
		{
			const auto s = status();
			Console::printf("[host] force host %s, phase=%s", g_enabled ? "ON" : "OFF", s.phase);
			if (!s.lobby_ok)
			{
				Console::printf("[host] lobby object not readable.");
				return;
			}
			// Which lobby object the ENGINE is actually using. This was the bug:
			// everything here read slot 0 while the live session sat on slot 1,
			// so every latched param went to an object nothing reads.
			Console::printf("[host] ACTIVE LOBBY = slot %d  (host=%d on slot0=%d slot1=%d)",
				active_lobby_index(), s.we_host,
				lobby(0) ? *reinterpret_cast<const std::int32_t*>(lobby(0) + OFF_WE_HOST) : -1,
				lobby(1) ? *reinterpret_cast<const std::int32_t*>(lobby(1) + OFF_WE_HOST) : -1);
			Console::printf("[host] lobby : host=%d dedicated=%d private=%d session=%d",
				s.we_host, s.dedicated, s.priv, s.have_session);
			Console::printf("[host] playlist %d (%s)  map='%s'  gametype='%s'  players %d..%d",
				s.playlist, s.playlist_disp.c_str(), s.cur_map.c_str(),
				s.cur_gametype.c_str(), s.cur_min, s.cur_max);
			Console::printf("[host] hostingEnabled=%d allowJoiningListenServer=%d",
				s.hosting_enabled, s.allow_listen);
			Console::printf("[host] want: play=%d search=%d map='%s' gametype='%s' players %d..%d",
				g_play_pl, g_search_pl, g_map.c_str(), g_gametype.c_str(), g_min, g_max);

			// The join gate is the part that actually makes you host, so report it
			// unconditionally and in plain terms (RULE A15).
			if (!g_joingate_installed)
			{
				Console::printf("[host] join gate: NOT INSTALLED — nothing is stopping you "
					"being put in someone else's lobby.");
			}
			else
			{
				const auto refused = g_joingate_refused.load(std::memory_order_relaxed);
				const bool armed = g_joingate_armed.load(std::memory_order_relaxed);
				Console::printf("[host] join gate: installed, %s, refused %u lobb%s%s",
					armed ? "ARMED (refusing other lobbies)" : "idle",
					refused, refused == 1 ? "y" : "ies",
					armed ? "" : (s.we_host ? " — you are host" : ""));
			}
			Console::printf("[host] host gate: platformSessionReady=%s | g_hostingEnabled "
				"re-asserted %llu time(s)%s",
				platform_session_ready() ? "yes" : "NO — hosting is refused AND latched off",
				static_cast<unsigned long long>(g_hosting_reasserts),
				g_hosting_reasserts ? "  <-- a host predicate is failing" : "");
		}
	}
}
