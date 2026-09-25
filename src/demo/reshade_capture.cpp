#include "pch.h"
#include "demo/reshade_capture.hpp"
#include "demo/demo_capture.hpp"
#include "Console.hpp"

#include "third_party/reshade/reshade_events.hpp"

#include <atomic>
#include <d3d11.h>
#include <Windows.h>

extern "C" BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE*, DWORD, LPDWORD);
extern "C" __declspec(dllexport) const char* NAME = "S2MP ProRes Capture";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Records ReShade's finished frame into the S2MP ProRes capture.";

namespace reshade_capture
{
	namespace
	{
		std::atomic_bool g_active{};
		std::atomic_bool g_received_frame{};
		bool g_attempted = false;
		bool g_missing_logged = false;
		ULONGLONG g_last_probe = 0;
		ULONGLONG g_first_probe = 0;

		void on_finish(reshade::api::effect_runtime* runtime,
			reshade::api::command_list*, reshade::api::resource_view rtv,
			reshade::api::resource_view)
		{
			if (!demo_capture::recording() || !runtime || !rtv.handle) return;
			auto* device = runtime->get_device();
			if (!device || device->get_api() != reshade::api::device_api::d3d11) return;
			const auto resource = device->get_resource_from_view(rtv);
			if (!resource.handle) return;
			demo_capture::set_post_effect_capture(true);
			auto* texture = reinterpret_cast<ID3D11Texture2D*>(resource.handle);
			demo_capture::on_post_effect_texture(texture);
			if (!g_received_frame.exchange(true))
				Console::printf("[capture] first ReShade post-effects frame captured");
		}

		HMODULE find_reshade()
		{
			HMODULE modules[1024]{};
			DWORD bytes = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return nullptr;
			const auto count = (std::min)(static_cast<size_t>(bytes / sizeof(HMODULE)), std::size(modules));
			for (size_t i = 0; i < count; ++i)
				if (GetProcAddress(modules[i], "ReShadeRegisterAddon") &&
					GetProcAddress(modules[i], "ReShadeRegisterEventForAddon")) return modules[i];
			return nullptr;
		}
	}

	void tick()
	{
		if (g_active.load() || g_attempted) return;
		const auto now = GetTickCount64();
		if (!g_first_probe) g_first_probe = now;
		if (now - g_last_probe < 2000) return;
		g_last_probe = now;
		const auto reshade_module = find_reshade();
		if (!reshade_module)
		{
			if (!g_missing_logged && now - g_first_probe >= 5000)
			{
				g_missing_logged = true;
				Console::printf("[capture] ReShade add-on API not detected. To include effects, install the official full add-on support build from https://reshade.me/ and restart the game. Normal game-frame recording remains available.");
			}
			return;
		}
		g_attempted = true;

		HMODULE own_module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&tick), &own_module)) return;
		using register_addon_t = bool(*)(void*, uint32_t);
		using register_event_t = void(*)(void*, reshade::addon_event, void*);
		const auto register_addon = reinterpret_cast<register_addon_t>(
			GetProcAddress(reshade_module, "ReShadeRegisterAddon"));
		const auto register_event = reinterpret_cast<register_event_t>(
			GetProcAddress(reshade_module, "ReShadeRegisterEventForAddon"));
		// SDK API 20; unsupported ReShade builds simply keep normal capture.
		if (!register_addon || !register_event || !register_addon(own_module, 20))
		{
			Console::printf("[capture] ReShade found, but add-on API 20 registration was refused. Recording will omit ReShade effects; use a compatible add-on enabled ReShade build.");
			return;
		}
		register_event(own_module, reshade::addon_event::reshade_finish_effects,
			reinterpret_cast<void*>(&on_finish));
		g_active.store(true);
		Console::printf("[capture] ReShade finish-effects hook registered; waiting for a post-effects frame.");
	}

	bool active() { return g_active.load(); }
}
