#include "pch.h"
#include "bots.hpp"

#include "Console.hpp"
#include "DevMode.hpp"
#include "GameUtil.hpp"
#include "Hook.hpp"
#include "game.h"
#include "ModPaths.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace bots
{
	namespace
	{
		// -------------------------------------------------------------------
		//  RULE A1 — the address arithmetic, written out, not done in the head.
		//  The `_b` literal is IDA - 0x1000.
		//
		//    SV_AddBot            0x0F2650 - 0x1000 = 0x0F1650
		//    svs_clients (ptr)    0xC5FBA58 - 0x1000 = 0xC5FAA58
		//    svs_clientCount      0xC5FBA50 - 0x1000 = 0xC5FAA50
		// -------------------------------------------------------------------
		//    dvar_com_sv_running  0x1BD3778 - 0x1000 = 0x1BD2778
		constexpr std::uintptr_t ADDR_SV_ADDBOT       = 0x0F1650;
		constexpr std::uintptr_t ADDR_SVS_CLIENTS     = 0xC5FAA58;
		constexpr std::uintptr_t ADDR_SVS_CLIENTCOUNT = 0xC5FAA50;
		constexpr std::uintptr_t ADDR_DVAR_SVRUN      = 0x1BD2778;
		//    hub byte (SV_AddBot's first gate)  0x1BD36F8 - 0x1000 = 0x1BD26F8
		constexpr std::uintptr_t ADDR_HUB_BYTE        = 0x1BD26F8;
		//    ctz serializer  sub_14B820          0x14B820 - 0x1000 = 0x14A820
		constexpr std::uintptr_t ADDR_CTZ_SERIALIZE   = 0x14A820;
		//    SV_BuildBotUserinfo                 0x0B8D80 - 0x1000 = 0x0B7D80
		constexpr std::uintptr_t ADDR_BUILD_BOT_UI    = 0x0B7D80;
		//    build local player's cosmetics      0x18DC40 - 0x1000 = 0x18CC40
		constexpr std::uintptr_t ADDR_BUILD_LOCAL_CTZ = 0x18CC40;

		// SV_AddBot's own range, so the ctz hook can tell WHO is serializing.
		// SV_AddBot @ IDA 0xF2650, size 0x250 -> [0xF2650, 0xF28A0)
		constexpr std::uintptr_t SV_ADDBOT_LO = 0x0F2650;
		constexpr std::uintptr_t SV_ADDBOT_HI = 0x0F28A0;

		// Proven from SV_AddBot itself: it walks svs_clients as a _DWORD* in
		// steps of 293404, and 293404 * 4 == 1173616.
		constexpr std::size_t CLIENT_STRIDE   = 1173616;
		// `v20[67670] = 2` -> 67670 * 4 == 270680.
		constexpr std::size_t CLIENT_ISBOT    = 270680;
		constexpr int         ISBOT_VALUE     = 2;
		// The name the engine copies into, checked by SV_BotGetRandomName's own
		// duplicate test: I_stricmp(client + 23299, candidate).
		constexpr std::size_t CLIENT_NAME     = 23299;

		// Com_sprintf(v34, 0x1F, "%s", a1) -> 31 bytes including the NUL.
		constexpr std::size_t MAX_NAME_LEN = 30;

		// RULE A17 — the EXACT signature from the decompile. Returns a
		// gentity_s* (qword_BC6B8C8 + 1048*slot); do not narrow it.
		using SV_AddBot_t = std::uint64_t(__fastcall*)(const char*, unsigned int);
		SV_AddBot_t SV_AddBot_orig = nullptr;

		std::atomic<bool> g_installed{false};
		std::atomic<bool> g_enabled{true};
		std::atomic<std::size_t> g_renamed{0};

		std::vector<std::string> g_names;   // as read from the file
		std::vector<std::size_t> g_order;   // shuffled indices, consumed in order
		std::size_t g_next = 0;
		std::string g_path;
		std::uint64_t g_stamp = 0;          // last write time of the file
		bool g_warned_empty = false;

		// The game root holds ~800 files, so mod data lives in its own folder.
		// S2MP-Mod\ already exists there (it is where the LUI dump went), so
		// reuse it rather than adding another top-level directory.
		// Per-build, because the Store package directory is not writable. On the
		// Steam build this returns byte for byte the old <exe dir>\S2MP-Mod, so the
		// names and uniforms already on disk keep working. See ModPaths.hpp.
		std::string mod_dir()
		{
			return mod_paths::mod_dir();
		}

		std::uint64_t file_stamp(const std::string& path)
		{
			WIN32_FILE_ATTRIBUTE_DATA fad{};
			if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
			{
				return 0;
			}
			return (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32)
				| fad.ftLastWriteTime.dwLowDateTime;
		}

		// The connect string is a USERINFO string built as `\name\%s`, so a
		// backslash or a quote inside a name would corrupt the whole thing.
		// Strip anything that could break it, plus control characters.
		std::string sanitise(const std::string& in)
		{
			std::string out;
			out.reserve(in.size());
			for (const unsigned char c : in)
			{
				if (c == '\\' || c == '"' || c == ';' || c < 0x20 || c == 0x7f)
				{
					continue;
				}
				out.push_back(static_cast<char>(c));
			}
			// Trim both ends.
			const auto b = out.find_first_not_of(" \t");
			if (b == std::string::npos)
			{
				return {};
			}
			const auto e = out.find_last_not_of(" \t");
			out = out.substr(b, e - b + 1);

			// '^' + digit is a colour code and is PASSED THROUGH deliberately, so
			// a hand-written coloured name works. A trailing lone '^' is not a
			// colour code though -- it is malformed, and would swallow whatever
			// the renderer reads next. Drop it.
			while (!out.empty() && out.back() == '^')
			{
				out.pop_back();
			}
			if (out.size() > MAX_NAME_LEN)
			{
				out.resize(MAX_NAME_LEN);
			}
			return out;
		}

		void write_template()
		{
			FILE* fh = nullptr;
			if (fopen_s(&fh, g_path.c_str(), "wb") != 0 || !fh)
			{
				return;
			}
			fputs(
				"# S2MP-Mod bot names -- one per line.\r\n"
				"#\r\n"
				"# Any bot added while you play gets the next name from this list.\r\n"
				"# Real players are never touched: this only hooks the engine's own\r\n"
				"# bot-add path, which a human connection cannot reach.\r\n"
				"#\r\n"
				"# Edit and save while the game is running -- it reloads by itself.\r\n"
				"# Lines starting with '#' are ignored. Max 30 characters per name.\r\n"
				"#\r\n"
				"# Tip: tools/s2_players.py pulls every account you have played with\r\n"
				"# out of your own recordings, so you can build this from real lobbies\r\n"
				"# instead of typing it.\r\n"
				"\r\n", fh);
			fclose(fh);
		}

		void reshuffle()
		{
			g_order.resize(g_names.size());
			for (std::size_t i = 0; i < g_order.size(); ++i)
			{
				g_order[i] = i;
			}
			std::random_device rd;
			std::mt19937 gen(rd());
			std::shuffle(g_order.begin(), g_order.end(), gen);
			g_next = 0;
		}

		std::size_t load_names()
		{
			g_names.clear();
			FILE* fh = nullptr;
			if (fopen_s(&fh, g_path.c_str(), "rb") != 0 || !fh)
			{
				write_template();
				g_stamp = file_stamp(g_path);
				reshuffle();
				return 0;
			}

			char line[512];
			while (fgets(line, sizeof(line), fh))
			{
				std::string s(line);
				const auto hash = s.find('#');
				if (hash != std::string::npos)
				{
					s = s.substr(0, hash);
				}
				s = sanitise(s);
				if (!s.empty())
				{
					g_names.push_back(s);
				}
			}
			fclose(fh);

			g_stamp = file_stamp(g_path);
			reshuffle();
			return g_names.size();
		}

		// Pick the next unused name. Returns empty when we have run out, in
		// which case the engine generates its own as normal.
		std::string next_name()
		{
			if (g_names.empty())
			{
				return {};
			}
			if (g_next >= g_order.size())
			{
				// Ran out this match. Reshuffle so a long match keeps going
				// rather than silently reverting to engine names forever.
				reshuffle();
			}
			return g_names[g_order[g_next++]];
		}

		void check_file_changed()
		{
			const auto now = file_stamp(g_path);
			if (now != 0 && now != g_stamp)
			{
				const auto n = load_names();
				Console::printf("[bots] botnames.txt changed -- reloaded %zu name(s)", n);
			}
		}

		// Read back the isBot field of the client the engine just created.
		//
		// REPORT ONLY. It does not gate anything -- the real guarantee is that a
		// human cannot reach SV_AddBot at all. This exists so that if that
		// assumption ever stops holding, we find out loudly instead of quietly.
		//
		// The slot is located by matching the NAME we just supplied, not by
		// deriving an index from the returned gentity: the gentity array is a
		// different array with a different stride (1048), and mixing the two
		// would be exactly the offset inference this project keeps banning.
		// client + 23299 is the name field, independently confirmed by
		// SV_BotGetRandomName's own duplicate test.
		int read_isbot_for(const std::string& name)
		{
			const auto clients = *reinterpret_cast<std::uint8_t**>(_b(ADDR_SVS_CLIENTS));
			const auto count = *reinterpret_cast<std::int32_t*>(_b(ADDR_SVS_CLIENTCOUNT));
			if (!clients || count <= 0)
			{
				return -1;
			}
			for (std::int32_t i = 0; i < count; ++i)
			{
				std::uint8_t* c = clients + static_cast<std::size_t>(i) * CLIENT_STRIDE;
				if (*reinterpret_cast<std::int32_t*>(c) == 0)
				{
					continue;                       // free slot
				}
				const char* nm = reinterpret_cast<const char*>(c + CLIENT_NAME);
				if (_strnicmp(nm, name.c_str(), MAX_NAME_LEN + 1) != 0)
				{
					continue;
				}
				return *reinterpret_cast<std::int32_t*>(c + CLIENT_ISBOT);
			}
			return -1;                              // not found (yet)
		}

		// ===================================================================
		//  BOT COSMETICS — uniform / camo / etc.
		//
		//  PROVEN: the bot's look travels in the connect string as
		//      \ctz\<serialized customization>
		//  and the serializer is
		//      sub_14B820(struct, buf, size) =
		//          Com_sprintf(buf, size, "%d|%d|%d|%d|%d|%d|%d|",
		//              dword@+0, +4, +8, +12, +16, +20, byte@+24);
		//  i.e. SEVEN integers.
		//
		//  ⭐ The SAME serializer builds a REAL PLAYER's ctz, in Dvar_InfoString:
		//      sub_18DC40(localClient, v35);      // build the local look
		//      sub_14B820(v35, v42, 128);
		//      Info_SetValueForKey(userinfo, "ctz", v42);
		//  so bots and humans share one format -- copying a real look onto a bot
		//  is a straight struct copy, not a translation.
		//
		//  WHY HOOK THE SERIALIZER rather than the generator: the struct is a
		//  LOCAL inside SV_AddBot, so we cannot reach it directly -- but
		//  sub_14B820 receives a POINTER to it. Overwriting through that pointer
		//  and then letting the ENGINE serialize keeps the wire format the
		//  engine's own. And the caller check means Dvar_InfoString (the real
		//  player's own ctz) is never touched.
		// ===================================================================
		using CtzSerialize_t  = int(__fastcall*)(std::uint64_t, char*, int);
		using BuildLocalCtz_t = void(__fastcall*)(int, void*);
		CtzSerialize_t CtzSerialize_orig = nullptr;

		std::atomic<bool> g_ctz_installed{false};
		// 0 = off, 1 = copy mine, 2 = explicit, 3 = random from the kit list
		std::atomic<int>  g_ctz_mode{0};
		std::int32_t      g_ctz_values[7]{};
		std::atomic<std::size_t> g_ctz_applied{0};
		std::atomic<int>  g_ctz_chance{50};     // % of bots that get a custom kit

		// A named kit is just the 7 integers with a label.
		struct kit_t
		{
			std::string  name;
			std::int32_t v[7];
		};
		std::vector<kit_t> g_kits;
		std::string g_kits_path;

		// <gamedir>\S2MP-Mod\botkits.txt   --  "Name = a|b|c|d|e|f|g"
		std::size_t load_kits()
		{
			g_kits.clear();
			FILE* fh = nullptr;
			if (fopen_s(&fh, g_kits_path.c_str(), "rb") != 0 || !fh)
			{
				if (fopen_s(&fh, g_kits_path.c_str(), "wb") == 0 && fh)
				{
					fputs(
						"# S2MP-Mod bot kits -- one per line:\r\n"
						"#     KitName = a|b|c|d|e|f|g\r\n"
						"#\r\n"
						"# The seven integers are the game's OWN customization format --\r\n"
						"# exactly what it serializes for you into userinfo as \\ctz\\.\r\n"
						"# Run `bot_look` on its own to print YOUR current seven values,\r\n"
						"# then paste them here under a name.\r\n"
						"#\r\n"
						"# `bot_look random 50` gives each new bot a 50%% chance of one of\r\n"
						"# these, and otherwise leaves the engine's usual uniform alone.\r\n"
						"\r\n", fh);
					fclose(fh);
				}
				return 0;
			}

			char line[512];
			while (fgets(line, sizeof(line), fh))
			{
				std::string s(line);
				const auto hash = s.find('#');
				if (hash != std::string::npos)
				{
					s = s.substr(0, hash);
				}
				const auto eq = s.find('=');
				if (eq == std::string::npos)
				{
					continue;
				}
				kit_t k{};
				k.name = sanitise(s.substr(0, eq));
				std::string rest = s.substr(eq + 1);
				int n = 0;
				std::size_t pos = 0;
				while (n < 7 && pos <= rest.size())
				{
					const auto bar = rest.find('|', pos);
					const std::string tok = rest.substr(pos,
						bar == std::string::npos ? std::string::npos : bar - pos);
					k.v[n++] = std::atoi(tok.c_str());
					if (bar == std::string::npos)
					{
						break;
					}
					pos = bar + 1;
				}
				if (!k.name.empty() && n == 7)
				{
					g_kits.push_back(k);
				}
			}
			fclose(fh);
			return g_kits.size();
		}

		bool in_sv_addbot(const void* ret)
		{
			const auto a = reinterpret_cast<std::uintptr_t>(ret);
			const auto lo = _b(SV_ADDBOT_LO - 0x1000);
			const auto hi = _b(SV_ADDBOT_HI - 0x1000);
			return a >= lo && a < hi;
		}

		int __fastcall ctz_serialize_stub(std::uint64_t s, char* buf, int size)
		{
			const int mode = g_ctz_mode.load(std::memory_order_relaxed);
			if (mode == 0 || !in_sv_addbot(_ReturnAddress()))
			{
				// Not ours, or disabled -- includes every real player's ctz.
				return CtzSerialize_orig(s, buf, size);
			}

			std::int32_t v[7]{};
			if (mode == 3)
			{
				// Random: each bot independently rolls for a custom kit, so a
				// lobby comes out mixed rather than uniformly dressed.
				if (g_kits.empty())
				{
					return CtzSerialize_orig(s, buf, size);
				}
				static std::mt19937 gen{std::random_device{}()};
				std::uniform_int_distribution<int> pct(1, 100);
				if (pct(gen) > g_ctz_chance.load(std::memory_order_relaxed))
				{
					// Left alone on purpose -- this bot wears the usual uniform.
					return CtzSerialize_orig(s, buf, size);
				}
				std::uniform_int_distribution<std::size_t> pick(0, g_kits.size() - 1);
				const auto& k = g_kits[pick(gen)];
				for (int i = 0; i < 7; ++i)
				{
					v[i] = k.v[i];
				}
			}
			else if (mode == 1)
			{
				// Copy the LOCAL PLAYER's look, using the engine's own builder.
				alignas(8) unsigned char mine[64]{};
				const auto build = reinterpret_cast<BuildLocalCtz_t>(_b(ADDR_BUILD_LOCAL_CTZ));
				build(0, mine);
				for (int i = 0; i < 6; ++i)
				{
					v[i] = *reinterpret_cast<const std::int32_t*>(mine + 4 * i);
				}
				v[6] = mine[24];
			}
			else
			{
				for (int i = 0; i < 7; ++i)
				{
					v[i] = g_ctz_values[i];
				}
			}

			// Write through the pointer the engine handed us, then let the
			// ENGINE serialize it, so the wire format stays its own.
			auto* dst = reinterpret_cast<std::uint8_t*>(s);
			for (int i = 0; i < 6; ++i)
			{
				*reinterpret_cast<std::int32_t*>(dst + 4 * i) = v[i];
			}
			dst[24] = static_cast<std::uint8_t>(v[6]);

			g_ctz_applied.fetch_add(1, std::memory_order_relaxed);
			return CtzSerialize_orig(s, buf, size);
		}

		// ===================================================================
		//  EMBLEM + CALLING CARD
		//
		//  These are NOT part of the ctz costume struct. SV_UserinfoChanged
		//  reads them as their OWN userinfo keys:
		//      patch       -> *(WORD*)(client + 23416)
		//      callingcard -> *(WORD*)(client + 23418)
		//  and SV_BuildBotUserinfo's template carries NEITHER -- which is
		//  exactly why every bot ends up on the default emblem.
		//
		//  So the fix is to append them to the bot's userinfo, using the
		//  engine's own key format. Nothing is faked: SV_UserinfoChanged parses
		//  them the same way it parses a real player's.
		//
		//  RULE A17: int SV_BuildBotUserinfo(char* Buffer, int size).
		// ===================================================================
		using BuildBotUserinfo_t = int(__fastcall*)(char*, int);
		BuildBotUserinfo_t BuildBotUserinfo_orig = nullptr;
		std::atomic<bool> g_ui_installed{false};
		std::atomic<bool> g_ui_enabled{false};

		std::vector<std::string> g_tags;   // clan tags, max 4 chars each
		std::string g_tags_path;
		std::vector<int> g_emblems;
		std::vector<int> g_cards;
		std::string g_emblems_path;
		std::string g_cards_path;

		std::size_t load_id_list(const std::string& path, std::vector<int>& out)
		{
			out.clear();
			FILE* fh = nullptr;
			if (fopen_s(&fh, path.c_str(), "rb") != 0 || !fh)
			{
				return 0;
			}
			char line[256];
			while (fgets(line, sizeof(line), fh))
			{
				std::string s(line);
				const auto hash = s.find('#');
				if (hash != std::string::npos)
				{
					s = s.substr(0, hash);
				}
				const auto b = s.find_first_not_of(" \t\r\n");
				if (b == std::string::npos)
				{
					continue;
				}
				out.push_back(std::atoi(s.c_str() + b));
			}
			fclose(fh);
			return out.size();
		}

		std::size_t load_tags()
		{
			g_tags.clear();
			FILE* fh = nullptr;
			if (fopen_s(&fh, g_tags_path.c_str(), "rb") != 0 || !fh)
			{
				return 0;
			}
			char line[256];
			while (fgets(line, sizeof(line), fh))
			{
				std::string t(line);
				const auto hash = t.find('#');
				if (hash != std::string::npos)
				{
					t = t.substr(0, hash);
				}
				t = sanitise(t);
				// The engine copies 8 bytes; keep it short so the tag is
				// never truncated mid-way in the killfeed.
				if (t.size() > 4)
				{
					t.resize(4);
				}
				if (!t.empty())
				{
					g_tags.push_back(t);
				}
			}
			fclose(fh);
			return g_tags.size();
		}

		int __fastcall build_bot_userinfo_stub(char* buffer, const int size)
		{
			const int r = BuildBotUserinfo_orig(buffer, size);
			if (!g_ui_enabled.load(std::memory_order_relaxed)
				|| (g_emblems.empty() && g_cards.empty() && g_tags.empty()) || !buffer)
			{
				return r;
			}

			static std::mt19937 gen{std::random_device{}()};
			char extra[192]{};
			int n = 0;

			// Clan tag. Three keys, because the engine needs all three:
			//   clanAbbrev  -> client + 23408
			//   ec_TagText  -> client + 23424
			//   ec_usingTag -> client + 23420   (0 = do not display it)
			// Setting the text without ec_usingTag shows nothing, which is the
			// obvious way to get this subtly wrong.
			if (!g_tags.empty())
			{
				std::uniform_int_distribution<std::size_t> d(0, g_tags.size() - 1);
				const std::string& t = g_tags[d(gen)];
				n += sprintf_s(extra + n, sizeof(extra) - n,
					"\\clanAbbrev\\%s\\ec_TagText\\%s\\ec_usingTag\\1",
					t.c_str(), t.c_str());
			}

			if (!g_emblems.empty())
			{
				std::uniform_int_distribution<std::size_t> d(0, g_emblems.size() - 1);
				n += sprintf_s(extra + n, sizeof(extra) - n, "\\patch\\%d",
					g_emblems[d(gen)]);
			}
			if (!g_cards.empty())
			{
				std::uniform_int_distribution<std::size_t> d(0, g_cards.size() - 1);
				n += sprintf_s(extra + n, sizeof(extra) - n, "\\callingcard\\%d",
					g_cards[d(gen)]);
			}

			// Append only if it genuinely fits -- a truncated userinfo string
			// would corrupt every key after the cut.
			const std::size_t used = strnlen(buffer, static_cast<std::size_t>(size));
			if (used + static_cast<std::size_t>(n) + 1 < static_cast<std::size_t>(size))
			{
				memcpy(buffer + used, extra, static_cast<std::size_t>(n) + 1);
			}
			return r;
		}

		void cmd_bot_look()
		{
			const auto* args = GameUtil::getCmdArgs();
			const int argc = args ? args->argc[args->nesting] : 0;

			if (argc > 1)
			{
				const std::string a = args->argv[args->nesting][1];
				if (a == "off" || a == "0")
				{
					g_ctz_mode = 0;
					Console::printf("[bots] bot look: OFF (engine's own random/preset)");
					return;
				}
				if (a == "mine")
				{
					g_ctz_mode = 1;
					Console::printf("[bots] bot look: COPY MINE -- new bots get your "
						"uniform/camo via the engine's own sub_18DC40. Add bots to see it.");
					return;
				}
				if (a == "reload")
				{
					Console::printf("[bots] reloaded %zu kit(s) from %s",
						load_kits(), g_kits_path.c_str());
					return;
				}
				if (a == "list")
				{
					Console::printf("[bots] %zu kit(s) in %s", g_kits.size(),
						g_kits_path.c_str());
					for (const auto& k : g_kits)
					{
						Console::printf("[bots]   %-24s %d|%d|%d|%d|%d|%d|%d",
							k.name.c_str(), k.v[0], k.v[1], k.v[2], k.v[3], k.v[4],
							k.v[5], k.v[6]);
					}
					if (g_kits.empty())
					{
						Console::printf("[bots]   (empty -- run `bot_look` to print your "
							"own 7 values and paste them in as a kit)");
					}
					return;
				}
				if (a == "random")
				{
					if (argc > 2)
					{
						g_ctz_chance = (std::max)(0, (std::min)(100,
							GameUtil::safeStringToInt(args->argv[args->nesting][2])));
					}
					load_kits();
					g_ctz_mode = 3;
					Console::printf("[bots] bot look: RANDOM -- each bot has a %d%% chance "
						"of one of %zu kit(s); the rest wear the usual uniform.",
						g_ctz_chance.load(), g_kits.size());
					if (g_kits.empty())
					{
						Console::printf("[bots] ⚠ no kits defined yet, so nothing will "
							"change. Add some to %s", g_kits_path.c_str());
					}
					return;
				}
				if (a == "kit" && argc > 2)
				{
					load_kits();
					const std::string want = args->argv[args->nesting][2];
					for (const auto& k : g_kits)
					{
						if (_stricmp(k.name.c_str(), want.c_str()) == 0)
						{
							for (int i = 0; i < 7; ++i)
							{
								g_ctz_values[i] = k.v[i];
							}
							g_ctz_mode = 2;
							Console::printf("[bots] bot look: kit '%s' -> %d|%d|%d|%d|%d|%d|%d",
								k.name.c_str(), k.v[0], k.v[1], k.v[2], k.v[3], k.v[4],
								k.v[5], k.v[6]);
							return;
						}
					}
					Console::printf("[bots] no kit named '%s' -- `bot_look list`",
						want.c_str());
					return;
				}
				if (argc >= 8)
				{
					for (int i = 0; i < 7; ++i)
					{
						g_ctz_values[i] = GameUtil::safeStringToInt(
							args->argv[args->nesting][1 + i]);
					}
					g_ctz_mode = 2;
					Console::printf("[bots] bot look: EXPLICIT %d|%d|%d|%d|%d|%d|%d",
						g_ctz_values[0], g_ctz_values[1], g_ctz_values[2],
						g_ctz_values[3], g_ctz_values[4], g_ctz_values[5],
						g_ctz_values[6]);
					return;
				}
			}

			// Show the current state AND your own values, so there is something
			// concrete to copy/edit rather than a blank prompt.
			alignas(8) unsigned char mine[64]{};
			const auto build = reinterpret_cast<BuildLocalCtz_t>(_b(ADDR_BUILD_LOCAL_CTZ));
			build(0, mine);
			Console::printf("[bots] ctz hook   : %s", g_ctz_installed.load() ? "installed" : "NOT INSTALLED");
			Console::printf("[bots] bot look   : %s",
				g_ctz_mode == 0 ? "off" : (g_ctz_mode == 1 ? "copy mine" : "explicit"));
			Console::printf("[bots] applied to : %zu bot(s) so far",
				static_cast<std::size_t>(g_ctz_applied.load()));
			Console::printf("[bots] YOUR look  : %d|%d|%d|%d|%d|%d|%d",
				*reinterpret_cast<int*>(mine + 0), *reinterpret_cast<int*>(mine + 4),
				*reinterpret_cast<int*>(mine + 8), *reinterpret_cast<int*>(mine + 12),
				*reinterpret_cast<int*>(mine + 16), *reinterpret_cast<int*>(mine + 20),
				mine[24]);
			Console::printf("[bots] usage: bot_look mine | off | random <pct> | kit <name> | list | reload | <7 integers>");
			Console::printf("[bots] kits file : %s (%zu loaded)", g_kits_path.c_str(), g_kits.size());
			Console::printf("[bots] (same 7-field format the game uses for YOU -- "
				"Dvar_InfoString serializes it with the very same function)");
		}

		std::uint64_t __fastcall sv_addbot_stub(const char* name, const unsigned int idx)
		{
			// The very first thing, before touching anything, so "the hook never
			// ran" is distinguishable from "it ran and did nothing" (RULE A3).
			if (!g_enabled.load(std::memory_order_relaxed))
			{
				return SV_AddBot_orig(name, idx);
			}

			check_file_changed();

			const std::string pick = next_name();
			if (pick.empty())
			{
				if (!g_warned_empty)
				{
					g_warned_empty = true;
					Console::printf("[bots] no names in %s -- bots keep their engine "
						"names. Put one name per line in that file.", g_path.c_str());
				}
				return SV_AddBot_orig(name, idx);
			}

			const std::uint64_t ent = SV_AddBot_orig(pick.c_str(), idx);
			if (!ent)
			{
				// The engine refused (no free slot, or we are in the hub). Give
				// the name back so it is not silently burned.
				if (g_next > 0)
				{
					--g_next;
				}
				return ent;
			}

			const auto n = g_renamed.fetch_add(1, std::memory_order_relaxed) + 1;
			const int isbot = read_isbot_for(pick);
			if (isbot == ISBOT_VALUE)
			{
				Console::printf("[bots] bot #%zu named \"%s\"  (isBot=%d, verified)",
					n, pick.c_str(), isbot);
			}
			else if (isbot < 0)
			{
				// The client exists but has not been filled in yet at this point
				// in the connect. Not an error -- just say we could not confirm.
				Console::printf("[bots] bot #%zu named \"%s\"  (isBot not readable yet)",
					n, pick.c_str());
			}
			else
			{
				// Should be impossible: SV_AddBot sets this field itself. Say so
				// loudly rather than carrying on quietly.
				Console::printf("[bots] WARNING bot #%zu named \"%s\" but isBot read "
					"%d (expected %d) -- report this log; the safety assumption may have "
					"changed.", n, pick.c_str(), isbot, ISBOT_VALUE);
			}
			return ent;
		}

		// SV_AddBot's FIRST gate is `if (unk_1BD36F8) return 0;`. This project
		// already measured that byte as the hub/frontend discriminator: 1 in
		// mp_hub_allies_slim, 0 on real maps (mp_shipment_s2, mp_canon_farm,
		// mp_sandbox_01), corroborated independently by the asset bracket and
		// by the native-recording gate, which uses the same byte.
		//
		// Reading it lets us say WHICH of SV_AddBot's two refusal reasons fired
		// instead of offering the user a guess.
		std::uint16_t hub_word()
		{
			return *reinterpret_cast<std::uint16_t*>(_b(ADDR_HUB_BYTE));
		}

		// svs_clients survives a match teardown, so a non-null pointer is NOT
		// proof that a server is up -- it can be the previous map's stale array.
		// com_sv_running is the authority (S2 dvar layout: +12 type, +16 value).
		bool server_running()
		{
			const auto slot = reinterpret_cast<const std::uint8_t* const*>(_b(ADDR_DVAR_SVRUN));
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !*slot)
			{
				return false;
			}
			if (!VirtualQuery(*slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
			{
				return false;
			}
			return *(*slot + 16) != 0;
		}

		// How many client slots exist, and how many are free.
		// SV_AddBot's own free-slot walk is `if (!*v6) break;` over svs_clients
		// in steps of 293404 dwords, so slot state == 0 means free. Same test.
		bool slot_counts(int& used, int& total)
		{
			const auto clients = *reinterpret_cast<std::uint8_t**>(_b(ADDR_SVS_CLIENTS));
			const auto count = *reinterpret_cast<std::int32_t*>(_b(ADDR_SVS_CLIENTCOUNT));
			if (!clients || count <= 0)
			{
				return false;
			}
			total = count;
			used = 0;
			for (std::int32_t i = 0; i < count; ++i)
			{
				if (*reinterpret_cast<std::int32_t*>(
					clients + static_cast<std::size_t>(i) * CLIENT_STRIDE) != 0)
				{
					++used;
				}
			}
			return true;
		}

		// Add bots ourselves instead of going through the menu.
		//
		// WHY THIS WORKS WHERE THE MENU DOES NOT: the menu path is gated by
		// LUI_SetBotsTeamLimit, whose own condition is
		//     !Com_IsOnlineGame() || (in a lobby && we are the game host)
		// so in an online-looking match the bot options are simply unavailable.
		// SV_AddBot is an ordinary function with no such gate -- its only checks
		// are the hub byte and a free client slot -- so calling it directly
		// sidesteps the menu entirely without faking any state.
		//
		// We deliberately call the PATCHED entry point with an empty name, so our
		// own detour runs and the existing (tested) naming, sanitising, isBot
		// verification and logging all apply unchanged.
		void cmd_bots_fill()
		{
			if (!g_installed.load())
			{
				Console::printf("[bots] hook is not installed -- cannot add bots.");
				return;
			}

			if (!server_running())
			{
				Console::printf("[bots] no server is running yet. Bots are SERVER "
					"CLIENTS, so the map has to be loaded first -- run this during "
					"the pre-match countdown, not on the lobby screen.");
				return;
			}

			int used = 0, total = 0;
			if (!slot_counts(used, total))
			{
				Console::printf("[bots] com_sv_running is set but there are no client "
					"slots (svs_clients null) -- the server is still starting up.");
				return;
			}

			const auto* args = GameUtil::getCmdArgs();
			int want = total - used;                       // default: fill the lobby
			if (args && args->argc[args->nesting] > 1)
			{
				want = GameUtil::safeStringToInt(args->argv[args->nesting][1]);
			}
			if (want <= 0)
			{
				Console::printf("[bots] slots: %d used of %d, %d free. "
					"Usage: bots_fill [count]", used, total, total - used);
				return;
			}

			// Check SV_AddBot's own first gate BEFORE calling, so a refusal is
			// explained rather than guessed at.
			const auto hub = hub_word();
			if (hub != 0)
			{
				Console::printf("[bots] REFUSED: SV_AddBot's hub gate is closed "
					"(unk_1BD36F8 = 0x%04X, must be 0). You are in the hub / "
					"frontend, not on a real map -- %d of %d slots are free, so "
					"slots are not the problem.", hub, total - used, total);
				Console::printf("[bots] svs_clientCount is %d; the hub branch of "
					"SV_Init uses a hardcoded 48, which matches. Get into an actual "
					"map and run this again.", total);
				return;
			}

			const auto entry = reinterpret_cast<SV_AddBot_t>(_b(ADDR_SV_ADDBOT));
			int added = 0;
			for (int i = 0; i < want; ++i)
			{
				// Alternate the faction index so they do not all come from one
				// costume preset. NOTE: SV_AddBot only uses this argument for
				// sub_6F72A0(a2), the costume preset -- and it does not use it at
				// all once a name is supplied. So this is NOT team assignment;
				// team is still decided elsewhere and remains an open question.
				if (!entry("", static_cast<unsigned int>((i & 1) ? 1 : 2)))
				{
					Console::printf("[bots] SV_AddBot refused after %d. hubByte=0x%04X "
						"(0 = ok), slots %d used of %d. If the hub byte is 0 then "
						"every slot is taken.", added, hub_word(), used + added, total);
					break;
				}
				++added;
			}

			slot_counts(used, total);
			Console::printf("[bots] added %d bot(s); slots now %d used of %d.",
				added, used, total);
		}

		void cmd_bot_names()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] > 1)
			{
				const std::string a = args->argv[args->nesting][1];
				if (a == "0" || a == "off")
				{
					set_enabled(false);
					Console::printf("[bots] renaming OFF -- bots keep their engine names.");
					return;
				}
				if (a == "1" || a == "on")
				{
					set_enabled(true);
					Console::printf("[bots] renaming ON.");
					return;
				}
				if (a == "reload")
				{
					Console::printf("[bots] reloaded %zu name(s) from %s",
						reload(), g_path.c_str());
					return;
				}
			}

			Console::printf("[bots] ---------------------------------------------");
			Console::printf("[bots] hook      : %s%s", g_installed.load() ? "installed" : "NOT INSTALLED",
				g_installed.load() ? "" : "  <- renaming cannot work");
			Console::printf("[bots] renaming  : %s", g_enabled.load() ? "ON" : "off");
			Console::printf("[bots] name list : %s", g_path.c_str());
			Console::printf("[bots] names     : %zu loaded, %zu used so far",
				g_names.size(), static_cast<std::size_t>(g_renamed.load()));
			if (g_names.empty())
			{
				Console::printf("[bots] the list is EMPTY -- put one name per line in that file.");
			}
			else
			{
				std::string preview;
				for (std::size_t i = 0; i < g_names.size() && i < 6; ++i)
				{
					preview += (i ? ", " : "") + g_names[i];
				}
				if (g_names.size() > 6)
				{
					preview += ", ...";
				}
				Console::printf("[bots] e.g.      : %s", preview.c_str());
			}
			Console::printf("[bots] usage     : bot_names [0|1|reload]");
			Console::printf("[bots] real players are never affected -- this hooks the");
			Console::printf("[bots] engine's bot-add path, which a human cannot reach.");
		}
	}

	bool enabled()
	{
		return g_installed.load(std::memory_order_relaxed)
			&& g_enabled.load(std::memory_order_relaxed);
	}

	void set_enabled(const bool on)
	{
		g_enabled.store(on, std::memory_order_relaxed);
	}

	std::size_t reload()
	{
		return load_names();
	}

	std::size_t name_count()
	{
		return g_names.size();
	}

	std::size_t renamed_count()
	{
		return g_renamed.load(std::memory_order_relaxed);
	}

	std::size_t kit_count()
	{
		return g_kits.size();
	}

	std::size_t reload_kits()
	{
		return load_kits();
	}

	int look_mode()
	{
		return g_ctz_mode.load(std::memory_order_relaxed);
	}

	int look_chance()
	{
		return g_ctz_chance.load(std::memory_order_relaxed);
	}

	void set_look_random(const int percent)
	{
		g_ctz_chance = (std::max)(0, (std::min)(100, percent));
		load_kits();
		g_ctz_mode = 3;
	}

	void set_look_off()
	{
		g_ctz_mode = 0;
	}

	bool slots(int& used, int& total)
	{
		if (!server_running())
		{
			return false;
		}
		return slot_counts(used, total);
	}

	bool in_hub()
	{
		return hub_word() != 0;
	}

	// One call for everything except the bot count -- names on, and a mixed
	// spread of the game's own uniforms. Deliberately does NOT add bots: how
	// many is the user's decision.
	void preset_lobby(const int uniform_percent)
	{
		set_enabled(true);
		load_names();
		set_look_random(uniform_percent);
		load_id_list(g_emblems_path, g_emblems);
		load_id_list(g_cards_path, g_cards);
		load_tags();
		g_ui_enabled = true;

		Console::printf("[bots] lobby preset: %zu real name(s), %zu uniform(s), "
			"%d%% of bots get a custom uniform; %zu emblem(s) and %zu calling card(s) "
			"in the pool.", g_names.size(), g_kits.size(), g_ctz_chance.load(),
			g_emblems.size(), g_cards.size());
		if (g_names.empty())
		{
			Console::printf("[bots] ⚠ no names in %s", g_path.c_str());
		}
		if (g_kits.empty())
		{
			Console::printf("[bots] ⚠ no kits in %s -- run tools/s2_botkits.py",
				g_kits_path.c_str());
		}
	}

	void init()
	{
		g_path = mod_dir() + "\\botnames.txt";
		const auto n = load_names();

		// RULE A3 — a hook is not installed until it says so, and a non-null
		// original is the only proof (Hook::create reports duplicates now).
		g_installed = Hook::create("SV_AddBot", _b(ADDR_SV_ADDBOT),
			&sv_addbot_stub, &SV_AddBot_orig) && SV_AddBot_orig != nullptr;

		Console::printf("[bots] SV_AddBot hook: %s (orig=%p), %zu name(s) from %s",
			g_installed.load() ? "OK" : "FAILED",
			reinterpret_cast<void*>(SV_AddBot_orig), n, g_path.c_str());

		if (g_installed.load() && n == 0)
		{
			Console::printf("[bots] name list is empty -- edit %s and add one name "
				"per line. It reloads by itself; no restart needed.", g_path.c_str());
		}

		// The ctz serializer. RULE A3.1 checked: nothing else in src/ hooks it.
		// Off by default, and the caller check means a real player's own ctz is
		// never touched even when it is on.
		g_ctz_installed = Hook::create("ctz_serialize", _b(ADDR_CTZ_SERIALIZE),
			&ctz_serialize_stub, &CtzSerialize_orig) && CtzSerialize_orig != nullptr;
		Console::printf("[bots] bot-look (ctz) hook: %s (orig=%p) -- `bot_look mine` "
			"gives bots your uniform/camo", g_ctz_installed.load() ? "OK" : "FAILED",
			reinterpret_cast<void*>(CtzSerialize_orig));

		// Emblem / calling card. Off by default; preset_lobby turns it on.
		g_ui_installed = Hook::create("SV_BuildBotUserinfo", _b(ADDR_BUILD_BOT_UI),
			&build_bot_userinfo_stub, &BuildBotUserinfo_orig)
			&& BuildBotUserinfo_orig != nullptr;
		Console::printf("[bots] emblem/card/tag hook: %s (orig=%p), %zu emblem(s) %zu card(s) %zu tag(s)",
			g_ui_installed.load() ? "OK" : "FAILED",
			reinterpret_cast<void*>(BuildBotUserinfo_orig), g_emblems.size(), g_cards.size(),
			g_tags.size());

		dev_mode::add_command("bot_names", cmd_bot_names);
		dev_mode::add_command("bots_fill", cmd_bots_fill);
		dev_mode::add_command("bot_look", cmd_bot_look);
	}
}
