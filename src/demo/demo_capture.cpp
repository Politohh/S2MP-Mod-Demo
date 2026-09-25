#include "pch.h"
#include "demo/demo_capture.hpp"

#include "Console.hpp"
#include "GameUtil.hpp"
#include "demo/demo_utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>

namespace demo_capture
{
	namespace
	{
		namespace fs = std::filesystem;

		// ---- settings ----------------------------------------------------
		int g_fps = 60;
		int g_profile = 3;              // ffmpeg prores_ks: 3 = 422 HQ
		std::string g_ffmpeg_override;  // demo_capture_ffmpeg <path>

		// ---- live state --------------------------------------------------
		// `g_recording` is the ONE flag on_present() tests every frame, so it is
		// atomic and everything else is only touched while it is false, or under
		// g_queue_lock. That keeps the not-recording case to a single load.
		std::atomic<bool> g_recording{ false };
		std::atomic<bool> g_post_effect_capture{ false };
		std::atomic<ULONGLONG> g_last_post_effect_frame{ 0 };

		HANDLE g_pipe_write = nullptr;
		HANDLE g_ffmpeg_process = nullptr;
		ID3D11Texture2D* g_staging = nullptr;   // CPU-readable copy target
		ID3D11Texture2D* g_resolve = nullptr;   // only when the back buffer is MSAA
		ID3D11Device* g_device = nullptr;
		ID3D11DeviceContext* g_context = nullptr;

		UINT g_width = 0;
		UINT g_height = 0;
		std::size_t g_frame_bytes = 0;
		fs::path g_out_path;
		// Resolved once in start(), on the client thread, and used on the first
		// presented frame. Resolving it again on the Present thread would mean a
		// filesystem probe inside the render path for no reason.
		fs::path g_ffmpeg_path;
		std::atomic<std::uint64_t> g_frames{ 0 };
		std::atomic<std::uint64_t> g_dropped{ 0 };
		std::string g_stop_reason;

		// Present/ReShade callback timestamps are measured before GPU readback.
		// The rawvideo pipe assigns exactly 1/g_fps seconds to every accepted
		// frame, so source cadence matters even when ffmpeg drops nothing.
		struct timing_stats
		{
			std::uint64_t callbacks = 0;
			std::uint64_t short_intervals = 0;
			std::uint64_t long_intervals = 0;
			std::uint64_t very_long_intervals = 0;
			std::uint64_t blocked_frames = 0;
			std::size_t max_queue_depth = 0;
			double first_ms = 0.0;
			double last_ms = 0.0;
			double max_interval_ms = 0.0;
			double max_readback_ms = 0.0;
			double max_queue_wait_ms = 0.0;
		};
		std::mutex g_timing_lock;
		timing_stats g_timing;

		[[nodiscard]] double monotonic_ms()
		{
			return std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void note_callback(const double now)
		{
			std::lock_guard<std::mutex> lock(g_timing_lock);
			const double frame_ms = 1000.0 / (g_fps > 0 ? g_fps : 60);
			if (g_timing.callbacks == 0)
				g_timing.first_ms = now;
			else
			{
				const double interval = now - g_timing.last_ms;
				if (interval < frame_ms * 0.5) ++g_timing.short_intervals;
				if (interval > frame_ms * 1.5) ++g_timing.long_intervals;
				if (interval > frame_ms * 2.5) ++g_timing.very_long_intervals;
				g_timing.max_interval_ms = (std::max)(g_timing.max_interval_ms, interval);
			}
			g_timing.last_ms = now;
			++g_timing.callbacks;
		}

		// ---- the pipe queue ----------------------------------------------
		// The Present thread does the GPU read-back (it has to -- that is where the
		// swap chain is), then hands the bytes over. Writing to a pipe can block
		// for as long as ffmpeg takes to encode, and doing that on Present would
		// stutter the game, so a worker owns the pipe.
		//
		// The queue is BOUNDED. Unbounded would turn "ffmpeg is slower than the
		// game" into an out-of-memory crash an hour into a session; at 1080p a
		// single frame is 8 MB, so the cap is memory, not politeness.
		constexpr std::size_t MAX_QUEUED = 8;

		std::mutex g_queue_lock;
		std::condition_variable g_queue_cv;
		std::deque<std::vector<std::uint8_t>> g_queue;
		bool g_writer_quit = false;
		std::thread g_writer;

		void writer_main()
		{
			for (;;)
			{
				std::vector<std::uint8_t> frame;
				{
					std::unique_lock<std::mutex> lock(g_queue_lock);
					g_queue_cv.wait(lock, [] { return g_writer_quit || !g_queue.empty(); });
					if (g_queue.empty())
					{
						if (g_writer_quit)
						{
							return;
						}
						continue;
					}
					frame = std::move(g_queue.front());
					g_queue.pop_front();
				}
				g_queue_cv.notify_all();   // a blocked producer may now have room

				const std::uint8_t* p = frame.data();
				std::size_t left = frame.size();
				while (left > 0)
				{
					DWORD wrote = 0;
					if (!WriteFile(g_pipe_write, p, static_cast<DWORD>(left), &wrote, nullptr)
						|| wrote == 0)
					{
						// ffmpeg exited or the pipe broke. Stop cleanly rather than
						// spinning on a dead handle for the rest of the session.
						g_recording.store(false, std::memory_order_relaxed);
						g_stop_reason = "the ffmpeg pipe closed (ffmpeg exited early -- "
							"the .ffmpeg.log beside the .mov says why)";
						return;
					}
					p += wrote;
					left -= wrote;
				}
			}
		}

		// ---- ffmpeg ------------------------------------------------------
		[[nodiscard]] fs::path find_ffmpeg()
		{
			if (!g_ffmpeg_override.empty())
			{
				return fs::path(g_ffmpeg_override);
			}
			// Next to the game executable first: that is where a mod's own
			// companion tools belong, and it is the copy the user controls.
			wchar_t exe[MAX_PATH]{};
			if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
			{
				const fs::path beside = fs::path(exe).parent_path() / L"ffmpeg.exe";
				std::error_code ec;
				if (fs::exists(beside, ec))
				{
					return beside;
				}
			}
			// Then PATH, so an existing install just works.
			wchar_t found[MAX_PATH]{};
			if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr))
			{
				return fs::path(found);
			}
			return {};
		}

		// ProRes wants a 10-bit planar format; 4444 additionally carries alpha.
		[[nodiscard]] const char* pix_fmt_for_profile(const int profile)
		{
			return (profile >= 4) ? "yuva444p10le" : "yuv422p10le";
		}

		[[nodiscard]] const char* profile_name(const int profile)
		{
			switch (profile)
			{
			case 0:  return "Proxy";
			case 1:  return "LT";
			case 2:  return "422";
			case 3:  return "422 HQ";
			case 4:  return "4444";
			case 5:  return "4444 XQ";
			default: return "?";
			}
		}

		// The back buffer's own layout decides what ffmpeg is told to expect.
		// Guessing here would produce a file with the colours swapped, which is
		// exactly the kind of bug that is invisible until you are in the edit.
		[[nodiscard]] const char* raw_pixel_format(const DXGI_FORMAT fmt)
		{
			switch (fmt)
			{
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return "bgra";
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return "rgba";
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				// DXGI packs R in bits 0..9, G in 10..19, B in 20..29.
				// FFmpeg's x2bgr10le reads those same bits in that order, so the
				// existing row-pitch copy preserves all 10 bits without conversion.
				// The top two alpha bits are unused by 422 ProRes.
				return "x2bgr10le";
			default:
				return nullptr;
			}
		}

		void release_resources()
		{
			if (g_staging) { g_staging->Release(); g_staging = nullptr; }
			if (g_resolve) { g_resolve->Release(); g_resolve = nullptr; }
			if (g_context) { g_context->Release(); g_context = nullptr; }
			if (g_device) { g_device->Release(); g_device = nullptr; }
		}

		// Tears everything down in the one correct order. Called by stop() and by
		// every failure path in start(), so a half-built capture never leaks a
		// process, a pipe or a texture.
		void shutdown_pipeline()
		{
			if (g_writer.joinable())
			{
				{
					std::lock_guard<std::mutex> lock(g_queue_lock);
					g_writer_quit = true;
				}
				g_queue_cv.notify_all();
				g_writer.join();
			}
			{
				std::lock_guard<std::mutex> lock(g_queue_lock);
				g_queue.clear();
				g_writer_quit = false;
			}

			// CLOSING THE WRITE END IS WHAT ENDS THE FILE. ffmpeg reads until EOF,
			// then writes the MOV index and exits; killing it instead would leave an
			// unplayable file. So: close, then wait.
			if (g_pipe_write)
			{
				CloseHandle(g_pipe_write);
				g_pipe_write = nullptr;
			}
			if (g_ffmpeg_process)
			{
				if (WaitForSingleObject(g_ffmpeg_process, 30000) == WAIT_TIMEOUT)
				{
					Console::printf("[capture] ffmpeg did not finish within 30 s -- the file "
						"may be truncated. Leaving it running to finish on its own.");
				}
				CloseHandle(g_ffmpeg_process);
				g_ffmpeg_process = nullptr;
			}
			release_resources();
		}

		[[nodiscard]] bool spawn_ffmpeg(const fs::path& ffmpeg, const char* in_pix_fmt)
		{
			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;

			HANDLE read_end = nullptr;
			if (!CreatePipe(&read_end, &g_pipe_write, &sa, 0))
			{
				Console::printf("[capture] could not create the pipe (%lu)", GetLastError());
				return false;
			}
			// The child gets the READ end only. Leaving our write end inheritable
			// means ffmpeg holds a handle to it too, and then it never sees EOF
			// when we close ours -- the classic pipe hang.
			SetHandleInformation(g_pipe_write, HANDLE_FLAG_INHERIT, 0);

			const auto cmd = std::format(
				"\"{}\" -hide_banner -loglevel error -y "
				"-f rawvideo -pixel_format {} -video_size {}x{} -framerate {} -i - "
				"-an -c:v prores_ks -profile:v {} -vendor apl0 -pix_fmt {} \"{}\"",
				ffmpeg.string(), in_pix_fmt, g_width, g_height, g_fps,
				g_profile, pix_fmt_for_profile(g_profile), g_out_path.string());

			// ffmpeg's own complaints are the ONLY way to tell a bad argument from a
			// bad pipe, and this is a windowless child in a GUI process -- its
			// stderr would go to a console that does not exist. Send it to a log
			// beside the .mov so "the pipe closed" always has a reason on disk.
			// A failure to open the log is not a failure to record: we carry on
			// with no redirection rather than refusing to capture over a log file.
			const auto log_path = fs::path(g_out_path).replace_extension(".ffmpeg.log");
			HANDLE log = CreateFileW(log_path.c_str(), FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (log == INVALID_HANDLE_VALUE)
			{
				log = nullptr;
			}

			STARTUPINFOA si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdInput = read_end;
			si.hStdOutput = log;
			si.hStdError = log;

			PROCESS_INFORMATION pi{};
			std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
			mutable_cmd.push_back('\0');

			const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr,
				TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

			// Ours regardless: the child has its own copy now, and holding these
			// open would keep the pipe alive past ffmpeg's exit.
			CloseHandle(read_end);
			if (log)
			{
				CloseHandle(log);
			}

			if (!ok)
			{
				Console::printf("[capture] could not start ffmpeg (%lu): %s",
					GetLastError(), ffmpeg.string().c_str());
				CloseHandle(g_pipe_write);
				g_pipe_write = nullptr;
				return false;
			}
			CloseHandle(pi.hThread);
			g_ffmpeg_process = pi.hProcess;
			return true;
		}

		// ---- commands ----------------------------------------------------
		void cmd_start()
		{
			const auto* args = GameUtil::getCmdArgs();
			std::string name;
			if (args && args->argc[args->nesting] >= 2)
			{
				name = args->argv[args->nesting][1];
			}
			start(name);
		}

		void cmd_stop() { stop(); }

		void cmd_toggle()
		{
			if (g_recording.load(std::memory_order_relaxed))
			{
				stop();
				return;
			}
			Console::printf("%s", status().c_str());
		}

		void cmd_fps()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[capture] output rate: %d fps   (demo_capture_fps <1..240>)",
					g_fps);
				return;
			}
			if (g_recording.load(std::memory_order_relaxed))
			{
				Console::printf("[capture] stop the recording before changing the rate -- "
					"it is baked into the file's header at start.");
				return;
			}
			const int requested = GameUtil::safeStringToInt(args->argv[args->nesting][1]);
			g_fps = std::clamp(requested, 1, 240);
			Console::printf("[capture] output rate: %d fps%s", g_fps,
				requested > 240 ? " (limit 240; this records presented frames, not frame-locked avidemo)" : "");
		}

		void cmd_profile()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				Console::printf("[capture] ProRes profile: %d (%s)   "
					"(demo_capture_profile <0 proxy|1 LT|2 422|3 HQ|4 4444|5 4444XQ>)",
					g_profile, profile_name(g_profile));
				return;
			}
			if (g_recording.load(std::memory_order_relaxed))
			{
				Console::printf("[capture] stop the recording before changing the profile.");
				return;
			}
			g_profile = std::clamp(GameUtil::safeStringToInt(args->argv[args->nesting][1]), 0, 5);
			Console::printf("[capture] ProRes profile: %d (%s)", g_profile, profile_name(g_profile));
		}

		void cmd_ffmpeg()
		{
			const auto* args = GameUtil::getCmdArgs();
			if (!args || args->argc[args->nesting] < 2)
			{
				const auto found = find_ffmpeg();
				Console::printf("[capture] ffmpeg: %s   (demo_capture_ffmpeg <full path>)",
					found.empty() ? "NOT FOUND -- put ffmpeg.exe next to the game exe, "
					"or on PATH, or set it here" : found.string().c_str());
				return;
			}
			g_ffmpeg_override = args->argv[args->nesting][1];
			std::error_code ec;
			Console::printf("[capture] ffmpeg: %s%s", g_ffmpeg_override.c_str(),
				fs::exists(g_ffmpeg_override, ec) ? "" : "   ⚠ that path does not exist");
		}

		void cmd_status() { Console::printf("%s", status().c_str()); }
	}

	// =====================================================================

	bool recording() { return g_recording.load(std::memory_order_relaxed); }
	std::uint64_t frame_count() { return g_frames.load(); }
	std::uint64_t dropped_count() { return g_dropped.load(); }

	std::string status()
	{
		if (!g_recording.load(std::memory_order_relaxed))
		{
			const auto ff = find_ffmpeg();
			return std::format(
				"[capture] idle | {} fps | ProRes {} | ffmpeg {}{}   "
				"(demo_capture_start [name])",
				g_fps, profile_name(g_profile),
				ff.empty() ? "NOT FOUND" : ff.string(),
				g_stop_reason.empty() ? "" : std::format(" | last stop: {}", g_stop_reason));
		}
		return std::format(
			"[capture] RECORDING {}x{} @ {} fps, ProRes {} | {} frames{} | {}",
			g_width, g_height, g_fps, profile_name(g_profile), g_frames.load(),
			g_dropped.load() ? std::format(", {} DROPPED (encoder behind)", g_dropped.load()) : "",
			g_out_path.string());
	}

	bool start(const std::string& name)
	{
		if (g_recording.load(std::memory_order_relaxed))
		{
			Console::printf("[capture] already recording -- demo_capture_stop first");
			return false;
		}

		const auto ffmpeg = find_ffmpeg();
		if (ffmpeg.empty())
		{
			Console::printf("[capture] ffmpeg.exe not found. Put it next to the game "
				"executable, or on PATH, or set it with demo_capture_ffmpeg <path>.");
			return false;
		}

		// Output lands beside the demos, in its own folder -- captures are large
		// and mixing them into the demo list would be unkind to the library.
		fs::path dir;
		if (const auto d = demo_utils::demos_directory())
		{
			dir = *d / "captures";
		}
		else
		{
			wchar_t exe[MAX_PATH]{};
			if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
			{
				Console::printf("[capture] could not work out where to write the file");
				return false;
			}
			dir = fs::path(exe).parent_path() / "captures";
		}
		std::error_code ec;
		fs::create_directories(dir, ec);

		std::string base = name;
		if (base.empty())
		{
			const auto now = std::chrono::system_clock::now();
			base = std::format("capture_{:%Y-%m-%d_%H%M%S}",
				std::chrono::floor<std::chrono::seconds>(now));
		}
		g_out_path = dir / (base + ".mov");

		g_ffmpeg_path = ffmpeg;
		g_stop_reason.clear();
		g_frames = 0;
		g_dropped = 0;
		{
			std::lock_guard<std::mutex> lock(g_timing_lock);
			g_timing = {};
		}
		g_recording.store(true, std::memory_order_relaxed);

		Console::printf("[capture] armed: %s", g_out_path.string().c_str());
		Console::printf("[capture]   the first presented frame sizes the file -- "
			"do not resize the window while recording.");
		return true;
	}

	void stop()
	{
		if (!g_recording.exchange(false, std::memory_order_relaxed)
			&& !g_ffmpeg_process && !g_writer.joinable())
		{
			Console::printf("[capture] not recording");
			return;
		}
		const auto frames = g_frames.load();
		const auto dropped = g_dropped.load();
		const auto path = g_out_path;

		Console::printf("[capture] finishing -- waiting for ffmpeg to close the file...");
		shutdown_pipeline();

		std::error_code ec;
		const auto size = fs::file_size(path, ec);
		Console::printf("[capture] wrote %llu frames (%.1f s at %d fps)%s",
			static_cast<unsigned long long>(frames),
			static_cast<double>(frames) / (g_fps > 0 ? g_fps : 60), g_fps,
			dropped ? std::format(", {} dropped", dropped).c_str() : "");
		timing_stats timing;
		{
			std::lock_guard<std::mutex> lock(g_timing_lock);
			timing = g_timing;
		}
		if (timing.callbacks > 1 && timing.last_ms > timing.first_ms)
		{
			const double elapsed = (timing.last_ms - timing.first_ms) / 1000.0;
			Console::printf("[capture] source cadence: %llu callbacks / %.2f s "
				"(%.1f fps observed); output %.2f s; intervals <half-frame %llu, "
				">1.5 frames %llu, >2.5 frames %llu, max %.1f ms",
				static_cast<unsigned long long>(timing.callbacks), elapsed,
				static_cast<double>(timing.callbacks - 1) / elapsed,
				static_cast<double>(frames) / (g_fps > 0 ? g_fps : 60),
				static_cast<unsigned long long>(timing.short_intervals),
				static_cast<unsigned long long>(timing.long_intervals),
				static_cast<unsigned long long>(timing.very_long_intervals),
				timing.max_interval_ms);
			Console::printf("[capture] pipeline: max readback+copy %.1f ms, "
				"max queue wait %.1f ms, waits >1 frame %llu, queue peak %zu/%zu",
				timing.max_readback_ms, timing.max_queue_wait_ms,
				static_cast<unsigned long long>(timing.blocked_frames),
				timing.max_queue_depth, MAX_QUEUED);
		}
		Console::printf("[capture] %s%s", path.string().c_str(),
			ec ? "   ⚠ could not stat the file" :
			std::format("   ({:.1f} MB)", static_cast<double>(size) / (1024.0 * 1024.0)).c_str());
	}

	void capture_texture(ID3D11Texture2D* back)
	{
		if (!g_recording.load(std::memory_order_relaxed) || !back)
		{
			if (back) back->Release();
			return;
		}
		const double callback_ms = monotonic_ms();
		note_callback(callback_ms);

		D3D11_TEXTURE2D_DESC desc{};
		back->GetDesc(&desc);

		// ---- first frame: size everything from the back buffer itself ----
		if (!g_staging)
		{
			const char* pix = raw_pixel_format(desc.Format);
			if (!pix)
			{
				Console::printf("[capture] back buffer format %d has no verified FFmpeg "
					"raw-video layout -- refusing rather than writing wrong colours. "
					"Report this number and it can be added.", static_cast<int>(desc.Format));
				back->Release();
				g_recording.store(false, std::memory_order_relaxed);
				g_stop_reason = "unsupported back buffer format";
				return;
			}
			// x2bgr10le treats the top two bits as padding. FFmpeg converts it
			// to yuva444p10le with opaque alpha for ProRes 4444/4444 XQ.
			// Do not interpret the game's two padding bits as transparency.

			back->GetDevice(&g_device);
			if (!g_device)
			{
				back->Release();
				g_recording.store(false, std::memory_order_relaxed);
				return;
			}
			g_device->GetImmediateContext(&g_context);

			g_width = desc.Width;
			g_height = desc.Height;
			g_frame_bytes = static_cast<std::size_t>(g_width) * g_height * 4;

			// MSAA back buffers cannot be copied to a staging texture directly --
			// they have to be resolved down first. Most titles present a resolved
			// single-sample buffer, so this is usually skipped.
			if (desc.SampleDesc.Count > 1)
			{
				D3D11_TEXTURE2D_DESC rd = desc;
				rd.SampleDesc.Count = 1;
				rd.SampleDesc.Quality = 0;
				rd.Usage = D3D11_USAGE_DEFAULT;
				rd.BindFlags = 0;
				rd.CPUAccessFlags = 0;
				rd.MiscFlags = 0;
				if (FAILED(g_device->CreateTexture2D(&rd, nullptr, &g_resolve)))
				{
					Console::printf("[capture] could not create the MSAA resolve target");
					back->Release();
					release_resources();
					g_recording.store(false, std::memory_order_relaxed);
					return;
				}
			}

			D3D11_TEXTURE2D_DESC sd = desc;
			sd.SampleDesc.Count = 1;
			sd.SampleDesc.Quality = 0;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &g_staging)))
			{
				Console::printf("[capture] could not create the read-back texture");
				back->Release();
				release_resources();
				g_recording.store(false, std::memory_order_relaxed);
				return;
			}

			if (!spawn_ffmpeg(g_ffmpeg_path, pix))
			{
				back->Release();
				release_resources();
				g_recording.store(false, std::memory_order_relaxed);
				g_stop_reason = "ffmpeg would not start";
				return;
			}

			{
				std::lock_guard<std::mutex> lock(g_queue_lock);
				g_writer_quit = false;
			}
			g_writer = std::thread(writer_main);

			Console::printf("[capture] recording %ux%u %s @ %d fps, ProRes %s",
				g_width, g_height, pix, g_fps, profile_name(g_profile));
		}

		// A resize mid-recording would change the frame size under ffmpeg, which
		// only ever agreed to one. Stop rather than corrupt the rest of the file.
		if (desc.Width != g_width || desc.Height != g_height)
		{
			back->Release();
			Console::printf("[capture] the window changed size (%ux%u -> %ux%u) -- stopping, "
				"because the file's frame size is fixed at its header.",
				g_width, g_height, desc.Width, desc.Height);
			g_stop_reason = "the window was resized";
			stop();
			return;
		}

		const double readback_begin_ms = monotonic_ms();
		if (g_resolve)
		{
			g_context->ResolveSubresource(g_resolve, 0, back, 0, desc.Format);
			g_context->CopyResource(g_staging, g_resolve);
		}
		else
		{
			g_context->CopyResource(g_staging, back);
		}
		back->Release();

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(g_context->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped)))
		{
			++g_dropped;
			return;
		}

		std::vector<std::uint8_t> frame(g_frame_bytes);
		// The mapped rows are padded to the driver's own pitch, which is NOT
		// width*4 in general. Copying the block wholesale would shear the picture.
		const auto row_bytes = static_cast<std::size_t>(g_width) * 4;
		const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
		std::uint8_t* dst = frame.data();
		for (UINT y = 0; y < g_height; ++y)
		{
			std::memcpy(dst, src, row_bytes);
			dst += row_bytes;
			src += mapped.RowPitch;
		}
		g_context->Unmap(g_staging, 0);
		const double readback_ms = monotonic_ms() - readback_begin_ms;

		const double queue_begin_ms = monotonic_ms();
		std::size_t queue_depth = 0;
		{
			std::unique_lock<std::mutex> lock(g_queue_lock);
			// Wait briefly for room. A short block keeps every frame when the
			// encoder is only momentarily behind; dropping after that keeps the
			// game responsive instead of freezing on a stalled ffmpeg.
			if (!g_queue_cv.wait_for(lock, std::chrono::milliseconds(100),
				[] { return g_queue.size() < MAX_QUEUED; }))
			{
				const double queue_wait_ms = monotonic_ms() - queue_begin_ms;
				{
					std::lock_guard<std::mutex> timing_lock(g_timing_lock);
					g_timing.max_readback_ms = (std::max)(g_timing.max_readback_ms, readback_ms);
					g_timing.max_queue_wait_ms = (std::max)(g_timing.max_queue_wait_ms, queue_wait_ms);
					++g_timing.blocked_frames;
					g_timing.max_queue_depth = MAX_QUEUED;
				}
				++g_dropped;
				return;
			}
			g_queue.push_back(std::move(frame));
			queue_depth = g_queue.size();
		}
		const double queue_wait_ms = monotonic_ms() - queue_begin_ms;
		{
			std::lock_guard<std::mutex> lock(g_timing_lock);
			g_timing.max_readback_ms = (std::max)(g_timing.max_readback_ms, readback_ms);
			g_timing.max_queue_wait_ms = (std::max)(g_timing.max_queue_wait_ms, queue_wait_ms);
			g_timing.max_queue_depth = (std::max)(g_timing.max_queue_depth, queue_depth);
			if (queue_wait_ms > 1000.0 / (g_fps > 0 ? g_fps : 60))
				++g_timing.blocked_frames;
		}
		g_queue_cv.notify_one();
		++g_frames;
	}

	void on_present(IDXGISwapChain* swap)
	{
		if (!swap) return;
		if (!recording() || post_effect_capture()) return;
		ID3D11Texture2D* back = nullptr;
		if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back)
			capture_texture(back);
	}

	void on_post_effect_texture(ID3D11Texture2D* texture)
	{
		if (!recording() || !texture) return;
		texture->AddRef();
		capture_texture(texture);
	}

	void set_post_effect_capture(const bool enabled)
	{
		g_last_post_effect_frame.store(enabled ? GetTickCount64() : 0);
		g_post_effect_capture.store(enabled);
	}
	bool post_effect_capture()
	{
		return g_post_effect_capture.load() &&
			GetTickCount64() - g_last_post_effect_frame.load() < 1000;
	}

	void init()
	{
		GameUtil::addCommand("demo_capture", cmd_toggle);
		GameUtil::addCommand("demo_capture_start", cmd_start);
		GameUtil::addCommand("demo_capture_stop", cmd_stop);
		GameUtil::addCommand("demo_capture_fps", cmd_fps);
		GameUtil::addCommand("demo_capture_profile", cmd_profile);
		GameUtil::addCommand("demo_capture_ffmpeg", cmd_ffmpeg);
		GameUtil::addCommand("demo_capture_status", cmd_status);

		const auto ff = find_ffmpeg();
		Console::printf("[capture] ProRes capture ready -- ffmpeg %s",
			ff.empty() ? "NOT FOUND (demo_capture_ffmpeg <path>, or put ffmpeg.exe "
			"next to the game exe)" : ff.string().c_str());
	}
}
