#include "pch.h"
#include "demo/demo_display.hpp"

#include "Console.hpp"
#include "GameUtil.hpp"
#include "ModPaths.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>

namespace demo_display
{
	namespace
	{
		// ---- addresses (RULE A1: the arithmetic is written out) ---------
		//
		//   com_maxfps dvar ptr    IDA 0x14DB7B0 - 0x1000 = 0x14DA7B0
		//
		// off_14DB7B0 is a POINTER to the dvar (Com_InitDvars assigns the
		// registrar's return value straight into it), same shape as
		// demo_camera's cg_fov pointer. Its current value is secure-encoded.
		//
		// RULE A14: the _b literal is resolved inside a function, never at
		// namespace scope.
		constexpr std::uintptr_t MAXFPS_DVAR_LITERAL = 0x14DA7B0;

		[[nodiscard]] std::uintptr_t maxfps_dvar_ptr_addr()
		{
			return _b(MAXFPS_DVAR_LITERAL);
		}

		// Duplicated rather than shared -- see demo_camera.cpp's note on why.
		bool readable(const void* p, const std::size_t n)
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
			const auto end = start + mbi.RegionSize;
			const auto want = reinterpret_cast<std::uintptr_t>(p);
			return want >= start && want + n <= end;
		}

		// Resolves the engine's secure-int dvar after Com_InitDvars.
		[[nodiscard]] dvar_t* resolve_dvar()
		{
			auto* slot = reinterpret_cast<std::uintptr_t*>(maxfps_dvar_ptr_addr());
			if (!readable(slot, sizeof(std::uintptr_t)))
			{
				return nullptr;
			}
			auto* dvar = reinterpret_cast<dvar_t*>(*slot);
			if (!readable(dvar, sizeof(dvar_t)) || dvar->type != DVAR_TYPE_INT_SECURE)
			{
				return nullptr;
			}
			return dvar;
		}

		// Registration gives com_maxfps a 0..250 domain. Widen the real
		// domain so both the engine console and the mod UI can use 1000.
		// Do this again if the game re-registers the dvar on a map change.
		bool ensure_domain()
		{
			auto* dvar = resolve_dvar();
			if (!dvar || dvar->domain.integer.min != 0)
			{
				return false;
			}
			if (dvar->domain.integer.max == 250)
			{
				dvar->domain.integer.max = 1000;
			}
			return dvar->domain.integer.max == 1000;
		}

		int g_target = -1;
		bool g_holding = false;
		ULONGLONG g_last_command_ms = 0;
		// Guards the ONE-TIME "apply whatever was saved last session" step
		// in tick() -- set the first time com_maxfps becomes resolvable,
		// whether or not a saved preference actually existed, so it is
		// never retried once answered.
		bool g_tried_persisted = false;

		[[nodiscard]] std::string prefs_path()
		{
			return mod_paths::mod_dir() + "/fps_cap.txt";
		}

		void save_target(const int fps)
		{
			if (FILE* f = nullptr; fopen_s(&f, prefs_path().c_str(), "wb") == 0 && f)
			{
				std::fprintf(f, "%d\n", fps);
				std::fclose(f);
			}
		}

		// -1 when there is no saved preference (or it is unreadable/nonsense).
		[[nodiscard]] int load_target()
		{
			FILE* f = nullptr;
			if (fopen_s(&f, prefs_path().c_str(), "rb") != 0 || !f)
			{
				return -1;
			}
			int v = -1;
			const bool ok = std::fscanf(f, "%d", &v) == 1;
			std::fclose(f);
			return (ok && v >= 0 && v <= 1000) ? v : -1;
		}

		void cmd_fps()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				const int v = fps_cap();
				if (v < 0)
				{
					Console::printf("fps cap: unavailable (com_maxfps not registered yet)");
				}
				else
				{
					Console::printf("fps cap: %d   (com_maxfps or demo_fps <0..1000>; "
						"0 = uncapped. demo_fps also saves the choice for future launches.)", v);
				}
				return;
			}
			set_fps_cap(std::atoi(args->argv[args->nesting][1]));
		}
	}

	// =====================================================================

	int fps_cap()
	{
		const auto* dvar = resolve_dvar();
		if (!dvar)
		{
			return -1;
		}
		const int v = GameUtil::getDvarSecureInt(dvar);
		return (v >= 0 && v <= 1000) ? v : -1;
	}

	bool fps_cap_available()
	{
		return fps_cap() >= 0;
	}

	void set_fps_cap(int fps)
	{
		fps = std::clamp(fps, 0, 1000);
		g_target = fps;
		g_holding = true;
		if (ensure_domain() &&
			GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("com_maxfps {}", fps)))
		{
			g_last_command_ms = GetTickCount64();
		}

		// This is now the standing preference -- every future launch should
		// come up already at this value, not just this session.
		save_target(fps);
	}

	void tick()
	{
		if (!ensure_domain())
		{
			return;
		}
		if (!g_holding)
		{
			// Nothing chosen yet THIS session. The moment the dvar becomes
			// resolvable (never true at init() -- Com_InitDvars has not run
			// yet), try once to pick up whatever was set last time, so the
			// cap is applied with no GUI interaction and no console command
			// required. If there is nothing saved, this simply never fires
			// again for the rest of the process.
			if (!g_tried_persisted && fps_cap_available())
			{
				g_tried_persisted = true;
				const int saved = load_target();
				if (saved >= 0)
				{
					set_fps_cap(saved);   // re-enters; sets g_holding and g_target
				}
			}
			return;
		}
		// The normal engine setter keeps the secure-int encoding valid.
		// Retry at most four times per second if an archived config or map
		// transition restores an older value.
		const auto now = GetTickCount64();
		if (fps_cap() != g_target && now - g_last_command_ms >= 250 &&
			GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, std::format("com_maxfps {}", g_target)))
		{
			g_last_command_ms = now;
		}
	}

	void init()
	{
		GameUtil::addCommand("demo_fps", cmd_fps);
	}
}
