#include "pch.h"
#include "demo_gui.hpp"

#include "demo/demo_game.hpp"
#include "demo/dolly.hpp"
#include "demo/demo_native.hpp"
#include "demo/demo_player.hpp"
#include "demo/demo_camera.hpp"
#include "demo/demo_capture.hpp"
#include "demo/demo_display.hpp"
#include "demo/demo_library.hpp"
#include "demo/demo_playback.hpp"
#include "demo/demo_recording.hpp"
#include "demo/demo_utils.hpp"
#include "demo/theater_camera.hpp"
#include "demo/bonecam.hpp"
#include "net/dlc.hpp"
#include "net/server_browser.hpp"
#include "net/bots.hpp"
#include "net/force_host.hpp"


#include "Console.hpp"
#include "DevMode.hpp"
#include "DvarInterface.hpp"
#include "FuncPointers.h"
#include "GameUtil.hpp"
#include "Hook.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace demo_gui
{
	namespace
	{

		bool g_open = false;

		// Result of the last "Fix Selected Demo" press, shown under the button.
		std::string g_native_fix_msg;
		bool g_native_fix_ok = false;
		bool g_timeline = true;
		bool g_imgui_ready = false;
		bool g_toggle_edge = false;
		bool g_f9_edge = false;
		bool g_insert_edge = false;
		bool g_space_edge = false;
		bool g_left_edge = false;
		bool g_right_edge = false;
		bool g_j_edge = false;
		bool g_k_edge = false;
		bool g_l_edge = false;

		// Wheel deltas tapped from wnd_proc_hook (WM_MOUSEWHEEL fires on whatever
		// thread pumps the game's window messages; demo_camera::on_wheel() is
		// called from there directly since it only ever touches its own small,
		// self-contained state -- no cross-thread hand-off needed, same as every
		// other direct-call editing function already used from this file.
		bool g_present_hooked = false;

		ID3D11Device* g_device = nullptr;
		ID3D11DeviceContext* g_context = nullptr;
		ID3D11RenderTargetView* g_rtv = nullptr;
		HWND g_hwnd = nullptr;
		WNDPROC g_wndproc_orig = nullptr;

		using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		Present_t g_present_orig = nullptr;
		void* g_present_target = nullptr;

		// IDXGISwapChain::ResizeBuffers MUST be hooked, not just Present.
		//
		// Our render target view holds a live reference to the swapchain's back
		// buffer. DXGI refuses to resize while any such reference exists -- it fails
		// with DXGI_ERROR_INVALID_CALL ("cannot resize, outstanding references") --
		// and the game calls ResizeBuffers whenever the window is minimised or
		// restored. That is the reported DirectX error on minimise.
		//
		// release_rtv() already existed for exactly this, but was DEAD CODE: defined
		// and never called from anywhere, so the RTV was created once and held for
		// the life of the process.
		using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(
			IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		ResizeBuffers_t g_resize_orig = nullptr;
		bool g_resize_hooked = false;

		// NOTE ON THREADING, which applies to this whole file:
		// everything here runs on the DXGI Present thread. Anything that touches
		// engine state -- seeking rewinds the demo ifstream and rewrites
		// connstate / clc sequences -- is handed to the client thread through the
		// command buffer rather than being called inline. That is why the buttons
		// issue console commands instead of calling demo_player directly.

		void release_rtv()
		{
			if (g_rtv)
			{
				g_rtv->Release();
				g_rtv = nullptr;
			}
		}

		void ensure_render_target(IDXGISwapChain* swap)
		{
			if (g_rtv)
			{
				return;
			}
			ID3D11Texture2D* back = nullptr;
			if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
			{
				return;
			}
			g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
			back->Release();
		}

		LRESULT CALLBACK wnd_proc_hook(HWND hwnd, const UINT msg, const WPARAM wparam, const LPARAM lparam)
		{
			if (g_imgui_ready && g_open)
			{
				if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
				{
					return 1;
				}
			}

			// Plain wheel = roll, Alt+wheel = FOV, but only while our own window
			// does not want the mouse (scrolling a list in the GUI must not also
			// roll the camera). demo_camera::on_wheel() self-gates on free-camera
			// mode and is safe to call from this thread -- see the note at the top
			// of this file. NEVER swallow the message: it always falls through to
			// CallWindowProcW so the game keeps seeing wheel input as normal.
			if (msg == WM_MOUSEWHEEL
				&& !(g_imgui_ready && g_open && ImGui::GetIO().WantCaptureMouse))
			{
				const float notches = static_cast<float>(static_cast<short>(HIWORD(wparam)))
					/ static_cast<float>(WHEEL_DELTA);
				const bool alt_held = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
				demo_camera::on_wheel(notches, alt_held);
			}

			return CallWindowProcW(g_wndproc_orig, hwnd, msg, wparam, lparam);
		}

		// =====================================================================
		//  THEME -- a field-dossier look: olive/charcoal ground, brass
		//  accents, parchment text. Applied once, to ImGui's own style
		//  table, so every existing widget picks it up with no call-site
		//  changes. The semantic status colours already scattered through
		//  the tabs (green = good, amber = warning, red = danger) are left
		//  untouched on purpose -- they already read as ops-panel status
		//  lights and fit this palette as-is.
		// =====================================================================
		void apply_wwii_theme()
		{
			ImGuiStyle& style = ImGui::GetStyle();
			ImVec4* c = style.Colors;

			const ImVec4 bg_dark      (0.086f, 0.090f, 0.071f, 1.00f);
			const ImVec4 bg_panel     (0.114f, 0.122f, 0.094f, 1.00f);
			const ImVec4 bg_panel_hi  (0.153f, 0.161f, 0.118f, 1.00f);
			const ImVec4 bg_panel_act (0.184f, 0.161f, 0.094f, 1.00f);
			const ImVec4 brass        (0.706f, 0.541f, 0.267f, 1.00f);
			const ImVec4 brass_dim    (0.549f, 0.416f, 0.220f, 0.55f);
			const ImVec4 brass_bright (0.827f, 0.667f, 0.353f, 1.00f);
			const ImVec4 olive_btn    (0.267f, 0.290f, 0.180f, 1.00f);
			const ImVec4 olive_btn_hi (0.345f, 0.373f, 0.235f, 1.00f);
			const ImVec4 olive_btn_act(0.427f, 0.365f, 0.180f, 1.00f);
			const ImVec4 text_cream   (0.878f, 0.839f, 0.729f, 1.00f);
			const ImVec4 text_dim     (0.549f, 0.541f, 0.463f, 1.00f);

			c[ImGuiCol_Text]                      = text_cream;
			c[ImGuiCol_TextDisabled]              = text_dim;
			c[ImGuiCol_WindowBg]                  = bg_dark;
			c[ImGuiCol_ChildBg]                   = ImVec4(bg_panel.x, bg_panel.y, bg_panel.z, 0.55f);
			c[ImGuiCol_PopupBg]                   = ImVec4(bg_dark.x, bg_dark.y, bg_dark.z, 0.98f);
			c[ImGuiCol_Border]                    = brass_dim;
			c[ImGuiCol_BorderShadow]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
			c[ImGuiCol_FrameBg]                   = bg_panel;
			c[ImGuiCol_FrameBgHovered]            = bg_panel_hi;
			c[ImGuiCol_FrameBgActive]             = bg_panel_act;
			c[ImGuiCol_TitleBg]                   = bg_dark;
			c[ImGuiCol_TitleBgActive]             = ImVec4(0.161f, 0.145f, 0.094f, 1.00f);
			c[ImGuiCol_TitleBgCollapsed]          = bg_dark;
			c[ImGuiCol_MenuBarBg]                 = bg_panel;
			c[ImGuiCol_ScrollbarBg]               = bg_dark;
			c[ImGuiCol_ScrollbarGrab]             = olive_btn;
			c[ImGuiCol_ScrollbarGrabHovered]      = olive_btn_hi;
			c[ImGuiCol_ScrollbarGrabActive]       = brass;
			c[ImGuiCol_CheckMark]                 = brass_bright;
			c[ImGuiCol_SliderGrab]                = brass;
			c[ImGuiCol_SliderGrabActive]          = brass_bright;
			c[ImGuiCol_Button]                    = olive_btn;
			c[ImGuiCol_ButtonHovered]             = olive_btn_hi;
			c[ImGuiCol_ButtonActive]              = olive_btn_act;
			c[ImGuiCol_Header]                    = ImVec4(olive_btn.x, olive_btn.y, olive_btn.z, 0.80f);
			c[ImGuiCol_HeaderHovered]             = ImVec4(olive_btn_hi.x, olive_btn_hi.y, olive_btn_hi.z, 0.90f);
			c[ImGuiCol_HeaderActive]              = olive_btn_act;
			c[ImGuiCol_Separator]                 = brass_dim;
			c[ImGuiCol_SeparatorHovered]          = brass;
			c[ImGuiCol_SeparatorActive]           = brass_bright;
			c[ImGuiCol_ResizeGrip]                = ImVec4(brass.x, brass.y, brass.z, 0.30f);
			c[ImGuiCol_ResizeGripHovered]          = ImVec4(brass.x, brass.y, brass.z, 0.65f);
			c[ImGuiCol_ResizeGripActive]           = brass_bright;
			c[ImGuiCol_Tab]                        = bg_panel;
			c[ImGuiCol_TabHovered]                 = olive_btn_hi;
			c[ImGuiCol_TabSelected]                = bg_panel_act;
			c[ImGuiCol_TabSelectedOverline]        = brass_bright;
			c[ImGuiCol_TabDimmed]                  = ImVec4(bg_panel.x, bg_panel.y, bg_panel.z, 0.70f);
			c[ImGuiCol_TabDimmedSelected]          = bg_panel_act;
			c[ImGuiCol_TabDimmedSelectedOverline]  = brass_dim;
			c[ImGuiCol_PlotLines]                  = brass;
			c[ImGuiCol_PlotLinesHovered]           = brass_bright;
			c[ImGuiCol_PlotHistogram]              = brass;
			c[ImGuiCol_PlotHistogramHovered]       = brass_bright;
			c[ImGuiCol_TableHeaderBg]              = ImVec4(olive_btn.x, olive_btn.y, olive_btn.z, 0.85f);
			c[ImGuiCol_TableBorderStrong]          = brass_dim;
			c[ImGuiCol_TableBorderLight]           = ImVec4(brass_dim.x, brass_dim.y, brass_dim.z, 0.35f);
			c[ImGuiCol_TableRowBg]                 = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
			c[ImGuiCol_TableRowBgAlt]              = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
			c[ImGuiCol_TextSelectedBg]             = ImVec4(brass.x, brass.y, brass.z, 0.35f);
			c[ImGuiCol_DragDropTarget]             = brass_bright;
			c[ImGuiCol_NavHighlight]               = brass_bright;
			c[ImGuiCol_NavWindowingHighlight]      = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
			c[ImGuiCol_NavWindowingDimBg]          = ImVec4(0.2f, 0.2f, 0.2f, 0.20f);
			c[ImGuiCol_ModalWindowDimBg]           = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);

			// Sharp-edged stamped-plate shapes: a little rounding to soften
			// pixel edges without going soft/bubbly, and a visible brass
			// border everywhere a panel meets another.
			style.WindowRounding    = 2.0f;
			style.ChildRounding     = 2.0f;
			style.FrameRounding     = 2.0f;
			style.PopupRounding     = 2.0f;
			style.ScrollbarRounding = 3.0f;
			style.GrabRounding      = 2.0f;
			style.TabRounding       = 2.0f;
			style.WindowBorderSize  = 1.0f;
			style.ChildBorderSize   = 1.0f;
			style.PopupBorderSize   = 1.0f;
			style.FrameBorderSize   = 1.0f;
			style.TabBarBorderSize  = 1.5f;
			style.WindowPadding     = ImVec2(12.0f, 12.0f);
			style.FramePadding      = ImVec2(8.0f, 5.0f);
			style.ItemSpacing       = ImVec2(8.0f, 6.0f);
			style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);
			style.IndentSpacing     = 18.0f;
			style.ScrollbarSize     = 14.0f;
			style.GrabMinSize       = 10.0f;
			style.CellPadding       = ImVec2(6.0f, 4.0f);
		}

		// A second, bold weight for the one banner line drawn in draw_ui().
		// Deliberately not threaded through the ~40 SeparatorText call sites
		// elsewhere in this file -- one accent spot, not a font swap on
		// every heading.
		ImFont* g_font_bold = nullptr;

		[[nodiscard]] bool file_exists(const char* path)
		{
			const DWORD attr = GetFileAttributesA(path);
			return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
		}

		// Windows ships Segoe UI on every install this mod targets, so this
		// is a real upgrade over ImGui's built-in pixel font with no asset
		// to bundle. Falls back to the built-in font (still fully usable)
		// if the system font is somehow missing -- this must never be able
		// to leave the atlas with zero fonts.
		void load_fonts()
		{
			ImGuiIO& io = ImGui::GetIO();
			ImFontConfig cfg{};
			cfg.OversampleH = 3;
			cfg.OversampleV = 1;
			cfg.PixelSnapH = true;

			ImFont* body = nullptr;
			if (file_exists("C:\\Windows\\Fonts\\segoeui.ttf"))
			{
				body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, &cfg);
			}
			if (!body)
			{
				io.Fonts->AddFontDefault();
			}

			if (file_exists("C:\\Windows\\Fonts\\segoeuib.ttf"))
			{
				g_font_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 20.0f, &cfg);
			}
		}

		void init_imgui(IDXGISwapChain* swap)
		{
			if (g_imgui_ready)
			{
				return;
			}
			if (FAILED(swap->GetDevice(IID_PPV_ARGS(&g_device))) || !g_device)
			{
				Console::printf("[demo] ImGui: GetDevice failed");
				return;
			}
			g_device->GetImmediateContext(&g_context);

			DXGI_SWAP_CHAIN_DESC desc{};
			swap->GetDesc(&desc);
			g_hwnd = desc.OutputWindow ? desc.OutputWindow : FindWindowA("S2", nullptr);
			if (!g_hwnd)
			{
				Console::printf("[demo] ImGui: no game HWND yet");
				return;
			}

			ImGui::CreateContext();
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			// Fonts must be added before the atlas is first built (the DX11
			// backend does that lazily on the first NewFrame), and the style
			// has to exist before draw_ui() ever reads it -- both belong
			// right here, once, at context creation.
			load_fonts();
			apply_wwii_theme();
			ImGui_ImplWin32_Init(g_hwnd);
			ImGui_ImplDX11_Init(g_device, g_context);
			ensure_render_target(swap);

			g_wndproc_orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
				g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc_hook)));

			g_imgui_ready = true;
			demo_player::refresh();
			Console::printf("[demo] Demo Tools ImGui ready (F9 / Insert to toggle)");
		}

		// =====================================================================
		//  DEMOS — the whole product in one tab
		// =====================================================================
		//
		// There are two demo ENGINES underneath (the game's own .demo and our
		// .dm_s2 container) and the user should never have to care. Everything
		// here goes through demo_player, which dispatches on the file
		// extension, so one list and one transport drive both.
		//
		// Thread note: this runs on the DXGI Present thread. Anything that
		// touches engine state is queued through the command buffer so it
		// lands on the client thread.
		void draw_demos_tab()
		{
			// ---- RECORDING --------------------------------------------------
			// The engine records by itself at every connect; there is no button
			// to press mid-match. So this is a setting, not an action.
			ImGui::SeparatorText("Recording");
			{
				bool on = demo_player::auto_record();
				if (ImGui::Checkbox("Record every match automatically (native)", &on))
				{
					GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
						on ? "demo_record 1" : "demo_record 0");
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"Only one recorder can be on at a time: ticking this turns our\n"
						"own capture off (saving it first if it is recording).\n\n"
						"The game writes the demo itself, from the moment you connect.\n"
						"Changing this takes effect on the NEXT match, because the\n"
						"engine only asks once, at cgame init.\n\n"
						"Public-match demos are repaired automatically when recording\n"
						"stops, so they play back like any other.\n\n"
						"Native PLAYBACK can get stuck loading a map on some demos --\n"
						"if that happens, this recording is still fine, it is only\n"
						"watching it back that is affected. Use the capture below\n"
						"instead if you need reliable playback right now.");
				}

				bool custom_on = demo_recording::is_armed() || demo_player::capturing();
				if (ImGui::Checkbox("Record with our own capture (.dm_s2)", &custom_on))
				{
					GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
						custom_on ? "demo_start" : "demo_stop_record");
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"Only one recorder can be on at a time: ticking this turns the\n"
						"native one off, and if it is already recording this match its\n"
						"file is saved and closed first.\n\n"
						"Our own recorder, entirely separate from the engine's. It\n"
						"captures live from the moment you connect, can be started\n"
						"or stopped mid-match, and plays back through Play without\n"
						"ever calling the engine's own cl_demo_play -- so it is not\n"
						"affected by native playback getting stuck.\n\n"
						"Ticking this before you connect arms it for the next match;\n"
						"the box will not show as actively recording until a match\n"
						"is actually feeding it data.");
				}
				if (demo_player::capturing())
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.55f, 1.0f), "writing now");
				}
			}

			// ---- LIBRARY ----------------------------------------------------
			ImGui::Spacing();
			ImGui::SeparatorText("Demos");

			if (ImGui::Button("Refresh"))
			{
				demo_player::refresh();
			}
			ImGui::SameLine();
			// The engine names its recordings x<4hex>_<8hex>, which tells the user
			// nothing. Every demo carries its map name in the footer, so this
			// renames them to <map>_<date>_<time> in place.
			if (ImGui::Button("Tidy names"))
			{
				std::string summary;
				demo_player::tidy_names(summary);
				g_native_fix_msg = "Tidy names: " + summary;
				g_native_fix_ok = true;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Renames engine demos from the game's generated hex name to\n"
					"<map>_<date>_<time>, read from the demo's own footer.\n\n"
					"Never overwrites: a clash gets a _2 suffix. Demos that are\n"
					"already named are left alone.");
			}
			const auto& demos = demo_player::list();

			// Which engine a demo belongs to matters (different playback, different
			// seeking, native can get stuck loading), so every row carries a
			// coloured tag and the list can be narrowed to one kind.
			const ImVec4 native_col(0.45f, 0.72f, 1.0f, 1.0f);
			const ImVec4 custom_col(1.0f, 0.72f, 0.30f, 1.0f);
			int n_native = 0;
			int n_custom = 0;
			for (const auto& d : demos)
			{
				(d.kind == demo_player::Kind::Engine ? n_native : n_custom) += 1;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%d demo(s):", static_cast<int>(demos.size()));
			ImGui::SameLine();
			ImGui::TextColored(native_col, "%d native", n_native);
			ImGui::SameLine();
			ImGui::TextColored(custom_col, "%d custom", n_custom);

			static int s_kind_filter = 0;   // 0 all, 1 native, 2 custom
			ImGui::RadioButton("All", &s_kind_filter, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Native (.demo)", &s_kind_filter, 1);
			ImGui::SameLine();
			ImGui::RadioButton("Custom (.dm_s2)", &s_kind_filter, 2);
			if (demos.empty())
			{
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "No demos yet.");
				ImGui::TextWrapped(
					"Play a match with recording on, then come back and press "
					"Refresh.");
			}
			else if (ImGui::BeginListBox("##demolist", ImVec2(-1, 190)))
			{
				for (int i = 0; i < static_cast<int>(demos.size()); ++i)
				{
					const auto& d = demos[i];
					const bool is_native = d.kind == demo_player::Kind::Engine;
					if ((s_kind_filter == 1 && !is_native) || (s_kind_filter == 2 && is_native))
					{
						continue;
					}
					const bool sel = demo_player::selected() == i;

					ImGui::PushID(i);
					if (ImGui::Selectable("##row", sel,
						ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 0)))
					{
						demo_player::set_selected(i);
					}
					ImGui::SameLine(0.0f, 0.0f);

					// Fixed-width tag column so names line up whichever kind it is.
					const float tag_w = ImGui::CalcTextSize("CUSTOM").x
						+ ImGui::GetStyle().ItemSpacing.x * 2.0f;
					const float tag_x = ImGui::GetCursorPosX();
					ImGui::TextColored(is_native ? native_col : custom_col, "%s",
						is_native ? "NATIVE" : "CUSTOM");
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip(is_native
							? "The game's own demo (.demo, in main\\demo).\n"
							  "Played by the engine's cl_demo_play."
							: "Our own capture (.dm_s2, in demos).\n"
							  "Played by the mod's theater.");
					}
					ImGui::SameLine(tag_x + tag_w);

					// A demo whose footer is missing will NOT play, and that is
					// worth saying in the list rather than at the moment the user
					// presses Play.
					const bool broken = d.kind == demo_player::Kind::Engine && !d.info.ok;
					if (broken)
					{
						ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", d.name.c_str());
					}
					else
					{
						ImGui::TextUnformatted(d.name.c_str());
					}

					// Right-hand column: what the file actually is.
					ImGui::SameLine(ImGui::GetContentRegionAvail().x - 250.0f);
					if (broken)
					{
						ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
							d.info.reason.c_str());
					}
					else
					{
						ImGui::TextDisabled("%-16s %-16s %s",
							d.info.map.empty() ? "?" : d.info.map.c_str(),
							d.info.date.c_str(),
							demo_library::human_size(d.size).c_str());
					}
					ImGui::PopID();
				}
				ImGui::EndListBox();
			}

			// Details for the SELECTED demo only. Duration needs a walk of the
			// packet stream, so it is done once per selection change, not per row
			// and not per frame.
			if (const auto* e = demo_player::selected_entry())
			{
				const auto& det = demo_player::selected_details();
				if (det.ok)
				{
					const bool sel_native = e->kind == demo_player::Kind::Engine;
					ImGui::TextColored(sel_native ? native_col : custom_col, "%s",
						sel_native ? "NATIVE" : "CUSTOM");
					ImGui::SameLine();
					if (sel_native)
					{
						ImGui::TextDisabled("%s   %s   %s   client %u",
							det.map.empty() ? "?" : det.map.c_str(),
							demo_library::human_duration(det.duration_ms).c_str(),
							demo_library::human_size(det.size).c_str(),
							det.client);
					}
					else
					{
						// The .dm_s2 container has no recording-slot or duration
						// field in its header, so show only what is really known.
						ImGui::TextDisabled("%s   %s   %s",
							det.map.empty() ? "?" : det.map.c_str(),
							det.date.c_str(),
							demo_library::human_size(det.size).c_str());
					}
				}
				else if (e->kind == demo_player::Kind::Engine)
				{
					ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
						"This demo will not play: %s", det.reason.c_str());
				}

				// Rename / delete. Both are irreversible, so delete asks twice.
				static char s_rename[128] = "";
				static std::filesystem::path s_rename_for;
				if (s_rename_for != e->path)
				{
					s_rename_for = e->path;
					const auto stem = e->path.stem().string();
					std::snprintf(s_rename, sizeof(s_rename), "%s", stem.c_str());
				}
				ImGui::SetNextItemWidth(240.0f);
				ImGui::InputText("##rename", s_rename, sizeof(s_rename));
				ImGui::SameLine();
				if (ImGui::Button("Rename"))
				{
					std::string why;
					if (demo_library::rename(e->path, s_rename, why))
					{
						g_native_fix_msg = "Renamed.";
						g_native_fix_ok = true;
						demo_player::refresh();
					}
					else
					{
						g_native_fix_msg = "Rename failed: " + why;
						g_native_fix_ok = false;
					}
				}
				ImGui::SameLine();
				static bool s_confirm_delete = false;
				if (!s_confirm_delete)
				{
					if (ImGui::Button("Delete"))
					{
						s_confirm_delete = true;
					}
				}
				else
				{
					ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Sure?");
					ImGui::SameLine();
					if (ImGui::Button("Yes, delete"))
					{
						std::string why;
						const bool ok = demo_library::remove(e->path, why);
						g_native_fix_msg = ok ? "Deleted." : ("Delete failed: " + why);
						g_native_fix_ok = ok;
						s_confirm_delete = false;
						demo_player::refresh();
					}
					ImGui::SameLine();
					if (ImGui::Button("Cancel"))
					{
						s_confirm_delete = false;
					}
				}
			}

			// Say WHY Play will not work rather than letting the button look
			// dead: CL_Demo_Play_f is wrapped in !com_sv_running, so with a
			// server up it is a SILENT no-op.
			const char* blocked = demo_player::blocked_reason();
			if (blocked)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%s", blocked);
			}

			ImGui::BeginDisabled(demos.empty() || blocked != nullptr);
			if (ImGui::Button("Play", ImVec2(90, 0)))
			{
				if (const auto* e = demo_player::selected_entry())
				{
					GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
						std::format("demo_play \"{}\"", e->name));
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!demo_player::playing());
			if (ImGui::Button("Stop", ImVec2(90, 0)))
			{
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_stop");
			}
			ImGui::EndDisabled();

			// Repair only applies to engine demos, so only offer it for one.
			{
				const auto* e = demo_player::selected_entry();
				const bool repairable = e && e->kind == demo_player::Kind::Engine;
				ImGui::SameLine();
				ImGui::BeginDisabled(!repairable);
				if (ImGui::Button("Repair", ImVec2(90, 0)))
				{
					// demo_native keeps its own selection; point it at the same
					// file before asking it to repair.
					const auto& nf = demo_native::files();
					for (int i = 0; i < static_cast<int>(nf.size()); ++i)
					{
						if (nf[i] == e->path)
						{
							demo_native::set_selected(i);
							break;
						}
					}
					bool ok = false;
					g_native_fix_msg = demo_native::repair_selected(ok);
					g_native_fix_ok = ok;
					demo_player::refresh();
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"Only needed if the game was killed mid-match, so recording\n"
						"never stopped cleanly. Splices the GSC script-string table\n"
						"back into a public-match demo that will not play.\n\n"
						"Safe on anything: a demo that already has it is left alone.");
				}
			}
			if (!g_native_fix_msg.empty())
			{
				ImGui::TextColored(
					g_native_fix_ok ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.7f, 0.3f, 1),
					"%s", g_native_fix_msg.c_str());
			}

			// ---- PLAYBACK ---------------------------------------------------
			// Only exists while something is playing. One transport, whichever
			// engine is behind it.
			if (!demo_player::playing())
			{
				return;
			}

			ImGui::Spacing();
			ImGui::SeparatorText("Playback");

			if (const float prog = demo_player::progress(); prog >= 0.0f)
			{
				char ov[32];
				std::snprintf(ov, sizeof(ov), "%.0f%%", prog * 100.0f);
				ImGui::ProgressBar(prog, ImVec2(-FLT_MIN, 0.0f), ov);
			}

            if (demo_native::seek_in_progress())
                ImGui::TextWrapped("Seeking demo; reloading if the requested time is outside the cache...");
            else if (demo_player::last_seek_failed())
                ImGui::TextWrapped("Seek failed: requested time was not reached. See console for details.");
			const bool paused = demo_player::paused();
			if (ImGui::Button(paused ? "Play##t" : "Pause##t", ImVec2(90, 0)))
			{
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_pause");
			}
			ImGui::SameLine();
			if (ImGui::Button("<< 10s", ImVec2(70, 0)))
			{
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_seek -10");
			}
			ImGui::SameLine();
			if (ImGui::Button("5s >>", ImVec2(70, 0)))
			{
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_seek 5");
			}
			ImGui::SameLine();
			ImGui::TextDisabled(demo_playback::seeking() ? "seeking..." : "");

			{
				float ts = demo_player::timescale();
				ImGui::SetNextItemWidth(200.0f);
				// LOGARITHMIC, not linear. 0.05 to 4.0 is a 80:1 range, so on a linear
				// track the whole slow-motion end would be squeezed into a few pixels
				// -- and that end is the one used for filming. Log scaling gives each
				// halving of speed the same travel.
				if (ImGui::SliderFloat("Speed", &ts, 0.05f, 10.0f, "%.2fx",
					ImGuiSliderFlags_Logarithmic))
				{
					GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
						std::format("demo_speed {:.3f}", ts));
				}
				ImGui::SameLine();
				if (ImGui::Button("1x"))
				{
					GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_speed 1");
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"Slows the HUD and the audio pitch with the picture, so a\n"
						"clip recorded slow and sped up in an editor sounds normal.");
				}
			}


			// ---- CLIPS ------------------------------------------------------
			// The engine ships a complete segment system with no front-end: the
			// commands are registered, the timeline materials exist, and the menu
			// that would drive it is Lua inside the fastfiles. These four buttons
			// ARE that missing front-end -- each one issues the engine's own
			// command, so there is nothing to go wrong that the engine would not
			// do to itself.
			if (demo_player::active() == demo_player::Kind::Engine)
			{
				ImGui::Spacing();
				ImGui::SeparatorText("Clip");

				if (ImGui::Button("Mark In", ImVec2(90, 0)))
				{
					demo_native::clip_mark_in();
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Records the CURRENT position as the start of a "
						"segment.\nConsole: cl_demo_savesegment 1");
				}
				ImGui::SameLine();
				if (ImGui::Button("Mark Out", ImVec2(90, 0)))
				{
					demo_native::clip_mark_out();
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Closes the segment at the current position and "
						"saves it.\nConsole: cl_demo_savesegment 0");
				}
				ImGui::SameLine();
				if (ImGui::Button("Preview", ImVec2(90, 0)))
				{
					demo_native::clip_preview();
				}
				ImGui::SameLine();
				if (ImGui::Button("Clear", ImVec2(90, 0)))
				{
					demo_native::clip_clear();
				}

				const int n = demo_native::clip_count();
				ImGui::SameLine();
				if (n >= 0)
				{
					ImGui::TextDisabled("%d segment(s)", n);
				}
				else
				{
					ImGui::TextDisabled("segments: ?");
				}
			}

			// ---- FRAMING ----------------------------------------------------
			ImGui::Spacing();
			ImGui::SeparatorText("Framing");
			{
				// Applied at the engine's final fov, so it works in first, third
				// and free camera, in native and custom demos alike -- and is not
				// limited to cg_fov's 50..100.
				if (demo_camera::fov_available())
				{
					float v = demo_camera::fov();
					ImGui::SetNextItemWidth(200.0f);
					if (ImGui::SliderFloat("FOV", &v, 5.0f, 160.0f, "%.0f"))
					{
						demo_camera::set_fov(v);
					}
					ImGui::SameLine();
					if (ImGui::Button(demo_camera::fov_overridden() ? "Reset##fov" : "Game##fov"))
					{
						demo_camera::clear_fov();
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Back to the game's own field of view.\n"
							"Alt + mouse wheel zooms while flying the free camera.\n"
							"Dolly points record the FOV too, so a zoom can be animated.");
					}
				}
				else
				{
					ImGui::TextDisabled("FOV: the field-of-view hook did not install");
				}

				// Third person distance/height. Two single instructions
				// repointed at floats we own -- the constants themselves are
				// shared by 17 and 12 other call sites and are NOT touched.
				if (demo_camera::framing_patched())
				{
					float* dist = demo_camera::distance();
					float* hgt = demo_camera::height();
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat("3rd person distance", dist, 0.0f, 400.0f, "%.0f");
					ImGui::SameLine();
					if (ImGui::Button("Reset##dist")) { *dist = 85.0f; }
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat("3rd person height", hgt, -50.0f, 150.0f, "%.0f");
					ImGui::SameLine();
					if (ImGui::Button("Reset##hgt")) { *hgt = 35.0f; }
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Stock is distance 85, height 35.");
					}
				}
				else
				{
					ImGui::TextDisabled("3rd person framing: not patchable in this build");
				}

				// Roll: the engine writes this field every frame from the usercmd,
				// which the mouse never populates. So it is plumbed and simply
				// never fed.
				float roll = demo_camera::roll();
				ImGui::SetNextItemWidth(200.0f);
				if (ImGui::SliderFloat("Camera roll", &roll, -45.0f, 45.0f, "%.1f deg"))
				{
					demo_camera::set_roll(roll);
				}
				ImGui::SameLine();
				if (ImGui::Button("0##roll"))
				{
					demo_camera::set_roll(0.0f);
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"Dutch angle for the FREE camera. 0 restores stock behaviour\n"
						"and stops us writing the field at all.");
				}

				if (ImGui::Button("High-res screenshot"))
				{
					demo_camera::screenshot();
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"The engine's own TILED capture (CL_Demo_CaptureScreenshot).\n"
						"Playback freezes and the camera sweeps while it captures the\n"
						"tiles - that is the capture working, not a fault.\n\n"
						"This is why F3 is blocked by default: it is a deliberate\n"
						"action, not something to trigger by accident.");
				}
			}

			// ---- CAMERA ----------------------------------------------------
			// One control for both systems: the enum values ARE the engine's own
			// camera mode numbers, and theater_camera answers the engine's two
			// mode predicates for the custom theater.
			ImGui::Spacing();
			ImGui::SeparatorText("Camera");
			{
				const auto cam = theater_camera::get_mode();
				const bool native = demo_player::active() == demo_player::Kind::Engine;

				if (!theater_camera::hooks_installed() && !native)
				{
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
						"Camera hooks FAILED - stuck in first person");
				}

				const auto cam_button = [&](const char* label,
					const theater_camera::camera_mode mode, const char* tip)
				{
					const bool on = (cam == mode);
					if (on)
					{
						ImGui::PushStyleColor(ImGuiCol_Button,
							ImVec4(0.20f, 0.45f, 0.25f, 1.0f));
					}
					if (ImGui::Button(label, ImVec2(88, 0)))
					{
						theater_camera::set_mode(mode);
					}
					if (on)
					{
						ImGui::PopStyleColor();
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", tip);
					}
				};

				cam_button("First", theater_camera::THEATER_CAMERA_FIRST_PERSON,
					"Ride the recorded player's own view.");
				ImGui::SameLine();
				cam_button("Third", theater_camera::THEATER_CAMERA_THIRD_PERSON,
					"Chase camera behind the recorded player.");
				ImGui::SameLine();
				cam_button("Free", theater_camera::THEATER_CAMERA_FREECAM,
					"Detached fly camera - move with the normal movement keys.\n"
					"Seeded from the current view so it does not jump.\n"
					"Dolly and Bone Cam both drive this mode.");

				// The engine bakes the freecam speed into CL_Demo_FreeCameraMove
				// as a constant with no dvar, which is why it flies so fast.
				// demo_native repoints that one instruction at a float we own.
				if (demo_native::freecam_speed_patched())
				{
					float* spd = demo_native::freecam_speed();
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat("Free camera speed", spd, 0.1f, 400.0f, "%.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
					ImGui::SameLine();
					if (ImGui::Button("Reset##freecam"))
					{
						*spd = 190.0f;   // engine stock
					}
				}
			}

            // PROBE ONLY: A/B the engine's exposure blending after rewind.
            // Do not force exposure, gamma, or LightSet tweak overrides.
            ImGui::Spacing();
            ImGui::SeparatorText("Exposure test");
            {
                const auto name = DvarInterface::toEngineString("r_tonemapBlend");
                const auto* dvar = Functions::_Dvar_FindVar(name.c_str());
                const bool supported = dvar && (dvar->type == DVAR_TYPE_BOOL
                    || dvar->type == DVAR_TYPE_BOOL_SECURE);
                const bool blend = supported && (dvar->type == DVAR_TYPE_BOOL_SECURE
                    ? GameUtil::decodeDvarSecureBool(dvar) : dvar->current.enabled);
                bool instant = supported && !blend;
                static int last_observed = -2;
                const int observed = supported ? static_cast<int>(blend) : -1;
                if (observed != last_observed)
                {
                    Console::printf("[exposure-test] observed r_tonemapBlend=%d type=%d (PROBE ONLY)",
                        observed, dvar ? static_cast<int>(dvar->type) : -1);
                    last_observed = observed;
                }
                ImGui::BeginDisabled(!supported);
                if (ImGui::Checkbox("Disable exposure blending (test)", &instant))
                {
                    const bool queued = GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
                        std::format("{} {}", name, instant ? 0 : 1));
                    Console::printf("[exposure-test] requested r_tonemapBlend=%d command=%s (PROBE ONLY)",
                        instant ? 0 : 1, queued ? "queued" : "not queued");
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Test for the dark-to-bright transition after rewind.\n"
                        "Compare the same path with this off and on.\n"
                        "Uncheck to restore blending. This is not a confirmed lighting fix.");
                if (!supported) ImGui::TextDisabled("Exposure blending control unavailable.");
            }

			// ---- HUD --------------------------------------------------------
			ImGui::Spacing();
			ImGui::SeparatorText("HUD");
			{
                const auto draw2d_name = DvarInterface::toEngineString("cg_draw2D");
                const auto* draw2d = Functions::_Dvar_FindVar(draw2d_name.c_str());
                // Secure bool storage is encoded; its first byte is not its value.
                const bool draw2d_supported = draw2d && (draw2d->type == DVAR_TYPE_BOOL
                    || draw2d->type == DVAR_TYPE_BOOL_SECURE);
                const bool hud_visible = draw2d_supported && (draw2d->type == DVAR_TYPE_BOOL_SECURE
                    ? GameUtil::decodeDvarSecureBool(draw2d) : draw2d->current.enabled);
                bool no_hud = draw2d_supported && !hud_visible;
                ImGui::BeginDisabled(!draw2d_supported);
                if (ImGui::Checkbox("No HUD", &no_hud))
                {
                    const bool queued = GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
                        std::format("{} {}", draw2d_name, no_hud ? 0 : 1));
                    Console::printf("[hud] No HUD %s: cg_draw2D type=%d, command %s",
                        no_hud ? "ON" : "OFF", static_cast<int>(draw2d->type),
                        queued ? "queued" : "not queued");
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Toggles cg_draw2d. The mod tools remain visible.");
                bool minimal = demo_native::hud_minimal();
				if (ImGui::Checkbox("Minimal HUD", &minimal))
				{
					demo_native::set_hud_minimal(minimal);
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"Keeps ONLY hitmarkers, killfeed and score popups.\n\n"
						"Those three do not share a system, which is what makes the\n"
						"split possible: hitmarkers are native, killfeed and score\n"
						"popups are LUI but EVENT-driven. Everything else (ammo,\n"
						"division, perks, health, morale, clan tag) is LUI\n"
						"MODEL-driven, so withholding those models drops it without\n"
						"touching the keepers.\n\n"
						"Turn this on BEFORE starting playback: withholding stops a\n"
						"model updating, it cannot tear down an element that was\n"
						"already built.");
				}

				static bool s_hide_division = true;
				if (ImGui::Checkbox("Hide division widget", &s_hide_division))
				{
					demo_native::suppress_division_model(s_hide_division);
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"The MP HUD strands a checkerboard division icon during\n"
						"playback, because a LUI model subscription throws mid-build.\n"
						"This withholds that ONE model and leaves the rest alone.");
				}
			}

			// ---- DIAGNOSTICS ------------------------------------------------
			// Developer mode only. The Com_Error watch is the only way to catch
			// the Arxan-hidden asset limit error, which reports its caller as an
			// IDA address -- invaluable during an investigation, noise otherwise.
			if (dev_mode::enabled())
			{
				const auto& errs = demo_native::errors();
				if (!errs.empty() && ImGui::CollapsingHeader("Com_Error log"))
				{
					if (ImGui::Button("Clear##comerr"))
					{
						demo_native::clear_errors();
					}
					if (ImGui::BeginListBox("##comerrlist", ImVec2(-1, 90)))
					{
						for (const auto& e : errs)
						{
							ImGui::Text("code=%d IDA_0x%llX %s", e.code,
								static_cast<unsigned long long>(e.ida_addr),
								e.message.c_str());
						}
						ImGui::EndListBox();
					}
				}
			}
		}

		// ---- SERVERS ----------------------------------------------------------
		// READ-ONLY view of the lobbies matchmaking returned for the current
		// search. Reached by a pure pointer walk (lobby -> mm -> results), with
		// the engine's own back-pointer as a sanity check. NO HOOKS -- the
		// previous implementation patched this path and broke Find Match.
		void draw_servers_tab()
		{
			const auto st = server_browser::status();

			ImGui::TextWrapped(
				"Lobbies Demonware returned for the CURRENT search, with real player "
				"counts and ping. Read-only and hook-free, so it cannot affect "
				"matchmaking.");
			ImGui::Separator();

			if (!st.reachable)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1), "Not in a lobby");
				ImGui::TextWrapped(
					"The lobby/matchmaking object did not resolve (or its back-pointer "
					"did not match, in which case this refuses rather than showing "
					"nonsense). Get to the menu/lobby and it will connect.");
				return;
			}

			ImGui::Text("Playlist: %s", server_browser::playlist_name(st.playlist).c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("(id %d)", st.playlist);

			// Manual scan / sweep. These issue REAL matchmaking queries by calling
			// the engine's own search tick -- the same call the LUI binding
			// sub_2960E0 makes -- so they are ordinary calls, not hooks.
			// ---- DLC entitlements ---------------------------------------------
			// Turning DLC off makes matchmaking search the BASE playlists, which
			// are the populated ones. Zeroes only the COUNT -- the table rows are
			// untouched, so re-enabling restores exactly what the game registered.
			{
				bool on = dlc::enabled();
				if (dlc::available() || on)
				{
					if (ImGui::Checkbox("DLC entitlements", &on))
					{
						dlc::set_enabled(on);
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip(
							"OFF makes matchmaking search the BASE playlists.\n"
							"DLC pools are often empty, so this usually finds games faster.\n\n"
							"Only the entitlement COUNT is zeroed - the table rows are left\n"
							"alone, so turning it back on restores exactly what the game\n"
							"registered. It is re-asserted every 250ms because the engine\n"
							"keeps re-registering entitlements.");
					}
					ImGui::SameLine();
					ImGui::TextDisabled("(%d of %d)", dlc::entry_count(),
						(std::max)(dlc::original_count(), dlc::entry_count()));
				}
				else
				{
					ImGui::TextDisabled("DLC entitlements: not registered yet");
				}
			}
			ImGui::Separator();

			// Say WHY a button will refuse, rather than letting it look dead.
			if (server_browser::busy_reason() != nullptr)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1), "%s",
					server_browser::busy_reason());
			}

			const bool sweeping = server_browser::sweeping();
			if (ImGui::Button("Scan this playlist"))
			{
				server_browser::scan_current();
			}
			ImGui::SameLine();
			if (ImGui::Button(sweeping ? "Stop sweep" : "Sweep all playlists"))
			{
				if (sweeping) { server_browser::sweep_stop(); }
				else          { server_browser::sweep_start(); }
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Walks every known playlist, ~2.5s each, issuing one real\n"
					"search per playlist and collecting the results.\n\n"
					"It writes params[11] (the playlist field the search blob is\n"
					"built from) and ALWAYS restores your original playlist,\n"
					"including if you abort or enter a match mid-sweep.");
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear"))
			{
				server_browser::clear_cache();
			}
			if (sweeping)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "%s",
					server_browser::sweep_status().c_str());
			}
			{
				// Results are CACHED, so say so -- otherwise a list that persists
				// after a search ends looks like stale nonsense rather than intent.
				const int age = server_browser::last_result_ms();
				ImGui::TextDisabled("%d cached%s", server_browser::cached_count(),
					age < 0 ? "  (nothing seen yet)"
					        : std::format("  (last result {:.0f}s ago)", age / 1000.0f).c_str());
			}

			// Poll at 2 Hz. These are engine reads and this runs per frame.
			static std::vector<server_browser::Server> s_list;
			static DWORD s_at = 0;
			const DWORD now = GetTickCount();
			if (now - s_at > 500)
			{
				s_at = now;
				// The cache: accumulated across playlists and kept when a search
				// ends. See server_browser.hpp for why this is not a live read.
				s_list = server_browser::servers();
			}

			if (s_list.empty())
			{
				ImGui::Separator();
				ImGui::TextWrapped(
					"No results right now.\n\nThis fills while the GAME is searching - "
					"press Find Match and watch it populate. Nothing in this tab starts "
					"a search: matchmaking is brokered by the service, and the one thing "
					"that reliably broke it before was us interfering with that path.");
				return;
			}

			ImGui::Separator();
			if (ImGui::BeginTable("##browser", 6,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
				| ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 26.0f);
				ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 74.0f);
				ImGui::TableSetupColumn("Ping",    ImGuiTableColumnFlags_WidthFixed, 46.0f);
				ImGui::TableSetupColumn("Region",  ImGuiTableColumnFlags_WidthFixed, 56.0f);
				ImGui::TableSetupColumn("Playlist");
				ImGui::TableSetupColumn("State",   ImGuiTableColumnFlags_WidthFixed, 92.0f);
				ImGui::TableHeadersRow();

				for (const auto& s : s_list)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();  ImGui::Text("%d", s.index);
					ImGui::TableNextColumn();  ImGui::Text("%d / %d", s.players, s.capacity);
					ImGui::TableNextColumn();
					if (s.ping > 0) { ImGui::Text("%d", s.ping); }
					else            { ImGui::TextDisabled("-"); }
					ImGui::TableNextColumn();  ImGui::Text("%d", s.datacenter);
					ImGui::TableNextColumn();  ImGui::TextUnformatted(s.playlist_name.c_str());
					ImGui::TableNextColumn();
					if (!s.joinable)
					{
						ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.35f, 1), "not joinable");
					}
					else if (s.throttled)
					{
						ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.35f, 1), "throttled");
					}
					else
					{
						ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "joinable");
					}
				}
				ImGui::EndTable();
			}

			ImGui::TextDisabled(
				"Counts and ping come from the engine's own search record (sub_2A1820 "
				"names these slots_total_count / slots_available_count / ping_ours).");
			ImGui::TextDisabled(
				"No Join button by design: steering the engine to one result means "
				"hooking the search path, which is what broke Find Match before.");
		}

		// ---- PLAYERS ----------------------------------------------------------
		// The lobby roster, with a kick. As the LOBBY host you remove someone with
		// the engine's own PartyHost_KickPlayer -- NOT SV_KickClient, which needs
		// com_sv_running (i.e. that you are the server, which you are not when a
		// dedicated server is running the match).
		void draw_players_tab()
		{
			// The member walk reads up to 48 slots x 33216 bytes with a
			// VirtualQuery per slot, so cache it rather than doing that at frame
			// rate on the Present thread.
			static std::vector<force_host::member_t> s_members;
			static double s_last = 0.0;
			static int    s_confirm = -1;

			const double now = ImGui::GetTime();
			if (now - s_last > 1.0)
			{
				s_last = now;
				s_members = force_host::lobby_members();
			}

			const auto st = force_host::status();
			const bool host = st.lobby_ok && st.we_host != 0;

			if (host)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.9f, 0.45f, 1.0f));
				ImGui::TextUnformatted("You are the lobby host - you can remove players.");
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
				ImGui::TextWrapped(
					"You are NOT the lobby host, so kicking will be refused. Turn on "
					"force host in the Host tab before searching.");
				ImGui::PopStyleColor();
			}

			ImGui::Spacing();
			ImGui::Text("%zu player(s) in the lobby", s_members.size());
			ImGui::Spacing();

			if (ImGui::BeginTable("##lobby", 4,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 28.0f);
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("SteamID64", ImGuiTableColumnFlags_WidthFixed, 160.0f);
				ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 110.0f);
				ImGui::TableHeadersRow();

				for (const auto& m : s_members)
				{
					ImGui::TableNextRow();
					ImGui::PushID(m.index);

					ImGui::TableSetColumnIndex(0);
					ImGui::Text("%d", m.index);

					ImGui::TableSetColumnIndex(1);
					if (m.is_me)
					{
						ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s  (you)",
							m.name.c_str());
					}
					else
					{
						ImGui::TextUnformatted(m.name.c_str());
					}

					ImGui::TableSetColumnIndex(2);
					ImGui::TextDisabled("%llu", static_cast<unsigned long long>(m.xuid));

					ImGui::TableSetColumnIndex(3);
					if (m.is_me)
					{
						ImGui::TextDisabled("-");
					}
					else if (!host)
					{
						ImGui::BeginDisabled();
						ImGui::Button("Kick");
						ImGui::EndDisabled();
					}
					else if (s_confirm == m.index)
					{
						// Two-step, because a kick cannot be undone and a misclick
						// removes the wrong person from someone's game.
						ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.16f, 0.16f, 1.0f));
						if (ImGui::Button("Confirm?"))
						{
							GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
								std::format("fh_kick {}", m.index));
							s_confirm = -1;
							s_last = 0.0;          // refresh the list next frame
						}
						ImGui::PopStyleColor();
					}
					else if (ImGui::Button("Kick"))
					{
						s_confirm = m.index;
					}

					ImGui::PopID();
				}
				ImGui::EndTable();
			}

			if (s_confirm >= 0)
			{
				ImGui::Spacing();
				ImGui::TextDisabled("Click Confirm? to remove, or anywhere else to cancel.");
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
				{
					s_confirm = -1;
				}
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextDisabled(
				"Kicks go through the engine's own PartyHost_KickPlayer, which sends\n"
				"%%ikickedFromParty to that member and removes them from the party.\n"
				"Console equivalents: fh_who (list), fh_kick <slot>.");
		}

		// ---- BOTS -------------------------------------------------------------
		// Deliberately ONE button plus a count. Everything that makes a bot lobby
		// look real (names, a spread of the game's own uniforms) is a single
		// preset; how MANY bots is the only thing left to the user, because that
		// is the one choice that is genuinely theirs.
		void draw_bots_tab()
		{
			static int  s_count = 11;      // 11 bots + you = a full-ish 12 lobby
			static int  s_uniform_pct = 50;
			static bool s_inited = false;
			if (!s_inited)
			{
				s_inited = true;
				s_uniform_pct = bots::look_chance();
			}

			const bool hub = bots::in_hub();
			int used = 0, total = 0;
			const bool have_server = bots::slots(used, total);

			// --- the one button ---------------------------------------------
			ImGui::TextUnformatted("Make a bot lobby look like a real one:");
			ImGui::Spacing();
			if (ImGui::Button("SET UP BOT LOBBY", ImVec2(-1.0f, 34.0f)))
			{
				bots::preset_lobby(s_uniform_pct);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Turns on real player names and a mixed spread of the game's own\n"
					"uniforms. Does NOT add bots -- that is the control below.");
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			// --- the one thing the user owns --------------------------------
			ImGui::TextUnformatted("Bots to add");
			ImGui::SetNextItemWidth(-90.0f);
			ImGui::SliderInt("##botcount", &s_count, 1, 17);
			ImGui::SameLine();
			if (ImGui::Button("Add", ImVec2(80.0f, 0.0f)))
			{
				// Must run on the CLIENT thread -- SV_AddBot touches server state
				// and this is the DXGI Present thread.
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
					std::format("bots_fill {}", s_count));
			}

			if (hub)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
				ImGui::TextWrapped(
					"You are in the hub / headquarters. SV_AddBot refuses here by its "
					"own first gate, so load an actual map first.");
				ImGui::PopStyleColor();
			}
			else if (!have_server)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
				ImGui::TextWrapped(
					"No server running yet. Bots are server clients, so the map has to "
					"be loaded -- add them during the pre-match countdown.");
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::Text("Slots: %d used of %d (%d free)", used, total, total - used);
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			// --- status, so the preset is never a black box ------------------
			const int mode = bots::look_mode();
			const char* mode_s =
				mode == 0 ? "engine default" :
				mode == 1 ? "copy of YOUR uniform" :
				mode == 2 ? "one fixed kit" : "random per bot";
			ImGui::Text("Names   : %s, %zu loaded",
				bots::enabled() ? "ON" : "off", bots::name_count());
			ImGui::Text("Uniforms: %s, %zu available", mode_s, bots::kit_count());
			ImGui::Text("Renamed : %zu bot(s) this session", bots::renamed_count());

			if (ImGui::CollapsingHeader("Options"))
			{
				ImGui::SetNextItemWidth(-160.0f);
				if (ImGui::SliderInt("custom uniform %", &s_uniform_pct, 0, 100))
				{
					if (mode == 3)
					{
						bots::set_look_random(s_uniform_pct);
					}
				}
				ImGui::TextWrapped(
					"Each bot rolls independently, so the rest keep the usual uniform "
					"and the lobby comes out mixed.");

				static std::string s_reload_msg;
				if (ImGui::Button("Reload names + uniforms"))
				{
					bots::reload();
					// reload_kits() returns the count -- report it rather than
					// discard it, so a mistyped botkits.txt shows up as "0".
					s_reload_msg = std::format("{} name(s), {} uniform(s)",
						bots::name_count(), bots::reload_kits());
				}
				ImGui::SameLine();
				if (ImGui::Button("Uniforms off"))
				{
					bots::set_look_off();
				}
				if (!s_reload_msg.empty())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("%s", s_reload_msg.c_str());
				}

				ImGui::Spacing();
				ImGui::TextDisabled(
					"Names come from S2MP-Mod\\botnames.txt, uniforms from botkits.txt\n"
					"(generate with tools/s2_botkits.py). Both reload while the game runs.");
			}
		}

		// ---- HOST -------------------------------------------------------------
		// Set it, play, host. No hotkey and no window to hit: the map and gametype
		// are HELD at your values, and matchmaking's search never reads either of
		// them (proven from sub_285DA0), so holding them costs nothing.
		// =====================================================================
		//  HOST TAB - deliberately one decision
		// =====================================================================
		//
		// This used to be three checkboxes, a playlist box, a DLC toggle, a
		// republish latch and four diagnostic panels, in an order you had to
		// know. All of that machinery still exists and every console command
		// still works, but normal use is: press Start, play a match.
		//
		// Developer mode only.
		void draw_host_tab()
		{
			// The engine tables change with the season, so they are re-read rather
			// than hardcoded -- but not every frame; that is 100 records x 568 B
			// plus two more tables, on the Present thread.
			static std::vector<force_host::Playlist> s_pls;
			static std::vector<std::string>          s_maps;
			static std::vector<std::string>          s_gts;
			static DWORD                             s_refresh = 0;
			const DWORD now = GetTickCount();
			if (s_pls.empty() || now - s_refresh > 2000)
			{
				s_refresh = now;
				s_pls = force_host::playlists();
				s_maps = force_host::maps();
				s_gts = force_host::gametypes();
			}

			ImGui::TextWrapped(
				"Own the LOBBY. The match itself still runs on a dedicated server -- "
				"that part is brokered by Demonware and no client can change it -- but "
				"you set the map, gametype, playlist and player counts, and you can "
				"kick people from the Players tab.");
			ImGui::Spacing();

			bool on = force_host::enabled();
			if (ImGui::Checkbox("Host my own match", &on))
			{
				force_host::set_enabled(on);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Refuses other people's lobbies while you search, so the game\n"
					"makes YOU the lobby host instead of putting you in someone\n"
					"else's. It stands down by itself once you are in a game, and\n"
					"gives up after 90 s so you can never be left unable to find one.\n"
					"\n"
					"It also holds the permissions the engine keeps clearing:\n"
					"  g_hostingEnabled = 1  (its only writer can ONLY clear it)\n"
					"  matchmaking_allowJoiningListenServer = 1\n"
					"  party_minplayers / party_maxplayers\n"
					"No hotkey and no timing: set it once and play.");
			}

			const auto st = force_host::status();
			if (!st.lobby_ok)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1), "No lobby object yet.");
			}
			else
			{
				ImGui::Text("Now: playlist %d (%s)  map %s  %s  %d..%d players",
					st.playlist,
					st.playlist_disp.empty() ? "?" : st.playlist_disp.c_str(),
					st.cur_map.empty() ? "?" : st.cur_map.c_str(),
					st.cur_gametype.empty() ? "?" : st.cur_gametype.c_str(),
					st.cur_min, st.cur_max);

				// "Lobby host" is the number that matters here. "dedicated" reading
				// YES is NORMAL and expected -- a dedicated server running the match
				// is the arrangement, not a failure -- so label it that way rather
				// than leaving a scary-looking flag with no context.
				if (st.we_host)
				{
					ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1),
						"Lobby host: YES  -- you control the settings and can kick.");
				}
				else
				{
					ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1),
						"Lobby host: no  -- someone else owns this lobby.");
				}
				ImGui::TextDisabled("session: %s   phase: %s   (match server: %s, expected)",
					st.have_session ? "yes" : "searching",
					st.phase,
					st.dedicated ? "dedicated" : "listen");
			}
			ImGui::Separator();

			// ---- what the match should PLAY ---------------------------------
			auto playlist_combo = [&](const char* label, int& cur, const bool allow_none)
			{
				std::string preview = "(leave the engine's)";
				for (const auto& p : s_pls)
				{
					if (p.id == cur)
					{
						preview = std::to_string(p.id) + "  " + p.name;
						break;
					}
				}
				bool changed = false;
				if (ImGui::BeginCombo(label, preview.c_str()))
				{
					if (allow_none && ImGui::Selectable("(leave the engine's)", cur < 0))
					{
						cur = -1;
						changed = true;
					}
					for (const auto& p : s_pls)
					{
						const auto row = std::to_string(p.id) + "  " + p.name;
						if (ImGui::Selectable(row.c_str(), p.id == cur))
						{
							cur = p.id;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
				return changed;
			};

			int play = force_host::play_playlist();
			if (playlist_combo("Play", play, true))
			{
				force_host::set_play_playlist(play);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"The playlist the match RUNS and REPORTS.\n"
					"params[11] is what GetGameLobbyPlaylistNum returns, so setting\n"
					"it is what makes the game say the right name -- the old tool\n"
					"never touched it, which is why it kept showing the playlist\n"
					"you searched.");
			}

			// map
			{
				std::string cur = force_host::map();
				const std::string preview = cur.empty() ? "(the playlist's own rotation)" : cur;
				if (ImGui::BeginCombo("Map", preview.c_str()))
				{
					if (ImGui::Selectable("(the playlist's own rotation)", cur.empty()))
					{
						force_host::set_map("");
					}
					for (const auto& m : s_maps)
					{
						if (ImGui::Selectable(m.c_str(), m == cur))
						{
							force_host::set_map(m);
						}
					}
					ImGui::EndCombo();
				}
			}

			// gametype
			{
				std::string cur = force_host::gametype();
				const std::string preview = cur.empty() ? "(the playlist's own)" : cur;
				if (ImGui::BeginCombo("Gametype", preview.c_str()))
				{
					if (ImGui::Selectable("(the playlist's own)", cur.empty()))
					{
						force_host::set_gametype("");
					}
					for (const auto& g : s_gts)
					{
						if (ImGui::Selectable(g.c_str(), g == cur))
						{
							force_host::set_gametype(g);
						}
					}
					ImGui::EndCombo();
				}
			}

			int mn = force_host::min_players();
			int mx = force_host::max_players();
			ImGui::SetNextItemWidth(90);
			if (ImGui::InputInt("Min", &mn)) { force_host::set_min_players(mn); }
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90);
			if (ImGui::InputInt("Max", &mx)) { force_host::set_max_players(mx); }
			ImGui::SameLine();
			ImGui::TextDisabled("players (engine max 18)");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"18 is the hard per-match ceiling and it cannot be raised.\n"
					"svs.clientCount is written only by SV_Init, from sv_maxclients,\n"
					"and every server array is allocated from it. Raising the dvar is\n"
					"FATAL: the server hunk is reserved in advance from a client count,\n"
					"quadratically, so SV_Init then allocates past the reservation and\n"
					"aborts with \"Memory Error: 6 161\". Tested in game.\n"
					"\n"
					"The hub's 48 comes from a hardcoded constant on SV_Init's other\n"
					"branch, not from any expansion.");
			}

			// Bots deliberately live in their OWN tab, not here -- they are a
			// separate feature with their own names/uniforms and their own toggle.

			// ---- advanced ----------------------------------------------------
			if (ImGui::CollapsingHeader("Advanced"))
			{
				ImGui::TextWrapped(
					"Search a BUSIER playlist than the one you want to run. The "
					"switch happens by itself the moment the lobby has a session -- "
					"no hotkey. Leave it on \"same as Play\" unless the playlist you "
					"want is empty.");
				int search = force_host::search_playlist();
				if (playlist_combo("Search in", search, true))
				{
					force_host::set_search_playlist(search);
				}
				if (search < 0)
				{
					ImGui::TextDisabled("same as Play (no phase, nothing to time)");
				}

				ImGui::Spacing();
				bool auto_apply = force_host::auto_apply();
				if (ImGui::Checkbox("Apply the playlist the engine's way", &auto_apply))
				{
					force_host::set_auto_apply(auto_apply);
				}
				ImGui::SameLine();
				ImGui::TextDisabled("(?)");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(
						"Calls sub_6563D0, the engine's own playlist applier -- map,\n"
						"gametype, display name, icon and the playlist's var-rule blob,\n"
						"exactly as the menu does it. CONFIRMED WORKING IN GAME.\n"
						"Runs once when the Play playlist changes, never in a loop\n"
						"(it execs config files). Your Map/Gametype choices are put\n"
						"back on top of it on the next tick.");
				}
				if (ImGui::Button("Apply now"))
				{
					force_host::queue_apply_playlist();
				}
			}

			ImGui::Separator();
			ImGui::TextDisabled(
				"No hotkey and no timing. The map spawn (sub_48C0C0) reads "
				"ui_mapname/ui_gametype when it spawns, and the matchmaking search "
				"never reads either -- so they are simply held correct.");
		}

		// =================================================================
		//  DOLLY — EDITING ONLY. Nothing is drawn from here.
		// =================================================================
		//
		// The markers and the path are drawn by the ENGINE (dolly::render, from
		// the existing R_EndFrame hook) using CG_WorldPosToScreenPos, so they
		// sit in the world at the right size and place. This panel only edits
		// the list, which is what ImGui is actually good for.
		// ---- Bone Cam --------------------------------------------------------
		// Deliberately its OWN tab rather than a section of the Dolly tab: the
		// dolly is native-only, while the bone cam works in both demo systems.
		void draw_bonecam_tab()
		{
			const bool native = demo_native::native_playing();
			const bool theater = demo_playback::is_playing();

			if (!native && !theater)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "No demo playing.");
				ImGui::TextDisabled(
					"Play a demo first. The bone cam rides a bone on a player's\n"
					"skeleton, so it needs a body on screen to attach to.");
				return;
			}

			ImGui::TextWrapped(
				"Locks the camera to a bone on a player's skeleton while your MOUSE "
				"still aims freely. The camera inherits the animation's real motion "
				"- recoil, sprint bob, the hit reaction - which a smooth path cannot fake.");
			ImGui::Separator();

			// Free camera is the only mode the engine routes through the mover we
			// ride, so say it plainly and offer the switch.
			const auto cam = theater_camera::get_mode();
			if (cam != theater_camera::THEATER_CAMERA_FREECAM)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
					"Needs FREE camera (it is the only mode the engine routes here).");
				ImGui::SameLine();
				if (ImGui::Button("Free camera"))
				{
					theater_camera::set_mode(theater_camera::THEATER_CAMERA_FREECAM);
				}
			}

			bool on = bonecam::enabled();
			if (ImGui::Checkbox("Lock camera to bone", &on))
			{
				bonecam::set_enabled(on);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(mouse still aims)");

			// ---- who ---------------------------------------------------------
			int ent = bonecam::entity();
			bool own = (ent < 0);
			if (ImGui::Checkbox("Attach to the demo's own player", &own))
			{
				bonecam::set_entity(own ? -1 : 0);
			}
			if (!own)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(90.0f);
				if (ImGui::InputInt("client", &ent))
				{
					bonecam::set_entity(ent < 0 ? 0 : ent);
				}
			}

			// ---- which bone --------------------------------------------------
			const auto list = bonecam::bones();
			ImGui::Separator();
			if (list.empty())
			{
				ImGui::TextDisabled(
					"No bone list yet. The list builds by itself once a player model\n"
					"is on screen - it does NOT need the lock enabled first.\n"
					"Run  bonecam_list  in the console: it names the exact reason.");
			}
			else
			{
				ImGui::Text("Bone %d / %zu:  %s", bonecam::bone_index(), list.size(),
					bonecam::bone_name());

				// Presets first: these are the shots people actually want, and
				// each is a substring match so it works across model naming.
				if (ImGui::SmallButton("Head"))    { bonecam::set_bone_by_name("head"); }
				ImGui::SameLine();
				if (ImGui::SmallButton("L hand"))  { bonecam::set_bone_by_name("wrist_le"); }
				ImGui::SameLine();
				if (ImGui::SmallButton("R hand"))  { bonecam::set_bone_by_name("wrist_ri"); }
				ImGui::SameLine();
				if (ImGui::SmallButton("Weapon"))  { bonecam::set_bone_by_name("tag_weapon"); }
				ImGui::SameLine();
				if (ImGui::SmallButton("Spine"))   { bonecam::set_bone_by_name("spine"); }

				static char filter[64] = "";
				ImGui::SetNextItemWidth(160.0f);
				ImGui::InputText("filter", filter, sizeof(filter));

				if (ImGui::BeginListBox("##bones", ImVec2(-1.0f, 160.0f)))
				{
					for (const auto& b : list)
					{
						if (filter[0] && b.name.find(filter) == std::string::npos)
						{
							continue;
						}
						const bool sel = (b.index == bonecam::bone_index());
						if (ImGui::Selectable(
							std::format("{:3}  {}", b.index, b.name).c_str(), sel))
						{
							bonecam::set_bone_index(b.index);
						}
					}
					ImGui::EndListBox();
				}
			}

			// ---- framing -----------------------------------------------------
			ImGui::Separator();
			float f, r, u;
			bonecam::get_offset(f, r, u);
			bool changed = false;
			changed |= ImGui::SliderFloat("forward", &f, -80.0f, 80.0f, "%.0f");
			changed |= ImGui::SliderFloat("right", &r, -80.0f, 80.0f, "%.0f");
			changed |= ImGui::SliderFloat("up", &u, -80.0f, 80.0f, "%.0f");
			if (changed)
			{
				bonecam::set_offset(f, r, u);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Offset is relative to where you are LOOKING, not the world,\n"
					"so 'forward -20' always pulls back from your own view.");
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Reset offset"))
			{
				bonecam::set_offset(0.0f, 0.0f, 0.0f);
			}

			float sm = bonecam::smoothing();
			if (ImGui::SliderFloat("smoothing", &sm, 0.0f, 0.95f, "%.2f"))
			{
				bonecam::set_smoothing(sm);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"0 = rigid, every animation jolt reaches the camera (rawest look).\n"
					"Higher damps the shake without touching your aim.");
			}

			ImGui::Separator();
			ImGui::TextWrapped("%s", bonecam::status().c_str());
		}

		void draw_dolly_tab()
		{
			// Dual-system, matching what dolly.cpp itself already drives on
			// (camera_ready()/active_time() — see dolly.hpp). Previously this read
			// demo_native directly, so the tab reported "no demo playing" and the
			// Free-camera button did nothing while a .dm_s2 custom-theater demo was
			// running, even though the dolly engine underneath already worked there.
			const bool playing = theater_camera::available();
			const bool native = demo_native::native_playing();
			const theater_camera::camera_mode mode = theater_camera::get_mode();
			// Smooth clock — the same one drive() evaluates on, so "inside the
			// shot" agrees with whether the camera is actually being driven.
			const int now = native
				? demo_native::demo_time_smooth()
				: demo_playback::current_time().value_or(0);

			if (!playing)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
					"No demo playing.");
				ImGui::TextDisabled(
					"Play one from the Demos tab (native or custom). The dolly "
					"stamps its points with the demo's own clock, so it has nothing "
					"to attach to until a demo is running.");
				return;
			}

			// The camera mode is not a detail: free camera is the only mode the
			// engine routes through CL_Demo_FreeCameraMove, which is where the
			// dolly writes. Say so plainly instead of silently doing nothing.
			if (mode == theater_camera::THEATER_CAMERA_FREECAM)
			{
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
					"Free camera (%s)  |  demo time %d.%02ds",
					native ? "native" : "custom theater",
					now / 1000, (now % 1000) / 10);
			}
			else
			{
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
					"Camera is %s (%s) — the dolly needs FREE camera.",
					mode == theater_camera::THEATER_CAMERA_FIRST_PERSON ? "first person"
						: (mode == theater_camera::THEATER_CAMERA_THIRD_PERSON ? "third person" : "unknown"),
					native ? "native" : "custom theater");
				ImGui::SameLine();
				if (ImGui::Button("Free camera"))
				{
					theater_camera::set_mode(theater_camera::THEATER_CAMERA_FREECAM);
				}
				if (!native && !theater_camera::hooks_installed())
				{
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
						"Custom-theater camera hooks failed to install — see "
						"demo_camdiag.");
				}
			}

			// Hotkey legend + a live roll/FOV readout, so a value nudged with the
			// wheel while flying is visible without switching tabs. Roll/FOV
			// themselves live on the Camera tab (demo_camera.hpp) — this is a
			// read-only mirror, not a second copy of the controls.
			ImGui::TextDisabled(
				"K place marker   L clear markers   J play from start   "
				"Shift sprint   Alt slow   Wheel roll   Alt+Wheel FOV");
			if (demo_camera::fov_available())
			{
				ImGui::TextDisabled("FOV %.0f deg   Roll %.1f deg",
					demo_camera::fov(), demo_camera::roll());
			}
			else
			{
				ImGui::TextDisabled("Roll %.1f deg   (FOV hook not installed)",
					demo_camera::roll());
			}

			ImGui::Separator();

			if (ImGui::Button("Add point here", ImVec2(140, 0)))
			{
				dolly::add_point();
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Records the free camera's position and angles, stamped with the\n"
					"current demo time. Fly to a spot, add a point, seek on, add\n"
					"another. Two points within 10 ms of each other replace, rather\n"
					"than stack (a 10 ms segment is a snap, not a move).");
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear all", ImVec2(100, 0)))
			{
				dolly::clear_points();
			}

			const auto pts = dolly::points();

			bool on = dolly::enabled();
			if (ImGui::Checkbox("Drive the camera", &on))
			{
				dolly::set_enabled(on);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"The camera follows the path across the points' own time span,\n"
					"and is FREE outside it — so you can keep flying around to set\n"
					"up more points without fighting it.\n\n"
					"Because the points are stamped on the demo clock, seeking,\n"
					"pausing and timescale all move the dolly correctly.");
			}
			ImGui::SameLine();
			bool markers = dolly::show_markers();
			if (ImGui::Checkbox("Show markers", &markers))
			{
				dolly::set_show_markers(markers);
			}

			if (pts.size() < 2)
			{
				ImGui::TextDisabled("%zu point(s) — needs 2 to interpolate.", pts.size());
			}
			else
			{
				const int span = pts.back().time - pts.front().time;
				const bool inside = now >= pts.front().time && now <= pts.back().time;
				ImGui::Text("%zu points over %.1fs   (%s)", pts.size(),
					static_cast<float>(span) / 1000.0f,
					inside ? "demo is inside the shot" : "demo is outside the shot");
			}

			ImGui::Separator();

			if (ImGui::BeginChild("##dolly_points", ImVec2(0, 220), true))
			{
				for (std::size_t i = 0; i < pts.size(); ++i)
				{
					ImGui::PushID(static_cast<int>(i));
					ImGui::Text("%2zu  %6.2fs  (%7.0f %7.0f %7.0f)  %5.1f/%5.1f  fov %s",
						i + 1, static_cast<float>(pts[i].time) / 1000.0f,
						pts[i].pos[0], pts[i].pos[1], pts[i].pos[2],
						pts[i].angles[0], pts[i].angles[1],
						pts[i].fov > 0.0f ? std::format("{:.0f}", pts[i].fov).c_str() : "-");
					ImGui::SameLine();
					if (ImGui::SmallButton("Go"))
					{
						// Seek whichever demo system is playing to this point.
						// dolly routes it: native keyframe jump / realtime skip, or
						// the custom theater's absolute seek.
						dolly::go_to_point(static_cast<int>(i));
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("Retime"))
					{
						// Same clock the point was stamped on -- demo_time_smooth()
						// is native-only and reads nothing useful in the theater.
						const int t = dolly::current_time();
						if (t >= 0)
						{
							dolly::retime_point(static_cast<int>(i), t);
						}
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("X"))
					{
						dolly::delete_point(static_cast<int>(i));
					}
					ImGui::PopID();
				}
				if (pts.empty())
				{
					ImGui::TextDisabled("No points yet. Fly the free camera somewhere "
						"and press \"Add point here\".");
				}
			}
			ImGui::EndChild();

			ImGui::TextDisabled(
				"Markers and the path are drawn by the game, not by this window — "
				"projected with the engine's own CG_WorldPosToScreenPos, so they "
				"scale correctly with FOV and resolution.");
		}

		// ---- DISPLAY ------------------------------------------------------
		// Field of view and frame-rate cap. Neither is gated on a demo
		// playing -- FOV is a plain dvar (demo_camera::fov()/set_fov()) and
		// the fps cap works in live play exactly as it does during
		// playback, so this is a top-level tab rather than a Demos section.
		void draw_display_tab()
		{
			ImGui::TextWrapped(
				"General video settings. These apply everywhere -- live play "
				"and demo playback alike.");
			ImGui::Spacing();

			// ---- FOV ----------------------------------------------------
			ImGui::SeparatorText("Field of View");
			{
				// LIVE-PLAY fov: the game's own cg_fov, which the engine only
				// accepts between 50 and 100. Demo/cinematic FOV has no such limit
				// and lives in the Demos tab (Framing) and on dolly points.
				const float f = demo_camera::game_fov();
				if (f > 0.0f)
				{
					float v = f;
					ImGui::SetNextItemWidth(260.0f);
					if (ImGui::SliderFloat("Degrees##fov2", &v, 50.0f, 100.0f, "%.0f"))
					{
						demo_camera::set_game_fov(v);
					}
					ImGui::SameLine();
					if (ImGui::Button("65"))  { demo_camera::set_game_fov(65.0f); }
					ImGui::SameLine();
					if (ImGui::Button("80"))  { demo_camera::set_game_fov(80.0f); }
					ImGui::SameLine();
					if (ImGui::Button("90"))  { demo_camera::set_game_fov(90.0f); }
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("65 is the game default. The game refuses values\n"
							"outside 50-100 here; for cinematic FOV in demos use\n"
							"Demos > Framing, which has no limit.");
					}
				}
				else
				{
					ImGui::TextDisabled("cg_fov not readable yet -- get to a match or a "
						"demo first.");
				}
			}

			// ---- FRAME RATE -----------------------------------------------
			ImGui::Spacing();
			ImGui::SeparatorText("Frame Rate Cap");
			{
				const int cur = demo_display::fps_cap();
				if (cur < 0)
				{
					ImGui::TextDisabled("com_maxfps not readable yet.");
				}
				else
				{
					static bool s_unlock = cur > 250;
					// Follow the engine's own value if something else changed
					// it while the checkbox was off, rather than fighting it.
					if (!s_unlock && cur > 250)
					{
						s_unlock = true;
					}

					int v = cur;
					ImGui::SetNextItemWidth(260.0f);
					if (!s_unlock)
					{
						if (ImGui::SliderInt("FPS##fps2", &v, 0, 250, v == 0 ? "uncapped" : "%d"))
						{
							demo_display::set_fps_cap(v);
						}
					}
					else
					{
						if (ImGui::SliderInt("FPS##fps2", &v, 30, 1000, "%d"))
						{
							demo_display::set_fps_cap(v);
						}
					}

					ImGui::SameLine();
					if (ImGui::Checkbox("Unlock past 250", &s_unlock))
					{
						// Snap to something sane on the way in/out so the
						// slider does not silently sit at an out-of-range
						// value for the mode it is now in.
						if (s_unlock && cur <= 250)
						{
							demo_display::set_fps_cap((std::max)(cur, 250));
						}
						else if (!s_unlock && cur > 250)
						{
							demo_display::set_fps_cap(250);
						}
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip(
							"com_maxfps is registered with a hard 0..250 domain, so the\n"
							"console command and the stock slider both clamp there. This\n"
							"writes the dvar's raw value directly to get past it -- the\n"
							"same technique already used for the free-camera speed and\n"
							"third-person framing.\n\n"
							"0 (only reachable below 250) is the engine's own uncapped.");
					}

					ImGui::Spacing();
					ImGui::TextUnformatted("Presets:");
					ImGui::SameLine();
					if (ImGui::SmallButton("60"))   { demo_display::set_fps_cap(60); }
					ImGui::SameLine();
					if (ImGui::SmallButton("144"))  { demo_display::set_fps_cap(144); }
					ImGui::SameLine();
					if (ImGui::SmallButton("240"))  { demo_display::set_fps_cap(240); }
					ImGui::SameLine();
					if (ImGui::SmallButton("500"))  { demo_display::set_fps_cap(500); }
					ImGui::SameLine();
					if (ImGui::SmallButton("Uncapped")) { demo_display::set_fps_cap(0); }
				}
			}
		}

		void draw_ui()
		{
			if (g_open)
			{
				ImGui::Begin("S2MP Tools", &g_open);

				// A quiet banner rather than a rename of the window itself --
				// the bold font is used here and nowhere else, so a missing
				// font file just means the line falls back to body weight
				// instead of anything breaking.
				if (g_font_bold) { ImGui::PushFont(g_font_bold); }
				ImGui::TextColored(ImVec4(0.827f, 0.667f, 0.353f, 1.0f), "S2MP FIELD OPERATIONS");
				if (g_font_bold) { ImGui::PopFont(); }
				ImGui::Separator();

				// The DEFAULT surface is the product: record a demo, play it
				// back, put a camera on it. Everything else in this mod exists
				// because this is a reverse-engineering project, and it only
				// appears in developer mode.
				const bool dev = dev_mode::enabled();

				if (ImGui::BeginTabBar("##s2mp_tabs"))
				{
					if (ImGui::BeginTabItem("Demos"))
					{
						draw_demos_tab();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Display"))
					{
						draw_display_tab();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Dolly"))
					{
						draw_dolly_tab();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Bone Cam"))
					{
						draw_bonecam_tab();
						ImGui::EndTabItem();
					}
					if (dev && ImGui::BeginTabItem("Servers"))
					{
						draw_servers_tab();
						ImGui::EndTabItem();
					}
					if (dev && ImGui::BeginTabItem("Host"))
					{
						draw_host_tab();
						ImGui::EndTabItem();
					}
					if (dev && ImGui::BeginTabItem("Bots"))
					{
						draw_bots_tab();
						ImGui::EndTabItem();
					}
					if (dev && ImGui::BeginTabItem("Players"))
					{
						draw_players_tab();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}

				// One line, at the bottom, so the extra machinery is
				// discoverable without being in the way.
				ImGui::Separator();
				{
					bool on = dev;
					if (ImGui::Checkbox("Developer mode", &on))
					{
						GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
							on ? "s2_dev 1" : "s2_dev 0");
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip(
							"Shows the hosting, matchmaking, bot and player tabs, and\n"
							"registers every diagnostic console command.\n\n"
							"Nothing is removed when this is off - it is only hidden.\n"
							"Console: s2_dev 1");
					}
					ImGui::SameLine();
					ImGui::TextDisabled("F9 / Insert: this window    F2: timeline");
				}
				ImGui::End();
			}

			// ---- NATIVE playback timeline (F1) ------------------------------
			// The custom-theater timeline below is driven by demo time. Native
			// playback has no equivalent clock exposed, so this one is driven by
			// the engine's own FILE cursor, which is a real position.
			//
			// It also reports the KEYFRAME RING, because that is what seeking
			// actually depends on: both engine seek functions return -1 when no
			// slot matches and their callers then do nothing, silently -- so
			// "seek is broken" and "the ring is empty" are indistinguishable
			// from the outside. Showing the ring makes the difference visible.
			if (g_timeline && demo_native::native_playing())
			{
				const float prog = demo_native::playback_progress();
				const auto ring = demo_native::keyframe_ring();
				ImGui::SetNextWindowPos(ImVec2(20, ImGui::GetIO().DisplaySize.y - 78),
					ImGuiCond_Always);
				ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - 40, 58),
					ImGuiCond_Always);
				ImGui::Begin("TimelineNative", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
					| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
					| ImGuiWindowFlags_NoNav);

				// ---- CLICK-TO-SEEK -------------------------------------------
				// The bar spans the SEEKABLE range (first..last keyframe), not
				// the whole demo, because that is honestly all we can jump to:
				// keyframes are written as playback passes, so there is nothing
				// ahead of the furthest point reached.
				const auto sr = demo_native::seek_range();
				char ov[80];
				if (sr.valid && sr.last_ms > sr.first_ms)
				{
					const float span = static_cast<float>(sr.last_ms - sr.first_ms);
					const float here = std::clamp(
						static_cast<float>(sr.now_ms - sr.first_ms) / span, 0.0f, 1.0f);
					std::snprintf(ov, sizeof(ov), "%.1fs / %.1fs  (%d keyframes)",
						(sr.now_ms - sr.first_ms) / 1000.0, span / 1000.0, sr.keyframes);
					ImGui::ProgressBar(here, ImVec2(-FLT_MIN, 16.0f), ov);
					// Click anywhere on the bar to jump there.
					if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
					{
						const ImVec2 mn = ImGui::GetItemRectMin();
						const ImVec2 mx = ImGui::GetItemRectMax();
						const float w = (std::max)(1.0f, mx.x - mn.x);
						const float f = std::clamp(
							(ImGui::GetIO().MousePos.x - mn.x) / w, 0.0f, 1.0f);
						demo_native::seek_to_time(
							sr.first_ms + static_cast<int>(f * span));
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Click to seek. The bar covers the seekable "
							"range (first..last keyframe), which is the ground already\n"
							"played -- keyframes are written as playback passes.");
					}
				}
				else if (prog >= 0.0f)
				{
					std::snprintf(ov, sizeof(ov), "%.0f%% (no keyframes yet)", prog * 100.0f);
					ImGui::ProgressBar(prog, ImVec2(-FLT_MIN, 16.0f), ov);
				}
				else
				{
					ImGui::TextDisabled("position unavailable");
				}

				const bool paused = demo_native::engine_paused();
				ImGui::Text("%s  %.1fx", paused ? "PAUSED" : "playing",
					demo_native::engine_timescale());
				ImGui::SameLine();
				if (!ring.valid)
				{
					ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1),
						"| keyframe ring unreadable - seeking cannot work");
				}
				else if (ring.used == 0 && ring.timed > 0)
				{
					// The decisive case: slots carry timestamps but fail the
					// engine's own test (slot+20 > 0), so every seek skips them.
					ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1),
						"| keyframes: %d timed but 0 ENGINE-VALID (slot+20 == 0) "
						"- seeks skip them all", ring.timed);
				}
				else if (ring.used == 0)
				{
					ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1),
						"| keyframes: 0 - nothing in the ring at all");
				}
				else
				{
					ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
						"| keyframes: %d valid / %d timed  (t %d..%d, idx %d)",
						ring.used, ring.timed, ring.min_time, ring.max_time,
						ring.current_index);
				}
				ImGui::SameLine();
				ImGui::TextDisabled("| F1 hides this");
				ImGui::End();
			}

			if (g_timeline && demo_playback::is_playing())
			{
				const auto bounds = demo_playback::time_bounds();
				const auto cur = demo_playback::current_time();
				if (bounds && cur)
				{
					const int span = (std::max)(1, bounds->second - bounds->first);
					const float t = static_cast<float>(*cur - bounds->first) / static_cast<float>(span);
					ImGui::SetNextWindowPos(ImVec2(20, ImGui::GetIO().DisplaySize.y - 60), ImGuiCond_Always);
					ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - 40, 40), ImGuiCond_Always);
					ImGui::Begin("Timeline", nullptr,
						ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove);
					ImGui::ProgressBar(t, ImVec2(-1, 0));
					ImGui::End();
				}
			}
		}

		// GetAsyncKeyState reports physical key state process-wide, so every hotkey here
		// must be gated manually or it fires while the game is alt-tabbed, while a LUI
		// menu or the console owns input, or while the user is typing in our own UI.
		bool game_has_focus()
		{
			const HWND fg = GetForegroundWindow();
			return fg && g_hwnd && fg == g_hwnd;
		}

		bool imgui_wants_keyboard()
		{
			return g_imgui_ready && ImGui::GetCurrentContext()
				&& ImGui::GetIO().WantCaptureKeyboard;
		}

		// WantTextInput is true ONLY while a text field has focus. WantCaptureKeyboard
		// is much broader: ConfigFlags_NavEnableKeyboard (set in init_imgui) makes it
		// true for as long as any of our windows is focused, which is essentially the
		// whole time the UI is open. Gating the window toggles on it meant F9 could not
		// close the window until you first clicked the game behind it.
		bool imgui_wants_text_input()
		{
			return g_imgui_ready && ImGui::GetCurrentContext()
				&& ImGui::GetIO().WantTextInput;
		}

		// Window toggles are allowed with a menu up (that is how you get out of one),
		// but never when unfocused or while typing into an ImGui field. "Typing" is
		// WantTextInput â€” see above; using WantCaptureKeyboard here was the bug.
		bool ui_hotkeys_allowed()
		{
			return game_has_focus() && !imgui_wants_text_input();
		}

		// Playback transport keys additionally require that nothing else owns the
		// keyboard. Accepts EITHER playback system: the custom theater or the
		// engine's own native playback. Previously this required
		// demo_playback::is_active_replay(), so during a native cl_demo_play the
		// timeline key and the transport keys all did nothing -- which is exactly
		// the "I pressed it and nothing appeared" symptom.
		bool transport_hotkeys_allowed()
		{
			return ui_hotkeys_allowed()
				&& !g_open
				&& demo_game::key_catchers() == 0
				&& (demo_playback::is_active_replay() || demo_native::native_playing());
		}

		bool edge(const int vk, bool& state)
		{
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool pressed = down && !state;
			state = down;
			return pressed;
		}

		void handle_hotkeys_present_only()
		{
			// Always sample so a key held down across a focus change cannot be seen as a
			// fresh press when focus returns.
			const bool f9 = edge(VK_F9, g_f9_edge);
			const bool ins = edge(VK_INSERT, g_insert_edge);
			// TIMELINE IS ON F1, NOT F2.
			// F2 is the engine's own theater camera cycle (CL_Demo_HandleAction
			// action 4/168, first/third/free person). Binding our timeline to the
			// same key meant pressing it fought the camera and, during native
			// playback, appeared to do nothing at all.
			const bool f1 = edge(VK_F1, g_toggle_edge);
			const bool space = edge(VK_SPACE, g_space_edge);
			const bool left = edge(VK_LEFT, g_left_edge);
			const bool right = edge(VK_RIGHT, g_right_edge);
			// Dollycam marker keys: K = drop a point, L = delete the last one,
			// J = play from the first point. Same edge/gate pattern as the
			// transport keys above -- polled every frame regardless of focus so a
			// key held across a focus change is never seen as a fresh press.
			const bool key_k = edge('K', g_k_edge);
			const bool key_l = edge('L', g_l_edge);
			const bool key_j = edge('J', g_j_edge);

			if (ui_hotkeys_allowed())
			{
				if (f9 || ins)
				{
					g_open = !g_open;
					if (g_open)
					{
						demo_player::refresh();
					}
				}
				if (f1)
				{
					g_timeline = !g_timeline;
				}
			}

			// ---- SPACE = pause, and ONLY ONE system may handle it ----------------
			// During NATIVE playback the ENGINE already binds space itself:
			// CL_Demo_HandleAction action 1/32 toggles cl_demo_pause, and our hook
			// on that function passes every action except the blocked one straight
			// through. So when we ALSO toggled pause here, the two cancelled in the
			// same frame and space appeared dead — precisely the regression that
			// showed up "since it was added to the GUI".
			//
			// The engine cannot see the key while our overlay owns game input
			// (key-catcher bit 0), so that is the only case we handle ourselves.
			// The ONLY thing that takes space away from pause/play is the s2mp
			// console (you are typing in it). Our own window being open does not.
			if (space && ui_hotkeys_allowed() && !InternalConsole::DEVONLY_consoleOpen())
			{
				const bool we_own_input = g_open || demo_game::key_catchers() != 0;

				if (demo_playback::is_playing())
				{
					// The custom theater has no engine-side binding, so space is
					// always ours. ⛔ 2026-09-15: this queued `demopause`, which the
					// productisation pass renamed to `demo_pause` -- the command no
					// longer existed, so space did nothing in custom demos. It was
					// also skipped whenever our window or any key catcher was up.
					GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "demo_pause");
				}
				else if (demo_native::native_playing() && we_own_input)
				{
					// Native: the engine toggles pause itself (action 1/32) whenever
					// it can see the key; only when we hold the keyboard must we do
					// it, or the two toggles cancel out.
					demo_native::toggle_pause();
				}
			}

			if (!transport_hotkeys_allowed())
			{
				return;
			}

			// Route to whichever playback system is actually running. The custom
			// theater takes `demopause` and a millisecond seek; native playback
			// goes through the engine's own CL_Demo_HandleAction, which is
			// keyframe-based rather than time-based.
			// Space is handled above, once, by whichever system actually owns it.
			// One seek for both systems. demo_player routes it: native gets a
			// realtime skip forward / keyframe jump back, the custom theater
			// gets an absolute seek. Queued as a command so it lands on the
			// client thread rather than running here on Present.
			if (left != right)
			{
				GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,
					right ? "demo_seek 5" : "demo_seek -10");
			}

			// Dollycam: these touch the point list directly rather than going
			// through Cbuf, exactly like the existing GUI buttons on the Dolly
			// tab -- there is no console command wrapping add/clear/play, so
			// there is nothing to queue.
			if (key_k) { dolly::add_point(); }
			// ⛔ 2026-09-21: L used to CLEAR THE WHOLE LIST. One mis-hit next to K
			// destroyed a path with no undo, and "delete cameras" means removing the
			// one you just placed, not the shot. L now deletes the LAST point, so
			// K/L are a matched place/unplace pair you can tap repeatedly.
			// Clearing everything is still on the Dolly tab's button and `dolly_clear`.
			if (key_l)
			{
				const int n = dolly::point_count();
				if (n > 0)
				{
					dolly::delete_point(n - 1);
				}
				else
				{
					Console::printf("[dolly] L: no points to delete (K adds one)");
				}
			}
			if (key_j) { dolly::play_from_start(); }
		}

		// ---- freeing the mouse while the UI is open ---------------------------
		// PROVEN 2026-08-08 from the S2 decompile. The per-frame mouse update
		// (IDA 0x788220) recentres the cursor every frame:
		//     if (sub_81350(...)) { ClipCursor(clientRect); SetCursorPos(centre); }
		//     else                  ClipCursor(0);
		// and sub_81350 @ IDA 0x81350 opens with
		//     if ((dword_1BAF4E0[0] & 1) != 0) { sub_7888E0(...); return 0; }
		// dword_1BAF4E0 IS the key-catcher word GameUtil::blockGameInput writes
		// (0x1BAE4E0_b == IDA 0x1BAF4E0). So setting bit 0 makes the predicate
		// return 0, which releases the clip and skips the recentre â€” the cursor
		// moves freely. That is the same mechanism the internal console uses.
		//
		// Re-asserted every frame because the engine and LUI both write this word
		// and a map load can drop our bit. On close we only clear it if WE set it
		// and the internal console does not still want it â€” bit 0 is shared.
		bool g_input_captured = false;

		void update_input_capture()
		{
			const bool want = g_imgui_ready && g_open;
			if (want)
			{
				// Idempotent: blockGameInput ORs the bit in.
				GameUtil::blockGameInput(true);
				g_input_captured = true;
				// Draw ImGui's own cursor. The engine decides OS cursor visibility
				// on this path itself (sub_7888E0), and it may well leave it
				// hidden, which would give a free but invisible pointer.
				if (ImGui::GetCurrentContext())
				{
					ImGui::GetIO().MouseDrawCursor = true;
				}
				return;
			}

			if (g_input_captured)
			{
				g_input_captured = false;
				if (ImGui::GetCurrentContext())
				{
					ImGui::GetIO().MouseDrawCursor = false;
				}
				// Never yank input back from the console, which shares bit 0.
				if (!InternalConsole::DEVONLY_consoleOpen())
				{
					GameUtil::blockGameInput(false);
				}
			}
		}

		HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swap, const UINT sync, const UINT flags)
		{
			init_imgui(swap);
			handle_hotkeys_present_only();
			update_input_capture();
			// PROBE ONLY, native playback only: watches the viewmodel `hide` byte
			// so the ~20s gun pop-in can be timed. No-op otherwise.
			demo_native::watch_viewmodel();
			// Ends the native session when the engine does, then starts a demo
			// that was waiting for the previous one to close. Both no-ops normally.
			demo_native::poll_session();
			demo_player::poll_pending();
			// Advances the playlist sweep if one is running. No-op otherwise, and
			// it aborts itself (restoring the playlist) if you enter a match.
			server_browser::tick();
			// Re-asserts DLC-off against the engine's ongoing re-registration.
			dlc::tick();
			// Holds the lobby at the map/gametype/playlist you chose. Compare-then-
			// write, so in steady state it issues nothing at all.
			force_host::tick();
			// Re-asserts the frame-rate cap while it is unlocked past 250.
			// No-op below that -- see demo_display.hpp for why.
			demo_display::tick();

			// ⭐ CAPTURE GOES HERE, AND ONLY HERE. Before the ImGui block below, so
			// the ProRes file contains clean game footage with no tool window in it.
			// Moving this call after the overlay draw would silently start baking
			// the UI into every recording.
			demo_capture::on_present(swap);

			// Skip all drawing while minimised. The back buffer is 0x0 then, so
			// creating a view or issuing draws is wasted at best and an error at
			// worst -- and ImGui asserts on a zero display size.
			const bool minimised = g_hwnd && IsIconic(g_hwnd);
			if (g_imgui_ready && !minimised)
			{
				ensure_render_target(swap);
				ImGui_ImplDX11_NewFrame();
				ImGui_ImplWin32_NewFrame();
				ImGui::NewFrame();
				draw_ui();
				ImGui::Render();
				if (g_rtv)
				{
					g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
					ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
				}
			}

			return g_present_orig(swap, sync, flags);
		}

		HRESULT STDMETHODCALLTYPE resize_buffers_hook(IDXGISwapChain* swap,
			const UINT count, const UINT w, const UINT h, const DXGI_FORMAT fmt,
			const UINT flags)
		{
			// Drop our back-buffer reference BEFORE the resize, or DXGI fails the call.
			// ImGui's DX11 backend also caches the render target, so let it release
			// its device objects too; they are recreated lazily on the next frame.
			release_rtv();
			if (g_imgui_ready)
			{
				ImGui_ImplDX11_InvalidateDeviceObjects();
			}
			const HRESULT hr = g_resize_orig(swap, count, w, h, fmt, flags);
			// Do NOT recreate here: on a minimise the new size is 0x0 and creating a
			// view against it is pointless. present_hook's ensure_render_target picks
			// it up on the next real frame.
			return hr;
		}

		// Both hook paths must install this: dxgi.dll's vtable is shared, so whichever
		// swapchain we read it from, the entry is the same function.
		void install_resize_hook(void* resize_fn)
		{
			if (g_resize_hooked || !resize_fn)
			{
				return;
			}
			g_resize_hooked = Hook::create("DXGI ResizeBuffers", resize_fn,
				reinterpret_cast<void*>(resize_buffers_hook),
				reinterpret_cast<void**>(&g_resize_orig));
			Console::printf(
				"[demo] ResizeBuffers hook: %s (releases our back-buffer view so the "
				"game can resize on minimise)",
				(g_resize_hooked && g_resize_orig) ? "OK" : "FAILED");
		}

		bool install_present_hook(void* present_fn)
		{
			if (g_present_hooked || !present_fn)
			{
				return g_present_hooked;
			}
			g_present_target = present_fn;
			if (!Hook::create("DXGI Present", g_present_target,
				reinterpret_cast<void*>(present_hook),
				reinterpret_cast<void**>(&g_present_orig)))
			{
				Console::printf("[demo] Present hook FAILED at %p", present_fn);
				return false;
			}
			g_present_hooked = true;
			Console::printf("[demo] hooked IDXGISwapChain::Present @ %p", present_fn);
			return true;
		}

		void hook_present_from_swap(IDXGISwapChain* swap)
		{
			if (!swap || g_present_hooked)
			{
				return;
			}
			void** vtable = *reinterpret_cast<void***>(swap);
			// IDXGISwapChain vtable: 8 = Present, 13 = ResizeBuffers.
			install_present_hook(vtable[8]);
			install_resize_hook(vtable[13]);
		}

		// Game already created its swapchain before demo::init â€” steal Present from a temporary
		// DXGI swapchain (same dxgi.dll vtable the game uses).
		bool hook_present_via_dummy()
		{
			if (g_present_hooked)
			{
				return true;
			}

			WNDCLASSEXA wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = DefWindowProcA;
			wc.hInstance = GetModuleHandleA(nullptr);
			wc.lpszClassName = "S2MPDemoDxDummy";
			RegisterClassExA(&wc);

			HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
				0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
			if (!hwnd)
			{
				Console::printf("[demo] dummy HWND create failed");
				return false;
			}

			DXGI_SWAP_CHAIN_DESC sd{};
			sd.BufferCount = 1;
			sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.OutputWindow = hwnd;
			sd.SampleDesc.Count = 1;
			sd.Windowed = TRUE;
			sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

			IDXGISwapChain* swap = nullptr;
			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* ctx = nullptr;
			D3D_FEATURE_LEVEL fl{};
			const HRESULT hr = D3D11CreateDeviceAndSwapChain(
				nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
				D3D11_SDK_VERSION, &sd, &swap, &device, &fl, &ctx);

			bool ok = false;
			if (SUCCEEDED(hr) && swap)
			{
				void** vtable = *reinterpret_cast<void***>(swap);
				ok = install_present_hook(vtable[8]);
				install_resize_hook(vtable[13]);
			}
			else
			{
				Console::printf("[demo] dummy D3D11CreateDeviceAndSwapChain failed (hr=0x%08X)",
					static_cast<unsigned>(hr));
			}

			if (ctx) ctx->Release();
			if (device) device->Release();
			if (swap) swap->Release();
			DestroyWindow(hwnd);
			UnregisterClassA(wc.lpszClassName, wc.hInstance);
			return ok;
		}

		using CreateSwapChain_t = HRESULT(WINAPI*)(
			IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
		CreateSwapChain_t g_create_sc_orig = nullptr;

		HRESULT WINAPI create_swap_chain_hook(IDXGIFactory* factory, IUnknown* device,
			DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap)
		{
			const HRESULT hr = g_create_sc_orig(factory, device, desc, swap);
			if (SUCCEEDED(hr) && swap && *swap)
			{
				hook_present_from_swap(*swap);
			}
			return hr;
		}

		void try_hook_create_swap_chain()
		{
			const HMODULE dxgi = GetModuleHandleA("dxgi.dll");
			if (!dxgi)
			{
				return;
			}
			const auto create_factory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
				GetProcAddress(dxgi, "CreateDXGIFactory"));
			if (!create_factory)
			{
				return;
			}
			IDXGIFactory* factory = nullptr;
			if (FAILED(create_factory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) || !factory)
			{
				return;
			}
			void** vt = *reinterpret_cast<void***>(factory);
			Hook::create("CreateSwapChain", vt[10],
				reinterpret_cast<void*>(create_swap_chain_hook),
				reinterpret_cast<void**>(&g_create_sc_orig));
			factory->Release();
		}

		void cmd_demo_ui()
		{
			g_open = !g_open;
			Console::printf("[demo] UI %s (needs Present hook â€” F9 also works once hooked)",
				g_open ? "open" : "closed");
		}
	}

	void init()
	{
		try_hook_create_swap_chain();
		if (!hook_present_via_dummy())
		{
			Console::printf("[demo] Present not hooked yet â€” UI will stay dead until a swapchain is seen");
		}
		GameUtil::addCommand("demo_ui", cmd_demo_ui);
		GameUtil::addCommand("demoui", cmd_demo_ui);
	}

	void toggle()
	{
		g_open = !g_open;
	}
}
