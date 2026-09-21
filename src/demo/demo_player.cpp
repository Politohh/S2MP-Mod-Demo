#include "pch.h"
#include "demo/demo_player.hpp"

#include "Console.hpp"
#include "GameUtil.hpp"
#include "demo/demo_library.hpp"
#include "demo/demo_native.hpp"
#include "demo/demo_playback.hpp"
#include "demo/demo_recording.hpp"
#include "demo/demo_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

namespace demo_player
{
	namespace
	{
		std::vector<Entry> g_list;
		int g_selected = 0;

		// A demo asked for while another is still loaded. It is started once the
		// old one has fully closed -- see play() for why it cannot start at once.
		std::mutex g_pending_lock;
		std::string g_pending_name;
		std::uint64_t g_pending_queued = 0;
		std::uint64_t g_pending_clear_since = 0;
		constexpr std::uint64_t PENDING_SETTLE_MS = 1500;
		constexpr std::uint64_t PENDING_GIVE_UP_MS = 30000;

		Kind kind_of(const std::filesystem::path& p)
		{
			const auto ext = p.extension().string();
			if (_stricmp(ext.c_str(), ".demo") == 0)
			{
				return Kind::Engine;
			}
			// DEMO_EXTENSION is a string_view, so give _stricmp a real pointer.
			if (_stricmp(ext.c_str(), std::string(demo_utils::DEMO_EXTENSION).c_str()) == 0)
			{
				return Kind::Custom;
			}
			return Kind::None;
		}

		void scan(const std::optional<std::filesystem::path>& dir, Kind kind)
		{
			if (!dir)
			{
				return;
			}
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(*dir, ec))
			{
				if (ec)
				{
					break;
				}
				if (!e.is_regular_file(ec) || kind_of(e.path()) != kind)
				{
					continue;
				}
				Entry entry;
				entry.path = e.path();
				entry.name = e.path().filename().string();
				entry.kind = kind;
				entry.size = e.file_size(ec);
				if (ec)
				{
					ec.clear();
					entry.size = 0;
				}
				// Two seeks: header + footer. Cheap enough for every row, and
				// it is what turns "x0147_6a77b6ea" into a map and a date.
				if (kind == Kind::Engine)
				{
					entry.info = demo_library::describe(entry.path);
				}
				else
				{
					// One small read: map name from the .dm_s2 map header, so a
					// custom row shows the same map/date columns as a native one.
					entry.info = demo_library::describe_custom(entry.path);
				}
				g_list.push_back(std::move(entry));
			}
		}

		// Resolve a bare name typed at the console ("dday", "d_day.0000") to a
		// file in either library. Exact filename first, then stem, so
		// `demo_play dday` finds dday.demo without the user knowing the format.
		const Entry* find(const std::string& want)
		{
			if (g_list.empty())
			{
				refresh();
			}
			for (const auto& e : g_list)
			{
				if (_stricmp(e.name.c_str(), want.c_str()) == 0)
				{
					return &e;
				}
			}
			for (const auto& e : g_list)
			{
				if (_stricmp(e.path.stem().string().c_str(), want.c_str()) == 0)
				{
					return &e;
				}
			}
			return nullptr;
		}

		// ---- console commands ---------------------------------------

		void cmd_play()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("usage: demo_play <name>   (see demo_list)");
				return;
			}
			const std::string want = args->argv[args->nesting][1];

			if (const Entry* e = find(want))
			{
				play(e->path);
				return;
			}
			// Not in either library, so treat it as a path and let the
			// extension pick the engine.
			const std::filesystem::path p(want);
			if (kind_of(p) != Kind::None && std::filesystem::exists(p))
			{
				play(p);
				return;
			}
			Console::printf("[demo] no demo called '%s'. Try demo_list.", want.c_str());
		}

		void cmd_stop() { stop(); }
		void cmd_pause() { toggle_pause(); }

		void cmd_seek()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("usage: demo_seek <seconds>   (negative rewinds)");
				return;
			}
			const double sec = std::atof(args->argv[args->nesting][1]);
			seek_relative(static_cast<std::int32_t>(sec * 1000.0));
		}

		// demo_seek_to <ms> [play] -- an ABSOLUTE seek on the active demo's own
		// clock. Everything that seeks from the GUI thread (dolly Go, J, the
		// timeline) queues this, so the seek itself always runs here, on the
		// client thread, through one code path for both engines.
		void cmd_seek_to()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("usage: demo_seek_to <demo time ms> [play]   (now: %d)",
					current_time());
				return;
			}
			seek_absolute(std::atoi(args->argv[args->nesting][1]));
			if (args->argc[args->nesting] >= 3
				&& _stricmp(args->argv[args->nesting][2], "play") == 0 && paused())
			{
				toggle_pause();
			}
		}

		void cmd_speed()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("playback speed: %.2fx   (demo_speed <0.05 .. 10.0>)",
					timescale());
				return;
			}
			set_timescale(static_cast<float>(std::atof(args->argv[args->nesting][1])));
		}

		void cmd_list()
		{
			refresh();
			if (g_list.empty())
			{
				Console::printf("[demo] no demos found.");
				if (const auto d = demo_native::native_demos_directory())
				{
					Console::printf("[demo]   engine demos: %s", d->string().c_str());
				}
				if (const auto d = demo_utils::demos_directory())
				{
					Console::printf("[demo]   custom demos: %s", d->string().c_str());
				}
				return;
			}
			Console::printf("[demo] %zu demo(s):", g_list.size());
			for (const auto& e : g_list)
			{
				Console::printf("  %-40s %s  %.1f MB",
					e.name.c_str(),
					e.kind == Kind::Engine ? "engine" : "custom",
					static_cast<double>(e.size) / (1024.0 * 1024.0));
			}
		}

		void cmd_record()
		{
			const auto* args = GameUtil::getCmdArgs();
			bool on = !auto_record();
			if (args && args->argc[args->nesting] >= 2)
			{
				on = args->argv[args->nesting][1][0] != '0';
			}
			// ONE RECORDER AT A TIME: switching native on switches our capture off,
			// and if it is writing this match right now, saves and closes that file.
			if (on && (demo_recording::is_armed() || demo_recording::is_recording()))
			{
				demo_recording::cancel();
				Console::printf("[demo] our capture turned off -- only one recorder at a "
					"time (native starts with the next match)");
			}
			set_auto_record(on);
			Console::printf("[demo] auto-record every match: %s%s",
				on ? "ON" : "off",
				on ? "  (takes effect on the next connect)" : "");
		}

	}

	// =================================================================

	void refresh()
	{
		g_list.clear();
		scan(demo_native::native_demos_directory(), Kind::Engine);
		scan(demo_utils::demos_directory(), Kind::Custom);

		// Newest first: the demo you just recorded is the one you want.
		std::error_code ec;
		std::stable_sort(g_list.begin(), g_list.end(),
			[&](const Entry& a, const Entry& b)
			{
				const auto ta = std::filesystem::last_write_time(a.path, ec);
				const auto tb = std::filesystem::last_write_time(b.path, ec);
				return ta > tb;
			});

		if (g_selected >= static_cast<int>(g_list.size()))
		{
			g_selected = g_list.empty() ? 0 : static_cast<int>(g_list.size()) - 1;
		}
	}

	const std::vector<Entry>& list() { return g_list; }
	int selected() { return g_selected; }

	void set_selected(int index)
	{
		if (index >= 0 && index < static_cast<int>(g_list.size()))
		{
			g_selected = index;
		}
	}

	const Entry* selected_entry()
	{
		if (g_selected < 0 || g_selected >= static_cast<int>(g_list.size()))
		{
			return nullptr;
		}
		return &g_list[static_cast<std::size_t>(g_selected)];
	}

	Kind active()
	{
		if (demo_native::native_playing())
		{
			return Kind::Engine;
		}
		if (demo_playback::is_playing())
		{
			return Kind::Custom;
		}
		return Kind::None;
	}

	bool playing() { return active() != Kind::None; }

	const char* blocked_reason()
	{
		// The engine wraps CL_Demo_Play_f in !com_sv_running, so with a server
		// up it is a SILENT no-op. Say so rather than letting the button look
		// dead.
		const Entry* e = selected_entry();
		if (e && e->kind == Kind::Engine && demo_native::server_running())
		{
			return "Leave the match first - engine demos will not start while a "
				"server is running.";
		}
		return nullptr;
	}

	bool play(const std::filesystem::path& path)
	{
		const Kind kind = kind_of(path);

		// The same custom demo again: restart it in place (a seek to its start)
		// rather than reloading the map underneath itself.
		if (kind == Kind::Custom && demo_playback::is_playing())
		{
			std::error_code ec;
			if (std::filesystem::equivalent(path, demo_playback::current_path(), ec))
			{
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_seek_to 0 play");
				Console::printf("[demo] restarting %s from the beginning",
					path.filename().string().c_str());
				return true;
			}
		}

		// ⛔ NEVER start one demo while another is still loaded. MEASURED
		// 2026-09-15 (minidump s2_mp64_ship.exe.CL0.1789476438.dmp): Play on a
		// custom demo while one was loaded ran stop() then StartServer again on
		// the still-running loopback server, and the second SV_SpawnServer
		// faulted in SV_ChangeMaxClients (IDA 0x6DA8C5). The 12:51 dump died the
		// same way on the image-asset limit. So close the old demo, go back to
		// the menu, and start the new one once that has happened -- the same
		// order native playback already requires.
		if (playing())
		{
			stop();
			{
				std::lock_guard<std::mutex> lock(g_pending_lock);
				g_pending_name = path.filename().string();
				g_pending_queued = GetTickCount64();
				g_pending_clear_since = 0;
			}
			Console::printf("[demo] closing the current demo first -- %s starts as soon "
				"as it has.", path.filename().string().c_str());
			return true;
		}

		switch (kind)
		{
		case Kind::Engine:
			if (demo_native::server_running())
			{
				Console::printf("[demo] cannot play an engine demo while a server is "
					"running - disconnect to the menu first.");
				return false;
			}
			return demo_native::play(path);

		case Kind::Custom:
			return demo_playback::play(path);

		default:
			Console::printf("[demo] '%s' is not a demo (.demo or %s).",
				path.filename().string().c_str(),
				std::string(demo_utils::DEMO_EXTENSION).c_str());
			return false;
		}
	}

	bool play_selected()
	{
		if (const Entry* e = selected_entry())
		{
			return play(e->path);
		}
		return false;
	}

	void stop()
	{
		const bool custom = demo_playback::is_playing();
		const bool native = demo_native::native_playing();
		if (custom)
		{
			demo_playback::stop();
		}
		// The engine has no "stop demo" command; disconnecting is how playback
		// ends. Doing it unconditionally would kick the user out of a live match,
		// so only when a demo is actually loaded -- and for BOTH kinds now. A
		// stopped custom demo used to leave its map and loopback server up, and
		// the next Play started a second server on top of it (the crash above).
		if (custom || native)
		{
			GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "disconnect");
		}
	}

	void poll_pending()
	{
		std::string name;
		{
			std::lock_guard<std::mutex> lock(g_pending_lock);
			if (g_pending_name.empty())
			{
				return;
			}
			const auto now = GetTickCount64();
			if (now - g_pending_queued > PENDING_GIVE_UP_MS)
			{
				Console::printf("[demo] gave up waiting for the previous demo to close; "
					"press Play again for %s", g_pending_name.c_str());
				g_pending_name.clear();
				return;
			}
			if (playing())
			{
				g_pending_clear_since = 0;
				return;
			}
			// Gone. Let the frontend finish coming back before loading again.
			if (g_pending_clear_since == 0)
			{
				g_pending_clear_since = now;
				return;
			}
			if (now - g_pending_clear_since < PENDING_SETTLE_MS)
			{
				return;
			}
			name = std::move(g_pending_name);
			g_pending_name.clear();
		}
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("demo_play \"{}\"", name));
	}

	bool paused()
	{
		switch (active())
		{
		case Kind::Engine: return demo_native::engine_paused();
		case Kind::Custom: return demo_playback::paused();
		default:           return false;
		}
	}

	void toggle_pause()
	{
		switch (active())
		{
		case Kind::Engine: demo_native::toggle_pause(); break;
		case Kind::Custom: demo_playback::toggle_pause(); break;
		default: break;
		}
	}

	float progress()
	{
		switch (active())
		{
		case Kind::Engine:
			return demo_native::playback_progress();

		case Kind::Custom:
		{
			const auto now = demo_playback::current_time();
			const auto bounds = demo_playback::time_bounds();
			if (!now || !bounds || bounds->second <= bounds->first)
			{
				return -1.0f;
			}
			const float f = static_cast<float>(*now - bounds->first)
				/ static_cast<float>(bounds->second - bounds->first);
			return std::clamp(f, 0.0f, 1.0f);
		}

		default:
			return -1.0f;
		}
	}

	float timescale()
	{
		switch (active())
		{
		case Kind::Engine: return demo_native::engine_timescale();
		case Kind::Custom: return demo_playback::timescale();
		default:           return 1.0f;
		}
	}

	void set_timescale(float value)
	{
		// 0.05, USER-MEASURED 2026-09-21: it plays cleanly at 1/20 speed and goes
		// visibly laggy below that. Was 0.1, briefly 0.01, now pinned where the
		// footage actually holds up.
		const float v = std::clamp(value, 0.05f, 10.0f);
		switch (active())
		{
		case Kind::Engine: demo_native::set_timescale(v); break;
		case Kind::Custom: demo_playback::set_timescale(v); break;
		default:
			Console::printf("[demo] nothing is playing.");
			break;
		}
	}

	std::int32_t current_time()
	{
		switch (active())
		{
		case Kind::Engine:
		{
			// The smooth clock (cl.serverTime), same kind of clock the custom
			// theater reports. The snapshot clock would lag by up to 50 ms.
			const int t = demo_native::demo_time_smooth();
			return (t >= 0) ? t : demo_native::demo_time();
		}
		case Kind::Custom:
			// Reports the pending target while a seek is outstanding.
			return demo_playback::current_time().value_or(-1);
		default:
			return -1;
		}
	}

	// ONE seek contract for both engines: land on the absolute demo time and
	// keep the current pause state. Native lands in the same frame; the custom
	// theater lands over the next few frames and reports the target meanwhile.
	void seek_absolute(std::int32_t ms)
	{
		ms = (std::max)(0, ms);
		switch (active())
		{
		case Kind::Engine: demo_native::seek_absolute_now(ms); break;
		case Kind::Custom: demo_playback::seek_to(ms); break;
		default:
			Console::printf("[demo] nothing is playing.");
			break;
		}
	}

	void seek_relative(std::int32_t ms)
	{
		if (ms == 0)
		{
			return;
		}
		// From the current time -- or, mid-seek, from where that seek is going,
		// so pressing rewind twice goes back twice as far instead of repeating.
		const int now = current_time();
		if (now < 0)
		{
			Console::printf("[demo] nothing is playing.");
			return;
		}
		seek_absolute(now + ms);
	}

	const demo_library::Info& selected_details()
	{
		static demo_library::Info cached;
		static std::filesystem::path cached_for;

		const Entry* e = selected_entry();
		if (!e)
		{
			cached = demo_library::Info{};
			cached_for.clear();
			return cached;
		}
		if (e->path != cached_for)
		{
			cached_for = e->path;
			// describe_full walks the packet stream for the duration, so it
			// runs once per selection change, never per frame and never for
			// the whole list.
			cached = (e->kind == Kind::Engine)
				? demo_library::describe_full(e->path)
				: e->info;
		}
		return cached;
	}

	int tidy_names(std::string& summary)
	{
		int renamed = 0, skipped = 0;
		// Copy the paths first: renaming invalidates the list we are walking.
		std::vector<std::filesystem::path> targets;
		for (const auto& e : g_list)
		{
			if (e.kind == Kind::Engine)
			{
				targets.push_back(e.path);
			}
		}
		for (const auto& p : targets)
		{
			std::string why;
			if (demo_library::auto_rename(p, why))
			{
				++renamed;
			}
			else
			{
				++skipped;
			}
		}
		refresh();
		summary = std::format("renamed {}, left alone {}", renamed, skipped);
		return renamed;
	}

	bool auto_record() { return demo_native::auto_record(); }
	void set_auto_record(bool on) { demo_native::set_auto_record(on); }
	bool capturing() { return demo_recording::is_recording(); }

	void init()
	{
		// The whole product-level command set. Everything else is developer
		// mode - see DevMode.hpp.
		GameUtil::addCommand("demo_play", cmd_play);
		GameUtil::addCommand("demo_stop", cmd_stop);
		GameUtil::addCommand("demo_pause", cmd_pause);
		GameUtil::addCommand("demo_seek", cmd_seek);
		GameUtil::addCommand("demo_seek_to", cmd_seek_to);
		GameUtil::addCommand("demo_speed", cmd_speed);
		GameUtil::addCommand("demo_list", cmd_list);
		GameUtil::addCommand("demo_record", cmd_record);

		refresh();
	}
}
