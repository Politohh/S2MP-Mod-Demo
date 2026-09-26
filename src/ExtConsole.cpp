//////////////////////////////////////
//            Ext Console
//	Logic for the external console(s)
//////////////////////////////////////
#include "pch.h"
#include <iostream>
#include <Windows.h>
#include <string>
#include <sstream>
#include <MinHook.h>
#include <array>
#include <signal.h>
#include "Console.hpp"
#include "ModBuild.hpp"
#include "DevMode.hpp"
#include "BuildMap.hpp"
#include <thread>
#include "FuncPointers.h"
#include "PrintPatches.hpp"
#include "Arxan.hpp"
#include "DvarInterface.hpp"
#include "DevDef.h"
#include "Loaders.hpp"
#include "Errors.hpp"
#include "Dvars.hpp"
#include "Binds.hpp"
#include "Exec.hpp"
#include "demo/demo.hpp"

HANDLE hProcess;
HINSTANCE hInst;

//these prints will be for external console only
void ExtConsole::coutInit(const std::string& s) {
	std::cout << "[INIT] " << s << std::endl;
}

void ExtConsole::coutInfo(const std::string& s) {
	std::cout << "[INFO] " << s << std::endl;
}

void ExtConsole::coutCustom(const std::string& s, const std::string& s2) {
	std::cout << "[" << s << "] " << s2 << std::endl;
}

void ExtConsole::consoleMainLoop() {
	std::string in;
	std::cout << "----------[ S2MP External Console ]----------" << std::endl;
	while (true) {
		std::cout << "> ";
		getline(std::cin, in);
		Console::execCmd(in);
	}
}

bool doZombiesMode = false;

void checkAndSetZombieMode() {
	const char* filename = "ZM";
	DWORD attributes = GetFileAttributesA(filename);
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		doZombiesMode = true;
		if (DeleteFileA(filename)) {
			DEV_PRINTF("Cleared zombiemode flag");
		}
		else {
			DEV_PRINTF("FAILED to clear zombiemode flag");
		}
	}

}

void infoPrintOffsets() {
	uintptr_t s2base = (uintptr_t)GetModuleHandle(NULL);
	uintptr_t s2baseOff = (uintptr_t)GetModuleHandle(NULL) + 0x1000;
	std::ostringstream oss;
	oss << "0x" << std::hex << s2base;
	std::string addressStr = oss.str();
	std::ostringstream oss2;
	oss2 << "0x" << std::hex << s2baseOff;
	std::string addressStr2 = oss2.str();
	Console::infoPrint("s2_mp64_ship Base at: " + addressStr);
	Console::infoPrint("s2_mp64_ship BaseOff at: " + addressStr2);
}



//0 - CLI, 1 - GUI, 2 - BOTH
void ExtConsole::extConInit(int extConsoleMode) {

	HINSTANCE hInstance = GetModuleHandle(nullptr);
	hProcess = GetCurrentProcess();
	if (extConsoleMode >= 1) {
		//wait for external console gui to be fully ready
		while (!ExternalConsoleGui::isExtConGuiReady()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	if (extConsoleMode == 0 || extConsoleMode == 2) {
		//title stuff
		int size = MultiByteToWideChar(CP_UTF8, 0, "S2MP-Mod External Console", -1, NULL, 0);
		wchar_t* wtitle = new wchar_t[size];
		MultiByteToWideChar(CP_UTF8, 0, "S2MP-Mod External Console", -1, wtitle, size);
		SetConsoleTitle(wtitle);
		std::cout << "extConsoleMode:" << extConsoleMode << "; CLI will be used" << std::endl;
	}


	infoPrintOffsets();
	Functions::init();
	//Console::print("Sys_Cwd(): " + std::string(Functions::_Sys_Cwd()));

	checkAndSetZombieMode();
	Console::printf("Setting engine mode to %s", doZombiesMode ? "Zombies" : "Multiplayer");
	if (doZombiesMode) {
		constexpr std::array<unsigned char, 1> ZOMBIES = { 0x02 }; //patch
		HANDLE pHandle = GetCurrentProcess();
		WriteProcessMemory(pHandle, (LPVOID)(0xD8AE894_b), ZOMBIES.data(), ZOMBIES.size(), nullptr);
	}
	if (!FindWindowA("S2", NULL)) {
		Console::print("Waiting for game to initialize...");
	}

	DeleteFileA("ZM");//just in case
	while (!FindWindowA("S2", NULL)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	DeleteFileA("ZM");//just in case
	// Which game build are we in? Every hardcoded address depends on the answer,
	// so say it out loud before anything uses one (RULE A15). Detection is by the
	// EXE_ERR_PROCESS_DEMO_FILE_FAILED string, the same signature RULE A2 uses to
	// verify a Cheat Engine attachment -- not a version number or a module size.
	// Package build number FIRST -- if a log is ever read without it, there is no
	// way to know which DLL wrote the lines underneath. See src/ModBuild.hpp.
	Console::printf("[s2mp] package build %d", mod_build::NUMBER);
	Console::printf("[build] detected: %s   (%zu addresses in the Store table)",
		build_map::name(), build_map::mapped_count());
	if (build_map::current() == build_map::Build::Unknown)
	{
		Console::printf("[build] WARNING: no build signature matched. Addresses are "
			"being used UNTRANSLATED, i.e. as if this were the Steam build. If this "
			"is not the Steam build, expect it to fault.");
	}

	// Developer mode decides which console commands and UI tabs exist, so it
	// has to be settled before any module registers anything.
	dev_mode::init();
	Console::printf("[s2mp] developer mode: %s", dev_mode::enabled() ? "ON" : "off");

	ArxanPatches::init();
	DebugPatches::init();
	PrintPatches::init();
	// Dvar mappings must be ready before theater reads stock dvars (mapname→1673, etc.).
	DvarInterface::init();
	demo::init();
	DevPatches::init();
	Console::registerCustomCommands();
	Console::registerCustomDvars();
	Console::registerCommandOverrides();
	Binds::init();
	Exec::init();
	Dvars::initPatches();
	Errors::init();
	InternalConsole::init();
	Loaders::initAssetLoaders();

	GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, "exec autoexec");

	// Drop leftover NativeTrace harness marker so it cannot silently re-arm native demos.
	DeleteFileA("autotest.txt");

	DeleteFileA("ZM");//just in case
	if (extConsoleMode == 0 || extConsoleMode == 2) {
		consoleMainLoop();
	}
}