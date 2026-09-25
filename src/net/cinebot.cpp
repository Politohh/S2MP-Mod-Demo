#include "pch.h"
#include "net/cinebot.hpp"
#include "net/cinebot_constants.hpp"

#include "Console.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"
#include "Hook.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <wincrypt.h>

#pragma comment(lib, "Advapi32.lib")

namespace
{
	using namespace std::chrono_literals;
	using namespace s2cinebot;

	struct vec3
	{
		float x{};
		float y{};
		float z{};
	};

	struct bot_state
	{
		void* entity{};
		int slot{-1};
		vec3 anchor{};
		bool active{};
		bool frozen{true};
		bool was_alive{};
		bool placed_once{};
		int assigned_team{};
		int menu_attempts{};
		int last_connection_state{-1};
		int last_team{-1};
		bool ever_connected{};
		bool rushing{};
		bool rush_reported{};
		ULONGLONG rush_started{};
		std::uint64_t rush_call_baseline{};
		vec3 rush_origin{};
		vec3 rush_target{};
	};

	struct rush_command
	{
		void* entity{};
		vec3 origin{};
		vec3 angles{};
		float forward_x{};
		float forward_y{};
		float run_distance{};
	};

	std::uintptr_t game_base{};
	std::filesystem::path module_directory;
	std::mutex log_mutex;
	std::mutex view_mutex;
	std::vector<cinebot::bot_view> published_bots;
	std::atomic_bool g_available{};
	std::atomic_int active_count{};
	std::atomic_bool update_pending{};
	std::atomic_bool spawn_pending{};
	std::atomic_bool move_pending{};
	std::atomic_bool toggle_pending{};
	std::atomic_bool rush_pending{};
	int activate_binding{};
	int ads_binding{};
	bool ads_held{};
	bool use_held{};
	inline constexpr std::size_t kMaxTrackedBots = 64;
	std::array<bot_state, kMaxTrackedBots> bots{};
	std::array<rush_command, kMaxTrackedBots> rush_commands{};
	std::array<std::atomic_uint, kMaxTrackedBots> rush_command_hits{};
	std::atomic_uint64_t test_client_command_calls{};
	std::mutex rush_mutex;
	using test_client_command_fn = std::int64_t(*)(void*, void*);
	test_client_command_fn original_test_client_command{};
	bool rush_hook_ready{};
	std::atomic_int selected_bot_slot{-1};
	float placement_distance = 250.0f;
	float last_trace_fraction = 1.0f;
	inline constexpr std::size_t kUsercmdButtonsOffset = 8;
	inline constexpr std::size_t kUsercmdForwardOffset = 52;
	inline constexpr std::size_t kUsercmdRightOffset = 53;
	inline constexpr std::size_t kUsercmdUpOffset = 54;
	inline constexpr std::uint64_t kBotSprintButton = 0x2;

	template <typename T>
	T at(const std::uintptr_t rva)
	{
		return reinterpret_cast<T>(game_base + rva);
	}

	void log(const std::string& message)
	{
		std::lock_guard lock(log_mutex);
		Console::printf("[cinebot] %s", message.c_str());
		SYSTEMTIME time{};
		GetLocalTime(&time);
		std::ofstream file(module_directory / "S2CineBot.log", std::ios::app);
		file << std::setfill('0') << '[' << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute
			<< ':' << std::setw(2) << time.wSecond << "] " << message << '\n';
	}

	bool executable_address(const void* address)
	{
		MEMORY_BASIC_INFORMATION info{};
		if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
			return false;
		const auto protection = info.Protect & 0xFF;
		return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
			protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
	}

	bool supported_image()
	{
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game_base);
		if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(game_base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kGameTimeDateStamp ||
			nt->OptionalHeader.SizeOfImage != kGameSizeOfImage)
			return false;

		return executable_address(at<void*>(rva::SV_AddBot)) &&
			executable_address(at<void*>(rva::SV_SpawnTestClient)) &&
			executable_address(at<void*>(rva::SV_SetAssignedTeam)) &&
			executable_address(at<void*>(rva::G_SetPlayerOrigin)) &&
			executable_address(at<void*>(rva::G_SetPlayerAngles)) &&
			executable_address(at<void*>(rva::G_GetPlayerUsercmd)) &&
			executable_address(at<void*>(rva::SV_LinkEntity)) &&
			executable_address(at<void*>(rva::SV_IsTestClient)) &&
			executable_address(at<void*>(rva::SV_RefreshTestClient)) &&
			executable_address(at<void*>(rva::G_LocationalTrace)) &&
			executable_address(at<void*>(rva::Scr_AddInt)) &&
			executable_address(at<void*>(rva::Scr_AddString)) &&
			executable_address(at<void*>(rva::Scr_Notify)) &&
			executable_address(at<void*>(rva::SL_GetString));
	}

	std::string sha256(const std::filesystem::path& path)
	{
		HCRYPTPROV provider{};
		HCRYPTHASH hash{};
		if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
			return {};
		if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
		{
			CryptReleaseContext(provider, 0);
			return {};
		}
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		bool ok = file != INVALID_HANDLE_VALUE;
		std::vector<BYTE> buffer(1024 * 1024);
		DWORD count{};
		while (ok)
		{
			if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
			{
				ok = false;
				break;
			}
			if (count == 0) break;
			ok = CryptHashData(hash, buffer.data(), count, 0) != FALSE;
		}
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
		std::array<BYTE, 32> digest{};
		DWORD size = static_cast<DWORD>(digest.size());
		ok = ok && CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &size, 0);
		CryptDestroyHash(hash);
		CryptReleaseContext(provider, 0);
		if (!ok || size != digest.size()) return {};
		std::ostringstream out;
		out << std::hex << std::uppercase << std::setfill('0');
		for (const auto byte : digest) out << std::setw(2) << static_cast<unsigned>(byte);
		return out.str();
	}

	bool in_custom_game()
	{
		using bool_fn = bool(*)();
		const auto loaded = at<bool_fn>(rva::SV_Loaded)();
		const auto local_client = at<bool(*)(int)>(rva::CL_IsLocalClientInGame)(0);
		const auto virtual_lobby = *at<const bool*>(rva::virtualLobby_Loaded);
		return loaded && local_client && !virtual_lobby;
	}

	bool capture_crosshair(vec3& result)
	{
		if (!in_custom_game()) return false;
		vec3 origin{};
		vec3 forward{};
		at<void(*)(int, float*)>(rva::CL_GetViewPos)(0, &origin.x);
		at<void(*)(int, float*)>(rva::CL_GetViewForward)(0, &forward.x);
		const auto length = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
		if (!std::isfinite(length) || length < 0.5f) return false;
		constexpr float trace_distance = 100000.0f;
		const vec3 end{origin.x + forward.x * trace_distance,
			origin.y + forward.y * trace_distance,
			origin.z + forward.z * trace_distance};
		alignas(16) std::array<std::byte, 0x80> trace{};
		using trace_fn = void(*)(void*, const float*, const float*, unsigned short, std::uint64_t, const void*);
		at<trace_fn>(rva::G_LocationalTrace)(trace.data(), &origin.x, &end.x, 0, 0x280E831, nullptr);
		const auto fraction = *reinterpret_cast<const float*>(trace.data());
		last_trace_fraction = fraction;
		if (std::isfinite(fraction) && fraction >= 0.0f && fraction < 1.0f)
		{
			result = {origin.x + (end.x - origin.x) * fraction,
				origin.y + (end.y - origin.y) * fraction,
				origin.z + (end.z - origin.z) * fraction};
		}
		else
		{
			result = {origin.x + forward.x * placement_distance,
				origin.y + forward.y * placement_distance,
				origin.z + forward.z * placement_distance};
		}
		return true;
	}

	void set_player_origin(void* entity, const vec3& origin)
	{
		// This is the same native sequence used by the stock GSC setorigin method
		// for a player entity. G_SetPlayerOrigin updates playerState (the generic
		// G_SetOrigin only updates the entity trajectory and is not sufficient).
		at<void(*)(void*, const float*, int)>(rva::G_SetPlayerOrigin)(entity, &origin.x, 1);
		at<void(*)(void*)>(rva::SV_LinkEntity)(entity);
		if (at<int(*)(void*)>(rva::SV_IsTestClient)(entity))
		{
			at<void(*)(void*)>(rva::SV_RefreshTestClient)(entity);
		}
	}

	vec3 entity_origin(const void* entity)
	{
		const auto* values = reinterpret_cast<const float*>(
			static_cast<const std::byte*>(entity) + kEntityOriginOffset);
		return {values[0], values[1], values[2]};
	}

	float distance(const vec3& a, const vec3& b)
	{
		return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z);
	}

	bool look_at(const vec3& from, const vec3& target, vec3& angles)
	{
		const float dx = target.x - from.x;
		const float dy = target.y - from.y;
		const float planar = std::hypot(dx, dy);
		if (planar < 16.0f) return false;
		constexpr float radians_to_degrees = 57.29577951308232f;
		angles = {
			-std::atan2(target.z - from.z, planar) * radians_to_degrees,
			std::atan2(dy, dx) * radians_to_degrees, 0.0f
		};
		return true;
	}

	void clear_rush_command(const int slot)
	{
		if (slot < 0 || static_cast<std::size_t>(slot) >= rush_commands.size()) return;
		std::lock_guard lock(rush_mutex);
		rush_commands[static_cast<std::size_t>(slot)] = {};
	}

	void clear_all_rush_commands()
	{
		std::lock_guard lock(rush_mutex);
		rush_commands = {};
	}

	std::int64_t test_client_command_hook(void* client, void* command)
	{
		++test_client_command_calls;
		if (!client || !command) return original_test_client_command(client, command);
		// This is the actual SV test-client command consumer. The Build 27
		// bot-input hook was installed but never matched these spawned clients.
		const auto* clients = *at<std::byte**>(rva::svs_clients);
		const auto client_address = reinterpret_cast<std::uintptr_t>(client);
		const auto base_address = reinterpret_cast<std::uintptr_t>(clients);
		if (!clients || client_address < base_address)
			return original_test_client_command(client, command);
		const auto offset = client_address - base_address;
		if (offset % kClientStride != 0)
			return original_test_client_command(client, command);
		const auto slot = offset / kClientStride;
		if (slot >= rush_commands.size())
			return original_test_client_command(client, command);
		rush_command rush{};
		{
			std::lock_guard lock(rush_mutex);
			rush = rush_commands[slot];
		}
		if (!rush.entity || rush.entity != at<std::byte*>(rva::g_entities) +
			slot * kEntityStride)
			return original_test_client_command(client, command);
		if (*reinterpret_cast<const int*>(static_cast<const std::byte*>(rush.entity) + kEntityHealthOffset) <= 0)
			return original_test_client_command(client, command);
		const auto position = entity_origin(rush.entity);
		const float progress = (position.x - rush.origin.x) * rush.forward_x +
			(position.y - rush.origin.y) * rush.forward_y;
		auto* bytes = static_cast<std::byte*>(command);
		// Override the native AI's command BEFORE the engine copies and runs it.
		// Physics still handles running, collision and death animation.
		const bool running = progress < rush.run_distance - 48.0f;
		bytes[kUsercmdForwardOffset] = std::byte(running ? 127 : 0);
		bytes[kUsercmdRightOffset] = std::byte(0);
		bytes[kUsercmdUpOffset] = std::byte(0);
		auto* buttons = reinterpret_cast<std::uint64_t*>(bytes + kUsercmdButtonsOffset);
		*buttons = running ? kBotSprintButton : 0;
		// G_SetPlayerAngles computes delta_angles against the PREVIOUS command.
		// Keep the new command's angle words equal to those previous words so
		// ClientThink cannot reapply an AI turn on top of our fixed world yaw.
		if (const auto* prior = at<const std::byte*(*)(void*)>(rva::G_GetPlayerUsercmd)(rush.entity))
			std::memcpy(bytes + 16, prior + 16, 3 * sizeof(std::int32_t));
		at<void(*)(void*, const float*)>(rva::G_SetPlayerAngles)(rush.entity, &rush.angles.x);
		++rush_command_hits[slot];
		return original_test_client_command(client, command);
	}

	unsigned int lui_notify_event()
	{
		using get_string_fn = unsigned int(*)(const char*, unsigned int);
		static unsigned int event{};
		if (!event)
		{
			// Scr_Notify consumes an SL runtime string, not a compiler canonical token.
			event = at<get_string_fn>(rva::SL_GetString)("luinotifyserver", 0);
		}
		return event;
	}

	void notify_team_select(void* entity, const int selection)
	{
		using add_int_fn = void(*)(int);
		using add_string_fn = void(*)(const char*);
		using notify_fn = void(*)(void*, unsigned int, unsigned int);
		// WWII's stock _menus.gsc listens for:
		// self notify("luinotifyserver", "team_select", selection)
		// VM arguments are pushed last-to-first.
		at<add_int_fn>(rva::Scr_AddInt)(selection);
		at<add_string_fn>(rva::Scr_AddString)("team_select");
		at<notify_fn>(rva::Scr_Notify)(entity, lui_notify_event(), 2);
	}

	void notify_class_select(void* entity)
	{
		using add_string_fn = void(*)(const char*);
		using notify_fn = void(*)(void*, unsigned int, unsigned int);
		// WWII's stock _menus.gsc listens for:
		// self notify("luinotifyserver", "class_select", "class0")
		at<add_string_fn>(rva::Scr_AddString)("class0");
		at<add_string_fn>(rva::Scr_AddString)("class_select");
		at<notify_fn>(rva::Scr_Notify)(entity, lui_notify_event(), 2);
	}

	bool valid_bot_entity(const bot_state& bot)
	{
		if (!bot.active || !bot.entity || bot.slot < 0) return false;
		const auto max_clients = *at<const int*>(rva::sv_maxclients);
		if (bot.slot >= max_clients) return false;
		const auto expected = at<std::byte*>(rva::g_entities) + static_cast<std::size_t>(bot.slot) * kEntityStride;
		if (expected != bot.entity) return false;
		const auto clients = *at<std::byte**>(rva::svs_clients);
		if (!clients) return false;
		const auto client = clients + static_cast<std::size_t>(bot.slot) * kClientStride;
		if (*reinterpret_cast<const int*>(client + kClientStateOffset) == 0) return false;
		if (*reinterpret_cast<const int*>(client + kClientTestClientOffset) == 0) return false;
		return *reinterpret_cast<void* const*>(client + kClientEntityOffset) == bot.entity;
	}

	bot_state* get_selected_bot()
	{
		if (selected_bot_slot >= 0 && static_cast<std::size_t>(selected_bot_slot) < bots.size())
		{
			auto& bot = bots[static_cast<std::size_t>(selected_bot_slot)];
			if (valid_bot_entity(bot)) return &bot;
		}
		return nullptr;
	}

	void publish_bots()
	{
		std::vector<cinebot::bot_view> next;
		for (const auto& bot : bots)
		{
			if (bot.active)
				next.push_back({bot.slot, bot.was_alive, bot.frozen,
					bot.anchor.x, bot.anchor.y, bot.anchor.z});
		}
		active_count.store(static_cast<int>(next.size()));
		std::lock_guard lock(view_mutex);
		published_bots = std::move(next);
	}

	void update_bot(bot_state& bot)
	{
		if (!bot.active) return;
		if (bot.slot >= 0)
		{
			const auto clients = *at<std::byte**>(rva::svs_clients);
			if (clients)
			{
				const auto session = clients + static_cast<std::size_t>(bot.slot) * kClientStride;
				const int state = *reinterpret_cast<const int*>(session + kClientStateOffset);
				const auto entity = static_cast<std::byte*>(bot.entity);
				const auto game_client = *reinterpret_cast<std::byte**>(entity + kEntityClientOffset);
				const int team = game_client ? *reinterpret_cast<const int*>(game_client + kGameClientTeamOffset) : -1;
				if (state != bot.last_connection_state || team != bot.last_team)
				{
					std::ostringstream line;
					line << "Bot slot " << bot.slot << " state=" << state << ", team=" << team << '.';
					log(line.str());
					bot.last_connection_state = state;
					bot.last_team = team;
				}
				if (state > 0) bot.ever_connected = true;
				if (bot.ever_connected && state == 0)
				{
					clear_rush_command(bot.slot);
					log("Bot slot " + std::to_string(bot.slot) + " disconnected before becoming alive.");
					bot.active = false;
					return;
				}
			}
		}
		if (!valid_bot_entity(bot)) return;

		const auto entity = static_cast<std::byte*>(bot.entity);
		auto* game_client = *reinterpret_cast<std::byte**>(entity + kEntityClientOffset);
		const auto has_client = game_client != nullptr;
		const auto health = *reinterpret_cast<int*>(entity + kEntityHealthOffset);
		const bool alive = has_client && health > 0;
		if (!alive && bot.menu_attempts < 150)
		{
			// team_select uses UI choices (0=axis, 1=allies), while sessionTeam
			// uses the engine enum (1=axis, 2=allies).
			const int team_selection = bot.assigned_team == 1 ? 0 : 1;
			if (bot.last_team != bot.assigned_team)
			{
				notify_team_select(bot.entity, team_selection);
			}
			if (bot.last_team == bot.assigned_team || bot.menu_attempts >= 5)
			{
				notify_class_select(bot.entity);
			}
			if (bot.menu_attempts == 0)
			{
				log(std::string("Started native WWII team/class notifications for slot ") +
					std::to_string(bot.slot) + ": team=" +
					(bot.assigned_team == 1 ? "axis" : "allies") + ", class=class0.");
			}
			++bot.menu_attempts;
		}
		if (alive && !bot.was_alive)
		{
			set_player_origin(bot.entity, bot.anchor);
			log("Bot slot " + std::to_string(bot.slot) + (bot.placed_once ?
				" respawn detected; restored the saved anchor." :
				" entered the match; applied the saved anchor."));
			bot.placed_once = true;
		}
		else if (!alive && bot.was_alive)
		{
			if (bot.rushing)
			{
				clear_rush_command(bot.slot);
				bot.rushing = false;
				bot.frozen = true; // Respawn returns to the saved stationary setup.
			}
			log("Bot slot " + std::to_string(bot.slot) +
				" death detected; placement is paused for the death animation.");
		}
		if (alive && bot.rushing && bot.frozen)
		{
			const auto hits = rush_command_hits[static_cast<std::size_t>(bot.slot)].load();
			if (hits > 0)
			{
				bot.frozen = false;
				log("F4 bot " + std::to_string(bot.slot) +
					" released after the test-client command override became active.");
			}
			else if (GetTickCount64() - bot.rush_started >= 500)
			{
				const auto calls = test_client_command_calls.load() - bot.rush_call_baseline;
				clear_rush_command(bot.slot);
				bot.rushing = false;
				log("F4 bot " + std::to_string(bot.slot) +
					" remains frozen: no matching test-client commands after 500 ms; hook calls=" +
					std::to_string(calls) + '.');
			}
		}
		if (alive && bot.rushing && !bot.rush_reported &&
			GetTickCount64() - bot.rush_started >= 1500)
		{
			const auto current = entity_origin(bot.entity);
			const auto moved = distance(current, bot.rush_origin);
			const auto toward = distance(bot.rush_origin, bot.rush_target) -
				distance(current, bot.rush_target);
			log("F4 rush observation: bot " + std::to_string(bot.slot) +
				" moved " + std::to_string(moved) + " units, " +
				std::to_string(toward) + " toward the player in 1.5s; overridden commands=" +
				std::to_string(rush_command_hits[static_cast<std::size_t>(bot.slot)].load()) + '.');
			bot.rush_reported = true;
		}

		if (game_client)
		{
			auto* flags = reinterpret_cast<int*>(game_client + kGameClientFlagsOffset);
			if (alive && bot.frozen) *flags |= 4;
			else *flags &= ~4;
		}
		bot.was_alive = alive;
	}

	void __cdecl update_on_main_thread()
	{
		update_pending = false;
		if (!in_custom_game())
		{
			clear_all_rush_commands();
			bots = {};
			selected_bot_slot = -1;
			publish_bots();
			return;
		}
		for (auto& bot : bots) update_bot(bot);
		publish_bots();
	}

	void __cdecl spawn_on_main_thread()
	{
		spawn_pending = false;
		if (!in_custom_game())
		{
			log("Spawn ignored: join and spawn into a private/custom match first.");
			MessageBeep(MB_ICONWARNING);
			return;
		}
		vec3 anchor{};
		if (!capture_crosshair(anchor))
		{
			log("Spawn failed: could not read the local camera.");
			MessageBeep(MB_ICONERROR);
			return;
		}

		using add_bot_fn = void*(*)(const char*, int);
		using spawn_test_client_fn = int(*)(void*);
		void* entity = at<add_bot_fn>(rva::SV_AddBot)("", 1);
		if (!entity)
		{
			log("SV_AddBot returned null (the lobby may be full or the bot system is unavailable).");
			MessageBeep(MB_ICONERROR);
			return;
		}

		const int slot = *static_cast<const std::int16_t*>(entity);
		if (slot < 0 || static_cast<std::size_t>(slot) >= bots.size())
		{
			log("Spawned bot slot is outside the CineBot tracking range.");
			MessageBeep(MB_ICONERROR);
			return;
		}
		const auto* local_entity = at<const std::byte*>(rva::g_entities);
		const auto* local_client = *reinterpret_cast<std::byte* const*>(local_entity + kEntityClientOffset);
		const int local_team = local_client ? *reinterpret_cast<const int*>(local_client + kGameClientTeamOffset) : 0;
		const int assigned_team = local_team == 1 ? 2 : 1;
		at<void(*)(char, int)>(rva::SV_SetAssignedTeam)(static_cast<char>(slot), assigned_team);
		const int result = at<spawn_test_client_fn>(rva::SV_SpawnTestClient)(entity);
		auto& bot = bots[static_cast<std::size_t>(slot)];
		clear_rush_command(slot);
		bot = {};
		bot.entity = entity;
		bot.slot = slot;
		bot.anchor = anchor;
		bot.active = true;
		bot.frozen = true;
		bot.assigned_team = assigned_team;
		selected_bot_slot = slot;
		publish_bots();
		std::ostringstream line;
		line << "Bot requested in slot " << slot << "; assigned team " << assigned_team
			<< " (local team " << local_team << "); SV_SpawnTestClient returned " << result
			<< "; trace fraction " << last_trace_fraction
			<< "; anchor=(" << anchor.x << ", " << anchor.y << ", " << anchor.z << ").";
		log(line.str());
		MessageBeep(MB_OK);
	}

	void __cdecl move_on_main_thread()
	{
		move_pending = false;
		auto* bot = get_selected_bot();
		if (!bot)
		{
			log("F7 ignored: there is no active CineBot to move.");
			MessageBeep(MB_ICONWARNING);
			return;
		}
		vec3 anchor{};
		if (!capture_crosshair(anchor)) return;
		bot->anchor = anchor;
		clear_rush_command(bot->slot);
		bot->frozen = true;
		bot->rushing = false;
		if (bot->was_alive) set_player_origin(bot->entity, bot->anchor);
		publish_bots();
		log("Moved bot slot " + std::to_string(bot->slot) +
			" to a new crosshair anchor and enabled freezing.");
		MessageBeep(MB_OK);
	}

	void __cdecl toggle_on_main_thread()
	{
		toggle_pending = false;
		auto* bot = get_selected_bot();
		if (!bot)
		{
			log("F8 ignored: there is no active CineBot.");
			return;
		}
		bot->frozen = !bot->frozen;
		clear_rush_command(bot->slot);
		bot->rushing = false;
		auto* entity = static_cast<std::byte*>(bot->entity);
		if (auto* game_client = *reinterpret_cast<std::byte**>(entity + kEntityClientOffset))
		{
			auto* flags = reinterpret_cast<int*>(game_client + kGameClientFlagsOffset);
			if (bot->frozen && bot->was_alive) *flags |= 4;
			else *flags &= ~4;
		}
		log("Bot slot " + std::to_string(bot->slot) +
			(bot->frozen ? " freezing enabled." : " freezing disabled."));
		publish_bots();
		MessageBeep(bot->frozen ? MB_OK : MB_ICONWARNING);
	}

	void __cdecl rush_on_main_thread()
	{
		rush_pending = false;
		if (!in_custom_game()) return;
		const auto* local = at<const std::byte*>(rva::g_entities);
		if (!*reinterpret_cast<void* const*>(local + kEntityClientOffset)) return;
		const auto target = entity_origin(local);
		int released = 0;
		for (auto& bot : bots)
		{
			if (!valid_bot_entity(bot) || !bot.was_alive) continue;
			const auto origin = entity_origin(bot.entity);
			vec3 angles{};
			if (!look_at(origin, target, angles)) continue;
			const auto run_distance = std::hypot(target.x - origin.x, target.y - origin.y);
			const rush_command command{bot.entity, origin, angles,
				(target.x - origin.x) / run_distance,
				(target.y - origin.y) / run_distance, run_distance};
			at<void(*)(void*, const float*)>(rva::G_SetPlayerAngles)(bot.entity, &angles.x);
			bot.rush_origin = origin;
			bot.rush_target = target;
			bot.rush_started = GetTickCount64();
			bot.rush_call_baseline = test_client_command_calls.load();
			bot.rush_reported = false;
			bot.rushing = true;
			bot.frozen = true; // Keep stationary until the command hook proves it is active.
			if (auto* game_client = *reinterpret_cast<std::byte**>(
				static_cast<std::byte*>(bot.entity) + kEntityClientOffset))
				*reinterpret_cast<int*>(game_client + kGameClientFlagsOffset) |= 4;
			rush_command_hits[static_cast<std::size_t>(bot.slot)] = 0;
			{
				std::lock_guard lock(rush_mutex);
				rush_commands[static_cast<std::size_t>(bot.slot)] = command;
			}
			++released;
		}
		publish_bots();
		log("F4 armed " + std::to_string(released) +
			" bot(s) toward the captured player position; release waits for command interception.");
	}

	bool queue_main_thread(void(__cdecl* callback)())
	{
		using critical_fn = void(*)(int);
		at<critical_fn>(rva::RtlEnterCriticalSection)(193);
		auto* count = at<unsigned int*>(rva::cmd_funcCount);
		auto** calls = at<void**>(rva::cmd_funcArray);
		bool queued = false;
		if (*count < 0x20)
		{
			calls[*count] = reinterpret_cast<void*>(callback);
			++*count;
			queued = true;
		}
		at<critical_fn>(rva::RtlLeaveCriticalSection)(193);
		return queued;
	}

	bool key_pressed(const int key)
	{
		return (GetAsyncKeyState(key) & 1) != 0;
	}

	bool game_is_foreground()
	{
		DWORD foreground_pid{};
		GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
		return foreground_pid == GetCurrentProcessId();
	}

	void queue_once(std::atomic_bool& pending, void(__cdecl* callback)(), const char* label)
	{
		if (pending.exchange(true)) return;
		if (!queue_main_thread(callback))
		{
			pending = false;
			log(std::string("Main-thread command queue was full while scheduling ") + label + '.');
		}
	}

}

namespace cinebot
{
	void init()
	{
		game_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		std::wstring path(32768, L'\0');
		const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0 || length >= path.size()) return;
		path.resize(length);
		module_directory = std::filesystem::path(path).parent_path() / "S2MP-Mod";
		std::error_code ec;
		std::filesystem::create_directories(module_directory, ec);
		if (!supported_image() || sha256(path) != kGameSha256)
		{
			log("Exact WWII executable check failed; CineBot is disabled for this build.");
			return;
		}
		placement_distance = static_cast<float>(GetPrivateProfileIntW(L"Placement", L"FallbackDistance", 250,
			(module_directory / "S2CineBot.ini").c_str()));
		if (placement_distance < 25.0f || placement_distance > 5000.0f) placement_distance = 250.0f;
		rush_hook_ready = executable_address(at<void*>(rva::SV_ProcessTestClientCommand)) &&
			Hook::create("CineBot_test_client_command", game_base + rva::SV_ProcessTestClientCommand,
				&test_client_command_hook, &original_test_client_command) && original_test_client_command;
		log(std::string("Directed bot movement hook: ") + (rush_hook_ready ? "ready." : "unavailable; F4 disabled."));
		g_available = true;
		activate_binding = Functions::_Key_GetBindingForCommand ?
			Functions::_Key_GetBindingForCommand("+activate") : 0;
		ads_binding = Functions::_Key_GetBindingForCommand ?
			Functions::_Key_GetBindingForCommand("+toggleads_throw") : 0;
		log("Integrated CineBot ready. ADS + bound Use=spawn, F4=straight run, F7=move, F8=toggle freeze. Use binding index " +
			std::to_string(activate_binding) + ", ADS binding index " +
			std::to_string(ads_binding) + '.');
		GameUtil::addCommand("cinebot_spawn", [] { spawn_at_crosshair(); });
		GameUtil::addCommand("cinebot_move", [] { move_selected_to_crosshair(); });
		GameUtil::addCommand("cinebot_freeze", [] { toggle_selected_freeze(); });
		GameUtil::addCommand("cinebot_rush", [] { rush_all_toward_player(); });
	}

	void tick()
	{
		if (!g_available.load()) return;
		if (game_is_foreground())
		{
			if (key_pressed(VK_F4)) rush_all_toward_player();
			if (key_pressed(VK_F7)) move_selected_to_crosshair();
			if (key_pressed(VK_F8)) toggle_selected_freeze();
		}
		if (active_count.load() == 0) return;
		static ULONGLONG last_update = 0;
		const auto now = GetTickCount64();
		if (now - last_update < 20) return;
		last_update = now;
		queue_once(update_pending, update_on_main_thread, "update");
	}

	bool available() { return g_available.load(); }
	int selected_slot() { return selected_bot_slot.load(); }

	void select(const int slot)
	{
		std::lock_guard lock(view_mutex);
		for (const auto& bot : published_bots)
			if (bot.slot == slot) { selected_bot_slot = slot; return; }
	}

	std::vector<bot_view> list()
	{
		std::lock_guard lock(view_mutex);
		return published_bots;
	}

	void spawn_at_crosshair()
	{
		if (g_available.load()) queue_once(spawn_pending, spawn_on_main_thread, "spawn");
	}
	void move_selected_to_crosshair()
	{
		if (g_available.load()) queue_once(move_pending, move_on_main_thread, "move");
	}
	void toggle_selected_freeze()
	{
		if (g_available.load()) queue_once(toggle_pending, toggle_on_main_thread, "freeze toggle");
	}

	void rush_all_toward_player()
	{
		if (g_available.load() && rush_hook_ready) queue_once(rush_pending, rush_on_main_thread, "rush all");
	}

	void on_game_key(const int key, const int down)
	{
		if (!g_available.load() || key < 0 || key >= 256) return;
		const int binding = *at<const int*>(rva::key_bindings + 16ull * key);
		if (ads_binding > 0 && binding == ads_binding) ads_held = down != 0;
		if (activate_binding <= 0) return;
		if (binding != activate_binding) return;
		if (!down) { use_held = false; return; }
		if (use_held) return;
		use_held = true;
		if ((ads_held || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) && game_is_foreground())
			spawn_at_crosshair();
	}
}
