//taken from old t6sp-mod build. TODO: update this

#include "pch.h"
#include "Logfile.hpp"
#include "Console.hpp"
#include <fstream>
#include <filesystem>
#include "GameUtil.hpp"
#include "Hook.hpp"
#include "DevDef.h"
#include "ModPaths.hpp"

bool Logfile::enabled = false;

// WHERE THE LOG GOES, per build.
//
// Steam: "main/s2mp_console.log", relative to the working directory, EXACTLY as
// before. That path is used when retrieving probe output; keep it stable.
//
// Store: the game lives in C:\Program Files\WindowsApps, which is not writable,
// and the working directory of a packaged app is not somewhere we should be
// writing either. The log joins the rest of the mod's data in LocalState.
static std::string log_path()
{
	if (build_map::current() == build_map::Build::Store)
	{
		// Forward slash on purpose: Windows accepts it, and it cannot be
		// mangled by an escaping mistake the way a backslash can.
		return mod_paths::mod_dir() + "/s2mp_console.log";
	}
	return "main/s2mp_console.log";
}

void Logfile::init() {
	// Keep the PREVIOUS session as s2mp_console.prev.log instead of deleting it.
	// A test run followed by a relaunch used to erase exactly the evidence it
	// produced -- 2026-09-15 a seek test left no trace for that reason.
	const std::string filePath = log_path();
	std::filesystem::path logFilePath(filePath);
	try {
		if (std::filesystem::exists(logFilePath)) {
			std::filesystem::path prev = logFilePath;
			prev.replace_extension(".prev.log");
			std::error_code ec;
			std::filesystem::remove(prev, ec);
			std::filesystem::rename(logFilePath, prev, ec);
			if (ec) {
				std::filesystem::remove(logFilePath);
			}
		}
	}
	catch (const std::filesystem::filesystem_error& e) {
#ifdef DEVELOPMENT_BUILD
		Console::devPrint("Logfile init error");
#endif // DEVELOPMENT_BUILD

	}
}

void Logfile::setEnabled(bool b) {
	Logfile::enabled = b;
}

void Logfile::append(std::string& text) {
	if (Logfile::enabled) {
		const std::string filePath = log_path();
		std::filesystem::path dirPath = std::filesystem::path(filePath).parent_path();
		if (!std::filesystem::exists(dirPath)) {
			std::filesystem::create_directories(dirPath);
		}
		std::ofstream outFile(filePath, std::ios::out | std::ios::app);
		if (!outFile) {
			return;
		}

		//check for newline at end
		if (!text.empty() && text.back() != '\n') {
			text += '\n';
		}
		outFile.write(text.c_str(), text.length());
		outFile.close();
	}
}