///////////////////////////////////////
//             Console
// Logic and Util for int and ext con
///////////////////////////////////////
#include "pch.h"
#include "Console.hpp"
#include "DevMode.hpp"
#include <string>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <array>
#include "FuncPointers.h"
#include "structs.h"
#include <regex>
#include "GameUtil.hpp"
#include "Noclip.hpp"
#include "DvarInterface.hpp"
#include "DevDef.h"
#include <Arxan.hpp>
#include "LogFile.hpp"
#include "ConfigManager.h"
#include "ImageLoader.hpp"
#include "Binds.hpp"
#include "Exec.hpp"

//Output to internal console without label
void Console::printIntCon(std::string text) {
	//Internal Console
	InternalConsole::addToOutputStack(text, 0);
}


//Output to all consoles without label
void Console::print(const std::string& text) {
	
	// External CLI
	std::cout << text << std::endl;
	// External Console Window
	ExternalConsoleGui::print(text);
	// Internal Console
	InternalConsole::addToOutputStack(text, 0);

	Logfile::append(const_cast<std::string&>(text));
}

/**
 * @brief Prints a formatted message to all consoles.
 *
 * Formats the given printf-style format string using the supplied
 * variable arguments and forwards the resulting message to
 * Console::print().
 *
 * @param fmt A printf-style format string.
 * @param ... Additional arguments referenced by the format string.
 */
void Console::printf(const char* fmt, ...) {
	if (!fmt) {
		return;
	}
	va_list args;
	va_start(args, fmt);

	std::vector<char> msgBuf(1024);
	vsnprintf(msgBuf.data(), msgBuf.size(), fmt, args);

	va_end(args);
	std::string finalMsg = msgBuf.data();

	Console::print(finalMsg);
}

//Output to all consoles with a label
void Console::labelPrint(std::string label, std::string text) {
	std::string s = "[" + label + "] " + text;
	//External CLI
	ExtConsole::coutCustom(label, text);
	//External Console Window
	ExternalConsoleGui::print(s);
	//Internal Console
	InternalConsole::addToOutputStack(s, 0);

	Logfile::append(s);
}

//Output to all consoles as info print
void Console::infoPrint(std::string text) {
	std::string s = "[INFO] " + text;
	//External CLI
	ExtConsole::coutInfo(text);
	//External Console Window
	ExternalConsoleGui::print(s);
	//Internal Console
	InternalConsole::addToOutputStack(s, 0);

	Logfile::append(s);
}

//TODO: add preprocessor directive for developer like in t6sp-mod
//Output to all consoles as client developer print
void Console::devPrint(std::string text) {
#ifdef DEVELOPMENT_BUILD
	std::string s = "[DEV] " + text;
	//External CLI
	ExtConsole::coutInfo(text);
	//External Console Window
	ExternalConsoleGui::print(s);
	//Internal Console
	InternalConsole::addToOutputStack(s, 0);


	Logfile::append(s);
#endif
}

//Output to all consoles as initialization print
void Console::initPrint(std::string text) {
	std::string s = "[INIT] " + text;
	//External CLI
	ExtConsole::coutInit(text);
	//External Console Window
	ExternalConsoleGui::print(s);
	//Internal Console
	InternalConsole::addToOutputStack(s, 0);

	Logfile::append(s);
}

//Parse command string into a vector of strings. Anything inside of quotes will be a single string
std::vector<std::string> Console::parseCmdToVec(const std::string& cmd) {
	std::vector<std::string> components;
	std::regex pattern(R"((\"[^\"]*\"|\S+))");
	auto words_begin = std::sregex_iterator(cmd.begin(), cmd.end(), pattern);
	auto words_end = std::sregex_iterator();

	for (auto it = words_begin; it != words_end; ++it) {
		std::string match = it->str();
		if (match.size() > 1 && match.front() == '"' && match.back() == '"') {
			match = match.substr(1, match.size() - 2);
		}
		components.push_back(match);
	}
	return components;
}

//TODO: move to gameutil
std::string toHex(uint32_t value) {
	std::stringstream ss;
	ss << std::hex << value;
	return ss.str();
}


void setenginemode() {
	CmdArgs* cmdArgs = GameUtil::getCmdArgs();
	if (!cmdArgs) {
		return;
	}

	int nest = cmdArgs->nesting;
	int count = cmdArgs->argc[nest];
	if (count != 2) {
		DEV_PRINTF("bozo");
		return;
	}

	const char** args = cmdArgs->argv[nest];
	unsigned int mode = static_cast<unsigned int>(std::strtoul(args[1], nullptr, 10));
	int* enginemode = reinterpret_cast<int*>(0xD8AE894_b);
	*enginemode = mode; //1 - MP | 2 - ZM | I think it has other flags packed in
}

void Console::registerCommandOverrides() {
	GameUtil::overrideCommand("bind", &Binds::bindCmd);
	GameUtil::overrideCommand("unbind", &Binds::unbindCmd);
	GameUtil::overrideCommand("unbindall", &Binds::unbindAllCmd);
	GameUtil::overrideCommand("exec", &Exec::execCmd);
}

// Lobby-parameter names. These are neither dvars nor real commands: the engine
// routes EVERY console command through Party_SetLobbyParamFromCmd before
// dispatch, so simply having a command by this name is what lets
// `party_maxplayers 18` reach the lobby. They only matter to the hosting work,
// so they live in developer mode with it.
void setupSpecialLobbyVars() {
	dev_mode::add_command("ui_mapname", &CustomCommands::none);
	dev_mode::add_command("ui_gametype", &CustomCommands::none);
	dev_mode::add_command("party_minplayers", &CustomCommands::none);
	dev_mode::add_command("party_maxplayers", &CustomCommands::none);
	dev_mode::add_command("party_matchedplayercount", &CustomCommands::none);
	dev_mode::add_command("party_minlobbytime", &CustomCommands::none);
	dev_mode::add_command("requireOpenNat", &CustomCommands::none);
	dev_mode::add_command("matchmaking_allowJoiningListenServer", &CustomCommands::none);
	dev_mode::add_command("rankedMatch", &CustomCommands::none);
}

void cgt() {
	void* rd = GameUtil::CG_GetLocalClientGlobals();
	if (!rd) {
		DEV_PRINTF("cg_t* is nullptr");
		return;
	}

	DEV_PRINTF("cg_t is at 0x%p", rd);
}

void Console::registerCustomCommands() {
	setupSpecialLobbyVars();
	dev_mode::register_command();

	// ---- the product -------------------------------------------------
	// Short on purpose. Everything a normal session needs, and nothing that
	// only makes sense mid-investigation.
	GameUtil::addCommand("map", &CustomCommands::changeMap);
	GameUtil::addCommand("clear", &InternalConsole::clearFullConsole);
	GameUtil::addCommand("unlockall", &CustomCommands::unlockAll);

	// ---- developer mode ----------------------------------------------
	// Cheats, engine debug overlays, asset dumpers and renderer switches.
	// All still here; `s2_dev 1` brings them back.
	dev_mode::add_command("noclip", &Noclip::toggle);
	dev_mode::add_command("ufo", &CustomCommands::toggleUfo);
	dev_mode::add_command("map_restart", &CustomCommands::mapRestart);
	dev_mode::add_command("fast_restart", &CustomCommands::fastRestart);
	dev_mode::add_command("god", &CustomCommands::god);
	dev_mode::add_command("notarget", &CustomCommands::notarget);
	dev_mode::add_command("demigod", &CustomCommands::demigod);
	dev_mode::add_command("trans", &CustomCommands::translateString);
	dev_mode::add_command("luidbg", &DevDraw::toggleLuaDebugGui);
	dev_mode::add_command("entdbg", &DevDraw::toggleEntityDebugGui);
	dev_mode::add_command("acdbg", &DevDraw::toggleAntiCheatDebugGui);
	dev_mode::add_command("posdbg", &DevDraw::togglePlayerOriginDebugGui);
	dev_mode::add_command("gscdbg", &DevDraw::toggleGscDebugGui);
	dev_mode::add_command("intcondbg", &DevDraw::toggleIntConDebugGui);
	dev_mode::add_command("listcmd", &CustomCommands::listAllCmds);
	dev_mode::add_command("r_fullbright", &CustomCommands::tempToggleFullbright);
	dev_mode::add_command("r_wireframe", &CustomCommands::tempToggleWireframe);
	dev_mode::add_command("r_togglePortals", &CustomCommands::togglePortals);
	dev_mode::add_command("listassetpool", &CustomCommands::listAssetPool);
	dev_mode::add_command("saveassetpool", &CustomCommands::saveAssetPool);
	dev_mode::add_command("dumpAllLuaFiles", &CustomCommands::dumpAllLuaFiles);
	dev_mode::add_command("dumpAllCSVFiles", &CustomCommands::dumpAllCSVFiles);
	dev_mode::add_command("dumpAllScriptFiles", &CustomCommands::dumpAllScriptFiles);
	dev_mode::add_command("give", &CustomCommands::give);
	dev_mode::add_command("dropweapon", &CustomCommands::dropWeapon);
	dev_mode::add_command("execbuiltin", reinterpret_cast<void (*)()>(0x64A2A0_b));
	dev_mode::add_command("reloadImages", &ImageLoader::reloadImages);
#ifdef DEVELOPMENT_BUILD
	dev_mode::add_command("dumpgscfunctions", &CustomCommands::dumpGscFunctions);
	dev_mode::add_command("cgt", &cgt);
	dev_mode::add_command("enginemode", &setenginemode);
	dev_mode::add_command("cmdtest", &CustomCommands::cmdTest);
	dev_mode::add_command("getCmdFuncAddr", &CustomCommands::getCmdFuncAddr);
#endif // DEVELOPMENT_BUILD

	if (ConfigManager::readConfigValue("s2mp-mod.cfg", "unlockall", false)) {
		CustomCommands::unlockAll();//might as well just call it directly
		Console::infoPrint("Unlock All set");
	}
}

void Console::registerCustomDvars() {
	DvarInterface::registerBool("g_dumpLui", 0, 0, "Dump LUI files on map load");
	DvarInterface::registerBool("g_dumpStringTables", 0, 0, "Dump StringTables when they are loaded");
	DvarInterface::registerBool("g_dumpRawfiles", 0, 0, "Dump RawFiles when they are loaded");
	DvarInterface::registerBool("printWorldInfo", 0, 0, "Prints GfxWorld build info on load");
	DvarInterface::registerBool("g_dumpMapEnts", 0, 0, "Dump MapEnts when they are loaded");
	DvarInterface::registerBool("g_dumpImages", 0, 0, "Dump Images when they are loaded. Can be unstable when loading maps. Best to turn off when doing so.");

	//zmcacutils.lua left in a check for a dvar named "unlockAllConsumables" so registering here makes the lua function work lol
	DvarInterface::registerBool("unlockAllConsumables", 0, 0, "Unlock all zombies consumables. Used by the unlockall command"); 
	DvarInterface::registerBool("unlockAllPassivePerks", 0, 0, "Unlock all zombies passive perks. Used by the unlockall command"); 

	//for gsc and any other system that still checks for this dvar
	Functions::_Dvar_RegisterInt("850", 0, 0, 4, 0); //force_ranking
	Functions::_Dvar_RegisterInt("5357", 0, 0, 1, 0); //isGamescomForceRankedMatch

	DvarInterface::registerFloat("cg_gun_x", 0.0, -3.4028235e38, 3.4028235e38, 0, "Forward position of the viewmodel");
	DvarInterface::registerFloat("cg_gun_y", 0.0, -3.4028235e38, 3.4028235e38, 0, "Right position of the viewmodel");
	DvarInterface::registerFloat("cg_gun_z", 0.0, -3.4028235e38, 3.4028235e38, 0, "Up position of the viewmodel");
}

//useful for testing commands and handling non-cmd/non-dvar stuff
bool execCustomDevCmd(const std::string& cmd) {
	std::vector<std::string> p = Console::parseCmdToVec(cmd);
	if (p.empty()) {
		return false;
	}

	std::string commandName = p[0];
	std::transform(commandName.begin(), commandName.end(), commandName.begin(), GameUtil::asciiToLower);

	//--------------------TEMP--------------------
	if (commandName == "trans") {
		if (p.size() == 2) {
			Console::print("Translated String: " + std::string(Functions::_SEH_SafeTranslateString(p[1].c_str())));
		}
		return true;
	}
	
	if (commandName == "send") {
		if (p.size() == 2) {
			std::string str = "%c \"" + p[1] + "\"";
			Functions::_SV_SendServerCommand(0i64, 0, str.c_str(), 101i64);
		}
		return true;
	}
	
	if (commandName == "up") {
		Functions::_LiveStorage_UploadStats(0);
		return true;
	}
	
	if (commandName == "ftest") {
		Functions::_R_RegisterFont("testfakefont.ttf", 16);
		return true;
	}
	//--------------------------------------------

	
	if (commandName == "quit") {
		exit(0);
		return true;
	}


	
	if (commandName == "cg_hudblood") {
		if (p.size() >= 2) {
			CustomCommands::toggleHudBlood(GameUtil::stringToBool(p[1]));
		}
		return true;
	}
	
	//TODO: register these as dvar?
	if (commandName == "r_fog") {
		if (p.size() >= 2) {
			CustomCommands::toggleFog(GameUtil::stringToBool(p[1]));
		}
		return true;
	}

	return false;
}

//Formats a commands and sends it to the dvar interface. Returns true if successful
bool setEngineDvar(std::string cmd) {
	std::vector<std::string> p = Console::parseCmdToVec(cmd);
	if (p.empty()) {
		return false;
	}

	return DvarInterface::setDvar(p[0], p);
}

//All consoles use this to execute commands. 
void Console::execCmd(std::string cmd, bool echoCommand) {
	if (cmd.find_first_not_of(" \t\r\n") == std::string::npos) {
		return;
	}

	const std::string originalCmd = cmd;

	if (execCustomDevCmd(cmd)) {
		return;
	}

	if (DvarInterface::setProtectedDvarFromPrefixedCommand(cmd)) {
		return;
	}

	if (DvarInterface::translatePrefixedCommand(cmd)) {
		if (echoCommand) {
			Console::printIntCon(originalCmd);
		}
		GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, cmd);
		return;
	}

	if (setEngineDvar(cmd)) {
		return;
	}

	if (echoCommand) {
		Console::printIntCon(originalCmd);
	}
	GameUtil::Cbuf_AddText(LOCAL_CLIENT_0, originalCmd);
}
