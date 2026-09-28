#include "pch.h"
#include "Exec.hpp"
#include "GameUtil.hpp"
#include "Console.hpp"
#include "FuncPointers.h"

#include <sstream>
#include <unordered_set>
#include <DevDef.h>

namespace {
	std::unordered_set<std::string> g_activeExecFiles;
	std::mutex g_autoexecMutex;

	std::string trimLine(const std::string& line) {
		const std::size_t first = line.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) {
			return "";
		}

		const std::size_t last = line.find_last_not_of(" \t\r\n");
		return line.substr(first, last - first + 1);
	}

	std::filesystem::path getPlayers2Path() {
		const char* cwd = Functions::_Sys_Cwd ? Functions::_Sys_Cwd() : nullptr;
		const std::filesystem::path basePath = (cwd && *cwd) ? std::filesystem::path(cwd) : std::filesystem::current_path();
		return basePath / "players2";
	}

	std::string normalizeCfgFilename(const std::string& rawFilename) {
		const std::string trimmedName = trimLine(rawFilename);
		if (trimmedName.empty()) {
			return "";
		}

		const std::filesystem::path filePath = std::filesystem::path(trimmedName).lexically_normal();
		if (filePath.empty() || filePath.is_absolute() || filePath.has_root_name()) {
			return "";
		}

		for (const auto& part : filePath) {
			if (part == "..") {
				return "";
			}
		}

		std::string normalizedName = filePath.generic_string();
		if (!GameUtil::toLower(normalizedName).ends_with(".cfg")) {
			normalizedName += ".cfg";
		}

		return normalizedName;
	}

	bool readBinaryFile(const std::filesystem::path& filePath, std::vector<std::uint8_t>& outData) {
		std::ifstream file(filePath, std::ios::binary);
		if (!file.is_open()) {
			return false;
		}

		file.seekg(0, std::ios::end);
		const std::streamoff fileSize = file.tellg();
		if (fileSize < 0) {
			return false;
		}

		file.seekg(0, std::ios::beg);
		outData.resize(static_cast<std::size_t>(fileSize));

		if (!outData.empty()) {
			file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(outData.size()));
		}

		return file.good() || file.eof();
	}

	bool hasLocalCfgFile(const std::string& rawFilename) {
		const std::string filename = normalizeCfgFilename(rawFilename);
		const std::string lower = GameUtil::toLower(filename);
		// The native handler owns encrypted profiles and control presets.
		if (filename.empty() || lower == "system_config_mp.cfg"
			|| lower == "user_config_mp.cfg" || lower.rfind("controls/", 0) == 0) {
			return false;
		}

		std::error_code error;
		return std::filesystem::is_regular_file(getPlayers2Path() / filename, error);
	}

	bool executeCfgFile(const std::string& rawFilename);

	void executeCfgText(const std::string& cfgText) {
		std::istringstream cfgStream(cfgText);
		std::string line;
		bool isFirstLine = true;

		while (std::getline(cfgStream, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}

			if (isFirstLine && line.size() >= 3
				&& static_cast<unsigned char>(line[0]) == 0xEF
				&& static_cast<unsigned char>(line[1]) == 0xBB
				&& static_cast<unsigned char>(line[2]) == 0xBF) {
				line.erase(0, 3);
			}
			isFirstLine = false;

			const std::string trimmedLine = trimLine(line);
			if (trimmedLine.empty() || trimmedLine.rfind("//", 0) == 0) {
				continue;
			}

			std::vector<std::string> parsedLine = Console::parseCmdToVec(trimmedLine);
			if (!parsedLine.empty()) {
				std::string commandName = GameUtil::toLower(parsedLine[0]);
				if (commandName == "exec" && parsedLine.size() == 2
					&& hasLocalCfgFile(parsedLine[1])) {
					executeCfgFile(parsedLine[1]);
					continue;
				}
			}

			Console::execCmd(trimmedLine, false);
		}
	}

	bool executeCfgFile(const std::string& rawFilename) {
		const std::string cfgFilename = normalizeCfgFilename(rawFilename);
		if (cfgFilename.empty()) {
			Console::print("exec <filename> : execute a cfg file from players2");
			return false;
		}

		const std::filesystem::path cfgFilePath = (getPlayers2Path() / cfgFilename).lexically_normal();
		const std::string normalizedExecPath = cfgFilePath.generic_string();
		if (g_activeExecFiles.contains(normalizedExecPath)) {
			Console::printf("Prevented recursive exec of cfg file: %s", cfgFilename.c_str());
			return false;
		}

		struct ExecScopeGuard {
			std::unordered_set<std::string>* activeExecFiles = nullptr;
			std::string normalizedPath;

			~ExecScopeGuard() {
				if (activeExecFiles) {
					activeExecFiles->erase(normalizedPath);
				}
			}
		};

		g_activeExecFiles.insert(normalizedExecPath);
		ExecScopeGuard execScopeGuard{ &g_activeExecFiles, normalizedExecPath };

		if (GameUtil::toLower(cfgFilename) == "system_config_mp.cfg") {
			Console::printf("Encrypted cfg not supported yet: %s", cfgFilename.c_str());
			return false;
		}

		std::vector<std::uint8_t> cfgData;
		if (!readBinaryFile(cfgFilePath, cfgData)) {
			Console::printf("Could not open cfg file: %s", cfgFilename.c_str());
			return false;
		}

		while (!cfgData.empty() && cfgData.back() == '\0') {
			cfgData.pop_back();
		}

		const std::string cfgText(reinterpret_cast<const char*>(cfgData.data()), cfgData.size());
		executeCfgText(cfgText);
		return true;
	}

	bool isDvarAutoexecLine(const std::string& line, const std::string& dvarName) {
		const std::string trimmedLine = trimLine(line);
		if (trimmedLine.empty() || trimmedLine.rfind("//", 0) == 0 || trimmedLine[0] == '#') {
			return false;
		}

		std::vector<std::string> parsedLine = Console::parseCmdToVec(trimmedLine);
		if (parsedLine.empty()) {
			return false;
		}

		const std::string commandName = GameUtil::toLower(parsedLine[0]);
		const std::string targetDvarName = GameUtil::toLower(dvarName);

		if (commandName == targetDvarName) {
			return true;
		}

		if ((commandName == "set" || commandName == "seta") && parsedLine.size() >= 2) {
			return GameUtil::toLower(parsedLine[1]) == targetDvarName;
		}

		return false;
	}
}

void Exec::init() {
	const std::filesystem::path players2Path = getPlayers2Path();
	const std::filesystem::path autoexecPath = players2Path / "autoexec.cfg";

	std::error_code createDirsError;
	std::filesystem::create_directories(players2Path, createDirsError);
	if (createDirsError) {
		Console::printf("Failed to create players2 folder for autoexec: %s", players2Path.string().c_str());
		return;
	}

	if (std::filesystem::exists(autoexecPath)) {
		return;
	}

	std::ofstream autoexecFile(autoexecPath, std::ios::out | std::ios::trunc);
	if (!autoexecFile.is_open()) {
		Console::printf("Failed to create autoexec.cfg: %s", autoexecPath.string().c_str());
		return;
	}

	autoexecFile << "// put dvars and commands here to execute each time the game starts\n";
}

void Exec::scrubDemoAutostartFromAutoexec() {
	const std::lock_guard<std::mutex> lock(g_autoexecMutex);

	const std::filesystem::path autoexecPath = getPlayers2Path() / "autoexec.cfg";
	if (!std::filesystem::exists(autoexecPath)) {
		return;
	}

	std::vector<std::string> kept;
	std::vector<std::string> removed;
	{
		std::ifstream in(autoexecPath);
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}

			const std::string trimmed = trimLine(line);
			const std::string lower = GameUtil::toLower(trimmed);

			// Drop anything that would auto-start a demo / theater session on boot.
			const bool isDemoAutostart =
				lower.starts_with("cl_demo_play") ||
				lower.starts_with("demo_play") ||
				lower.starts_with("demoplay") ||
				lower == "connect demo" ||
				lower.starts_with("connect demo ");

			if (isDemoAutostart) {
				removed.push_back(trimmed);
				continue;
			}
			kept.push_back(line);
		}
	}

	if (removed.empty()) {
		return;
	}

	std::ofstream out(autoexecPath, std::ios::out | std::ios::trunc);
	if (!out.is_open()) {
		Console::printf("[demo] failed to rewrite autoexec.cfg while scrubbing demo autostart");
		return;
	}
	for (const std::string& line : kept) {
		out << line << '\n';
	}

	for (const std::string& line : removed) {
		Console::printf("[demo] removed boot autostart from autoexec.cfg: %s", line.c_str());
	}
}

bool Exec::updateAutoexecDvar(const std::string& dvarName, const std::string& value) {
	const std::lock_guard<std::mutex> lock(g_autoexecMutex);

	const std::filesystem::path players2Path = getPlayers2Path();
	const std::filesystem::path autoexecPath = players2Path / "autoexec.cfg";

	std::error_code createDirsError;
	std::filesystem::create_directories(players2Path, createDirsError);
	if (createDirsError) {
		Console::printf("Failed to create players2 folder for autoexec: %s", players2Path.string().c_str());
		return false;
	}

	std::vector<std::string> lines;
	{
		std::ifstream autoexecFile(autoexecPath);
		std::string line;

		while (std::getline(autoexecFile, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}

			lines.push_back(line);
		}
	}

	const std::string updatedLine = dvarName + " " + value;
	bool updatedExistingLine = false;

	for (std::string& line : lines) {
		if (isDvarAutoexecLine(line, dvarName)) {
			line = updatedLine;
			updatedExistingLine = true;
		}
	}

	if (!updatedExistingLine) {
		lines.push_back(updatedLine);
	}

	std::ofstream autoexecFile(autoexecPath, std::ios::out | std::ios::trunc);
	if (!autoexecFile.is_open()) {
		Console::printf("Failed to update autoexec.cfg: %s", autoexecPath.string().c_str());
		return false;
	}

	for (const std::string& line : lines) {
		autoexecFile << line << '\n';
	}

	return autoexecFile.good();
}

void Exec::execCmd() {
	CmdArgs* cmdArgs = GameUtil::getCmdArgs();
	if (!cmdArgs) {
		return;
	}

	int nest = cmdArgs->nesting;
	int count = cmdArgs->argc[nest];
	const char** args = cmdArgs->argv[nest];

	if (count != 2 || !args || !args[1]) {
		Console::print("exec <filename> : execute a cfg file from players2");
		return;
	}

	const std::string filename = args[1];
	if (hasLocalCfgFile(filename)) {
		executeCfgFile(filename);
	} else {
		// Built-in match rules must run only through the engine. Executing the
		// same rawfile again here duplicates its commands and fills the Cbuf.
		Functions::_Cmd_Exec_f();
	}
}
