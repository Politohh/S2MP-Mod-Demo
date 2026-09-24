#include "pch.h"
#include "bonecam.hpp"

#include "demo/demo_game.hpp"
#include "demo/demo_native.hpp"
#include "demo/demo_playback.hpp"
#include "demo/theater_camera.hpp"

#include "Console.hpp"
#include "GameUtil.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

// =============================================================================
//  HOW THE BONE POSITION IS OBTAINED — every step read out of the engine
// =============================================================================
//
// The recipe is CG_GetWorldTagPos's own (sub_45540 @ IDA 0x45540), which is the
// engine's function for exactly this. Transcribed:
//
//     dobjIndex = word_AC4EAD0[entnum + 2113 * localClient]     0 = no DObj
//     dobj      = qword_AC4EAB8 + 480 * dobjIndex
//     mat       = sub_3E8BC0(centity, dobj, boneIndex)
//     worldPos  = { mat[+16], mat[+20], mat[+24] } + qword_8AFCB38[+108/112/116]
//
// sub_3E8BC0 is the by-INDEX world bone matrix getter. Its tail is
//     result = sub_7BF2C0(dobj);  if (result) result += 32 * boneIndex;
// i.e. the DObjAnimMat array (32 bytes/bone: quat[4] then trans[3] then weight),
// which independently confirms BOTH the 32-byte stride and that +16 is the
// translation. Everything before that tail is lazy bone evaluation, so the call
// also guarantees the bone is posed for this frame.
//
// ⭐ BY INDEX, NOT BY NAME. sub_3E8D60 is the by-name wrapper and is just
//     DObjGetBoneIndex(dobj, tag, &idx) ? sub_3E8BC0(cent, dobj, idx) : 0
// so driving the camera by index needs no string interning at all — which also
// keeps us away from SL_GetCanonicalString, an Arxan JUMPOUT thunk (RULE A5).
// Names are resolved separately, for the UI only, and cannot break the camera.
//
// ⚠ qword_8AFCB38 is the SAME renderer global demo_game::cg_globals_for derives
// cg from (0x8AFCB38 - 0x1000 == 0x8AFBB38). Its +108/112/116 float triple is a
// world offset the engine adds to every tag position; omitting it puts the
// camera in the wrong place on maps that use one.
// =============================================================================

namespace bonecam
{
	namespace
	{
		constexpr int LOCAL_CLIENT = 0;

		// RULE A1 — arithmetic written out, never done in the head.
		//   CG_GetEntity   IDA 0x13A00   - 0x1000 = 0x12A00
		//   sub_3E8BC0     IDA 0x3E8BC0  - 0x1000 = 0x3E7BC0
		//   SL_ConvertToString IDA 0x688C90 - 0x1000 = 0x687C90
		//   word_AC4EAD0   IDA 0xAC4EAD0 - 0x1000 = 0xAC3AD0
		//   qword_AC4EAB8  IDA 0xAC4EAB8 - 0x1000 = 0xAC3AB8
		//   qword_8AFCB38  IDA 0x8AFCB38 - 0x1000 = 0x8AFBB38
		constexpr std::size_t ADDR_CG_GET_ENTITY = 0x12A00;
		constexpr std::size_t ADDR_WORLD_BONE_MAT = 0x3E7BC0;
		constexpr std::size_t ADDR_SL_CONVERT_TO_STRING = 0x687C90;
		constexpr std::size_t ADDR_ENT_TO_DOBJ = 0xAC3AD0;
		constexpr std::size_t ADDR_DOBJ_ARRAY = 0xAC3AB8;
		constexpr std::size_t ADDR_RENDERER_DATA = 0x8AFBB38;

		constexpr std::size_t ENTS_PER_CLIENT = 2113;   // from sub_45540's own index
		constexpr std::size_t DOBJ_STRIDE = 480;
		constexpr std::size_t BONE_MAT_TRANS = 16;      // DObjAnimMat: quat[4], trans[3]
		constexpr std::size_t WORLD_OFFSET = 108;       // renderer data + 108/112/116

		// DObj fields used only for the NAME table (a UI nicety). Read-only and
		// fully guarded: a wrong offset yields a rejected pointer or a
		// non-printable string, both of which degrade to "bone N".
		constexpr std::size_t DOBJ_NUM_MODELS = 16;     // u8
		constexpr std::size_t DOBJ_NUM_BONES = 18;      // u16
		constexpr std::size_t DOBJ_MODELS = 368;        // ptr to model array
		constexpr std::size_t XMODEL_NUM_BONES = 8;     // u16
		constexpr std::size_t XMODEL_BONE_NAMES = 88;   // scr_string_t*

		std::atomic<bool> g_enabled{false};
		std::atomic<int> g_bone{-1};
		std::atomic<int> g_entity{-1};                  // -1 = the demo's own client
		std::atomic<float> g_off_f{0.0f}, g_off_r{0.0f}, g_off_u{0.0f};
		std::atomic<float> g_smooth{0.0f};

		std::mutex g_lock;
		std::vector<bone_entry> g_bones;                // guarded by g_lock
		std::string g_bone_name = "<none>";             // guarded by g_lock
		int g_bones_for_ent = -2;
		int g_bones_count = -1;

		// last applied position, for smoothing
		bool g_have_last = false;
		float g_last[3]{};
		std::atomic<std::uint64_t> g_applied{0};
		std::atomic<int> g_last_fail{0};                // why apply() bailed

		enum fail_reason
		{
			FAIL_NONE = 0, FAIL_DISABLED, FAIL_NO_DEMO, FAIL_NOT_FREECAM,
			FAIL_NO_CG, FAIL_NO_DOBJ, FAIL_NO_MAT, FAIL_BAD_POS, FAIL_NO_BONE,
		};

		// ---- bone-list diagnostics -------------------------------------------
		// "No bone list" has SIX possible causes; reporting the one that actually
		// fired is the difference between a fix and another guess (RULE A15).
		enum list_state
		{
			LIST_NEVER_RAN = 0, LIST_NO_DEMO, LIST_NO_CG, LIST_BAD_ENT,
			LIST_NO_DOBJ, LIST_BAD_COUNT, LIST_OK,
		};
		std::atomic<int> g_list_state{LIST_NEVER_RAN};
		std::atomic<std::uint64_t> g_ticks{0};
		std::atomic<int> g_dbg_ent{-1};
		std::atomic<std::uintptr_t> g_dbg_dobj{0};
		std::atomic<int> g_dbg_dobj_idx{-1};
		std::atomic<int> g_dbg_count{-1};
		std::atomic<bool> g_dbg_named{false};

		// RULE A6 — a global belonging to an idle subsystem holds junk, not zero.
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

		// The entity number to ride. Players use entity index == client number,
		// so the demo's own client (cg + 22904) is the default.
		[[nodiscard]] int resolve_entity(void* cg)
		{
			const int forced = g_entity.load(std::memory_order_relaxed);
			if (forced >= 0)
			{
				return forced;
			}
			const auto* slot = reinterpret_cast<const std::uint8_t*>(
				static_cast<char*>(cg) + 22904);
			return readable(slot, 1) ? static_cast<int>(*slot) : -1;
		}

		// dobj = qword_AC4EAB8 + 480 * word_AC4EAD0[ent + 2113*client].
		// A zero index means the entity has no DObj this frame (not drawn), which
		// is a normal state, not an error.
		[[nodiscard]] void* dobj_for(const int entnum)
		{
			if (entnum < 0 || static_cast<std::size_t>(entnum) >= ENTS_PER_CLIENT)
			{
				return nullptr;
			}
			const auto* table = reinterpret_cast<const std::uint16_t*>(_b(ADDR_ENT_TO_DOBJ));
			const std::size_t slot = static_cast<std::size_t>(entnum)
				+ ENTS_PER_CLIENT * static_cast<std::size_t>(LOCAL_CLIENT);
			if (!readable(table + slot, sizeof(std::uint16_t)))
			{
				return nullptr;
			}
			const std::uint16_t idx = table[slot];
			g_dbg_dobj_idx.store(static_cast<int>(idx), std::memory_order_relaxed);
			if (!idx)
			{
				return nullptr;      // no DObj for that entity this frame
			}
			const auto base = *reinterpret_cast<const std::uintptr_t*>(_b(ADDR_DOBJ_ARRAY));
			if (!base)
			{
				return nullptr;
			}
			auto* dobj = reinterpret_cast<void*>(base + DOBJ_STRIDE * idx);
			if (!readable(dobj, DOBJ_MODELS + 8))
			{
				return nullptr;
			}
			g_dbg_dobj.store(reinterpret_cast<std::uintptr_t>(dobj), std::memory_order_relaxed);
			return dobj;
		}

		// The engine's own world offset, added to every tag position.
		void add_world_offset(float* p)
		{
			const auto rd = *reinterpret_cast<const std::uintptr_t*>(_b(ADDR_RENDERER_DATA));
			const auto* off = reinterpret_cast<const float*>(rd + WORLD_OFFSET);
			if (rd && readable(off, 12))
			{
				p[0] += off[0];
				p[1] += off[1];
				p[2] += off[2];
			}
		}

		// Standard CoD AngleVectors, implemented locally rather than calling the
		// engine's — it costs nothing and avoids depending on another signature.
		void angle_vectors(const float* ang, float* fwd, float* right, float* up)
		{
			constexpr float k = 0.01745329252f;   // pi/180
			const float sp = std::sin(ang[0] * k), cp = std::cos(ang[0] * k);
			const float sy = std::sin(ang[1] * k), cy = std::cos(ang[1] * k);
			const float sr = std::sin(ang[2] * k), cr = std::cos(ang[2] * k);
			fwd[0] = cp * cy;  fwd[1] = cp * sy;  fwd[2] = -sp;
			right[0] = -sr * sp * cy + cr * sy;
			right[1] = -sr * sp * sy - cr * cy;
			right[2] = -sr * cp;
			up[0] = cr * sp * cy + sr * sy;
			up[1] = cr * sp * sy - sr * cy;
			up[2] = cr * cp;
		}

		// Bone names, for the UI only. INFERRED XModel layout, so every step is
		// guarded and any failure falls back to "bone N" — it can mislabel, never
		// crash, and the camera itself never depends on it.
		void rebuild_bone_list(void* dobj, const int entnum)
		{
			const auto* nbones = reinterpret_cast<const std::uint16_t*>(
				static_cast<char*>(dobj) + DOBJ_NUM_BONES);
			if (!readable(nbones, 2))
			{
				return;
			}
			const int count = static_cast<int>(*nbones);
			g_dbg_count.store(count, std::memory_order_relaxed);
			if (count <= 0 || count > 1024)
			{
				g_list_state.store(LIST_BAD_COUNT, std::memory_order_relaxed);
				return;
			}
			g_list_state.store(LIST_OK, std::memory_order_relaxed);
			{
				std::lock_guard<std::mutex> lock(g_lock);
				if (g_bones_for_ent == entnum && g_bones_count == count)
				{
					return;                       // same model, nothing to redo
				}
			}

			std::vector<bone_entry> out;
			out.reserve(static_cast<std::size_t>(count));

			// Try the model's own scr_string_t bone-name table.
			const std::uint32_t* names = nullptr;
			const auto models = *reinterpret_cast<const std::uintptr_t*>(
				static_cast<char*>(dobj) + DOBJ_MODELS);
			if (models && readable(reinterpret_cast<const void*>(models), 8))
			{
				const auto model0 = *reinterpret_cast<const std::uintptr_t*>(models);
				if (model0 && readable(reinterpret_cast<const void*>(model0 + 8), 8))
				{
					const auto xmodel = *reinterpret_cast<const std::uintptr_t*>(model0 + 8);
					if (xmodel && readable(reinterpret_cast<const void*>(
						xmodel + XMODEL_BONE_NAMES), 8))
					{
						const auto tbl = *reinterpret_cast<const std::uintptr_t*>(
							xmodel + XMODEL_BONE_NAMES);
						if (tbl && readable(reinterpret_cast<const void*>(tbl),
							sizeof(std::uint32_t) * static_cast<std::size_t>(count)))
						{
							names = reinterpret_cast<const std::uint32_t*>(tbl);
						}
					}
				}
			}

			using SL_ConvertToString_fn = const char* (__fastcall*)(int);
			const auto sl = reinterpret_cast<SL_ConvertToString_fn>(_b(ADDR_SL_CONVERT_TO_STRING));

			for (int i = 0; i < count; ++i)
			{
				bone_entry e{};
				e.index = i;
				bool named = false;
				if (names)
				{
					const char* s = sl(static_cast<int>(names[i]));
					if (s && readable(s, 2))
					{
						// Accept only clean identifier-ish text, so a wrong offset
						// shows as "bone N" rather than convincing garbage.
						std::size_t n = 0;
						while (n < 63 && s[n] >= 0x20 && s[n] < 0x7F) ++n;
						if (n >= 2 && s[n] == '\0')
						{
							e.name.assign(s, n);
							named = true;
						}
					}
				}
				if (!named)
				{
					e.name = std::format("bone {}", i);
				}
				out.push_back(std::move(e));
			}

			g_dbg_named.store(names != nullptr, std::memory_order_relaxed);
			std::lock_guard<std::mutex> lock(g_lock);
			g_bones = std::move(out);
			g_bones_for_ent = entnum;
			g_bones_count = count;
			const int b = g_bone.load(std::memory_order_relaxed);
			g_bone_name = (b >= 0 && b < static_cast<int>(g_bones.size()))
				? g_bones[static_cast<std::size_t>(b)].name : "<none>";
		}

		void cmd_bonecam()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2)
			{
				const char* a = args->argv[args->nesting][1];
				set_enabled(!(a[0] == '0' && a[1] == '\0'));
			}
			else
			{
				set_enabled(!g_enabled.load(std::memory_order_relaxed));
			}
			Console::printf("%s", status().c_str());
		}

		void cmd_bone()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[bonecam] bone = %d (%s). Usage: bonecam_bone "
					"<index|name>   (bonecam_list to see them)",
					g_bone.load(std::memory_order_relaxed), bone_name());
				return;
			}
			const char* a = args->argv[args->nesting][1];
			char* end = nullptr;
			const long v = std::strtol(a, &end, 10);
			if (end && end != a && *end == '\0')
			{
				set_bone_index(static_cast<int>(v));
			}
			else if (!set_bone_by_name(a))
			{
				Console::printf("[bonecam] no bone matching '%s' on this model "
					"(bonecam_list to see them)", a);
				return;
			}
			Console::printf("[bonecam] bone = %d (%s)",
				g_bone.load(std::memory_order_relaxed), bone_name());
		}

		void cmd_list()
		{
			const auto list = bones();
			if (list.empty())
			{
				// RULE A15 — name the cause instead of restating the symptom.
				const char* why = "?";
				switch (g_list_state.load(std::memory_order_relaxed))
				{
				case LIST_NEVER_RAN:
					why = "the per-frame tick has NEVER RUN — demo_native's "
						  "CG_PublishHudModel call-out is not reaching us";
					break;
				case LIST_NO_DEMO:  why = "no demo is playing"; break;
				case LIST_NO_CG:    why = "cg is not resolvable yet"; break;
				case LIST_BAD_ENT:  why = "could not resolve which entity to attach to"; break;
				case LIST_NO_DOBJ:
					why = "that entity has NO DObj right now (dobjIndex 0) — the "
						  "player model is not being drawn. Try FREE camera, or "
						  "bonecam_ent <client> for a player you can see";
					break;
				case LIST_BAD_COUNT: why = "the model reported an implausible bone count"; break;
				default: why = "list built but empty"; break;
				}
				Console::printf("[bonecam] no bone list: %s", why);
				Console::printf("[bonecam]   ticks=%llu entity=%d dobjIndex=%d dobj=%p "
					"boneCount=%d names=%s",
					static_cast<unsigned long long>(g_ticks.load(std::memory_order_relaxed)),
					g_dbg_ent.load(std::memory_order_relaxed),
					g_dbg_dobj_idx.load(std::memory_order_relaxed),
					reinterpret_cast<void*>(g_dbg_dobj.load(std::memory_order_relaxed)),
					g_dbg_count.load(std::memory_order_relaxed),
					g_dbg_named.load(std::memory_order_relaxed) ? "resolved" : "unresolved");
				return;
			}
			Console::printf("[bonecam] %zu bones on the attached model:", list.size());
			std::string line;
			for (std::size_t i = 0; i < list.size(); ++i)
			{
				line += std::format("{}:{}  ", list[i].index, list[i].name);
				if ((i % 6) == 5)
				{
					Console::printf("   %s", line.c_str());
					line.clear();
				}
			}
			if (!line.empty())
			{
				Console::printf("   %s", line.c_str());
			}
		}

		void cmd_offset()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 4)
			{
				set_offset(std::strtof(args->argv[args->nesting][1], nullptr),
					std::strtof(args->argv[args->nesting][2], nullptr),
					std::strtof(args->argv[args->nesting][3], nullptr));
			}
			float f, r, u;
			get_offset(f, r, u);
			Console::printf("[bonecam] offset fwd=%.1f right=%.1f up=%.1f "
				"(relative to where you are looking)", f, r, u);
		}

		void cmd_smooth()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2)
			{
				set_smoothing(std::strtof(args->argv[args->nesting][1], nullptr));
			}
			Console::printf("[bonecam] smoothing = %.2f (0 = rigid, follows every "
				"animation jolt; higher = heavier damping)", smoothing());
		}

		void cmd_ent()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (args && args->argc[args->nesting] >= 2)
			{
				set_entity(std::atoi(args->argv[args->nesting][1]));
			}
			const int e = g_entity.load(std::memory_order_relaxed);
			Console::printf("[bonecam] attached to %s",
				e < 0 ? "the demo's own player (default)"
				      : std::format("entity/client {}", e).c_str());
		}
	}

	void init()
	{
		GameUtil::addCommand("bonecam", cmd_bonecam);
		GameUtil::addCommand("bonecam_bone", cmd_bone);
		GameUtil::addCommand("bonecam_list", cmd_list);
		GameUtil::addCommand("bonecam_offset", cmd_offset);
		GameUtil::addCommand("bonecam_smooth", cmd_smooth);
		GameUtil::addCommand("bonecam_ent", cmd_ent);
		Console::printf("[bonecam] ready — lock the free camera to a player bone "
			"(bonecam, bonecam_list, bonecam_bone <index|name>)");
	}

	// Once per frame, client thread, cg valid. Keeps the bone list current for
	// the UI *regardless* of whether the lock is on or the camera is free.
	//
	// ⭐ WHY THIS IS SEPARATE FROM apply(). The list used to be built inside
	// apply(), which meant it only appeared once the lock was already enabled AND
	// the camera was in free mode AND the engine's mover was actually running.
	// That is backwards — you need the list in order to CHOOSE a bone — and it
	// produced nothing at all wherever the mover does not run.
	void tick()
	{
		g_ticks.fetch_add(1, std::memory_order_relaxed);

		// Throttle: this walks pointers and calls VirtualQuery, and the model
		// only changes when the player does. ~4 Hz is far more than enough.
		static unsigned int n = 0;
		if ((n++ & 15u) != 0)
		{
			return;
		}

		if (!demo_native::native_playing() && !demo_playback::is_playing())
		{
			g_list_state.store(LIST_NO_DEMO, std::memory_order_relaxed);
			return;
		}
		void* cg = demo_game::cg_globals_for(LOCAL_CLIENT);
		if (!cg)
		{
			g_list_state.store(LIST_NO_CG, std::memory_order_relaxed);
			return;
		}
		const int entnum = resolve_entity(cg);
		g_dbg_ent.store(entnum, std::memory_order_relaxed);
		if (entnum < 0)
		{
			g_list_state.store(LIST_BAD_ENT, std::memory_order_relaxed);
			return;
		}
		void* dobj = dobj_for(entnum);
		if (!dobj)
		{
			g_list_state.store(LIST_NO_DOBJ, std::memory_order_relaxed);
			return;
		}
		rebuild_bone_list(dobj, entnum);
	}

	// Runs on the client thread from dolly's CL_Demo_FreeCameraMove stub, AFTER
	// the engine's mover. The mover has just written the freecam angles from your
	// usercmd, so overwriting only the origin keeps mouse look intact.
	void apply()
	{
		if (!g_enabled.load(std::memory_order_relaxed))
		{
			g_last_fail.store(FAIL_DISABLED, std::memory_order_relaxed);
			return;
		}
		const bool native = demo_native::native_playing();
		if (!native && !demo_playback::is_playing())
		{
			g_last_fail.store(FAIL_NO_DEMO, std::memory_order_relaxed);
			return;
		}
		// Only the free camera routes through the mover we are riding.
		if (theater_camera::get_mode() != theater_camera::THEATER_CAMERA_FREECAM)
		{
			g_last_fail.store(FAIL_NOT_FREECAM, std::memory_order_relaxed);
			return;
		}
		// RULE A21: our own validated cg, never one the engine resolves for us.
		void* cg = demo_game::cg_globals_for(LOCAL_CLIENT);
		if (!cg || (native && (!demo_native::cgame_active()
			|| demo_native::seek_in_progress())))
		{
			g_last_fail.store(FAIL_NO_CG, std::memory_order_relaxed);
			return;
		}
		auto* freecam = reinterpret_cast<float*>(static_cast<char*>(cg) + 2355648);
		if (!readable(freecam, 36))
		{
			g_last_fail.store(FAIL_NO_CG, std::memory_order_relaxed);
			return;
		}

		const int entnum = resolve_entity(cg);
		void* dobj = dobj_for(entnum);
		if (!dobj)
		{
			// Normal while the player is not being drawn; say so rather than
			// silently freezing the camera at its last spot.
			g_last_fail.store(FAIL_NO_DOBJ, std::memory_order_relaxed);
			return;
		}
		rebuild_bone_list(dobj, entnum);

		int bone = g_bone.load(std::memory_order_relaxed);
		if (bone < 0)
		{
			g_last_fail.store(FAIL_NO_BONE, std::memory_order_relaxed);
			return;
		}

		using CG_GetEntity_fn = void* (__fastcall*)(int, unsigned int);
		using WorldBoneMat_fn = std::uintptr_t (__fastcall*)(void*, void*, unsigned short);
		auto* cent = reinterpret_cast<CG_GetEntity_fn>(_b(ADDR_CG_GET_ENTITY))(
			LOCAL_CLIENT, static_cast<unsigned int>(entnum));
		if (!cent)
		{
			g_last_fail.store(FAIL_NO_DOBJ, std::memory_order_relaxed);
			return;
		}
		const auto mat = reinterpret_cast<WorldBoneMat_fn>(_b(ADDR_WORLD_BONE_MAT))(
			cent, dobj, static_cast<unsigned short>(bone));
		const auto* trans = reinterpret_cast<const float*>(mat + BONE_MAT_TRANS);
		if (!mat || !readable(trans, 12))
		{
			g_last_fail.store(FAIL_NO_MAT, std::memory_order_relaxed);
			return;
		}

		float pos[3] = { trans[0], trans[1], trans[2] };
		add_world_offset(pos);
		// A bone that has not been posed reads as an exact zero or a wild value;
		// either would fling the camera across the map.
		for (int i = 0; i < 3; ++i)
		{
			if (!std::isfinite(pos[i]) || std::fabs(pos[i]) > 1.0e6f)
			{
				g_last_fail.store(FAIL_BAD_POS, std::memory_order_relaxed);
				return;
			}
		}

		// Camera-relative offset, using the angles the mover just set from the
		// mouse — so "back 20" always means behind where YOU are looking.
		float f = g_off_f.load(std::memory_order_relaxed);
		float r = g_off_r.load(std::memory_order_relaxed);
		float u = g_off_u.load(std::memory_order_relaxed);
		if (f != 0.0f || r != 0.0f || u != 0.0f)
		{
			float fwd[3], rgt[3], upv[3];
			angle_vectors(freecam + 3, fwd, rgt, upv);
			for (int i = 0; i < 3; ++i)
			{
				pos[i] += fwd[i] * f + rgt[i] * r + upv[i] * u;
			}
		}

		// Exponential position damping. Reset on a big jump so a seek or a
		// respawn snaps instead of sliding across the level.
		const float s = std::clamp(g_smooth.load(std::memory_order_relaxed), 0.0f, 0.99f);
		if (s > 0.0f && g_have_last)
		{
			const float dx = pos[0] - g_last[0], dy = pos[1] - g_last[1], dz = pos[2] - g_last[2];
			if ((dx * dx + dy * dy + dz * dz) < (512.0f * 512.0f))
			{
				for (int i = 0; i < 3; ++i)
				{
					pos[i] = g_last[i] + (pos[i] - g_last[i]) * (1.0f - s);
				}
			}
		}
		g_last[0] = pos[0]; g_last[1] = pos[1]; g_last[2] = pos[2];
		g_have_last = true;

		freecam[0] = pos[0];
		freecam[1] = pos[1];
		freecam[2] = pos[2];
		// Zero the mover's velocity, or its integrator keeps adding motion on top
		// of a position we are pinning every frame and the camera fights itself.
		freecam[6] = 0.0f;
		freecam[7] = 0.0f;
		freecam[8] = 0.0f;

		g_applied.fetch_add(1, std::memory_order_relaxed);
		g_last_fail.store(FAIL_NONE, std::memory_order_relaxed);
	}

	bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

	void set_enabled(const bool on)
	{
		g_enabled.store(on, std::memory_order_relaxed);
		g_have_last = false;          // do not lerp in from a stale position
	}

	int bone_index() { return g_bone.load(std::memory_order_relaxed); }

	void set_bone_index(const int index)
	{
		g_bone.store(index, std::memory_order_relaxed);
		g_have_last = false;
		std::lock_guard<std::mutex> lock(g_lock);
		g_bone_name = (index >= 0 && index < static_cast<int>(g_bones.size()))
			? g_bones[static_cast<std::size_t>(index)].name : "<none>";
	}

	bool set_bone_by_name(const char* name)
	{
		if (!name || !*name)
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(g_lock);
		// exact (case-insensitive) first, then a substring match so "wrist" works
		for (int pass = 0; pass < 2; ++pass)
		{
			for (const auto& b : g_bones)
			{
				const bool hit = (pass == 0)
					? (_stricmp(b.name.c_str(), name) == 0)
					: (b.name.find(name) != std::string::npos);
				if (hit)
				{
					g_bone.store(b.index, std::memory_order_relaxed);
					g_bone_name = b.name;
					g_have_last = false;
					return true;
				}
			}
		}
		return false;
	}

	const char* bone_name()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_bone_name.c_str();
	}

	int entity() { return g_entity.load(std::memory_order_relaxed); }

	void set_entity(const int ent)
	{
		g_entity.store(ent, std::memory_order_relaxed);
		g_have_last = false;
		std::lock_guard<std::mutex> lock(g_lock);
		g_bones_for_ent = -2;         // force a rebuild for the new model
	}

	void get_offset(float& fwd, float& right, float& up)
	{
		fwd = g_off_f.load(std::memory_order_relaxed);
		right = g_off_r.load(std::memory_order_relaxed);
		up = g_off_u.load(std::memory_order_relaxed);
	}

	void set_offset(const float fwd, const float right, const float up)
	{
		g_off_f.store(fwd, std::memory_order_relaxed);
		g_off_r.store(right, std::memory_order_relaxed);
		g_off_u.store(up, std::memory_order_relaxed);
	}

	float smoothing() { return g_smooth.load(std::memory_order_relaxed); }

	void set_smoothing(const float value)
	{
		g_smooth.store(std::clamp(value, 0.0f, 0.99f), std::memory_order_relaxed);
	}

	std::vector<bone_entry> bones()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_bones;
	}

	// RULE A15 — report unconditionally, and name the reason it is not running.
	std::string status()
	{
		const char* why = "";
		switch (g_last_fail.load(std::memory_order_relaxed))
		{
		case FAIL_DISABLED:    why = "off"; break;
		case FAIL_NO_DEMO:     why = "no demo playing"; break;
		case FAIL_NOT_FREECAM: why = "camera is not FREE — switch to Freecam"; break;
		case FAIL_NO_CG:       why = "cgame not ready"; break;
		case FAIL_NO_DOBJ:     why = "that player has no model right now (not drawn?)"; break;
		case FAIL_NO_MAT:      why = "bone not found on this model"; break;
		case FAIL_BAD_POS:     why = "bone position not posed yet"; break;
		case FAIL_NO_BONE:     why = "no bone chosen (bonecam_list, bonecam_bone <n>)"; break;
		default:               why = "running"; break;
		}
		const int e = g_entity.load(std::memory_order_relaxed);
		float f, r, u;
		get_offset(f, r, u);
		return std::format(
			"[bonecam] {} | bone {} ({}) | {} | offset {:.0f}/{:.0f}/{:.0f} | smooth {:.2f} "
			"| frames applied {} | {}",
			g_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
			g_bone.load(std::memory_order_relaxed), bone_name(),
			e < 0 ? std::string("own player") : std::format("entity {}", e),
			f, r, u, smoothing(),
			g_applied.load(std::memory_order_relaxed), why);
	}
}
