#include "pch.h"
#include "DevMode.hpp"

#include "Console.hpp"
#include "GameUtil.hpp"
#include "ModPaths.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace dev_mode
{
	namespace
	{
		struct hook_t
		{
			void (*fn)() = nullptr;
			bool fired = false;
		};

		bool g_enabled = false;
		bool g_inited = false;
		std::vector<hook_t>& hooks()
		{
			// Function-local so it is constructed on first use. A namespace
			// scope vector would be at the mercy of static init order, and
			// on_enable() can be called from another translation unit's
			// init() (RULE A14, applied to a container rather than an
			// address).
			static std::vector<hook_t> v;
			return v;
		}

		std::string flag_path()
		{
			return mod_paths::mod_dir() + "/dev_mode";
		}

		void fire_pending()
		{
			// Index-based: a callback may itself call on_enable().
			for (std::size_t i = 0; i < hooks().size(); ++i)
			{
				if (hooks()[i].fired || hooks()[i].fn == nullptr)
				{
					continue;
				}
				hooks()[i].fired = true;
				hooks()[i].fn();
			}
		}

		void cmd_dev()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("developer mode: %s", g_enabled ? "ON" : "off");
				Console::printf("  s2_dev 1   show the engine/matchmaking tabs and "
					"all diagnostic commands");
				Console::printf("  s2_dev 0   back to the demo tools only");
				return;
			}
			set(std::atoi(args->argv[args->nesting][1]) != 0);
		}
	}

	void init()
	{
		if (g_inited)
		{
			return;
		}
		g_inited = true;

		if (FILE* f = nullptr; fopen_s(&f, flag_path().c_str(), "rb") == 0 && f)
		{
			int v = 0;
			if (std::fscanf(f, "%d", &v) == 1)
			{
				g_enabled = (v != 0);
			}
			std::fclose(f);
		}
		if (g_enabled)
		{
			fire_pending();
		}
	}

	bool enabled()
	{
		return g_enabled;
	}

	void set(bool on)
	{
		if (on == g_enabled)
		{
			Console::printf("developer mode already %s", on ? "ON" : "off");
			return;
		}
		g_enabled = on;

		if (FILE* f = nullptr; fopen_s(&f, flag_path().c_str(), "wb") == 0 && f)
		{
			std::fprintf(f, "%d\n", on ? 1 : 0);
			std::fclose(f);
		}

		if (on)
		{
			fire_pending();
			Console::printf("developer mode ON — extra tabs and diagnostic commands "
				"are now available.");
		}
		else
		{
			Console::printf("developer mode off — the window now shows the demo "
				"tools only. Commands already registered this session stay usable.");
		}
	}

	void on_enable(void (*fn)())
	{
		if (fn == nullptr)
		{
			return;
		}
		hooks().push_back({ fn, false });
		if (g_enabled)
		{
			hooks().back().fired = true;
			fn();
		}
	}

	void add_command(const char* name, void (*fn)())
	{
		// Held by value so the list survives whatever produced it. `name` is
		// always a string literal at every call site, but copying is free
		// here and removes the question.
		struct pending_t
		{
			std::string name;
			void (*fn)();
		};
		static std::vector<pending_t> pending;

		if (g_enabled)
		{
			GameUtil::addCommand(name, fn);
			return;
		}

		pending.push_back({ name, fn });

		// One flush hook for the whole list, installed on the first deferred
		// command. It drains `pending` so a command can never register twice.
		static bool flush_installed = false;
		if (!flush_installed)
		{
			flush_installed = true;
			on_enable([]
			{
				for (const auto& p : pending)
				{
					GameUtil::addCommand(p.name.c_str(), p.fn);
				}
				pending.clear();
			});
		}
	}

	void register_command()
	{
		GameUtil::addCommand("s2_dev", cmd_dev);
	}
}
