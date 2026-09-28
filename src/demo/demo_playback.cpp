#include "pch.h"
#include "demo_playback.hpp"
#include "demo/demo_native.hpp"

#include "demo/demo_game.hpp"
#include "demo/demo_timescale.hpp"
#include "demo/demo_utils.hpp"
#include "demo/theater_camera.hpp"

#include "hud/broadcaster.hpp"
#include "hud/wii_aim.hpp"

#include "Console.hpp"
#include "DevMode.hpp"
#include "DvarInterface.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"
#include "Hook.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <intrin.h>
#include <vector>

namespace demo_playback
{
	namespace
	{
		// Diagnostic chatter gate — see demo_playback.hpp. Default OFF: the probes
		// below fire per frame and flooded the console badly enough to hide real
		// output. Nothing is removed, only silenced. `demo_verbose 1` restores it.
		// Declared here because the earliest gated site is ~line 527.
		bool g_verbose = false;

		struct playback_t
		{
			std::ifstream file;
			std::filesystem::path path;
			demo_utils::loaded_demo demo{};
			std::vector<std::uint8_t> chunk;
			std::optional<std::pair<std::int32_t, std::int32_t>> bounds;

			bool armed{};
			bool paused{};
			bool seeking{};
			bool finished{}; // EOF reached — keep suppressing loopback, freeze on last snap
			int seek_ttl{};
			// Monotonic theater clock, in demo serverTime ms.  Advanced once per client
			// frame as clock_ms += wall_delta * scale.  It is NEVER recomputed by
			// re-scaling the whole elapsed interval: a retroactive rescale teleports the
			// clock the instant demotimescale changes (60 s in, 1.0 -> 0.5 jumps half a
			// minute backwards) and slams the snapshot feed gate shut.
			double clock_ms{};
			int last_wall{-1};
			// advance_clock() discard accounting — see the [rate] probe.
			int adv_ok{}, adv_skip_nosnap{}, adv_skip_paused{}, adv_skip_seeking{};
			int adv_skip_dt{}, adv_clamped{}, adv_clamped_ms{};
			// Absolute demo serverTime we are burst-feeding towards (<0 = not seeking).
			// H1 model: forward = open the feed gate and burst; backward = restart the file
			// from the map header and burst forward to the target.
			int seek_target{-1};
			// Deferred rewind FF: set on backward seek, applied once after PRIMED→ACTIVE
			// (FirstSnapshot has re-owned serverTimeDelta). -1 = none.
			int rewind_ff_target{-1};
			// ⭐ THE SNAPSHOT IS STALE ACROSS A REWIND RESTART, AND THAT USED TO END
			// THE SEEK ON ITS FIRST FRAME.
			//
			// A rewind rewinds the FILE, but cl.snap still holds the pre-rewind
			// snapshot until replayed data arrives. service_seek's completion test is
			// `snap_server_time() >= target`, and for a BACKWARD seek that is true
			// immediately — the stale snapshot is by definition ahead of the target.
			// Measured: target=263491, snap=263950, fed=1, "seek done" on frame one,
			// nothing replayed, and the deferred FF discarded unused.
			//
			// So a rewind must additionally wait for FRESH data. rewind_from is the
			// snapshot time at the moment of the restart; any snapshot BELOW it can
			// only have come from the replayed stream, which is the arrival signal.
			int rewind_from{-1};
			bool rewind_fresh{};
			int first_snap{-1};
			// The first snapshot time the FILE ever delivered -- i.e. where the
			// recording really starts. Unlike first_snap it SURVIVES a rewind restart
			// (restart clears first_snap and re-seeds it), so it is what a rewind
			// target is clamped to. Measured 2026-09-15: with no footer there were no
			// bounds, rewinds asked for 258950 / 254003 / 249065 when the file starts
			// at 263950, and a seek for data that does not exist never completes.
			int earliest_snap{-1};
			// The pause finish_playback() puts on at end-of-file, as opposed to
			// one the user asked for. A rewind from the end lifts only this one --
			// the same as native, where rewinding a completed demo resumes it.
			bool paused_by_finish{};
			std::size_t network_cursor{};
			int pre_gs_burst{};
			bool gamestate_seen{};
			bool gamestate_applied{};
			bool native_gamestate_seen{};
			bool cleared_not_active{};
			int active_frames{};
			int last_logged_connstate{-1};
			bool logged_weapon{};
			bool logged_draw_probe{};
			bool select_weapon_done{};
			int draw_frames{};
			// Count EVERY DrawActiveFrame entry (not only theater) so we can tell
			// "SCR never calls us" from "called but not ACTIVE yet".
			std::atomic<int> draw_enter_any{0};
			std::atomic<int> draw_leave_any{0};
			std::atomic<int> draw_enter{0};
			std::atomic<int> draw_leave{0};
			std::atomic<std::int64_t> draw_last_ret{0};
			std::atomic<int> createcmd_enter{0};
			std::atomic<int> createcmd_leave{0};
			bool snap_clock_reset{};
			std::uint32_t fed_total{};

			[[nodiscard]] bool open() const { return file.is_open(); }

			// Pure read — advance_clock() is the only writer.
			int demo_clock_ms() const
			{
				if (last_wall < 0 || first_snap < 0)
				{
					return first_snap > 0 ? first_snap : demo_game::server_time();
				}
				return static_cast<int>(clock_ms);
			}

			// TWO callers as of 2026-08-17: CL_ClientFrame (20 Hz, the authority for the
			// feed gate and seek) and CG_DrawActiveFrame (render rate, which refines the
			// serverTime delta so non-1x playback is smooth). That is safe and is in
			// fact more accurate: this integrates dt from wall time and updates
			// last_wall BEFORE any early return, so calling it more often just gives
			// finer dt and can never double-count or dump a held interval.
			// demo_clock_ms() stays a pure read.
			void advance_clock()
			{
				const int wall = demo_game::now_ms();
				const int dt_raw = (last_wall < 0) ? 0 : (wall - last_wall);
				last_wall = wall;
				// PROBE: which of the four discard paths is eating wall time. A frame
				// skipped here still advanced last_wall, so its time is lost permanently —
				// that is how the clock can read < 1.00x while the feed looks healthy.
				if (first_snap < 0)
				{
					++adv_skip_nosnap;
					return;
				}
				if (paused) { ++adv_skip_paused; return; }
				if (seeking) { ++adv_skip_seeking; return; }
				if (dt_raw <= 0) { ++adv_skip_dt; return; }
				// A hitch (map load, alt-tab, debugger break) must not push a large
				// scaled block of demo time in one frame — that bursts the feed.
				const int dt = dt_raw > 250 ? 250 : dt_raw;
				if (dt_raw > 250)
				{
					adv_clamped_ms += dt_raw - 250;
					++adv_clamped;
				}
				++adv_ok;
				clock_ms += static_cast<double>(dt) * static_cast<double>(demo_timescale::factor());
			}

			void reset_clock(const int target_ms)
			{
				clock_ms = static_cast<double>(target_ms);
				last_wall = demo_game::now_ms();
			}

			void close()
			{
				// Unconditional: if a rewind was in flight when playback stopped, the
				// hold must not outlive it or the in-game menu is suppressed for the
				// rest of the session.
				demo_native::hold_connection_state_active(false);
				// The engine may have torn down underneath us (that is often WHY we are
				// closing), so force the cached clientActive pointer to be re-probed
				// before anything dereferences it again.
				demo_game::invalidate_client_active();
				file.close();
				path.clear();
				demo = {};
				chunk.clear();
				bounds.reset();
				armed = paused = seeking = finished = false;
				seek_ttl = first_snap = 0;
				earliest_snap = -1;
				paused_by_finish = false;
				clock_ms = 0.0;
				last_wall = -1;
				seek_target = rewind_ff_target = rewind_from = -1;
				rewind_fresh = false;
				network_cursor = pre_gs_burst = fed_total = 0;
				active_frames = 0;
				gamestate_seen = gamestate_applied = native_gamestate_seen = cleared_not_active = false;
				logged_weapon = select_weapon_done = logged_draw_probe = false;
				draw_frames = 0;
				draw_enter_any.store(0, std::memory_order_relaxed);
				draw_leave_any.store(0, std::memory_order_relaxed);
				draw_enter.store(0, std::memory_order_relaxed);
				draw_leave.store(0, std::memory_order_relaxed);
				draw_last_ret.store(0, std::memory_order_relaxed);
			createcmd_enter.store(0, std::memory_order_relaxed);
			createcmd_leave.store(0, std::memory_order_relaxed);
			snap_clock_reset = false;
				last_logged_connstate = -1;
			}
		};

		playback_t g_play;
		bool g_feed_call = false;
		// Last demo viewangles stamped by CreateCmd — used when lookup misses (pause/gap).
		float g_last_demo_angles[3]{};
		bool g_have_last_demo_angles{};

		bool read_id(std::ifstream& in, demo_utils::demo_data_id& id, bool& one_byte)
		{
			std::uint8_t id_byte = 0;
			in.read(reinterpret_cast<char*>(&id_byte), 1);
			if (!in)
			{
				return false;
			}
			one_byte = (id_byte & static_cast<std::uint8_t>(demo_utils::demo_data_id::one_byte_flag)) != 0;
			id = static_cast<demo_utils::demo_data_id>(id_byte & ~static_cast<std::uint8_t>(demo_utils::demo_data_id::flags));
			return true;
		}

		bool read_payload(std::ifstream& in, std::vector<std::uint8_t>& buf, const bool one_byte)
		{
			buf.clear();
			std::uint32_t size = 0;
			in.read(reinterpret_cast<char*>(&size), one_byte ? 1 : 4);
			if (!in)
			{
				return false;
			}
			if (one_byte)
			{
				size = static_cast<std::uint8_t>(size);
			}
			if (size > demo_utils::MAX_SIZE)
			{
				return false;
			}
			buf.resize(size);
			if (size)
			{
				in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
			}
			return static_cast<bool>(in);
		}

		bool start_file(const std::filesystem::path& path)
		{
			g_play.close();
			g_have_last_demo_angles = false;
			g_last_demo_angles[0] = g_last_demo_angles[1] = g_last_demo_angles[2] = 0.f;
			g_play.file.open(path, std::ios::binary);
			if (!g_play.file)
			{
				return false;
			}
			g_play.path = path;
			g_play.demo = {};
			g_play.bounds = std::nullopt;
			g_play.reset_clock(0);
			g_play.first_snap = -1;
			g_play.earliest_snap = -1;
			return true;
		}

		void bootstrap_map()
		{
			if (g_play.demo.map.empty())
			{
				Console::printf("[demo] bootstrap skipped (map empty)");
				return;
			}

			if (!g_play.demo.gametype.empty())
			{
				const auto eng = DvarInterface::toEngineString("g_gametype");
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
					std::format("{} {}\n", eng, g_play.demo.gametype));
			}
			// Match H1-Mod's party::start_map bootstrap: seed the UI map name and
			// enter the engine's StartServer lifecycle instead of jumping directly
			// to the low-level SV_StartMap routine. StartServer performs the normal
			// session/virtual-lobby setup that precedes map streaming.
			GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
				std::format("ui_mapname {}\n", g_play.demo.map));

			demo_game::sv_migrate() = 0;
			Console::printf("[demo] bootstrap StartServer('%s') gt=%s",
				g_play.demo.map.c_str(),
				g_play.demo.gametype.empty() ? "?" : g_play.demo.gametype.c_str());

			if (Functions::_UI_RunMenuScript)
			{
				const char* start_server = "StartServer";
				Functions::_UI_RunMenuScript(LOCAL_CLIENT_0, &start_server);
				return;
			}

			// Older/partial address tables may not expose the UI dispatcher. Keep
			// the previous entry point as a compatibility fallback only.
			if (Functions::_SV_StartMap)
			{
				Console::printf("[demo] StartServer unavailable; falling back to SV_StartMap");
				Functions::_SV_StartMap(LOCAL_CLIENT_0, g_play.demo.map.c_str(), false);
			}
			else
			{
				Console::printf("[demo] bootstrap skipped (no map-load entry point)");
			}
		}

		void arm_replay()
		{
			g_play.armed = true;
			g_play.finished = false;
			demo_utils::reset_demo_read_gate();

			Console::printf("[demo] armed at PRIMED — feeding from offset %zu (gs_in_file=%s)",
				g_play.demo.post_map_header_offset,
				g_play.demo.gamestate ? "yes" : "no");

			// NEVER touch clientActive / Arxan getters here. Live dump proved even an
			// E8 thunk into CL_GetLocalClientActive spins forever. Only reset the global
			// feed clock; snap fields wait until GetPredicted hands us a cached pointer.
			const int st_before = demo_game::server_time();
			demo_game::set_theater_server_time(0);
			g_play.snap_clock_reset = false;
			if (demo_game::client_active_for())
			{
				demo_game::reset_snap_clock();
				g_play.snap_clock_reset = true;
				Console::printf("[demo] gate reset (cached ca) serverTime %d -> 0 snap=%d",
					st_before, demo_game::snap_server_time());
			}
			else
			{
				Console::printf("[demo] gate: serverTime %d -> 0 (snap reset deferred, no ca cache)",
					st_before);
			}

			// Do NOT seed first_snap from the footer — that permanently opens the gate
			// against snap=0 and dumps the whole file in one frame.
			if (g_play.demo.post_map_header_offset)
			{
				g_play.file.clear();
				g_play.file.seekg(static_cast<std::streamoff>(g_play.demo.post_map_header_offset));
			}

			g_play.first_snap = -1;
			g_play.reset_clock(0);
			g_play.fed_total = 0;
			g_play.pre_gs_burst = 0;
			g_play.gamestate_seen = false;
			g_play.gamestate_applied = false;
			g_play.native_gamestate_seen = false;
			g_play.cleared_not_active = false;
			g_play.active_frames = 0;

			if (g_play.demo.gamestate)
			{
				// Only seed cmd baseline at PRIMED. Configstring memcpy stays on the feed
				// path (never during SV_StartMap bootstrap — that hung previously).
				demo_utils::note_command_baseline(static_cast<int>(g_play.demo.gamestate->svr_cmd_seq));
				if (auto* clc = demo_game::clc_for(0))
				{
					demo_game::write_i(clc, demo_game::CLC_SERVER_COMMAND_SEQUENCE,
						static_cast<int>(g_play.demo.gamestate->svr_cmd_seq));
					demo_game::write_i(clc, demo_game::CLC_LAST_EXECUTED_SERVER_COMMAND,
						static_cast<int>(g_play.demo.gamestate->svr_cmd_seq));
				}
				Console::printf("[demo] seeded cmd baseline=%u (GS strings deferred to feed)",
					g_play.demo.gamestate->svr_cmd_seq);
			}
			Console::printf("[demo] arm_replay done");
		}

		void maybe_deferred_snap_reset()
		{
			if (!g_play.armed || g_play.snap_clock_reset || !demo_game::client_active_for())
			{
				return;
			}
			demo_game::reset_snap_clock();
			g_play.snap_clock_reset = true;
			Console::printf("[demo] deferred snap clock reset (ca=%p snap=%d serverTime=%d)",
				demo_game::client_active_for(),
				demo_game::snap_server_time(),
				demo_game::server_time());
		}

		void finish_playback()
		{
			if (g_play.finished)
			{
				return;
			}
			g_play.finished = true;
			g_play.paused_by_finish = !g_play.paused;
			g_play.paused = true;
			// Pin clock to the last applied snap and KEEP armed/open so loopback stays muted.
			const int snap_t = demo_game::snap_server_time();
			if (snap_t > 0)
			{
				demo_game::set_theater_server_time(snap_t);
				if (g_play.first_snap < 0)
				{
					g_play.first_snap = snap_t;
				}
			}
			Console::printf(
				"[demo] end of file after %u chunks (connstate=%d snap=%d) — frozen; demo_stop to leave",
				g_play.fed_total, demo_game::connstate(), snap_t);
		}

		bool parse_during_playback()
		{
			if (!g_play.open() || !g_play.armed)
			{
				return false;
			}
			if (g_play.paused && !g_play.seeking)
			{
				return true;
			}
			if (g_play.finished)
			{
				return true;
			}

			if (!demo_utils::continue_demo_reading())
			{
				if (g_play.fed_total == 0)
				{
					static int gate_log = 0;
					if ((gate_log++ % 120) == 0)
					{
						Console::printf("[demo] feed gate closed (snap=%d serverTime=%d) — waiting",
							demo_game::snap_server_time(), demo_game::server_time());
					}
				}
				return true;
			}

			int fed = 0;
			int tlv_steps = 0;
			// Keep the first post-arm tick tiny so a hanging Parse/GS can't look like a
			// soft-lock with no breadcrumbs. Ramp up once we've promoted to ACTIVE.
			int feed_budget = (demo_game::connstate() >= demo_game::CA_ACTIVE) ? 16 : 1;
			if (g_play.seek_target >= 0)
			{
				// Scrubbing: the clock is already parked on the target, so burn through
				// buffered chunks instead of trickling per frame for several seconds.
				//
				// ⭐ The sub-ACTIVE case matters as much as the ACTIVE one, and used to
				// be missed. A REWIND restarts the stream and drops connstate to PRIMED,
				// so until the engine re-promotes through CL_FirstSnapshot this ran at
				// the 1-per-frame arming budget — 20 messages a second, to replay a
				// gamestate and reach the first snapshot. 32 keeps a breadcrumb trail
				// (the reason the arming budget is tiny) while not making a rewind wait
				// seconds to re-prime.
				feed_budget = (demo_game::connstate() >= demo_game::CA_ACTIVE) ? 128 : 32;
			}
			const int tlv_budget = feed_budget * 4;
			while (demo_utils::continue_demo_reading() && fed < feed_budget && tlv_steps < tlv_budget)
			{
				++tlv_steps;
				demo_utils::demo_data_id id{};
				bool one_byte = false;
				if (!read_id(g_play.file, id, one_byte))
				{
					Console::printf("[demo] feed: read_id failed (fed=%u eof=%d)",
						g_play.fed_total, g_play.file.eof() ? 1 : 0);
					break;
				}
				if (id == demo_utils::demo_data_id::eof)
				{
					finish_playback();
					return false;
				}
				if (!read_payload(g_play.file, g_play.chunk, one_byte))
				{
					Console::printf("[demo] feed: read_payload failed id=%u",
						static_cast<unsigned>(id));
					break;
				}

				if (g_play.fed_total == 0 && fed == 0)
				{
					Console::printf("[demo] feed first TLV id=%u size=%zu",
						static_cast<unsigned>(id), g_play.chunk.size());
				}

				switch (id)
				{
				case demo_utils::demo_data_id::map_header:
					break;
				case demo_utils::demo_data_id::update_gamestate_data:
					// Already hydrated by load_demo_file — don't zlib-decompress again mid-frame.
					if (!g_play.demo.gamestate)
					{
						Console::printf("[demo] feed: decompressing update_gamestate_data (%zu)",
							g_play.chunk.size());
						demo_utils::process_gamestate_data(g_play.chunk, g_play.demo.gamestate);
					}
					break;
				case demo_utils::demo_data_id::gamestate_message:
				{
					// A native gamestate is a complete CL_ParseServerMessage packet.  Feed it
					// through the engine parser so CL_ParseGamestate performs its own client
					// state/configstring/entity initialization.  Do not call the post-parse
					// helper directly: it assumes the parser's transient state is live.
					g_play.native_gamestate_seen = true;
					Console::printf("[demo] feed: native gamestate message size=%zu connstate=%d...",
						g_play.chunk.size(), demo_game::connstate());
					const int saved_state = demo_game::connstate();
					if (saved_state >= demo_game::CA_PRIMED)
					{
						// CL_ParseServerMessage rejects a gamestate once the client is PRIMED.
						// Native playback receives it while CONNECTED, before promotion.
						demo_game::connstate() = demo_game::CA_CONNECTED;
					}
					g_feed_call = true;
					demo_utils::process_network_data(g_play.chunk);
					g_feed_call = false;
					g_play.gamestate_applied = true;
					Console::printf("[demo] feed: native gamestate returned connstate=%d snap=%d",
						demo_game::connstate(), demo_game::snap_server_time());
					if (demo_game::connstate() == demo_game::CA_CONNECTED)
					{
						// Preserve a prior valid state only if the parser did not promote at all.
						demo_game::connstate() = saved_state;
					}
					break;
				}
				case demo_utils::demo_data_id::network_data:
				{
					if (g_play.demo.gamestate && !g_play.gamestate_applied
						&& !g_play.native_gamestate_seen)
					{
						Console::printf("[demo] feed: applying recorded GS before first net chunk...");
						if (demo_utils::apply_recorded_gamestate(*g_play.demo.gamestate))
						{
							g_play.gamestate_applied = true;
						}
						else
						{
							Console::printf("[demo] warning: GS apply failed — feeding without restore");
							g_play.gamestate_applied = true; // don't retry every chunk
						}
					}

					const bool verbose_feed = demo_game::connstate() < demo_game::CA_ACTIVE
						|| g_play.fed_total < 4;
					if (verbose_feed)
					{
						Console::printf("[demo] feed: ParseServerMessage size=%zu...", g_play.chunk.size());
					}
					g_feed_call = true;
					demo_utils::process_network_data(g_play.chunk);
					g_feed_call = false;
					if (verbose_feed)
					{
						Console::printf("[demo] feed: ParseServerMessage returned (connstate=%d snap=%d)",
							demo_game::connstate(), demo_game::snap_server_time());
					}
					++g_play.fed_total;
					++fed;

					if (demo_game::connstate() == demo_game::CA_PRIMED
						&& demo_game::clear_snap_not_active() && !g_play.cleared_not_active)
					{
						g_play.cleared_not_active = true;
						Console::printf("[demo] cleared SNAPFLAG_NOT_ACTIVE so CL_FirstSnapshot can run");
					}

					if (g_verbose && (g_play.fed_total == 1 || (g_play.fed_total % 64) == 0))
					{
						Console::printf("[demo] fed=%u connstate=%d snap=%d serverTime=%d",
							g_play.fed_total, demo_game::connstate(),
							demo_game::snap_server_time(), demo_game::server_time());
					}

					if (g_play.first_snap < 0)
					{
						const int snap_t = demo_game::snap_server_time();
						if (snap_t > 0)
						{
							g_play.first_snap = snap_t;
							if (g_play.earliest_snap < 0 || snap_t < g_play.earliest_snap)
							{
								g_play.earliest_snap = snap_t;
							}
							g_play.reset_clock(snap_t);
							Console::printf("[demo] first_snap seeded from snap.serverTime=%d", snap_t);
						}
					}
					if (!g_play.gamestate_seen)
					{
						++g_play.pre_gs_burst;
						if (g_play.pre_gs_burst == 1)
						{
							std::uint32_t flags = 0;
							std::memcpy(&flags, g_play.chunk.data() + 4, 4);
							Console::printf("[demo] first network chunk size=%zu zlib=%u readcount=%u",
								g_play.chunk.size(), flags >> 31, flags & 0x7FFFFFFFu);
							if (g_play.chunk.size() < 256 && !g_play.gamestate_applied)
							{
								Console::printf(
									"[demo] warning: tiny first network chunk and no gamestate blob — "
									"re-record with this DLL (session buffer from match connect).");
							}
						}
						if (g_play.pre_gs_burst > 1)
						{
							g_play.gamestate_seen = true;
						}
					}
					break;
				}
				case demo_utils::demo_data_id::predicted_data:
					demo_utils::process_predicted_data(g_play.chunk, g_play.demo);
					break;
				case demo_utils::demo_data_id::gen_footer:
					if (g_play.chunk.size() >= 8)
					{
						std::int32_t first = 0;
						std::int32_t last = 0;
						std::memcpy(&first, g_play.chunk.data(), 4);
						std::memcpy(&last, g_play.chunk.data() + 4, 4);
						g_play.bounds = std::make_pair(first, last);
					}
					break;
				default:
					break;
				}
			}
			return true;
		}

		// Prefer newest sample at/before t within 500ms (original working window).
		// Allow nearest only within 1000ms — unbounded nearest caused angle regression
		// after clock-sync when demo_clock ran ahead of the predicted ring.
		const demo_utils::predicted_sample_t* pick_predicted(const int server_time)
		{
			const auto& ring = g_play.demo.predicted;
			const std::size_t count = ring.size();
			if (!count)
			{
				return nullptr;
			}

			const demo_utils::predicted_sample_t* best_before = nullptr;
			const demo_utils::predicted_sample_t* nearest = nullptr;
			int best_before_dt = INT_MAX;
			int nearest_dt = INT_MAX;

			for (std::size_t i = 0; i < count; ++i)
			{
				const auto& p = ring[(g_play.demo.predicted_index + count - 1 - i) % count];
				const int dt = server_time - p.cad.serverTime;
				const int adt = dt >= 0 ? dt : -dt;
				if (adt < nearest_dt)
				{
					nearest_dt = adt;
					nearest = &p;
				}
				if (dt >= 0 && dt < best_before_dt && dt < 500)
				{
					best_before_dt = dt;
					best_before = &p;
				}
			}
			if (best_before)
			{
				return best_before;
			}
			return (nearest && nearest_dt < 1000) ? nearest : nullptr;
		}

		// Bracketing samples around t, for interpolation.  Recorded samples arrive at the
		// snapshot rate (~50 ms / 20 Hz) but we render at 60-144 fps, so consuming a single
		// sample verbatim makes view angles and motion a visible 20 Hz step function — a
		// new angle only every third frame at 60 fps.  The old jittering clock dithered
		// which sample got picked and smeared that into general jitter; once the clock is
		// smooth the stair-step is obvious, which is why this surfaced with the timescale fix.
		// Returns false when nothing usable is in range.  Never extrapolates past the newest
		// sample — beyond it we hold, exactly as before.
		bool pick_predicted_span(const int server_time,
			const demo_utils::predicted_sample_t** out_before,
			const demo_utils::predicted_sample_t** out_after,
			float* out_frac)
		{
			*out_before = nullptr;
			*out_after = nullptr;
			*out_frac = 0.0f;

			const auto& ring = g_play.demo.predicted;
			const std::size_t count = ring.size();
			if (!count)
			{
				return false;
			}

			const demo_utils::predicted_sample_t* before = nullptr;
			const demo_utils::predicted_sample_t* after = nullptr;
			const demo_utils::predicted_sample_t* nearest = nullptr;
			int before_dt = INT_MAX;
			int after_dt = INT_MAX;
			int nearest_dt = INT_MAX;
			int newest = 0;
			int valid = 0;

			for (std::size_t i = 0; i < count; ++i)
			{
				const auto& p = ring[(g_play.demo.predicted_index + count - 1 - i) % count];
				// Unwritten ring slots carry serverTime 0; they are not samples.
				if (p.cad.serverTime <= 0)
				{
					continue;
				}
				++valid;
				if (p.cad.serverTime > newest)
				{
					newest = p.cad.serverTime;
				}
				const int dt = server_time - p.cad.serverTime;
				const int adt = dt >= 0 ? dt : -dt;
				if (adt < nearest_dt)
				{
					nearest_dt = adt;
					nearest = &p;
				}
				if (dt >= 0)
				{
					if (dt < before_dt && dt < 500)
					{
						before_dt = dt;
						before = &p;
					}
				}
				else if (adt < after_dt && adt < 500)
				{
					after_dt = adt;
					after = &p;
				}
			}

			// DIAGNOSTIC. The question this answers: does a FORWARD sample ever exist to
			// interpolate toward? The feed gate only delivers demo data up to the current
			// time, so if `after` is always NONE the interpolation is a no-op and angles
			// still step at the sample rate — which would explain "unchanged".
			// Rate-limited; runs for every caller of this function.
			{
				static int diag = 0;
				if (g_verbose && g_play.armed && (diag < 16 || (diag % 512) == 0))
				{
					Console::printf(
						"[ang] t=%d before=%d dt=%d after=%d adt=%d newest=%d lead=%d valid=%d",
						server_time,
						before ? before->cad.serverTime : -1,
						before ? before_dt : -1,
						after ? after->cad.serverTime : -1,
						after ? after_dt : -1,
						newest, newest - server_time, valid);
				}
				++diag;
			}

			if (before)
			{
				*out_before = before;
				const int span = before_dt + after_dt;
				if (after && span > 0)
				{
					*out_after = after;
					*out_frac = static_cast<float>(before_dt) / static_cast<float>(span);
				}
				return true;
			}
			// Same fallback window the discrete picker used — keep the previously
			// validated behaviour when we have nothing to interpolate between.
			if (nearest && nearest_dt < 1000)
			{
				*out_before = nearest;
				return true;
			}
			return false;
		}

		// Shortest-arc interpolation. Plain lerp breaks crossing +/-180 yaw, which is
		// exactly where a fast flick lands.
		float lerp_angle(const float a, const float b, const float f)
		{
			float d = b - a;
			while (d > 180.0f)
			{
				d -= 360.0f;
			}
			while (d < -180.0f)
			{
				d += 360.0f;
			}
			return a + d * f;
		}

		bool lookup_viewangles(const int server_time, float out[3])
		{
			const demo_utils::predicted_sample_t* before = nullptr;
			const demo_utils::predicted_sample_t* after = nullptr;
			float frac = 0.0f;
			if (!pick_predicted_span(server_time, &before, &after, &frac))
			{
				return false;
			}
			for (int i = 0; i < 3; ++i)
			{
				out[i] = after
					? lerp_angle(before->viewangles[i], after->viewangles[i], frac)
					: before->viewangles[i];
			}
			return true;
		}

		// Mirror CL_SavePredicted into the CA archive ring. WritePacket (which normally
		// calls SavePredicted) is suppressed in theater, so the PS-delta path's
		// CL_GetDemoViewAnglesFromArchive would otherwise hit empty
		// slots (serverTime==0) and overwrite snapshot angles with zeros.
		void seed_predicted_archive(void* clientActive, const int server_time)
		{
			const auto* p = pick_predicted(server_time);
			if (!p || !clientActive)
			{
				return;
			}
			const int idx = demo_game::archive_index(clientActive) & 255;
			auto* slot = demo_game::archive_slot(clientActive, idx);
			if (!slot)
			{
				return;
			}
			const int stamp = p->cad.serverTime ? p->cad.serverTime : server_time;
			if (slot->serverTime == stamp)
			{
				return;
			}
			slot->serverTime = stamp;
			std::memcpy(&slot->origin[0], &p->cad.origin[0], sizeof(slot->origin));
			std::memcpy(&slot->velocity[0], &p->cad.velocity[0], sizeof(slot->velocity));
			slot->bobA = p->cad.bobA;
			slot->bobB = p->cad.bobB;
			// Prefer dedicated sample angles (always written); fall back to cad copy.
			slot->viewangles[0] = p->viewangles[0];
			slot->viewangles[1] = p->viewangles[1];
			slot->viewangles[2] = p->viewangles[2];
			// CL_GetDemoViewAnglesFromArchive also restores these from the same slot.
			slot->extra0 = p->cad.extra0;
			slot->w0 = p->cad.w0;
			slot->w1 = p->cad.w1;
			slot->w2 = p->cad.w2;
			demo_game::write_i(clientActive, demo_game::CA_ARCHIVE_INDEX, (idx + 1) & 255);
		}

		void apply_viewangles_to_ps(void* player_state, const float angles[3])
		{
			// S2 CG_CalcViewValues copies ps+576 → refdef. Only durable inject point
			// (same as CL_GetDemoViewAnglesFromArchive). Do NOT write CA_CMD / cmd shorts —
			// those are mouse+kick space, not world viewangles.
			if (!player_state)
			{
				return;
			}
			demo_game::write_f(player_state, demo_game::PS_VIEWANGLES, angles[0]);
			demo_game::write_f(player_state, demo_game::PS_VIEWANGLES + 4, angles[1]);
			demo_game::write_f(player_state, demo_game::PS_VIEWANGLES + 8, angles[2]);
		}

		// Overlay motion/angles onto a playerState that already has weapons/health
		// from the snapshot. Mirrors CL_GetPredicted (+ viewangles / extra0 that
		// CL_GetDemoViewAnglesFromArchive would write when IsDemoPlaying).
		bool inject_predicted(void* /*clientActive*/, const int ps_time, void* player_state)
		{
			const demo_utils::predicted_sample_t* p = nullptr;
			const demo_utils::predicted_sample_t* next = nullptr;
			float frac = 0.0f;
			if (!pick_predicted_span(ps_time, &p, &next, &frac) || !player_state)
			{
				return false;
			}

			auto& ps = *static_cast<demo_game::demo_playerState_t*>(player_state);
			// Origin/velocity step at the same ~20 Hz as the angles; interpolate them
			// together or the camera slides smoothly while the body snaps.
			if (next)
			{
				for (int i = 0; i < 3; ++i)
				{
					ps.origin[i] = p->cad.origin[i]
						+ (next->cad.origin[i] - p->cad.origin[i]) * frac;
					ps.velocity[i] = p->cad.velocity[i]
						+ (next->cad.velocity[i] - p->cad.velocity[i]) * frac;
				}
			}
			else
			{
				std::memcpy(&ps.origin[0], &p->cad.origin[0], sizeof(ps.origin));
				std::memcpy(&ps.velocity[0], &p->cad.velocity[0], sizeof(ps.velocity));
			}
			// Discrete state — these are selections, not continuous quantities, so they
			// take the sample we are on rather than a blend.
			ps.bobByte = static_cast<std::uint8_t>(p->cad.bobA);
			ps.movementDir = p->cad.bobB;
			float angles[3]{};
			for (int i = 0; i < 3; ++i)
			{
				angles[i] = next
					? lerp_angle(p->viewangles[i], next->viewangles[i], frac)
					: p->viewangles[i];
			}
			apply_viewangles_to_ps(player_state, angles);
			// GetDemoViewAnglesFromArchive → a3[204] = archive.extra0 (ps+816).
			demo_game::write_i(player_state, demo_game::PS_ARCHIVE_EXTRA0, p->cad.extra0);
			g_last_demo_angles[0] = angles[0];
			g_last_demo_angles[1] = angles[1];
			g_last_demo_angles[2] = angles[2];
			g_have_last_demo_angles = true;
			return true;
		}

		// After Predict each frame: hold ps+576 to demo archive angles (GetPredicted alone
		// only runs on snapshot transitions).
		// Default OFF — matches H1, which has NO per-frame angle override.
		//
		// PROVEN divergence (2026-08-07): S2's CL_GetPredicted (0x462650) is called only by
		// CG_InterpolatePlayerState_S2 (0x668A90), which is called by CL_ParseSnapshot
		// (0x464C00). That is the structural mirror of MWR, where
		// CL_GetPredictedPlayerInformationForServerTime (0x39AEE0) has exactly one caller,
		// MSG_ReadDeltaPlayerstate (0x7255D0). So inject_predicted already writes recorded
		// angles into the SNAPSHOT playerState at parse time, exactly as H1 does, and the
		// engine then interpolates snap->cg every frame in CG_CalcEntityLerpOrigins (0x69185).
		//
		// This function ran afterwards at 0x697E0 — after that interpolation, before
		// CG_CalcViewValues reads ps+240h at 0x69B03 — replacing the smooth interpolated
		// angle with a single discrete sample. H1 has no equivalent write. That is what makes
		// playback step. Toggle with `demo_angle_override` to A/B it in game.
		bool g_angle_per_frame_override = false;

		void sync_ps_viewangles_after_predict(void* cg)
		{
			if (!g_angle_per_frame_override)
			{
				return;
			}
			if (!cg || !g_play.open() || !g_play.armed)
			{
				return;
			}
			int t = demo_game::server_time();
			const int cg_t = demo_game::read_i(cg, demo_game::CG_TIME);
			if (cg_t > 0)
			{
				t = cg_t;
			}
			float angles[3]{};
			if (lookup_viewangles(t, angles))
			{
				apply_viewangles_to_ps(cg, angles);
				// DIAGNOSTIC. Answers "is the renderer consuming our value?" — refdef is
				// computed by CG_CalcViewValues AFTER this point, so what we read here is
				// LAST frame's camera. If it never tracks the angles we write, our write is
				// not reaching the camera and the producer was never the problem.
				{
					static int diag = 0;
					if (diag < 16 || (diag % 512) == 0)
					{
						const auto* refdef = reinterpret_cast<const float*>(
							static_cast<char*>(cg) + demo_game::CG_REFDEF_VIEWANGLES);
						Console::printf(
							"[ang] t=%d wrote pitch=%.2f yaw=%.2f | refdef(prev) pitch=%.2f yaw=%.2f",
							t, angles[0], angles[1], refdef[0], refdef[1]);
					}
					++diag;
				}
				g_last_demo_angles[0] = angles[0];
				g_last_demo_angles[1] = angles[1];
				g_last_demo_angles[2] = angles[2];
				g_have_last_demo_angles = true;
			}
			else if (g_have_last_demo_angles)
			{
				apply_viewangles_to_ps(cg, g_last_demo_angles);
			}
		}

		// Single theater clock: force CA_SERVER_TIME_DELTA so SetCGameTime advances at
		// demo_clock speed (S2 has no Com_TimeScaleMsec). Keeps feed gate == engine time.
		// One-shot WeaponDef census. Tests one hypothesis that covers BOTH viewmodel
		// symptoms at once: if most weapon indices resolve to a null WeaponDef, then
		// remote players holding those render no third-person gun (only the few whose
		// index happens to land on a populated entry do), and a local held weapon that
		// resolves to nothing trips CG_BuildViewmodelDObj gate 1 — "never shows".
		// Reads only the pointer table (2047 entries, confirmed indexable by sub_3B6E40);
		// entries are never dereferenced, so a stale/garbage WeaponDef* cannot fault us.
		void log_weapon_table_census()
		{
			auto** table = demo_game::weapon_def_table();
			if (!table)
			{
				Console::printf("[vm] WeaponDef table pointer is NULL");
				return;
			}
			int populated = 0;
			std::string sample;
			for (int i = 1; i < demo_game::WEAPON_NONE; ++i)
			{
				if (!table[i])
				{
					continue;
				}
				++populated;
				if (populated <= 12)
				{
					sample += std::to_string(i);
					sample += ' ';
				}
			}
			unsigned int held = 0;
			unsigned int held_alt = 0;
			const char* held_state = "no cg";
			if (void* cg = demo_game::cg_globals_for(0))
			{
				auto* base = static_cast<char*>(cg);
				held = *reinterpret_cast<const std::uint16_t*>(base + demo_game::PS_WEAPON_HELD);
				held_alt = *reinterpret_cast<const std::uint16_t*>(
					base + demo_game::PS_WEAPON_HELD_ALT);
				if (held == 0)
				{
					held_state = "ZERO -> gate 1 blocks the viewmodel";
				}
				else if (held >= demo_game::WEAPON_NONE)
				{
					held_state = "out of range";
				}
				else
				{
					held_state = table[held] ? "resolves OK" : "NULL WeaponDef";
				}
			}
			Console::printf("[vm] WeaponDef census: %d/%d populated; first: %s",
				populated, demo_game::WEAPON_NONE - 1,
				sample.empty() ? "(none)" : sample.c_str());
			Console::printf("[vm] local ps.held=%u heldAlt=%u -> %s",
				held, held_alt, held_state);
		}

		// MEASURED 2026-08-07: CL_ClientFrame runs at 20/s (the snapshot cadence; the engine
		// itself does cl_serverTime += 50 there) while the game draws at ~230/s. Pinning
		// clientActive.serverTime to a discrete value therefore freezes cg.time between
		// client frames and quantizes every interpolation to 20 Hz — high FPS, 20 Hz motion.
		//
		// Letting CL_SetCGameTime derive serverTime = serverTimeDelta + cls_realtime instead
		// costs nothing at 1x: the demo clock advances at wall rate (measured ratio 1.00), so
		// the delta is CONSTANT frame to frame and serverTime rises smoothly with no sawtooth.
		// At non-1x the delta changes each client frame, which would saw by 50ms*(1-scale),
		// so pinning is retained there.
		bool g_smooth_clock = true;

		// [lerp] PROBE ONLY. Frames remaining in the burst; 0 = silent. Armed by the
		// `demo_lerp` command. See the sampler in cg_draw_active_frame_stub.

		// The 1x restriction here is CORRECT and is deliberately retained.
		//
		//   at 1x   the theater target advances at wall rate, so serverTimeDelta is
		//           CONSTANT. Writing only the delta lets the engine derive
		//           serverTime = delta + cls_realtime every render frame -- genuinely
		//           smooth, and measured as such (d=+5..+16 on every rendered frame).
		//   off 1x  the target advances at scale x while cls_realtime advances at 1x,
		//           so a delta-only scheme drifts at the wrong rate between writes.
		//           CL_SetCGameTime also RATCHETS (serverTime is clamped up to
		//           oldFrameServerTime and can never fall back), so an overshoot below
		//           1x cannot be corrected afterwards. The non-1x path therefore
		//           re-stamps serverTime directly, after the engine's recompute.
		//
		// ⚠ 2026-08-17: I briefly made this return g_smooth_clock unconditionally,
		// thinking the pin was the source of the timescale stepping. It is not -- the
		// pin already ran at RENDER rate (CL_SetCGameTime does), so the staircase came
		// from the VALUE being pinned, demo_clock_ms(), which only changed 20x/s
		// because advance_clock() was called solely from CL_ClientFrame. Fixed there
		// instead. Removing this restriction would break slow motion against the
		// ratchet, so do not "simplify" it again.
		bool smooth_clock_active()
		{
			if (!g_smooth_clock)
			{
				return false;
			}
			const float scale = demo_timescale::factor();
			return scale >= 0.999f && scale <= 1.001f;
		}

		// pin_client_time=false writes only the delta and lets the engine derive serverTime.
		// Seek/pause/rewind still pin, because there we want hard control of the clock.
		void apply_theater_clock(const int target_ms, const bool pin_client_time = true)
		{
			if (void* cl = demo_game::client_active_for(0))
			{
				const int realtime = demo_game::cls_realtime();
				demo_game::write_i(cl, demo_game::CA_SERVER_TIME_DELTA, target_ms - realtime);
				if (pin_client_time)
				{
					demo_game::write_i(cl, demo_game::CA_SERVER_TIME, target_ms);
				}
			}
			// The global cl_serverTime stays pinned regardless: it drives the feed gate
			// (snap.serverTime <= cl_serverTime) and the render-thread target, both of
			// which want the demo clock, not a realtime-derived value.
			demo_game::set_theater_server_time(target_ms);
		}

		void rebase_clock_to(const int target_ms)
		{
			g_play.reset_clock(target_ms);
		}

		// Bounded rewind trace. A rewind is a LIFECYCLE (restart -> re-prime ->
		// re-feed -> land), and "it shows the old frame" can come from any stage,
		// so one line per client frame for a bounded window shows which stage
		// stalls. Capped, so it can never become a per-frame flood (RULE A18).
		int g_rewind_trace = 0;

		void trace_rewind(const char* where)
		{
			if (g_rewind_trace <= 0)
			{
				return;
			}
			--g_rewind_trace;
			int st = -1, old_ff = -1, delta = -1, valid = -1;
			if (void* cl = demo_game::client_active_for(0))
			{
				st = demo_game::read_i(cl, demo_game::CA_SERVER_TIME);
				old_ff = demo_game::read_i(cl, demo_game::CA_OLD_FRAME_SERVER_TIME);
				delta = demo_game::read_i(cl, demo_game::CA_SERVER_TIME_DELTA);
				valid = demo_game::read_i(cl, demo_game::CA_SNAP_VALID);
			}
			Console::printf(
				"[rw] %-10s cs=%d snap=%d valid=%d | serverTime=%d oldFrame=%d delta=%d "
				"| global=%d | target=%d ffTarget=%d fed=%u",
				where, demo_game::connstate(), demo_game::snap_server_time(), valid,
				st, old_ff, delta, demo_game::server_time(),
				g_play.seek_target, g_play.rewind_ff_target, g_play.fed_total);
		}

		bool restart_stream_for_rewind()
		{
			if (!g_play.open() || !g_play.armed)
			{
				return false;
			}

			// CL_SetCGameTime re-promotes through CL_FirstSnapshot once fresh snapshots
			// land; leaving it ACTIVE makes the replayed stream look like time going
			// backwards (engine error 440) and the snapshot is dropped.
			//
			// ⭐ But dropping out of CA_ACTIVE is ALSO what opened the in-game menu
			// mid-rewind. sub_357AD0 publishes LUI model 100
			// ("cg.Shared.connectionStateActive") OUTSIDE its connstate gate, and LUI
			// drives the menu from it. Writing connstate here bypasses
			// CL_SetClientState, so no engine notification fires -- but that publisher
			// POLLS connstate every frame and tells LUI anyway.
			//
			// Hold the publish for the duration. Cleared on the ACTIVE transition in
			// cl_client_frame_stub and unconditionally in close(), so an aborted or
			// failed rewind cannot leave the menu suppressed forever.
			if (demo_game::connstate() == demo_game::CA_ACTIVE)
			{
				demo_native::hold_connection_state_active(true);
				demo_game::connstate() = demo_game::CA_PRIMED;
			}

			const int cmd_base = g_play.demo.gamestate
				? static_cast<int>(g_play.demo.gamestate->svr_cmd_seq)
				: 0;
			if (auto* clc = demo_game::clc_for(0))
			{
				// Stale sequences make CG_ExecuteNewServerCommands walk a ring that the
				// replay is about to rewrite from the start.
				demo_game::write_i(clc, demo_game::CLC_SERVER_COMMAND_SEQUENCE, cmd_base);
				demo_game::write_i(clc, demo_game::CLC_LAST_EXECUTED_SERVER_COMMAND, cmd_base);

				// ⭐ And align the CLIENT reliable pair before a single replayed message
				// lands. sub_606B0 drops the connection on
				//     reliableSequence - reliableAcknowledge > 128
				// and a rewind is precisely when that gap appears: reliableSequence has
				// been climbing all session (the engine keeps enqueueing even though the
				// theater suppresses CL_WritePacket) while the recorded acknowledge is
				// about to jump back to the demo's opening value. That is the
				// EXE_DISCONNECTED that killed the last rewind attempt.
				const int ack = demo_game::read_i(clc, demo_game::CLC_RELIABLE_ACKNOWLEDGE);
				demo_game::write_i(clc, demo_game::CLC_RELIABLE_SEQUENCE, ack);
			}
			demo_utils::note_command_baseline(cmd_base);
			demo_utils::reset_demo_read_gate();
			const int restart_time = g_play.bounds
				? g_play.bounds->first
				: (g_play.demo.time_bounds ? g_play.demo.time_bounds->first : 0);
			demo_game::reset_snap_clock_for_rewind(restart_time);

			g_play.file.clear();
			g_play.file.seekg(static_cast<std::streamoff>(g_play.demo.post_map_header_offset));

			g_play.finished = false;
			g_play.first_snap = -1;
			g_play.reset_clock(restart_time);
			g_play.fed_total = 0;
			g_play.pre_gs_burst = 0;
			g_play.gamestate_seen = false;
			g_play.gamestate_applied = false;
			g_play.native_gamestate_seen = false;
			g_play.cleared_not_active = false;
			g_play.active_frames = 0;
			g_play.snap_clock_reset = true;
			g_play.demo.predicted_index = 0;
			g_play.demo.predicted.fill({});
			g_have_last_demo_angles = false;
			return true;
		}

		bool seek_to_ms(int target_ms)
		{
			if (!g_play.open() || !g_play.armed)
			{
				return false;
			}

			if (const auto b = g_play.bounds ? g_play.bounds : g_play.demo.time_bounds)
			{
				target_ms = std::clamp(target_ms, b->first, b->second);
			}
			// A recording has no data before its first snapshot. Ask for earlier and
			// the rewind restarts, replays from the start, and then waits forever for
			// a snapshot at the target that is never coming (the clock stays parked on
			// it). Land on the start instead -- which is also what a user pressing
			// rewind near the beginning means.
			if (g_play.earliest_snap > 0 && target_ms < g_play.earliest_snap)
			{
				target_ms = g_play.earliest_snap;
			}

			const int snap_now = demo_game::snap_server_time();
			const bool backward = snap_now > 0 && target_ms < snap_now;
			if (backward)
			{
				if (!restart_stream_for_rewind())
				{
					return false;
				}
				if (g_play.paused_by_finish)
				{
					g_play.paused = false;
					g_play.paused_by_finish = false;
				}
				// H1: do NOT fast-forward before FirstSnapshot. Defer delta bump until
				// PRIMED→ACTIVE so FirstSnapshot can re-own serverTimeDelta first.
				g_play.rewind_ff_target = target_ms;
				g_play.seek_target = target_ms;
				g_play.seeking = true;
				g_play.seek_ttl = 0;
				// Remember where the STALE snapshot sat, so service_seek can tell
				// replayed data apart from it.
				g_play.rewind_from = (snap_now > 0) ? snap_now : -1;
				g_play.rewind_fresh = false;
				g_rewind_trace = 40;      // bounded window, ~2s of client frames
				trace_rewind("restart");
				Console::printf("[demo] seek -> %d ms (restart; FF deferred until ACTIVE)",
					target_ms);
				return true;
			}

			const int gap = (snap_now > 0) ? (target_ms - snap_now) : 0;

			// Backstop. Without a footer (a recording that was never finalised has
			// none) there are no bounds to clamp to, so a units bug upstream went
			// straight through: 2026-09-15 a dolly Go asked for 287,634,276 ms from
			// 288,350, the fast-forward bumped serverTimeDelta by ~80 hours, and the
			// FX update spun forever on the resulting time step. No real seek is
			// over an hour in one frame; refuse it and say why.
			constexpr int MAX_FORWARD_SEEK_MS = 60 * 60 * 1000;
			if (gap > MAX_FORWARD_SEEK_MS)
			{
				Console::printf("[demo] seek -> %d ms refused: %d ms ahead of %d is over an "
					"hour -- almost certainly a units bug in the caller", target_ms, gap,
					snap_now);
				return false;
			}

			g_play.seek_target = target_ms;
			g_play.seeking = true;
			g_play.seek_ttl = 8;
			g_play.rewind_ff_target = -1;
			// A forward seek must not inherit an unfinished rewind's freshness wait.
			// Measured 2026-09-15: a stalled rewind left rewind_from=263950, the next
			// forward seek to 268950 reached snap 269100 -- which is not below
			// 263950, so it never counted as fresh -- and the clock stayed parked on
			// 268950 forever. That was the "press forward and it doesn't always work".
			g_play.rewind_from = -1;
			g_play.rewind_fresh = false;

			if (gap > 0)
			{
				demo_utils::fast_forward_demo(static_cast<std::uint32_t>(gap));
			}
			apply_theater_clock(target_ms);
			Console::printf("[demo] seek -> %d ms (forward from %d)", target_ms, snap_now);
			return true;
		}


		// Runs every client frame while a seek is outstanding.
		void service_seek()
		{
			if (g_play.seek_target < 0)
			{
				return;
			}
			trace_rewind("service");
			// Still waiting for FirstSnapshot after rewind restart.
			if (g_play.rewind_ff_target >= 0
				&& demo_game::connstate() < demo_game::CA_ACTIVE)
			{
				return;
			}
			const int target = g_play.seek_target;

			// Apply the deferred rewind fast-forward HERE, the first frame we are
			// ACTIVE, rather than from a PRIMED→ACTIVE edge in the frame stub.
			// Measured: that edge is missed — the trace showed the seek reaching
			// `service` already at cs=10 with ffTarget still pending, so the FF was
			// discarded unused. Being ACTIVE is the condition that actually matters
			// (it means FirstSnapshot has re-owned serverTimeDelta); the transition
			// was only ever a proxy for it, and a lossy one.
			if (g_play.rewind_ff_target >= 0
				&& demo_game::connstate() >= demo_game::CA_ACTIVE)
			{
				g_play.rewind_ff_target = -1;
				trace_rewind("ff-here");
			}

			if (g_play.seek_ttl > 0)
			{
				--g_play.seek_ttl;
				apply_theater_clock(target);
				return;
			}
			const int snap_now = demo_game::snap_server_time();

			// Has replayed data arrived yet? Only a snapshot BELOW where the stale
			// one sat can have come from the restarted stream.
			//
			// ⭐ Second signal, and the one that matters at the START of a demo:
			// restart_stream_for_rewind -> reset_snap_clock_for_rewind forces
			// CA_SNAP_VALID to 0 (the [rw] restart line reads `valid=0`), and only
			// CL_ParseSnapshot on a REPLAYED message sets it back to 1. The time test
			// alone fails when you rewind from the start: the replay's first snapshot
			// EQUALS rewind_from (263950 == 263950), is never "below" it, and the seek
			// waited forever. That was the "press rewind twice" glitch.
			int snap_valid = 0;
			if (void* cl = demo_game::client_active_for(0))
			{
				snap_valid = demo_game::read_i(cl, demo_game::CA_SNAP_VALID);
			}
			if (g_play.rewind_from >= 0 && !g_play.rewind_fresh && snap_now > 0
				&& (snap_now < g_play.rewind_from || snap_valid != 0))
			{
				g_play.rewind_fresh = true;
				trace_rewind("fresh");
			}

			// For a rewind, "caught up" needs BOTH: fresh data, and that data having
			// replayed forward to the target. Testing only `snap >= target` completed
			// the seek on frame one against the pre-rewind snapshot.
			const bool awaiting_replay = (g_play.rewind_from >= 0) && !g_play.rewind_fresh;
			const bool caught_up = !awaiting_replay && snap_now >= target;
			if (!caught_up && !g_play.finished)
			{
				// Keep the clock parked on the target every frame. The engine's own
				// snapshot-driven correction pulls the delta back toward whatever
				// snapshot it currently holds, so this has to be re-asserted rather
				// than written once.
				apply_theater_clock(target);
				return;
			}

			g_play.seek_target = -1;
			g_play.seeking = false;
			g_play.seek_ttl = 0;
			g_play.rewind_ff_target = -1;
			g_play.rewind_from = -1;
			g_play.rewind_fresh = false;
			if (g_play.first_snap < 0 && snap_now > 0)
			{
				g_play.first_snap = snap_now;
			}
			rebase_clock_to(g_play.finished ? snap_now : target);
			Console::printf("[demo] seek done (target=%d snap=%d fed=%u)",
				target, snap_now, g_play.fed_total);
			trace_rewind("done");
			g_rewind_trace = 0;
		}

		void (*CL_ClientFrame_orig)() = nullptr;
		thread_local bool g_in_set_cgame_time = false;
		thread_local bool g_in_write_packet = false;
		void cl_client_frame_stub()
		{
			const int cs_before = demo_game::connstate();
			CL_ClientFrame_orig();

			if (!g_play.open())
			{
				// Live loading diagnostics, including the PRIMED -> ACTIVE boundary.
				// Read state only; do not promote a client or bypass streaming gates.
				if (!demo_native::native_playing())
				{
					static int last_state = -1;
					static DWORD last_report = 0;
					const int state = demo_game::connstate();
					const DWORD now = GetTickCount();
					if (state != last_state || (state == demo_game::CA_PRIMED && now - last_report >= 5000))
					{
						demo_game::invalidate_client_active();
						const void* cl = state >= demo_game::CA_PRIMED ? demo_game::client_active_for() : nullptr;
						Console::printf("[match] live client %d -> %d: valid=%d flags=0x%X new=%d snapTime=%d",
							last_state, state,
							cl ? demo_game::read_i(cl, demo_game::CA_SNAP_VALID) : -1,
							cl ? demo_game::read_i(cl, demo_game::CA_SNAP_FLAGS) : 0,
							cl ? demo_game::read_i(cl, demo_game::CA_NEW_SNAPSHOTS) : -1,
							cl ? demo_game::read_i(cl, demo_game::CA_SNAP_SERVERTIME) : -1);
						last_state = state;
						last_report = now;
					}
				}
				return;
			}

			const int cs = demo_game::connstate();
			if (cs != g_play.last_logged_connstate)
			{
				Console::printf("[demo] connstate %d -> %d", g_play.last_logged_connstate, cs);
				g_play.last_logged_connstate = cs;
			}

			if (!g_play.armed)
			{
				if (cs >= demo_game::CA_PRIMED)
				{
					arm_replay();
				}
				return;
			}

			maybe_deferred_snap_reset();

			if (cs == demo_game::CA_DISCONNECTED)
			{
				g_play.close();
				return;
			}

			// Re-validate the cached clientActive pointer this frame. One VirtualQuery
			// per frame; in steady state client_active_for() is then a compare. This is
			// what stops a freed pointer being dereferenced after an engine teardown --
			// see the crash note on client_active_for().
			demo_game::invalidate_client_active();

			// The single per-frame clock tick.  Must run ahead of every early return
			// below so last_wall keeps tracking while paused / seeking / finished.
			g_play.advance_clock();

			// PROBE ONLY. Separates two different causes of "playback feels slow even at
			// high FPS", which need opposite fixes:
			//   clientFrames/s << draws/s  -> the theater clock (and therefore cg.time and
			//     all interpolation) only updates once per CL_ClientFrame, so the demo's
			//     temporal resolution is capped below the render rate.
			//   ratio < 1.00               -> the demo clock is losing time against the
			//     wall clock, i.e. playback is literally running in slow motion.
			if (g_play.armed && !g_play.paused)
			{
				static int win_start = 0;
				static int client_frames = 0;
				static int draws_at_start = 0;
				static int clock_at_start = 0;
				const int now = demo_game::now_ms();
				const int draws_now = g_play.draw_enter_any.load(std::memory_order_relaxed);
				const int clock_now = g_play.demo_clock_ms();
				if (win_start == 0)
				{
					win_start = now;
					draws_at_start = draws_now;
					clock_at_start = clock_now;
				}
				++client_frames;
				const int win = now - win_start;
				if (win >= 1000)
				{
					const int clock_adv = clock_now - clock_at_start;
					if (g_verbose) Console::printf(
						"[rate] clientFrames=%.0f/s draws=%.0f/s demoClock=%dms per %dms wall (ratio %.2f) "
						"| adv ok=%d noSnap=%d paused=%d seeking=%d dt0=%d clamp=%d(-%dms) scale=%.2f",
						client_frames * 1000.0 / win,
						(draws_now - draws_at_start) * 1000.0 / win,
						clock_adv, win,
						static_cast<double>(clock_adv) / static_cast<double>(win),
						g_play.adv_ok, g_play.adv_skip_nosnap, g_play.adv_skip_paused,
						g_play.adv_skip_seeking, g_play.adv_skip_dt,
						g_play.adv_clamped, g_play.adv_clamped_ms,
						demo_timescale::factor());
					g_play.adv_ok = g_play.adv_skip_nosnap = g_play.adv_skip_paused = 0;
					g_play.adv_skip_seeking = g_play.adv_skip_dt = 0;
					g_play.adv_clamped = g_play.adv_clamped_ms = 0;
					win_start = now;
					client_frames = 0;
					draws_at_start = draws_now;
					clock_at_start = clock_now;
				}
			}

			// Back at CA_ACTIVE, so LUI may hear about the connection state again. This
			// is deliberately OUTSIDE the rewind_ff_target branch below: the hold must
			// be released on ANY return to ACTIVE, including a rewind that took a
			// different path or failed, or the in-game menu stays suppressed for the
			// rest of the session.
			if (cs >= demo_game::CA_ACTIVE && demo_native::connection_state_held())
			{
				demo_native::hold_connection_state_active(false);
			}

			// H1 rewind: apply deferred FF only after FirstSnapshot (PRIMED→ACTIVE).
			if (g_play.rewind_ff_target >= 0
				&& cs_before < demo_game::CA_ACTIVE
				&& cs >= demo_game::CA_ACTIVE)
			{
				const int target = g_play.rewind_ff_target;
				const int snap_now = demo_game::snap_server_time();
				const int gap = target - (snap_now > 0 ? snap_now : 0);
				g_play.rewind_ff_target = -1;
				if (gap > 0)
				{
					demo_utils::fast_forward_demo(static_cast<std::uint32_t>(gap));
					Console::printf("[demo] rewind FF +%d ms after FirstSnapshot (snap=%d)",
						gap, snap_now);
					trace_rewind("ff-applied");
				}
				g_play.seek_ttl = 8;
				apply_theater_clock(target);
			}

			if (g_play.finished && g_play.seek_target < 0)
			{
				const int snap_t = demo_game::snap_server_time();
				if (snap_t > 0)
				{
					apply_theater_clock(snap_t);
				}
				return;
			}

			// advance_clock() holds the clock while paused, so there is no paused-wall
			// bookkeeping left to do here — just keep the engine parked on the snapshot.
			if (g_play.paused && !g_play.seeking)
			{
				const int snap_t = demo_game::snap_server_time();
				if (snap_t > 0)
				{
					if (void* cl = demo_game::client_active_for(0))
					{
						const int st = demo_game::read_i(cl, demo_game::CA_SERVER_TIME);
						const int delta = demo_game::read_i(cl, demo_game::CA_SERVER_TIME_DELTA);
						if (st > snap_t)
						{
							demo_game::write_i(cl, demo_game::CA_SERVER_TIME_DELTA,
								delta - (st - snap_t));
						}
					}
					apply_theater_clock(snap_t);
				}
				return;
			}

			if (cs >= demo_game::CA_ACTIVE && g_play.active_frames < 3)
			{
				++g_play.active_frames;
				if (g_verbose) Console::printf("[demo] ACTIVE feed tick %d ok (fed=%u snap=%d serverTime=%d)",
					g_play.active_frames, g_play.fed_total,
					demo_game::snap_server_time(), demo_game::server_time());
				// Post-FirstSnapshot Soft gate snapshot (theater) — compare to live SoftState.
				if (g_play.active_frames == 3)
				{
					// Frame 3, not 1 — give the snapshot PS and the weapon assets a
					// couple of ticks to settle before censusing them.
					log_weapon_table_census();
				}
			}
			if (cs >= demo_game::CA_ACTIVE
				&& !g_play.logged_draw_probe
				&& g_play.draw_leave.load(std::memory_order_relaxed) > 0)
			{
				g_play.logged_draw_probe = true;
				Console::printf(
					"[demo] draw_probe any=%d/%d theater=%d/%d ret=%lld create=%d/%d",
					g_play.draw_enter_any.load(std::memory_order_relaxed),
					g_play.draw_leave_any.load(std::memory_order_relaxed),
					g_play.draw_enter.load(std::memory_order_relaxed),
					g_play.draw_leave.load(std::memory_order_relaxed),
					static_cast<long long>(g_play.draw_last_ret.load(std::memory_order_relaxed)),
					g_play.createcmd_enter.load(std::memory_order_relaxed),
					g_play.createcmd_leave.load(std::memory_order_relaxed));
			}

			service_seek();

			if (!g_play.finished && g_play.seek_target < 0)
			{
				apply_theater_clock(g_play.demo_clock_ms(), !smooth_clock_active());
			}
		}

		// Match the known-good reference behavior: once replay is armed, suppress the
		// entire live WritePacket call. The demo archive is seeded by GetPredicted.
		void (*CL_WritePacket_orig)(int) = nullptr;
		void cl_write_packet_stub(const int local_client_num)
		{
			if (g_play.open() && g_play.armed)
			{
				return;
			}
			CL_WritePacket_orig(local_client_num);
		}

		// Clamp target seq so seek/fast-forward never runs past what the theater feed has buffered.
		void (*CG_ExecuteNewServerCommands_orig)(unsigned int, int) = nullptr;
		void cg_execute_new_server_commands_stub(const unsigned int local_client_num, int latest_sequence)
		{
			if (g_play.open())
			{
				if (auto* clc = demo_game::clc_for(static_cast<int>(local_client_num)))
				{
					const int received = demo_game::read_i(clc, demo_game::CLC_RELIABLE_ACKNOWLEDGE);
					if (latest_sequence > received)
					{
						latest_sequence = received;
					}
				}
			}
			CG_ExecuteNewServerCommands_orig(local_client_num, latest_sequence);
		}

		// CL_GetPredictedVehicleForServerTime — archive is dead during replay.
		bool (*CL_GetPredictedWeapon_orig)(void*, int, void*) = nullptr;
		bool cl_get_predicted_weapon_stub(void* clientActive, const int server_time, void* weapon_block)
		{
			demo_game::note_client_active(clientActive);
			maybe_deferred_snap_reset();
			if (g_play.open() && g_play.armed)
			{
				return false;
			}
			return CL_GetPredictedWeapon_orig(clientActive, server_time, weapon_block);
		}

		bool (*CL_GetPredicted_orig)(void*, int, void*) = nullptr;
		bool cl_get_predicted_stub(void* clientActive, const int server_time, void* player_state)
		{
			demo_game::note_client_active(clientActive);
			maybe_deferred_snap_reset();
			if (g_play.open() && g_play.armed && clientActive && player_state)
			{
				// Seed archive first so CL_IsDemoPlaying → GetDemoViewAnglesFromArchive
				// (immediately after this call in sub_668A90) sees recorded angles.
				seed_predicted_archive(clientActive, server_time);
				// H1 empty-gun fix: call orig first so weapons/health/flags stay from the
				// fed snapshot PS, then overlay only motion + viewangles from demo archive.
				CL_GetPredicted_orig(clientActive, server_time, player_state);
				inject_predicted(clientActive, server_time, player_state);
				return true;
			}
			return CL_GetPredicted_orig(clientActive, server_time, player_state);
		}

		// Default OFF. Enables the engine's native demo clock inside CL_SetCGameTime.
		bool g_native_demo_clock = false;
		std::atomic<int> g_native_read_blocked{0};

		// SAFETY NET — always active while armed, independent of demo_native_clock.
		// The native reader must never touch an engine demo file handle we do not own.
		// Returning 0 is also what stops CL_SetCGameTime's feed loop, so our own feed
		// stays the only source of snapshots.
		int (*CL_Demo_ReadMessage_orig)(unsigned int) = nullptr;
		int cl_demo_read_message_stub(const unsigned int local_client_num)
		{
			if (g_play.open() && g_play.armed)
			{
				g_native_read_blocked.fetch_add(1, std::memory_order_relaxed);
				return 0;
			}

			// NATIVE engine playback (cl_demo_play) runs through here because the
			// theater is not armed. demo_native owns that path: it stops the engine
			// priming loop (CL_SetCGameTime 0x86D97) from reading past the type-0
			// terminator into the footer, which is the EXE_ERR_PROCESS_DEMO_FILE_FAILED
			// runaway. Additive only — the armed path above is untouched, and when no
			// native demo is playing both calls are no-ops.
			int handled = 0;
			if (demo_native::intercept_read(local_client_num, &handled))
			{
				return handled;
			}
			const int r = CL_Demo_ReadMessage_orig(local_client_num);
			demo_native::note_read_result(local_client_num, r);
			return r;
		}

		// Scope flags for the native demo-only branches. The predicate is spoofed only
		// while the engine is rendering replay state or entering FirstSnapshot; feed
		// parsing and SetCGameTime continue to see the live-match result.
		thread_local bool g_in_draw_active_frame = false;
		thread_local bool g_in_first_snapshot = false;

		// Custom playback owns parsed live-match state; it must never put clc into the
		// engine's native demo-reader mode.  Expose demo semantics only to the two
		// render/bootstrap scopes that consume them.
		bool (*CL_IsDemoPlaying_orig)(int) = nullptr;
		bool cl_is_demo_playing_stub(const int local_client_num)
		{
			// The minimal HUD's theater gate. sub_33E550 (IsBroadcaster) early-
			// returns during demo playback, which kills every broadcaster-gated
			// widget in theater. This lies to THAT ONE CALL SITE ONLY, and only
			// while the minimal HUD is on -- off, it is a single relaxed atomic
			// load. See hud/broadcaster.hpp. RULE A3.1: extending this existing
			// stub instead of adding a second hook on CL_IsDemoPlaying.
			if (broadcaster::hide_demo_from_isbroadcaster(_ReturnAddress()))
			{
				return false;
			}
			if (g_feed_call)
			{
				return CL_IsDemoPlaying_orig(local_client_num);
			}
			if (g_in_first_snapshot
				&& g_play.open() && g_play.armed && !g_play.finished)
			{
				return true;
			}
			if (g_in_set_cgame_time)
			{
				// EXPERIMENT (demo_native_clock, default off). Letting this return true
				// inside CL_SetCGameTime switches on the engine's OWN demo clock — the
				// LABEL_44 path that back-computes cls_realtime from serverTime. The
				// native reader it would also unlock is neutralised in
				// cl_demo_read_message_stub, so no engine demo file is ever touched.
				if (g_native_demo_clock && g_play.open() && g_play.armed
					&& !g_play.finished
					&& demo_game::connstate() >= demo_game::CA_ACTIVE)
				{
					return true;
				}
				return CL_IsDemoPlaying_orig(local_client_num);
			}
			if (g_in_draw_active_frame
				&& g_play.open() && g_play.armed && !g_play.finished
				&& demo_game::connstate() >= demo_game::CA_ACTIVE)
			{
				return true;
			}
			return CL_IsDemoPlaying_orig(local_client_num);
		}

		// ---- TEMP read-only Soft/viewmodel chain probes (remove after verified) ----
		using CG_DrawActiveFrame_fn = std::int64_t (*)(int, int, int);
		CG_DrawActiveFrame_fn CG_DrawActiveFrame_orig = nullptr;
		std::int64_t cg_draw_active_frame_stub(const int local_client_num, const int draw_mode,
			const int demo_type_arg)
		{
			g_play.draw_enter_any.fetch_add(1, std::memory_order_relaxed);
			const bool theater = g_play.open() && g_play.armed
				&& demo_game::connstate() >= demo_game::CA_ACTIVE;
			if (theater)
			{
				g_play.draw_enter.fetch_add(1, std::memory_order_relaxed);
			}

			g_in_draw_active_frame = true;
			const auto result = CG_DrawActiveFrame_orig(local_client_num, draw_mode, demo_type_arg);
			g_in_draw_active_frame = false;
			g_play.draw_leave_any.fetch_add(1, std::memory_order_relaxed);
			if (theater)
			{
				g_play.draw_leave.fetch_add(1, std::memory_order_relaxed);
				g_play.draw_last_ret.store(result, std::memory_order_relaxed);
				++g_play.draw_frames;
			}
			return result;
		}

		// CL_FirstSnapshot @ 0x74910 — diagnostic scope only. The original function
		// must see the real live-match CL_IsDemoPlaying state.
		using CL_FirstSnapshot_fn = std::int64_t (*)(int);
		CL_FirstSnapshot_fn CL_FirstSnapshot_orig = nullptr;
		std::int64_t cl_first_snapshot_stub(const int local_client_num)
		{
			const bool live = local_client_num == 0 && !g_play.open() && !demo_native::native_playing();
			if (live)
			{
				static DWORD last_entry = 0;
				const DWORD now = GetTickCount();
				if (!last_entry || now - last_entry >= 5000)
				{
					Console::printf("[match] first snapshot enter: connection=%d", demo_game::connstate());
					last_entry = now;
				}
			}
			const bool was_in_first_snapshot = g_in_first_snapshot;
			g_in_first_snapshot = true;
			const auto result = CL_FirstSnapshot_orig(local_client_num);
			g_in_first_snapshot = was_in_first_snapshot;
			if (live && demo_game::connstate() == demo_game::CA_ACTIVE)
				Console::printf("[match] first snapshot activated the live client");
			return result;
		}

		// sub_4DA580 — residency poll used by FirstSnapshot demo branch.
		// CL_CreateCmd is still part of DrawActiveFrame in S2.  Let the engine build
		// the command first, then remove live buttons/movement while replay owns input.
		using CL_CreateCmd_fn = void* (*)(void*, int);
		CL_CreateCmd_fn CL_CreateCmd_orig = nullptr;
		void* cl_create_cmd_stub(void* cmd, const int local_client_num)
		{
			g_play.createcmd_enter.fetch_add(1, std::memory_order_relaxed);
			// Wii pointer aiming rides this stub (RULE A3.1: no second hook on
			// CL_CreateCmd). It snapshots the live angles, lets the engine apply
			// the mouse, then repacks the usercmd. No-op unless `wii_aim` is on
			// and no demo system owns the camera.
			wii_aim::before_create_cmd(local_client_num);
			void* result = CL_CreateCmd_orig(cmd, local_client_num);
			wii_aim::after_create_cmd(cmd, local_client_num);
			g_play.createcmd_leave.fetch_add(1, std::memory_order_relaxed);
			if (!g_play.open() || !g_play.armed || !cmd)
			{
				return result;
			}

			auto* bytes = static_cast<std::uint8_t*>(cmd);

			// FREE CAMERA needs its movement input. CL_Demo_FreeCameraMove (IDA
			// 0x913AE0) builds its wish velocity from THIS command, via the
			// clientActive cmd ring CG_PredictPlayerState hands it:
			//     forwardmove = (char)cmd[+0x34]   rightmove = (char)cmd[+0x35]
			//     buttons     = cmd[+0x8]: bit 0 -> up (+127), 0x800000 -> down
			// Zeroing them below is exactly "the free camera turns but will not
			// move": look survives because the angle shorts at +0x10..+0x1B were
			// never part of the strip. So in free camera keep ONLY those fields;
			// every other button (fire, ADS, weapon switch) is still removed.
			constexpr std::uint64_t FREECAM_BUTTONS = 0x1 | 0x800000;
			const bool freecam = theater_camera::custom_theater_freecam_active();
			const std::uint64_t buttons = *reinterpret_cast<std::uint64_t*>(bytes + 0x8);
			const std::uint8_t forward = bytes[0x34];
			const std::uint8_t right = bytes[0x35];

			*reinterpret_cast<std::uint64_t*>(bytes + 0x8) = 0;
			std::memset(bytes + 0x1C, 0, 0x18);
			bytes[0x34] = 0;
			bytes[0x35] = 0;
			bytes[0x38] = 0;

			if (freecam)
			{
				*reinterpret_cast<std::uint64_t*>(bytes + 0x8) = buttons & FREECAM_BUTTONS;
				bytes[0x34] = forward;
				bytes[0x35] = right;
			}
			return result;
		}

		// Prediction runs inside the original and overwrites ps viewangles.  Restore
		// the recorded angles at the stable post-prediction boundary used by the same
		// DrawActiveFrame before CG_CalcViewValues consumes them.
		using CG_UpdateLocalPlayerState_fn = std::int64_t (*)(int);
		CG_UpdateLocalPlayerState_fn CG_UpdateLocalPlayerState_orig = nullptr;
		std::int64_t cg_update_local_player_state_stub(const int local_client_num)
		{
			const auto result = CG_UpdateLocalPlayerState_orig(local_client_num);
			// Same post-prediction boundary the theater uses below: Wii pointer
			// aiming turns the predicted AIM angles into CAMERA angles here.
			wii_aim::after_update_local_player_state(local_client_num);
			if (g_play.open() && g_play.armed
				&& demo_game::connstate() >= demo_game::CA_ACTIVE)
			{
				if (void* cg = demo_game::cg_globals_for(local_client_num))
				{
					sync_ps_viewangles_after_predict(cg);
				}
			}
			return result;
		}

		// DIAGNOSTIC, default OFF (`demo_force_images 1`). Confirmed root cause is that the
		// viewmodel's images are never resident, so CG_UpdateViewModel latches hide=1.
		// Forcing this true while theater is armed tells us whether residency is the WHOLE
		// story: if the gun appears (even at a low mip) it is, and the real fix is to find
		// why the streaming request never happens. This is a probe, not the fix.
		// Registered via GameUtil::addCommand — the same path the working `demotimescale`
		// command uses. The earlier Dvar_RegisterBool attempt never produced a usable
		// console name; the pointer is non-null and the call sits inside init(), so the
		// reason is still unknown and is NOT assumed here.

		void cmd_native_clock()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2 && args->argv[args->nesting][1])
			{
				g_native_demo_clock = args->argv[args->nesting][1][0] != '0';
			}
			else
			{
				g_native_demo_clock = !g_native_demo_clock;
			}
			Console::printf(
				"[demo] demo_native_clock = %d (1 = engine's own demo clock; native reader stays blocked, blocked=%d)",
				g_native_demo_clock ? 1 : 0,
				g_native_read_blocked.load(std::memory_order_relaxed));
		}

		void cmd_smooth_clock()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2 && args->argv[args->nesting][1])
			{
				g_smooth_clock = args->argv[args->nesting][1][0] != '0';
			}
			else
			{
				g_smooth_clock = !g_smooth_clock;
			}
			Console::printf("[demo] demo_smooth_clock = %d (1 = engine derives serverTime per render frame)",
				g_smooth_clock ? 1 : 0);
		}

		void cmd_verbose()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2 && args->argv[args->nesting][1])
			{
				g_verbose = args->argv[args->nesting][1][0] != '0';
			}
			else
			{
				g_verbose = !g_verbose;
			}
			Console::printf("[demo] demo_verbose = %d (1 = per-frame probe spam)",
				g_verbose ? 1 : 0);
		}

		void cmd_angle_override()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2 && args->argv[args->nesting][1])
			{
				g_angle_per_frame_override = args->argv[args->nesting][1][0] != '0';
			}
			else
			{
				g_angle_per_frame_override = !g_angle_per_frame_override;
			}
			Console::printf("[demo] demo_angle_override = %d (0 = H1 behaviour, engine interpolates)",
				g_angle_per_frame_override ? 1 : 0);
		}

		void (*CL_SetCGameTime_orig)(unsigned int) = nullptr;
		void cl_set_cgame_time_stub(const unsigned int local_client_num)
		{
			const bool playback_tick = local_client_num == 0
				&& g_play.open() && g_play.armed && !g_play.finished
				&& !g_in_set_cgame_time;
			if (playback_tick)
			{
				demo_game::clear_snap_not_active(static_cast<int>(local_client_num));
				// At non-1x speed our theater clock owns serverTimeDelta.  Suppress only
				// the ACTIVE-state AdjustTimeDelta edge, before the engine consumes it.
				// ParseServerMessage runs after this call and may set newSnapshots again;
				// that value must remain visible for the rest of the frame.
				const float scale = demo_timescale::factor();
				if (demo_game::connstate() >= demo_game::CA_ACTIVE
					&& (scale < 0.999f || scale > 1.001f))
				{
					if (void* cl = demo_game::client_active_for(static_cast<int>(local_client_num)))
					{
						demo_game::write_i(cl, demo_game::CA_NEW_SNAPSHOTS, 0);
					}
				}
			}
			const bool was_in_set_cgame_time = g_in_set_cgame_time;
			g_in_set_cgame_time = true;
			CL_SetCGameTime_orig(local_client_num);
			g_in_set_cgame_time = was_in_set_cgame_time;
			if (playback_tick)
			{
				// CL_SetCGameTime derives serverTime = max(serverTime, serverTimeDelta +
				// cls_realtime).  cls_realtime runs at 1.0x, so the anchor we stamped at the
				// END of the previous CL_ClientFrame comes back with a full frame of
				// UNSCALED real time added on top.  That is the timescale bug: below 1x the
				// max() clamp ratchets the clock ahead of the scaled target (slow-mo does
				// nothing, or stutters as the next write yanks it back), and above 1x the
				// same injection shows up as snap-back jitter.  Re-stamp here — after the
				// engine's recompute and before the feed gate reads it — so our clock wins.
				// Skipping this re-stamp in smooth mode is the point: it is what was
				// overwriting the engine's continuously-derived serverTime with a value
				// that only changes 20 times a second.
				// ⭐ THE TIMESCALE STEPPING FIX, 2026-08-17.
				//
				// This re-stamp already ran at RENDER rate -- CL_SetCGameTime does, as
				// the [lerp] burst proved (serverTime moved on every rendered frame).
				// But the VALUE it stamps, demo_clock_ms(), only changed 20 times a
				// second, because advance_clock() was called solely from
				// CL_ClientFrame. Pinning serverTime to a value that only updates at
				// 20 Hz IS the staircase the user saw the moment they touched
				// timescale: high FPS, smooth engine clock, 20 Hz motion.
				//
				// Integrating here first makes the stamped value change every rendered
				// frame. advance_clock() integrates dt*scale from wall time and updates
				// last_wall BEFORE any early return, so a second caller cannot
				// double-count or dump a held interval -- it just gives finer dt.
				//
				// At 1x this whole block is skipped (smooth_clock_active()), because
				// the delta is constant there and the engine derives serverTime
				// smoothly by itself. Off 1x the delta is not constant AND
				// CL_SetCGameTime ratchets serverTime upward, so it must be stamped
				// directly rather than derived.
				if (!g_play.paused && g_play.seek_target < 0 && g_play.first_snap >= 0
					&& !smooth_clock_active())
				{
					g_play.advance_clock();
					apply_theater_clock(g_play.demo_clock_ms());
				}

				const bool was_in_feed = g_in_set_cgame_time;
				g_in_set_cgame_time = true;
				parse_during_playback();
				g_in_set_cgame_time = was_in_feed;
			}
		}

	}

	namespace
	{

		bool cmd_play(const char* arg)
		{
			if (!arg || !*arg)
			{
				Console::printf("usage: demo_play <file.dm_s2>");
				return false;
			}
			std::filesystem::path path(arg);
			if (!path.has_extension())
			{
				path += demo_utils::DEMO_EXTENSION;
			}
			if (!std::filesystem::exists(path))
			{
				if (const auto dir = demo_utils::demos_directory())
				{
					const auto alt = *dir / path.filename();
					if (std::filesystem::exists(alt))
					{
						path = alt;
					}
				}
			}
			if (!start_file(path))
			{
				Console::printf("[demo] could not open %s", arg);
				return false;
			}
			demo_utils::load_demo_file(path, g_play.demo);
			g_play.bounds = g_play.demo.time_bounds;
			g_play.file.clear();
			g_play.file.seekg(0);
			g_play.last_logged_connstate = demo_game::connstate();
			Console::printf("[demo] playback started: %s (map=%s gt=%s gs=%s)",
				path.string().c_str(),
				g_play.demo.map.c_str(),
				g_play.demo.gametype.c_str(),
				g_play.demo.gamestate ? "yes" : "no");
			bootstrap_map();
			return true;
		}

		void cmd_stop()
		{
			g_play.close();
			Console::printf("[demo] playback stopped");
		}

		void cmd_replay()
		{
			if (g_play.path.empty())
			{
				return;
			}
			const auto path = g_play.path;
			cmd_stop();
			cmd_play(path.string().c_str());
		}

	}

	void init()
	{
		Hook::create("CL_ClientFrame", reinterpret_cast<void*>(0x6DF940_b),
			reinterpret_cast<void*>(cl_client_frame_stub),
			reinterpret_cast<void**>(&CL_ClientFrame_orig));

		Hook::create("CL_WritePacket", reinterpret_cast<void*>(demo_game::addr_CL_WritePacket()),
			reinterpret_cast<void*>(cl_write_packet_stub),
			reinterpret_cast<void**>(&CL_WritePacket_orig));

		Hook::create("CL_GetPredicted", reinterpret_cast<void*>(demo_game::addr_CL_GetPredicted()),
			reinterpret_cast<void*>(cl_get_predicted_stub),
			reinterpret_cast<void**>(&CL_GetPredicted_orig));

		// CG_InterpolatePlayerState_S2 calls 0x462720 to restore the predicted
		// weapon block from clientActive's archive.  That archive contains movement
		// samples, not recorded weapon state.  Native demo playback skips this live
		// prediction restore and keeps the weapon fields parsed from the snapshot.
		Hook::create("CL_GetPredictedWeapon", reinterpret_cast<void*>(demo_game::addr_CL_GetPredictedWeapon()),
			reinterpret_cast<void*>(cl_get_predicted_weapon_stub),
			reinterpret_cast<void**>(&CL_GetPredictedWeapon_orig));

		Hook::create("CL_Demo_ReadMessage",
			reinterpret_cast<void*>(demo_game::addr_CL_Demo_ReadMessage()),
			reinterpret_cast<void*>(cl_demo_read_message_stub),
			reinterpret_cast<void**>(&CL_Demo_ReadMessage_orig));






		Hook::create("CL_SetCGameTime", reinterpret_cast<void*>(demo_game::addr_CL_SetCGameTime()),
			reinterpret_cast<void*>(cl_set_cgame_time_stub),
			reinterpret_cast<void**>(&CL_SetCGameTime_orig));


		Hook::create("CL_FirstSnapshot", reinterpret_cast<void*>(demo_game::addr_CL_FirstSnapshot()),
			reinterpret_cast<void*>(cl_first_snapshot_stub),
			reinterpret_cast<void**>(&CL_FirstSnapshot_orig));

		Hook::create("CL_IsDemoPlaying", reinterpret_cast<void*>(demo_game::addr_CL_IsDemoPlaying()),
			reinterpret_cast<void*>(cl_is_demo_playing_stub),
			reinterpret_cast<void**>(&CL_IsDemoPlaying_orig));

		Hook::create("CG_DrawActiveFrame", reinterpret_cast<void*>(demo_game::addr_CG_DrawActiveFrame()),
			reinterpret_cast<void*>(cg_draw_active_frame_stub),
			reinterpret_cast<void**>(&CG_DrawActiveFrame_orig));

		Hook::create("CL_CreateCmd", reinterpret_cast<void*>(demo_game::addr_CL_CreateCmd()),
			reinterpret_cast<void*>(cl_create_cmd_stub),
			reinterpret_cast<void**>(&CL_CreateCmd_orig));

		Hook::create("CG_UpdateLocalPlayerStateToClientActive",
			reinterpret_cast<void*>(demo_game::addr_CG_UpdateLocalPlayerState()),
			reinterpret_cast<void*>(cg_update_local_player_state_stub),
			reinterpret_cast<void**>(&CG_UpdateLocalPlayerState_orig));

		Hook::create("CG_ExecuteNewServerCommands",
			reinterpret_cast<void*>(demo_game::addr_CG_ExecuteNewServerCommands()),
			reinterpret_cast<void*>(cg_execute_new_server_commands_stub),
			reinterpret_cast<void**>(&CG_ExecuteNewServerCommands_orig));

		// Playing, stopping, pausing and seeking are the PRODUCT and belong to
		// demo_player, which routes them to whichever engine owns the file.
		// What is left here is developer-only: A/B switches for fixes that are
		// already shipped, kept so a regression can be reproduced on demand.
		dev_mode::on_enable([]
		{
			GameUtil::addCommand("demo_angle_override", cmd_angle_override);
			GameUtil::addCommand("demo_smooth_clock", cmd_smooth_clock);
			GameUtil::addCommand("demo_native_clock", cmd_native_clock);
			GameUtil::addCommand("demo_verbose", cmd_verbose);
			// `demo_lead` alone reports it; `demo_lead 0` restores the pre-fix
			// gate so the stepping bug can be reproduced on demand.
			GameUtil::addCommand("demo_lead", []
			{
				const auto* args = GameUtil::getCmdArgs();
				if (args && args->argc[args->nesting] > 1)
				{
					demo_utils::set_interp_lead_ms(std::atoi(args->argv[args->nesting][1]));
				}
				Console::printf("[demo] interpolation lead = %d ms (feed keeps the "
					"newest snapshot this far ahead of the render clock; 0 = the old "
					"gate, which left ~38%% of frames with no forward snapshot)",
					demo_utils::interp_lead_ms());
			});
			GameUtil::addCommand("demo_replay", cmd_replay);
		});
	}

	bool play(const std::filesystem::path& path)
	{
		return cmd_play(path.string().c_str());
	}

	void stop()
	{
		cmd_stop();
	}

	bool seek_to(std::int32_t absolute_ms)
	{
		if (!g_play.open())
		{
			return false;
		}
		return seek_to_ms(absolute_ms);
	}

	bool is_playing() { return g_play.open(); }
	bool suppress_local_messages() { return g_play.open() && g_play.armed; }
	bool is_replay_armed() { return g_play.armed; }
	bool is_active_replay()
	{
		return g_play.open() && g_play.armed
			&& demo_game::connstate() >= demo_game::CA_ACTIVE;
	}
	bool paused() { return g_play.paused; }

	void toggle_pause()
	{
		if (g_play.open())
		{
			g_play.paused = !g_play.paused;
			g_play.paused_by_finish = false;
		}
	}

	std::optional<std::int32_t> current_time()
	{
		if (!g_play.open() || !g_play.armed)
		{
			return std::nullopt;
		}
		// While a seek is outstanding the clock is frozen at where it STARTED
		// (advance_clock skips while seeking). Report where it is GOING, so the
		// progress bar, the dolly, and a second rewind/forward press all work
		// from the pending target -- exactly as they do on native, whose seek
		// lands in the same frame.
		if (g_play.seek_target >= 0)
		{
			return g_play.seek_target;
		}
		return g_play.demo_clock_ms();
	}

	std::optional<std::pair<std::int32_t, std::int32_t>> time_bounds()
	{
		if (g_play.bounds)
		{
			return g_play.bounds;
		}
		return g_play.demo.time_bounds;
	}

	const std::filesystem::path& current_path()
	{
		return g_play.path;
	}

	void forward(const std::uint32_t msec)
	{
		seek_to_ms(g_play.demo_clock_ms() + static_cast<int>(msec));
	}

	bool rewind(const std::optional<std::uint32_t> msec)
	{
		return seek_to_ms(g_play.demo_clock_ms() - static_cast<int>(msec.value_or(5000)));
	}


	bool seeking()
	{
		return g_play.seek_target >= 0;
	}

	float timescale()
	{
		return demo_timescale::factor();
	}

	void set_timescale(const float value)
	{
		// Clock continuity is structural now: advance_clock() integrates
		// wall_delta * scale, so a new scale simply applies from this instant forward
		// and there is nothing to rebase.  This also means changing the dvar directly
		// (`demotimescale 0.5` at the console) is just as safe as calling this — the
		// old retroactive rescale teleported the clock whenever that happened.
		demo_timescale::set_factor(value);
	}

	bool verbose() { return g_verbose; }


}
