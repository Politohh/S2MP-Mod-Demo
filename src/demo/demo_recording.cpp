#include "pch.h"
#include "demo_recording.hpp"

#include "demo/demo_game.hpp"
#include "demo/demo_native.hpp"
#include "demo/demo_playback.hpp"
#include "demo/demo_utils.hpp"

#include "Console.hpp"
#include "DevMode.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"
#include "Hook.hpp"

#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace demo_recording
{
	namespace
	{
		dvar_t* demo_autorecord = nullptr;

		// ---- on-screen notice ---------------------------------------------
		// Written on the CLIENT thread (the CL_ParseServerMessage hook or a
		// console command), drawn on the RENDER thread from R_EndFrame, hence
		// the mutex. The engine's own recorder shows a notice of its own; this
		// is the equivalent for our capture.
		struct notice_t
		{
			std::mutex lock;
			std::string text;
			std::uint64_t shown_at{};
			bool recording{};     // red dot while recording, green once saved
		};
		notice_t g_notice;
		constexpr std::uint64_t NOTICE_MS = 6000;
		constexpr std::uint64_t NOTICE_FADE_MS = 1000;

		void notify(std::string text, const bool recording)
		{
			std::lock_guard<std::mutex> guard(g_notice.lock);
			g_notice.text = std::move(text);
			g_notice.shown_at = GetTickCount64();
			g_notice.recording = recording;
		}

		// H1-style: buffer from first gamestate (match connect) so mid-match demo_start
		// still includes headers + update_gamestate_data + prior network TLVs.
		struct recorder_t
		{
			std::ofstream file;
			std::filesystem::path path;
			std::vector<std::uint8_t> buffer;
			bool session_live{};
			std::int32_t first_time{};
			std::int32_t last_time{};

			[[nodiscard]] bool file_active() const
			{
				return file.is_open();
			}

			[[nodiscard]] bool buffering() const
			{
				return session_live;
			}

			void clear_session()
			{
				if (file_active())
				{
					demo_utils::write_general_footer(file, first_time, last_time);
					demo_utils::write_end_of_file(file);
					file.close();
					Console::printf("[demo] wrote %s", path.string().c_str());
					notify("Saved demo: " + path.filename().string(), false);
				}
				path.clear();
				buffer.clear();
				session_live = false;
				first_time = 0;
				last_time = 0;
			}
		};

		recorder_t g_rec;

		// CL_ParseServerMessage is entered with readcount already past the netchan header, and it
		// decompresses/parses from data + readcount. Playback must restore that same cursor, so
		// the entry readcount rides in the low bits of the flags dword (bit31 = useZlib).
		std::vector<std::uint8_t> pack_network(const msg_t& msg, const int entry_readcount)
		{
			std::vector<std::uint8_t> out(8 + static_cast<std::size_t>(msg.cursize));
			const std::uint32_t seq = demo_game::clc_for(0)
				? static_cast<std::uint32_t>(demo_game::clc_server_message_sequence(0))
				: 0u;
			std::uint32_t flags = static_cast<std::uint32_t>(entry_readcount) & 0x7FFFFFFFu;
			if (msg.useZlib)
			{
				flags |= 0x80000000u;
			}
			std::memcpy(out.data(), &seq, 4);
			std::memcpy(out.data() + 4, &flags, 4);
			if (msg.data && msg.cursize > 0)
			{
				std::memcpy(out.data() + 8, msg.data, msg.cursize);
			}
			return out;
		}

		void begin_session_buffer()
		{
			g_rec.buffer.clear();
			g_rec.session_live = true;
			g_rec.first_time = 0;
			g_rec.last_time = 0;
			demo_utils::write_general_header(g_rec.buffer);
			demo_utils::write_mod_header(g_rec.buffer);
			demo_utils::write_map_header(g_rec.buffer);
			if (!demo_utils::write_gamestate_data(g_rec.buffer))
			{
				// NOT an error. write_gamestate_payload returns false while
				// demo_game::gs_data_count() is still 0, and this whole block is
				// retried every frame until the configstring pool populates — which
				// used to emit dozens of identical lines and drown the console.
				// Report the first one per session only.
				static bool warned = false;
				if (!warned || demo_playback::verbose())
				{
					warned = true;
					Console::printf(
						"[demo] gamestate not ready yet (configstrings empty) - retrying "
						"each frame, silenced until it succeeds");
				}
			}
			else
			{
				Console::printf("[demo] session buffer started (headers + gamestate)");
			}
		}

		bool flush_buffer_to_file(const char* reason)
		{
			if (g_rec.file_active())
			{
				Console::printf("[demo] already recording %s", g_rec.path.string().c_str());
				return true;
			}
			if (!g_rec.session_live || g_rec.buffer.empty())
			{
				Console::printf(
					"[demo] cannot start (%s): no session buffer yet — join a match first "
					"(buffer starts at the connect gamestate)",
					reason);
				return false;
			}

			const auto dir = demo_utils::demos_directory();
			if (!dir)
			{
				Console::printf("[demo] cannot start (%s): demos folder unavailable", reason);
				return false;
			}
			const auto path = demo_utils::next_demo_path(*dir);
			if (!path)
			{
				Console::printf("[demo] cannot start (%s): no free demo filename", reason);
				return false;
			}

			g_rec.file.open(*path, std::ios::binary);
			if (!g_rec.file_active())
			{
				Console::printf("[demo] cannot start (%s): failed to open %s",
					reason, path->string().c_str());
				return false;
			}

			g_rec.path = *path;
			g_rec.file.write(reinterpret_cast<const char*>(g_rec.buffer.data()),
				static_cast<std::streamsize>(g_rec.buffer.size()));
			g_rec.buffer.clear();
			Console::printf("[demo] recording %s (flushed %s buffer)",
				path->string().c_str(), reason);
			notify("Recording demo: " + path->filename().string(), true);
			return true;
		}

		void maybe_auto_flush()
		{
			if (g_rec.file_active())
			{
				return;
			}
			if (!demo_autorecord || !demo_autorecord->current.enabled)
			{
				return;
			}
			if (!g_rec.first_time)
			{
				return;
			}
			flush_buffer_to_file("autorecord");
		}

		void write_network_packet(const std::span<const std::uint8_t> pkt)
		{
			if (g_rec.file_active())
			{
				demo_utils::write_network_data(g_rec.file, pkt);
			}
			else if (g_rec.session_live)
			{
				demo_utils::write_network_data(g_rec.buffer, pkt);
			}
		}

		void write_predicted(const demo_utils::predicted_sample_t& sample, const std::uint8_t index)
		{
			if (g_rec.file_active())
			{
				demo_utils::write_predicted_player(g_rec.file, sample, index);
			}
			else if (g_rec.session_live)
			{
				demo_utils::write_predicted_player(g_rec.buffer, sample, index);
			}
		}

		void capture_predicted(void* clientActive)
		{
			if (!clientActive || (!g_rec.file_active() && !g_rec.session_live))
			{
				return;
			}
			const int idx = (demo_game::archive_index(clientActive) + 255) & 255;
			const auto* entry = demo_game::archive_slot(clientActive, idx);
			if (!entry || !entry->serverTime)
			{
				return;
			}

			// Keep movement/bob aligned with the archived prediction slot. Angles must be
			// CA_VIEWANGLES (ps+576 space) — NOT refdef. Refdef is ps viewangles after
			// CG_CalcViewValues post-process; injecting it back into ps double-applies
			// recoil/kick and aims completely wrong. Engine demos restore archive angles
			// via CL_GetDemoViewAnglesFromArchive (0x465A70).
			demo_utils::predicted_sample_t sample{};
			sample.cad = *entry;
			sample.viewangles[0] = entry->viewangles[0];
			sample.viewangles[1] = entry->viewangles[1];
			sample.viewangles[2] = entry->viewangles[2];
			write_predicted(sample, static_cast<std::uint8_t>(idx));
		}

		void capture_network(const int old_cs, const int new_cs, const msg_t& msg, const int entry_readcount)
		{
			if (demo_playback::is_playing() || demo_playback::suppress_local_messages())
			{
				return;
			}
			if (new_cs < demo_game::CA_PRIMED && old_cs < demo_game::CA_CONNECTED)
			{
				return;
			}

			// Match connect / map change: restart buffer with headers + live configstrings.
			if (old_cs < demo_game::CA_PRIMED)
			{
				if (g_rec.file_active())
				{
					g_rec.clear_session();
				}
				begin_session_buffer();
			}

			if (!g_rec.session_live && !g_rec.file_active())
			{
				// Mid-match without a prior connect buffer (mod loaded late): snapshot now.
				begin_session_buffer();
			}

			const auto pkt = pack_network(msg, entry_readcount);
			const bool is_gamestate = old_cs >= demo_game::CA_CONNECTED
				&& old_cs < demo_game::CA_PRIMED
				&& new_cs >= demo_game::CA_PRIMED;
			if (is_gamestate)
			{
				if (g_rec.file_active())
					demo_utils::write_gamestate_message(g_rec.file, pkt);
				else if (g_rec.session_live)
					demo_utils::write_gamestate_message(g_rec.buffer, pkt);
				Console::printf("[demo] captured native gamestate message size=%zu", pkt.size());
			}
			else
			{
				write_network_packet(pkt);
			}

			const int t = demo_game::server_time();
			if (!g_rec.first_time)
			{
				g_rec.first_time = t;
			}
			g_rec.last_time = t;

			maybe_auto_flush();
		}

		void (*CL_ParseServerMessage_orig)(int, msg_t*) = nullptr;
		void cl_parse_server_message_stub(const int client_num, msg_t* msg)
		{
			// H1 drops live msgs once armed. Do NOT gate on is_playing() alone — that
			// mutes loopback the instant we hit PRIMED, before ClientFrame can arm/feed,
			// which permanently sticks connstate at 9 if arm/apply fails.
			if (demo_playback::is_replay_armed() && !demo_utils::is_demo_feeding()
				&& demo_game::connstate() >= demo_game::CA_PRIMED)
			{
				return;
			}
			const int old_cs = demo_game::connstate();
			const int entry_readcount = msg ? msg->readcount : 0;

			// NATIVE demo playback only (no-op otherwise, so live play and the
			// custom theater are untouched). Packet 0 of every shipped .demo
			// carries the svc_gamestate without the 2-byte length prefix that
			// CL_ParseServerMessage unconditionally reads; the engine then decodes
			// zero bytes and silently loses the gamestate. Repair it in place so
			// the engine's own parser handles it.
			demo_native::repair_gamestate_message(msg);

			CL_ParseServerMessage_orig(client_num, msg);
			const int new_cs = demo_game::connstate();
			if (!demo_playback::is_playing() && msg && msg->data && msg->cursize > 0)
			{
				capture_network(old_cs, new_cs, *msg, entry_readcount);
			}

			// Drop session when fully disconnected.
			if (new_cs == demo_game::CA_DISCONNECTED && old_cs != demo_game::CA_DISCONNECTED)
			{
				if (!g_rec.file_active())
				{
					g_rec.buffer.clear();
					g_rec.session_live = false;
					g_rec.first_time = 0;
					g_rec.last_time = 0;
				}
			}
		}

		void (*CL_SavePredicted_orig)(void*, int) = nullptr;
		void cl_save_predicted_stub(void* clientActive, const int server_time)
		{
			demo_game::note_client_active(clientActive);
			CL_SavePredicted_orig(clientActive, server_time);
			if (!demo_playback::is_playing())
			{
				capture_predicted(clientActive);
			}
		}

		void cmd_demostart()
		{
			if (demo_autorecord)
			{
				demo_autorecord->current.enabled = true;
			}

			if (demo_playback::is_playing())
			{
				Console::printf("[demo] cannot record while playing a demo");
				return;
			}

			// ONE RECORDER AT A TIME. Turn the engine's auto-record off so the next
			// connect does not start a second recording, and if the engine is
			// already writing THIS match, finalise that file now (it is saved, with
			// its footer) rather than recording the same match twice.
			if (demo_native::auto_record())
			{
				demo_native::set_auto_record(false);
				Console::printf("[demo] native auto-record turned off -- only one recorder "
					"at a time");
			}
			if (demo_native::native_recording())
			{
				const bool stopped = demo_native::stop_native_recording();
				Console::printf(stopped
					? "[demo] stopped the native recording of this match (saved) so only "
					  "our capture records it"
					: "[demo] could not stop the native recording of this match -- both "
					  "recorders may capture it");
			}

			const int cs = demo_game::connstate();
			if (cs < demo_game::CA_PRIMED)
			{
				Console::printf(
					"[demo] auto-record armed (connstate=%d) — buffer starts at match gamestate",
					cs);
				return;
			}

			// Mid-match: ensure we have a GS snapshot even if the mod loaded after connect.
			if (!g_rec.session_live && !g_rec.file_active())
			{
				begin_session_buffer();
			}

			if (!flush_buffer_to_file("demo_start"))
			{
				return;
			}
			Console::printf("[demo] recording live (connstate=%d) — use demo_stop_record when done", cs);
		}

		void cmd_demostop()
		{
			if (demo_autorecord)
			{
				demo_autorecord->current.enabled = false;
			}
			stop();
			Console::printf("[demo] recording stopped");
		}
	}

	void start()
	{
		cmd_demostart();
	}

	void init()
	{
		if (Functions::_Dvar_RegisterBool)
		{
			demo_autorecord = static_cast<dvar_t*>(
				Functions::_Dvar_RegisterBool("demoautorecord", false, 0));
		}
		// Product-level, not developer-only: this is our own recorder, and
		// it is the one that keeps working when native playback doesn't.
		GameUtil::addCommand("demo_start", cmd_demostart);
		GameUtil::addCommand("demostart", cmd_demostart);
		GameUtil::addCommand("demo_stop_record", cmd_demostop);
		GameUtil::addCommand("demostop", cmd_demostop);

		Hook::create("CL_ParseServerMessage", reinterpret_cast<void*>(0x4639D0_b),
			reinterpret_cast<void*>(cl_parse_server_message_stub),
			reinterpret_cast<void**>(&CL_ParseServerMessage_orig));

		Hook::create("CL_SavePredicted", reinterpret_cast<void*>(0x464580_b),
			reinterpret_cast<void*>(cl_save_predicted_stub),
			reinterpret_cast<void**>(&CL_SavePredicted_orig));
	}

	bool is_recording()
	{
		return g_rec.file_active();
	}

	bool is_armed()
	{
		return demo_autorecord && demo_autorecord->current.enabled;
	}

	void stop()
	{
		g_rec.clear_session();
	}

	void cancel()
	{
		cmd_demostop();
	}

	void render_notice()
	{
		if (!Functions::_R_AddCmdDrawText || !Functions::_R_AddCmdDrawStretchPic)
		{
			return;
		}
		// The console's font and white material are registered ONCE at init, so
		// nothing here reaches into the asset system from the render thread.
		font_t* font = InternalConsole::consoleFont;
		Material* white = InternalConsole::getMaterialWhite();
		if (!font || !white || font->pixelHeight <= 0)
		{
			return;
		}

		std::string text;
		std::uint64_t shown_at = 0;
		bool recording = false;
		{
			std::lock_guard<std::mutex> guard(g_notice.lock);
			if (g_notice.text.empty())
			{
				return;
			}
			text = g_notice.text;
			shown_at = g_notice.shown_at;
			recording = g_notice.recording;
		}
		const std::uint64_t age = GetTickCount64() - shown_at;
		if (age >= NOTICE_MS)
		{
			return;
		}
		const std::uint64_t left = NOTICE_MS - age;
		const float alpha = left < NOTICE_FADE_MS
			? static_cast<float>(left) / static_cast<float>(NOTICE_FADE_MS) : 1.0f;

		// Render-target size: the same globals drawConsole and dolly use
		// (IDA 0x1C86268 / 0x1C8626C).
		const int sw = *reinterpret_cast<int*>(0x1C85268_b);
		const int sh = *reinterpret_cast<int*>(0x1C8526C_b);
		if (sw <= 0 || sh <= 0)
		{
			return;
		}

		// No text-width function is mapped, so size the panel from the glyph
		// count. The console font is monospace-ish; 0.55 of the pixel height per
		// character is close enough that the panel only ever runs a little wide.
		const float ph = static_cast<float>(font->pixelHeight);
		const float pad = ph * 0.6f;
		const float dot = ph * 0.55f;
		const float text_w = static_cast<float>(text.size()) * ph * 0.55f;
		const float box_w = pad + dot + pad * 0.8f + text_w + pad;
		const float box_h = ph + pad * 2.0f;
		const float x = (static_cast<float>(sw) - box_w) * 0.5f;
		const float y = static_cast<float>(sh) * 0.08f;

		float bg[4] = { 0.0f, 0.0f, 0.0f, 0.65f * alpha };
		Functions::_R_AddCmdDrawStretchPic(x, y, box_w, box_h, 0.0f, 0.0f, 1.0f, 1.0f, bg, white);

		// Red while recording, green once saved.
		float dot_col[4] = {};
		if (recording)
		{
			dot_col[0] = 0.95f; dot_col[1] = 0.18f; dot_col[2] = 0.18f;
		}
		else
		{
			dot_col[0] = 0.35f; dot_col[1] = 0.90f; dot_col[2] = 0.40f;
		}
		dot_col[3] = alpha;
		Functions::_R_AddCmdDrawStretchPic(x + pad, y + (box_h - dot) * 0.5f, dot, dot,
			0.0f, 0.0f, 1.0f, 1.0f, dot_col, white);

		float text_col[4] = { 1.0f, 1.0f, 1.0f, alpha };
		Functions::_R_AddCmdDrawText(text.c_str(), 0x7FFFFFFF, font, 0, 0, font->pixelHeight,
			x + pad + dot + pad * 0.8f, y + pad + ph, 1.0f, 1.0f, 0.0f, text_col, 0);
	}
}
