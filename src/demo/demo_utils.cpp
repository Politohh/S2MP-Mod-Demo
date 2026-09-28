#include "pch.h"
#include "demo_utils.hpp"

#include "Console.hpp"
#include "DvarInterface.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

#include <zlib.h>

namespace demo_utils
{
	namespace
	{
		bool g_demo_feeding = false;
		int g_last_snap_time = 0;

		template <typename T>
		T read_pod(const std::uint8_t* p)
		{
			T v{};
			std::memcpy(&v, p, sizeof(T));
			return v;
		}

		// sub_99960 performs these calls after the live receive path has parsed a
		// message.  The recording hook sees the message at this exact point (the
		// netchan header and the two sequence dwords have already been consumed),
		// so playback must retain the same commit edge after CL_ParseServerMessage.
		void commit_live_message_state(const int client_num, msg_t& msg,
			const int previous_message_sequence, const int previous_server_reliable_sequence,
			const int incoming_message_sequence)
		{
			auto* clc = demo_game::clc_for(client_num);
			if (!clc)
			{
				return;
			}

			if (msg.overflowed)
			{
				// Match sub_99960's error rollback. The dword at 0x20140 is
				// deliberately not touched here -- and that instinct is now proven
				// right for a better reason than it was written for: it is an
				// S2-ONLY field with no reference-build counterpart and no
				// established meaning (CLC_S2_ONLY_0x20140). It is the exact field
				// the doctrine records an earlier implementation corrupting.
				demo_game::write_i(clc, demo_game::CLC_SERVER_MESSAGE_SEQUENCE,
					previous_message_sequence);
				demo_game::write_i(clc, demo_game::CLC_SERVER_RELIABLE_SEQUENCE,
					previous_server_reliable_sequence);
				// Roll the client's own sequence back with it, for the reason in the
				// feed: reliableSequence - reliableAcknowledge > 128 is a hard
				// Com_Error, and nothing is ever transmitted while armed.
				demo_game::write_i(clc, demo_game::CLC_RELIABLE_SEQUENCE,
					previous_server_reliable_sequence);
				return;
			}

			// ⭐ WHAT THIS BLOCK ACTUALLY IS -- established 2026-08-17.
			//
			// These were written as generic "message event" functions. They are not.
			// They are the NATIVE DEMO RECORDER's per-message append chain.
			// The live receive loop, sub_99960, calls exactly this sequence after
			// CL_ParseServerMessage:
			//     sub_910440, sub_91C6C0, sub_91CFA0, sub_91D2A0, sub_91CEC0
			// and this function replicates it faithfully. So replaying a .dm_s2 through
			// the theater ALSO writes a native .demo, using the engine's own header and
			// footer. Observed in game.
			//
			// It only began working when clc_for()'s base was corrected: the state
			// pointer below is read from clc+346136, so with the old (wrong) global it
			// came back 0 and the append silently returned on every message.
			using RecordReady_fn = bool (*)(int);
			using RecordPending_fn = char (*)(int);
			using RecordBegin_fn = std::int64_t (*)(std::int64_t, int);
			using RecordData_fn = std::int64_t (*)(std::int64_t, unsigned int, int,
				const void*, int);
			using RecordEnd_fn = std::int64_t (*)(unsigned int);

			const auto record_ready = reinterpret_cast<RecordReady_fn>(0x90F440_b); // IDA 0x910440
			// CL_Demo_IsRecordPendingFirstSnapshot: reads clc+346132. StartRecord sets
			// it to 1; CL_ParseSnapshot's helper clears it on the first snapshot. The
			// append runs only once it reads 0 -- recording is armed, but it needs a
			// snapshot to anchor to first.
			const auto record_pending = reinterpret_cast<RecordPending_fn>(0x91B6C0_b); // IDA 0x91C6C0
			if (!record_ready(client_num) || record_pending(client_num))
			{
				return;
			}

			// clc+346136 -- the state pointer the append trio takes as arg0, and the
			// slot CL_Demo_StartRecord fills in. Zero means recording is not running.
			const auto record_state = *reinterpret_cast<std::uintptr_t*>(clc + 346136);
			if (!record_state || !msg.data || msg.cursize < 4)
			{
				return;
			}

			const auto record_begin = reinterpret_cast<RecordBegin_fn>(0x91BFA0_b); // IDA 0x91CFA0
			const auto record_data = reinterpret_cast<RecordData_fn>(0x91C2A0_b);   // IDA 0x91D2A0
			const auto record_end = reinterpret_cast<RecordEnd_fn>(0x91BEC0_b);     // IDA 0x91CEC0
			record_begin(record_state, client_num);
			record_data(record_state, static_cast<unsigned int>(client_num),
				incoming_message_sequence, msg.data + 4, msg.cursize - 4);
			record_end(static_cast<unsigned int>(client_num));
		}
	}

	bool is_demo_feeding()
	{
		return g_demo_feeding;
	}

	demo_data_id operator|(demo_data_id lhs, demo_data_id rhs)
	{
		using U = std::underlying_type_t<demo_data_id>;
		return demo_data_id(static_cast<U>(lhs) | static_cast<U>(rhs));
	}

	demo_data_id operator&(demo_data_id lhs, demo_data_id rhs)
	{
		using U = std::underlying_type_t<demo_data_id>;
		return demo_data_id(static_cast<U>(lhs) & static_cast<U>(rhs));
	}

	std::string_view get_dvar_string(const std::string_view name, const bool allow_empty)
	{
		if (!Functions::_Dvar_FindVar)
		{
			return allow_empty ? std::string_view{} : std::string_view("");
		}
		// WWII ships most stock dvars/commands as numeric engine IDs ("mapname" → "1673").
		const std::string engine = DvarInterface::toEngineString(std::string(name));
		const dvar_t* dvar = Functions::_Dvar_FindVar(engine.c_str());
		// Fall back to the friendly name for mod-registered dvars (demoautorecord, etc.).
		if (!dvar && engine != name)
		{
			dvar = Functions::_Dvar_FindVar(std::string(name).c_str());
		}
		if (!dvar || !dvar->current.string || !dvar->current.string[0])
		{
			return allow_empty ? std::string_view{} : std::string_view("");
		}
		return dvar->current.string;
	}

	std::string get_mapname_lower(const bool fallback)
	{
		auto s = std::string(get_dvar_string("mapname", fallback));
		if (s.empty() && fallback)
		{
			s = "unknown";
		}
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	std::string get_gametype_lower(const bool fallback)
	{
		auto s = std::string(get_dvar_string("g_gametype", fallback));
		if (s.empty() && fallback)
		{
			s = "unknown";
		}
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	std::string get_short_mapname_lower(const bool fallback)
	{
		auto map = get_mapname_lower(fallback);
		if (map.starts_with("mp_"))
		{
			map.erase(0, 3);
		}
		return map;
	}

	std::optional<std::filesystem::path> demos_directory()
	{
		std::string base(get_dvar_string("fs_basepath", false));
		if (base.empty() && Functions::_Sys_Cwd)
		{
			if (const char* cwd = Functions::_Sys_Cwd())
			{
				base = cwd;
			}
		}
		if (base.empty())
		{
			char mod[MAX_PATH]{};
			if (GetModuleFileNameA(nullptr, mod, MAX_PATH))
			{
				base = std::filesystem::path(mod).parent_path().string();
			}
		}
		if (base.empty())
		{
			return std::nullopt;
		}
		auto dir = std::filesystem::path(base) / "demos";
		std::error_code ec;
		if (!std::filesystem::exists(dir))
		{
			if (!std::filesystem::create_directories(dir, ec))
			{
				Console::printf("[demo] create_directories(%s) failed", dir.string().c_str());
				return std::nullopt;
			}
		}
		return dir;
	}

	std::optional<std::filesystem::path> next_demo_path(const std::filesystem::path& dir)
	{
		const auto map = get_short_mapname_lower(true);
		for (int i = 0; i < 10'000; ++i)
		{
			const auto path = dir / std::format("{}.{:04}{}", map, i, DEMO_EXTENSION);
			if (!std::filesystem::exists(path))
			{
				return path;
			}
		}
		return std::nullopt;
	}

	void write_id_and_size(std::ostream& out, const demo_data_id id, const std::size_t size)
	{
		if (size < 256)
		{
			const auto complete = id | demo_data_id::one_byte_flag;
			const auto id_byte = static_cast<std::uint8_t>(complete);
			const auto size_byte = static_cast<std::uint8_t>(size);
			out.write(reinterpret_cast<const char*>(&id_byte), 1);
			out.write(reinterpret_cast<const char*>(&size_byte), 1);
		}
		else
		{
			const auto id_byte = static_cast<std::uint8_t>(id);
			const auto size32 = static_cast<std::uint32_t>(size);
			out.write(reinterpret_cast<const char*>(&id_byte), 1);
			out.write(reinterpret_cast<const char*>(&size32), 4);
		}
	}

	void write_id_and_size(std::vector<std::uint8_t>& out, const demo_data_id id, const std::size_t size)
	{
		if (size < 256)
		{
			out.push_back(static_cast<std::uint8_t>(id | demo_data_id::one_byte_flag));
			out.push_back(static_cast<std::uint8_t>(size));
		}
		else
		{
			out.push_back(static_cast<std::uint8_t>(id));
			const auto size32 = static_cast<std::uint32_t>(size);
			const auto* p = reinterpret_cast<const std::uint8_t*>(&size32);
			out.insert(out.end(), p, p + 4);
		}
	}

	namespace
	{
		void append_bytes(std::vector<std::uint8_t>& out, const void* data, const std::size_t size)
		{
			const auto* p = static_cast<const std::uint8_t*>(data);
			out.insert(out.end(), p, p + size);
		}

		int g_command_baseline = 0;
	}

	void write_network_data(std::ostream& out, const std::span<const std::uint8_t> data)
	{
		write_id_and_size(out, demo_data_id::network_data, data.size());
		out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	}

	void write_network_data(std::vector<std::uint8_t>& out, const std::span<const std::uint8_t> data)
	{
		write_id_and_size(out, demo_data_id::network_data, data.size());
		append_bytes(out, data.data(), data.size());
	}

	void write_gamestate_message(std::ostream& out, const std::span<const std::uint8_t> data)
	{
		write_id_and_size(out, demo_data_id::gamestate_message, data.size());
		out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	}

	void write_gamestate_message(std::vector<std::uint8_t>& out, const std::span<const std::uint8_t> data)
	{
		write_id_and_size(out, demo_data_id::gamestate_message, data.size());
		append_bytes(out, data.data(), data.size());
	}

	void write_predicted_player(std::ostream& out, const predicted_sample_t& sample, const std::uint8_t index)
	{
		// id+index + SavePredicted fields through w2 (58B). Old demos were 50B without extras.
		constexpr std::size_t payload = 60;
		write_id_and_size(out, demo_data_id::predicted_data, payload);
		const auto id = static_cast<std::uint8_t>(predicted_data_id::player);
		out.write(reinterpret_cast<const char*>(&id), 1);
		out.write(reinterpret_cast<const char*>(&index), 1);
		out.write(reinterpret_cast<const char*>(&sample.cad.serverTime), 4);
		out.write(reinterpret_cast<const char*>(&sample.cad.origin[0]), 12);
		out.write(reinterpret_cast<const char*>(&sample.cad.velocity[0]), 12);
		const std::int32_t bob = sample.cad.bobA & 0xFF;
		out.write(reinterpret_cast<const char*>(&bob), 4);
		out.write(reinterpret_cast<const char*>(&sample.cad.bobB), 4);
		out.write(reinterpret_cast<const char*>(&sample.viewangles[0]), 12);
		out.write(reinterpret_cast<const char*>(&sample.cad.extra0), 4);
		out.write(reinterpret_cast<const char*>(&sample.cad.w0), 2);
		out.write(reinterpret_cast<const char*>(&sample.cad.w1), 2);
		out.write(reinterpret_cast<const char*>(&sample.cad.w2), 2);
	}

	void write_predicted_player(std::vector<std::uint8_t>& out, const predicted_sample_t& sample, const std::uint8_t index)
	{
		constexpr std::size_t payload = 60;
		write_id_and_size(out, demo_data_id::predicted_data, payload);
		const auto id = static_cast<std::uint8_t>(predicted_data_id::player);
		out.push_back(id);
		out.push_back(index);
		append_bytes(out, &sample.cad.serverTime, 4);
		append_bytes(out, &sample.cad.origin[0], 12);
		append_bytes(out, &sample.cad.velocity[0], 12);
		const std::int32_t bob = sample.cad.bobA & 0xFF;
		append_bytes(out, &bob, 4);
		append_bytes(out, &sample.cad.bobB, 4);
		append_bytes(out, &sample.viewangles[0], 12);
		append_bytes(out, &sample.cad.extra0, 4);
		append_bytes(out, &sample.cad.w0, 2);
		append_bytes(out, &sample.cad.w1, 2);
		append_bytes(out, &sample.cad.w2, 2);
	}

	void write_mod_header(std::ostream& out)
	{
		const auto mod = get_dvar_string("fs_game", true);
		if (mod.empty())
		{
			return;
		}
		write_id_and_size(out, demo_data_id::mod_header, mod.size() + 1);
		out.write(mod.data(), static_cast<std::streamsize>(mod.size()));
		const char nul = '\0';
		out.write(&nul, 1);
	}

	void write_mod_header(std::vector<std::uint8_t>& out)
	{
		const auto mod = get_dvar_string("fs_game", true);
		if (mod.empty())
		{
			return;
		}
		write_id_and_size(out, demo_data_id::mod_header, mod.size() + 1);
		append_bytes(out, mod.data(), mod.size());
		out.push_back(0);
	}

	bool write_map_header(std::ostream& out)
	{
		const auto map = get_mapname_lower(false);
		const auto gt = get_gametype_lower(false);
		if (map.empty() || gt.empty())
		{
			return false;
		}
		const auto size = map.size() + gt.size() + 2;
		write_id_and_size(out, demo_data_id::map_header, size);
		const char nul = '\0';
		out.write(map.c_str(), static_cast<std::streamsize>(map.size()));
		out.write(&nul, 1);
		out.write(gt.c_str(), static_cast<std::streamsize>(gt.size()));
		out.write(&nul, 1);
		return true;
	}

	bool write_map_header(std::vector<std::uint8_t>& out)
	{
		const auto map = get_mapname_lower(false);
		const auto gt = get_gametype_lower(false);
		if (map.empty() || gt.empty())
		{
			return false;
		}
		write_id_and_size(out, demo_data_id::map_header, map.size() + gt.size() + 2);
		append_bytes(out, map.c_str(), map.size());
		out.push_back(0);
		append_bytes(out, gt.c_str(), gt.size());
		out.push_back(0);
		return true;
	}

	void write_general_header(std::ostream& out)
	{
		const std::string desc = "S2MP-Mod client demo";
		const std::string ver(DEMO_CODE_VERSION);
		const std::string map = get_mapname_lower(true);
		const std::string gt = get_gametype_lower(true);
		const std::size_t size = desc.size() + ver.size() + map.size() + gt.size() + 4;
		write_id_and_size(out, demo_data_id::gen_header, size);
		const char nul = '\0';
		auto write_str = [&](const std::string& s)
		{
			out.write(s.c_str(), static_cast<std::streamsize>(s.size()));
			out.write(&nul, 1);
		};
		write_str(desc);
		write_str(ver);
		write_str(map);
		write_str(gt);
	}

	void write_general_header(std::vector<std::uint8_t>& out)
	{
		const std::string desc = "S2MP-Mod client demo";
		const std::string ver(DEMO_CODE_VERSION);
		const std::string map = get_mapname_lower(true);
		const std::string gt = get_gametype_lower(true);
		write_id_and_size(out, demo_data_id::gen_header, desc.size() + ver.size() + map.size() + gt.size() + 4);
		auto write_str = [&](const std::string& s)
		{
			append_bytes(out, s.c_str(), s.size());
			out.push_back(0);
		};
		write_str(desc);
		write_str(ver);
		write_str(map);
		write_str(gt);
	}

	void write_general_footer(std::ostream& out, const std::int32_t first, const std::int32_t last)
	{
		const auto dur = (last - first) / 1000;
		const auto fmt = std::format("[{:02}:{:02}:{:02}]", dur / 3600, (dur % 3600) / 60, dur % 60);
		const auto size = 8 + fmt.size() + 1;
		write_id_and_size(out, demo_data_id::gen_footer, size);
		out.write(reinterpret_cast<const char*>(&first), 4);
		out.write(reinterpret_cast<const char*>(&last), 4);
		const char nul = '\0';
		out.write(fmt.c_str(), static_cast<std::streamsize>(fmt.size()));
		out.write(&nul, 1);
	}

	void write_end_of_file(std::ostream& out)
	{
		write_id_and_size(out, demo_data_id::eof, 0);
	}

	namespace zlib
	{
		std::vector<std::uint8_t> compress(const std::span<const std::uint8_t> input)
		{
			if (input.empty())
			{
				return {};
			}
			const auto bound = ::compressBound(static_cast<uLong>(input.size()));
			std::vector<std::uint8_t> out(bound);
			uLongf out_len = bound;
			if (::compress(out.data(), &out_len, input.data(), static_cast<uLong>(input.size())) != Z_OK)
			{
				return {};
			}
			out.resize(out_len);
			return out;
		}

		std::vector<std::uint8_t> decompress(const std::span<const std::uint8_t> input, const std::size_t hint)
		{
			if (input.empty())
			{
				return {};
			}
			std::vector<std::uint8_t> out(hint ? hint : input.size() * 4);
			uLongf out_len = static_cast<uLongf>(out.size());
			if (::uncompress(out.data(), &out_len, input.data(), static_cast<uLong>(input.size())) != Z_OK)
			{
				return {};
			}
			out.resize(out_len);
			return out;
		}
	}

	namespace
	{
		bool write_gamestate_payload(std::uint32_t& svr_cmd_seq, packed_gamestate_sizes& pgs,
			std::vector<std::uint8_t>& string_data, std::vector<std::uint8_t>& string_offsets)
		{
			const int data_count = demo_game::gs_data_count();
			constexpr int k_string_data_cap = 0x20000;
			const auto offsets_bytes = static_cast<std::size_t>(demo_game::MAX_CONFIGSTRINGS) * sizeof(int);
			if (data_count <= 0 || data_count > k_string_data_cap)
			{
				return false;
			}

			const std::span<const std::uint8_t> sd(
				reinterpret_cast<const std::uint8_t*>(demo_game::gs_string_data()),
				static_cast<std::size_t>(data_count));
			const std::span<const std::uint8_t> so(
				reinterpret_cast<const std::uint8_t*>(demo_game::gs_string_offsets()),
				offsets_bytes);

			string_data = zlib::compress(sd);
			string_offsets = zlib::compress(so);
			if (string_data.empty() || string_offsets.empty())
			{
				return false;
			}
			if (string_data.size() >= k_string_data_cap || string_offsets.size() > offsets_bytes)
			{
				return false;
			}

			svr_cmd_seq = 0;
			if (demo_game::clc_for(0))
			{
				svr_cmd_seq = static_cast<std::uint32_t>(
					demo_game::read_i(demo_game::clc_for(0), demo_game::CLC_SERVER_COMMAND_SEQUENCE));
			}

			pgs = {};
			pgs.string_data = string_data.size();
			pgs.string_offsets = string_offsets.size();
			pgs.compressed = 1;
			return true;
		}
	}

	bool write_gamestate_data(std::ostream& out)
	{
		std::uint32_t svr_cmd_seq = 0;
		packed_gamestate_sizes pgs{};
		std::vector<std::uint8_t> string_data;
		std::vector<std::uint8_t> string_offsets;
		if (!write_gamestate_payload(svr_cmd_seq, pgs, string_data, string_offsets))
		{
			return false;
		}
		const persistent_data_t persistent{};
		const auto size = 12 + string_data.size() + string_offsets.size() + sizeof(persistent_data_t);
		write_id_and_size(out, demo_data_id::update_gamestate_data, size);
		out.write(reinterpret_cast<const char*>(&svr_cmd_seq), 4);
		out.write(reinterpret_cast<const char*>(&pgs), 8);
		out.write(reinterpret_cast<const char*>(string_data.data()), static_cast<std::streamsize>(string_data.size()));
		out.write(reinterpret_cast<const char*>(string_offsets.data()), static_cast<std::streamsize>(string_offsets.size()));
		out.write(reinterpret_cast<const char*>(&persistent), sizeof(persistent));
		return true;
	}

	bool write_gamestate_data(std::vector<std::uint8_t>& out)
	{
		std::uint32_t svr_cmd_seq = 0;
		packed_gamestate_sizes pgs{};
		std::vector<std::uint8_t> string_data;
		std::vector<std::uint8_t> string_offsets;
		if (!write_gamestate_payload(svr_cmd_seq, pgs, string_data, string_offsets))
		{
			return false;
		}
		const persistent_data_t persistent{};
		const auto size = 12 + string_data.size() + string_offsets.size() + sizeof(persistent_data_t);
		write_id_and_size(out, demo_data_id::update_gamestate_data, size);
		append_bytes(out, &svr_cmd_seq, 4);
		append_bytes(out, &pgs, 8);
		append_bytes(out, string_data.data(), string_data.size());
		append_bytes(out, string_offsets.data(), string_offsets.size());
		append_bytes(out, &persistent, sizeof(persistent));
		return true;
	}

	// ⭐ MEASURED 2026-08-17 (the [lerp] burst) -- this gate is why theater playback
	// was steppy, and it was wrong in TWO ways at once.
	//
	// Sampled inside CG_DrawActiveFrame at render rate during a real playback:
	//
	//   serverTime=291244 (d=+6) snap=291250 lead= +6  pinned=291221
	//   serverTime=291250 (d=+6) snap=291250 lead= +0  pinned=291221
	//   serverTime=291257 (d=+7) snap=291250 lead= -7  pinned=291221
	//   serverTime=291263 (d=+6) snap=291250 lead=-13  pinned=291221
	//   serverTime=291270 (d=+7) snap=291300 lead=+30  pinned=291271
	//
	// The engine's clock is FINE: clientActive.serverTime rose every single rendered
	// frame (d=+5..+16, never 0), so g_smooth_clock does its job. But `lead`
	// (snap - serverTime) went NEGATIVE on 9 of 24 frames -- 38%. A negative lead
	// means the time being rendered has run PAST the newest parsed snapshot, so
	// there is no forward snapshot to interpolate toward and those frames snap to
	// the last known state. That is the stepping.
	//
	// Two causes, both fixed here:
	//
	//   1. WRONG CLOCK. The gate used demo_game::server_time() -- the GLOBAL
	//      cl_serverTime, which the theater pins once per CLIENT frame (20 Hz).
	//      The renderer interpolates at clientActive.serverTime instead, which
	//      advances per RENDER frame. In the sample above `pinned` trails
	//      `serverTime` by up to 42 ms, so the feed was deciding when to pull the
	//      next snapshot using a clock that lags the one doing the drawing.
	//
	//   2. NO LEAD. Even against the right clock, feeding "until the snapshot
	//      passes now" leaves lead ~= 0 and it goes negative as soon as the render
	//      clock advances. Live play never looks like this: the client deliberately
	//      holds render time BEHIND the newest snapshot by about one snapshot
	//      interval, which is what gives the interpolator something to aim at.
	//      S2's cadence is 20 Hz -- CL_ClientFrame does cl_serverTime += 50 -- so
	//      one interval is 50 ms.
	//
	// Feeding further ahead does NOT play the demo faster: it only makes the data
	// available earlier. The clock is untouched, so seek, pause and timescale are
	// unaffected.
	int g_interp_lead_ms = 50;   // one snapshot interval; 0 restores the old gate

	bool continue_demo_reading()
	{
		// Gate against the clock the RENDERER uses, not the pinned global.
		int now = demo_game::server_time();
		if (void* cl = demo_game::client_active_for(0))
		{
			const int st = demo_game::read_i(cl, demo_game::CA_SERVER_TIME);
			if (st > 0)
			{
				now = st;
			}
		}
		return demo_game::snap_server_time() <= now + g_interp_lead_ms;
	}

	int interp_lead_ms() { return g_interp_lead_ms; }
	void set_interp_lead_ms(const int ms) { g_interp_lead_ms = ms < 0 ? 0 : ms; }

	void reset_demo_read_gate()
	{
		g_last_snap_time = 0;
	}

	void note_command_baseline(const int svr_cmd_seq)
	{
		g_command_baseline = svr_cmd_seq;
	}


	bool apply_recorded_gamestate(const gamestate_t& gs)
	{
		const auto expect_off = static_cast<std::size_t>(demo_game::MAX_CONFIGSTRINGS) * sizeof(int);
		if (gs.string_offsets.size() != expect_off)
		{
			Console::printf("[demo] GS offsets size mismatch (%zu vs %zu)",
				gs.string_offsets.size(), expect_off);
			return false;
		}
		if (gs.string_data.empty() || gs.data_count <= 0
			|| static_cast<std::size_t>(gs.data_count) > gs.string_data.size()
			|| gs.data_count > 0x20000)
		{
			Console::printf("[demo] GS string_data invalid (count=%d size=%zu)",
				gs.data_count, gs.string_data.size());
			return false;
		}

		auto* offsets = demo_game::gs_string_offsets();
		auto* data = demo_game::gs_string_data();
		if (!offsets || !data)
		{
			Console::printf("[demo] GS live pointers null");
			return false;
		}

		Console::printf("[demo] applying GS memcpy (cmd_seq=%u dataCount=%d)...",
			gs.svr_cmd_seq, gs.data_count);
		std::memcpy(offsets, gs.string_offsets.data(), gs.string_offsets.size());
		std::memcpy(data, gs.string_data.data(), static_cast<std::size_t>(gs.data_count));
		demo_game::gs_data_count() = gs.data_count;


		if (auto* clc = demo_game::clc_for(0))
		{
			demo_game::write_i(clc, demo_game::CLC_SERVER_COMMAND_SEQUENCE, static_cast<int>(gs.svr_cmd_seq));
			demo_game::write_i(clc, demo_game::CLC_LAST_EXECUTED_SERVER_COMMAND, static_cast<int>(gs.svr_cmd_seq));
		}
		note_command_baseline(static_cast<int>(gs.svr_cmd_seq));
		Console::printf("[demo] applied recorded gamestate (cmd_seq=%u dataCount=%d)",
			gs.svr_cmd_seq, gs.data_count);
		return true;
	}

	void process_gamestate_data(const std::span<const std::uint8_t> buffer, std::optional<gamestate_t>& out)
	{
		if (buffer.size() < 12)
		{
			return;
		}
		std::size_t off = 0;
		const auto svr_cmd_seq = read_pod<std::uint32_t>(buffer.data() + off);
		off += 4;
		packed_gamestate_sizes pgs{};
		std::memcpy(&pgs, buffer.data() + off, 8);
		off += 8;
		if (!pgs.compressed)
		{
			return;
		}
		const auto need = off + pgs.string_data + pgs.string_offsets + sizeof(persistent_data_t);
		if (buffer.size() != need)
		{
			return;
		}

		const std::span sd(buffer.data() + off, pgs.string_data);
		off += pgs.string_data;
		const std::span so(buffer.data() + off, pgs.string_offsets);
		off += pgs.string_offsets;
		(void)off;

		auto string_data = zlib::decompress(sd, 0x20000);
		auto string_offsets = zlib::decompress(so,
			static_cast<std::size_t>(demo_game::MAX_CONFIGSTRINGS) * sizeof(int));
		if (string_data.empty() || string_offsets.size()
			!= static_cast<std::size_t>(demo_game::MAX_CONFIGSTRINGS) * sizeof(int))
		{
			return;
		}

		gamestate_t gs{};
		gs.svr_cmd_seq = svr_cmd_seq;
		gs.data_count = static_cast<std::int32_t>(string_data.size());
		gs.string_data = std::move(string_data);
		gs.string_offsets = std::move(string_offsets);
		out = std::move(gs);
	}

	// H1 model: bump ONLY clientActive.serverTimeDelta. CL_SetCGameTime derives
	// serverTime = max(serverTime, serverTimeDelta + cls_realtime). Never park the
	// global feed clock here — that races FirstSnapshot and causes rewind crashes.
	void fast_forward_demo(const std::uint32_t msec)
	{
		if (demo_game::connstate() < demo_game::CA_PRIMED)
		{
			return;
		}
		if (void* cl = demo_game::client_active_for(0))
		{
			const int cur = demo_game::read_i(cl, demo_game::CA_SERVER_TIME_DELTA);
			demo_game::write_i(cl, demo_game::CA_SERVER_TIME_DELTA, cur + static_cast<int>(msec));
		}
	}

	void process_network_data(const std::span<const std::uint8_t> buffer)
	{
		if (buffer.size() <= 8)
		{
			return;
		}

		const auto seq = read_pod<std::uint32_t>(buffer.data());
		const auto ack_flags = read_pod<std::uint32_t>(buffer.data() + 4);
		const std::span payload(buffer.data() + 8, buffer.size() - 8);

		msg_t msg{};
		msg.data = reinterpret_cast<char*>(const_cast<std::uint8_t*>(payload.data()));
		msg.maxsize = static_cast<int>(payload.size());
		msg.cursize = static_cast<int>(payload.size());
		// Restore the recorded parse cursor (netchan header already consumed at record time).
		msg.readcount = static_cast<int>(ack_flags & 0x7FFFFFFFu);
		if (msg.readcount < 0 || msg.readcount > msg.cursize)
		{
			msg.readcount = 0;
		}
		msg.bit = msg.readcount * 8;
		msg.useZlib = static_cast<int>(ack_flags >> 31);

		// sub_99960 consumes two dwords immediately before entering
		// CL_ParseServerMessage: incoming message sequence, then server reliable
		// sequence (the latter's high bit is the zlib flag). The recorder stores
		// entry_readcount, so recover both values from the original message even
		// for demos written before the outer sequence field was corrected.
		int incoming_message_sequence = static_cast<int>(seq);
		int server_reliable_sequence = 0;
		const auto entry = static_cast<std::size_t>(msg.readcount);
		if (entry >= 8 && entry <= payload.size())
		{
			incoming_message_sequence = static_cast<int>(
				read_pod<std::uint32_t>(payload.data() + entry - 8));
			server_reliable_sequence = static_cast<int>(
				read_pod<std::uint32_t>(payload.data() + entry - 4) & 0x7FFFFFFFu);
			msg.useZlib = static_cast<int>(
				read_pod<std::uint32_t>(payload.data() + entry - 4) >> 31);
		}

		int previous_message_sequence = 0;
		int previous_server_reliable_sequence = 0;
		if (auto* clc = demo_game::clc_for(0))
		{
			previous_message_sequence = demo_game::read_i(clc,
				demo_game::CLC_SERVER_MESSAGE_SEQUENCE);
			previous_server_reliable_sequence = demo_game::read_i(clc,
				demo_game::CLC_SERVER_RELIABLE_SEQUENCE);
			demo_game::write_i(clc, demo_game::CLC_SERVER_MESSAGE_SEQUENCE,
				incoming_message_sequence);
			demo_game::write_i(clc, demo_game::CLC_SERVER_RELIABLE_SEQUENCE,
				server_reliable_sequence);

			// ⭐ THE REWIND DISCONNECT FIX, 2026-08-17.
			//
			// sub_606B0 (CL_AddReliableCommand) is Q3's client-command enqueue:
			//     if (reliableSequence - reliableAcknowledge > 128)  // MAX_RELIABLE_COMMANDS
			//         Com_Error(1, "%s", "EXE_DISCONNECTED...");
			//
			// The theater suppresses CL_WritePacket while armed, so the client NEVER
			// sends and its reliable ring is inert -- but ~20 engine functions
			// (CL_SetCGameTime and CL_ParseServerMessage_Internal among them) keep
			// ENQUEUEING, so reliableSequence climbs all session. Meanwhile we write
			// reliableAcknowledge from the RECORDED stream. On a rewind the stream
			// restarts, the recorded ack jumps back to the demo's opening value, the
			// difference blows past 128, and the engine drops the connection -- which
			// is exactly the EXE_DISCONNECTED seen mid-rewind.
			//
			// ⚠ This gap could not form before the clc base was corrected earlier
			// today: the write landed in an unrelated object, so real
			// reliableAcknowledge was never touched. Fixing the base is what exposed
			// it, and it would have bitten any long playback eventually, not just a
			// rewind.
			//
			// Since nothing is ever transmitted, holding reliableSequence in step with
			// the acknowledge is lossless -- it is what a client with nothing
			// outstanding looks like -- and it makes the overflow unreachable.
			demo_game::write_i(clc, demo_game::CLC_RELIABLE_SEQUENCE,
				server_reliable_sequence);
		}

		g_demo_feeding = true;
		demo_game::call_CL_ParseServerMessage(0, &msg);
		g_demo_feeding = false;
		commit_live_message_state(0, msg, previous_message_sequence,
			previous_server_reliable_sequence, incoming_message_sequence);

		g_last_snap_time = demo_game::snap_server_time();
	}

	void process_predicted_data(const std::span<const std::uint8_t> buffer, loaded_demo& demo)
	{
		// 50 = legacy (no extra0/w*); 60 = full SavePredicted fields through w2.
		if (buffer.size() < 50)
		{
			return;
		}
		const auto id = static_cast<predicted_data_id>(buffer[0]);
		if (id != predicted_data_id::player)
		{
			return;
		}
		auto& slot = demo.predicted[demo.predicted_index % demo.predicted.size()];
		slot.cad = {};
		slot.cad.serverTime = read_pod<std::int32_t>(buffer.data() + 2);
		std::memcpy(&slot.cad.origin[0], buffer.data() + 6, 12);
		std::memcpy(&slot.cad.velocity[0], buffer.data() + 18, 12);
		slot.cad.bobA = read_pod<std::int32_t>(buffer.data() + 30);
		slot.cad.bobB = read_pod<std::int32_t>(buffer.data() + 34);
		std::memcpy(&slot.viewangles[0], buffer.data() + 38, 12);
		slot.cad.viewangles[0] = slot.viewangles[0];
		slot.cad.viewangles[1] = slot.viewangles[1];
		slot.cad.viewangles[2] = slot.viewangles[2];
		if (buffer.size() >= 60)
		{
			slot.cad.extra0 = read_pod<std::int32_t>(buffer.data() + 50);
			slot.cad.w0 = read_pod<std::uint16_t>(buffer.data() + 54);
			slot.cad.w1 = read_pod<std::uint16_t>(buffer.data() + 56);
			slot.cad.w2 = read_pod<std::uint16_t>(buffer.data() + 58);
		}
		++demo.predicted_index;
	}

	bool read_chunk(std::ifstream& file, std::vector<std::uint8_t>& buf, bool one_byte)
	{
		buf.clear();
		std::uint32_t size = 0;
		file.read(reinterpret_cast<char*>(&size), one_byte ? 1 : 4);
		if (!file)
		{
			return false;
		}
		if (one_byte)
		{
			size = static_cast<std::uint8_t>(size);
		}
		if (size > MAX_SIZE)
		{
			return false;
		}
		buf.resize(size);
		if (size)
		{
			file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
		}
		return static_cast<bool>(file);
	}

	bool load_demo_file(const std::filesystem::path& path, loaded_demo& out)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			return false;
		}

		out = {};
		std::vector<std::uint8_t> buf;
		while (file)
		{
			std::uint8_t id_byte = 0;
			file.read(reinterpret_cast<char*>(&id_byte), 1);
			if (!file)
			{
				break;
			}
			const bool one_byte = (id_byte & static_cast<std::uint8_t>(demo_data_id::one_byte_flag)) != 0;
			const auto id = static_cast<demo_data_id>(id_byte & ~static_cast<std::uint8_t>(demo_data_id::flags));
			if (id == demo_data_id::eof)
			{
				break;
			}
			if (!read_chunk(file, buf, one_byte))
			{
				break;
			}

			switch (id)
			{
			case demo_data_id::map_header:
			{
				const std::string_view s(reinterpret_cast<const char*>(buf.data()), buf.size());
				const auto nul = s.find('\0');
				out.map = std::string(s.substr(0, nul));
				if (nul != std::string_view::npos && nul + 1 < s.size())
				{
					const auto gt = s.substr(nul + 1);
					const auto nul2 = gt.find('\0');
					out.gametype = std::string(gt.substr(0, nul2));
				}
				out.post_map_header_offset = static_cast<std::size_t>(file.tellg());
				break;
			}
			case demo_data_id::network_data:
				out.network.insert(out.network.end(), buf.begin(), buf.end());
				break;
			case demo_data_id::predicted_data:
				process_predicted_data(buf, out);
				break;
			case demo_data_id::update_gamestate_data:
				process_gamestate_data(buf, out.gamestate);
				break;
			case demo_data_id::gen_footer:
				if (buf.size() >= 8)
				{
					const auto first = read_pod<std::int32_t>(buf.data());
					const auto last = read_pod<std::int32_t>(buf.data() + 4);
					out.time_bounds = std::make_pair(first, last);
				}
				break;
			default:
				break;
			}
		}
		return !out.map.empty();
	}
}
