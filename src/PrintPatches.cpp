///////////////////////////////////////////////////////
//                  Print Patches
//	For reimplementing stripped print statements
////////////////////////////////////////////////////////
#include "pch.h"
#include "Console.hpp"
#include "PrintPatches.hpp"
#include "demo/demo_playback.hpp" // demo_playback::verbose() gates the zone-load probe
#include <FuncPointers.h>
#include "Hook.hpp"
#include "structs.h"
#include "GameUtil.hpp"
#include <cctype>
#include <cstring>

namespace {
    bool isMeaningfulCString(const char* value) {
        if (!value) {
            return false;
        }

        return std::strcmp(value, "<null>") != 0
            && std::strcmp(value, "<invalid>") != 0
            && std::strcmp(value, "<unreadable>") != 0
            && std::strcmp(value, "<unterminated>") != 0
            && std::strcmp(value, "<unterminated/unreadable>") != 0;
    }

    bool looksLikeText(const char* value) {
        if (!isMeaningfulCString(value) || !*value) {
            return false;
        }

        size_t inspected = 0;
        size_t printable = 0;

        for (; value[inspected] != '\0' && inspected < 160; ++inspected) {
            const unsigned char ch = static_cast<unsigned char>(value[inspected]);
            if (std::isprint(ch) || std::isspace(ch)) {
                ++printable;
            }
        }

        return inspected > 0 && printable >= (inspected * 3) / 4;
    }

    void logHksStringCandidates(const HksObject* object) {
        if (!object || object->value == 0) {
            return;
        }

        const uintptr_t valueAddress = static_cast<uintptr_t>(object->value);
        const size_t offsets[] = { 0x0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x30 };
        bool printedCandidate = false;

        for (const size_t offset : offsets) {
            const uintptr_t address = valueAddress + offset;

            const char* inlineText = GameUtil::safeCString(reinterpret_cast<const char*>(address), 1024);
            if (looksLikeText(inlineText)) {
                Console::printf("LUI VM error text [value+0x%zX]: %s", offset, inlineText);
                printedCandidate = true;
            }

            if (!GameUtil::isReadablePtr(reinterpret_cast<const void*>(address), sizeof(const char*))) {
                continue;
            }

            const char* indirectText = *reinterpret_cast<const char* const*>(address);
            indirectText = GameUtil::safeCString(indirectText, 1024);
            if (looksLikeText(indirectText)) {
                Console::printf("LUI VM error text [*(value+0x%zX)]: %s", offset, indirectText);
                printedCandidate = true;
            }
        }

        if (!printedCandidate) {
            Console::print("LUI VM error text: no readable string candidates found");
        }
    }

    void logLuiVmState(void* luiVm) {
        if (!luiVm) {
            Console::print("LUI VM snapshot: null");
            return;
        }

        lua_State* state = reinterpret_cast<lua_State*>(luiVm);
        if (!GameUtil::isReadablePtr(state, sizeof(lua_State))) {
            Console::printf("LUI VM snapshot unreadable: %p", luiVm);
            return;
        }

        HksObject* base = state->m_apistack.base;
        HksObject* top = state->m_apistack.top;
        HksObject* end = state->m_apistack.end;

        Console::printf("LUI VM stack: base=%p top=%p end=%p", base, top, end);

        if (!top || !base || top <= base) {
            Console::print("LUI VM stack is empty at error time");
            return;
        }

        const HksObject* topObject = top - 1;
        if (!GameUtil::isReadablePtr(topObject, sizeof(HksObject))) {
            Console::printf("LUI VM top object unreadable: %p", topObject);
            return;
        }

        Console::printf("LUI VM top object: type=%d value=%p", topObject->type, reinterpret_cast<void*>(topObject->value));

        logHksStringCandidates(topObject);
    }
}

typedef void(*CM_LoadMap)(const char* name, int* checksum);
CM_LoadMap _CM_LoadMap = nullptr;

typedef void(*FS_Startup)(const char* gameName);
FS_Startup _FS_Startup = nullptr;

typedef void(*SV_Shutdown)(const char* finalmsg);
SV_Shutdown _SV_Shutdown = nullptr;

typedef void(*SV_SpawnServer)(const char* server, int mapIsPreloaded, int savegame, int isRestart);
SV_SpawnServer _SV_SpawnServer = nullptr;

typedef void(*LUI_LoadLuaFile)(const char* filename, void* luivm);
LUI_LoadLuaFile _LUI_LoadLuaFile = nullptr;

typedef void(*LUI_Init)(void* func, void* alsofunc);
LUI_Init _LUI_Init = nullptr;

typedef void(*DB_TryLoadXFileInternal)(const char* zoneName, int zoneFlags, int isBaseMap);
DB_TryLoadXFileInternal _DB_TryLoadXFileInternal = nullptr;

typedef void(*DB_LoadXZone)(XZoneInfo* zoneInfo, __int64 zoneCount, __int64 waitAlloc, __int64 skipReadAlwaysLoadedAssets);
DB_LoadXZone _DB_LoadXZone = nullptr;

typedef void(*G_InitGame)(int levelTime, unsigned int randomSeed, int restart, int registerDvars, int savegame);
G_InitGame _G_InitGame = nullptr;

typedef void(*Online_PatchStreamer_va)(const char* label, const char* fmt, ...);
Online_PatchStreamer_va _Online_PatchStreamer_va = nullptr;

typedef void(*Com_WriteConfig_f)(int localClientNum, const char* name);
Com_WriteConfig_f _Com_WriteConfig_f = nullptr;

typedef char(*DB_FileExists)(const char* zoneName, FF_DIR source);
DB_FileExists _DB_FileExists = nullptr;

typedef char(*LUI_Error)(const char* error, void* luiVm);
LUI_Error _LUI_Error = nullptr;

typedef char(*_printf)(const char* const Format, ...);
_printf __printf = nullptr;

typedef void(*Load_GfxBuildInfo)(bool atStreamStart);
Load_GfxBuildInfo _Load_GfxBuildInfo = nullptr;

typedef bool(*LUI_BeginEvent)(LocalClientNum_t localClientNum, const char* eventName, void* luaVM);
LUI_BeginEvent _LUI_BeginEvent = nullptr;

typedef bool(*CG_NotifyVirtualLobbySceneLoaded)(const char* name);
CG_NotifyVirtualLobbySceneLoaded _CG_NotifyVirtualLobbySceneLoaded = nullptr;

typedef int(*Scr_IsValidGameType)(const char* gametype);
Scr_IsValidGameType _Scr_IsValidGameType = nullptr;

void hook_CM_LoadMap(const char* name, int* checksum) {
    if (name) {
        Console::printf("Loading Map: %s", name);
    }
    else {
        Console::infoPrint("Invalid name passed into CM_LoadMap");
    }
    _CM_LoadMap(name, checksum);
}

void hook_FS_Startup(const char* gameName) {
    Console::print("----- FS_Startup -----");
    _FS_Startup(gameName);
}

void hook_SV_Shutdown(const char* finalmsg) {
    Console::print("----- Server Shutdown -----");
    _SV_Shutdown(finalmsg);
}

void hook_SV_SpawnServer(const char* server, int mapIsPreloaded, int savegame, int isRestart) {
    Console::print("------ Server Initialization ------");
    Console::printf("Server: %s", server);
    Console::printf("[match] server start: preloaded=%d savegame=%d restart=%d",
        mapIsPreloaded, savegame, isRestart);
    
    _SV_SpawnServer(server, mapIsPreloaded, savegame, isRestart);
}

void hook_LUI_LoadLuaFile(const char* filename, void* luivm) {
    Console::printf("LUI: Loading LUA File \"%s\"", filename);
    _LUI_LoadLuaFile(filename, luivm);
}

void hook_LUI_Init(void* func, void* alsofunc) {
    Console::print("LUI: Starting up...");
    _LUI_Init(func, alsofunc);
}

void hook_DB_TryLoadXFileInternal(const char* zoneName, int zoneFlags, int isBaseMap) {
    Console::printf("Loading Zone: %s.ff", zoneName);
    _DB_TryLoadXFileInternal(zoneName, zoneFlags, isBaseMap);
}

void hook_DB_LoadXZone(XZoneInfo* zoneInfo, __int64 zoneCount, __int64 waitAlloc, __int64 skipReadAlwaysLoadedAssets) {
	// Zone-load probe: two lines per zone, so it fired on every fastfile at boot and
	// on every map load. Gated behind demo_verbose; the useful "Adding fastfile"
	// line still always prints.
	if (demo_playback::verbose())
	{
		const auto byte_before = *reinterpret_cast<const std::uint8_t*>(0x27CDC67_b); // IDA byte_27CEC67
		Console::printf("[probe] A58D0 ENTER 27CEC67_before=%u zoneCount=%lld waitAlloc=%lld",
			static_cast<unsigned>(byte_before), zoneCount, waitAlloc);
	}
    Console::printf("Adding fastfile '%s' to queue", zoneInfo->name);
    _DB_LoadXZone(zoneInfo, zoneCount, waitAlloc, skipReadAlwaysLoadedAssets);
	if (demo_playback::verbose())
	{
		const auto byte_after = *reinterpret_cast<const std::uint8_t*>(0x27CDC67_b); // IDA byte_27CEC67
		Console::printf("[probe] A58D0 LEAVE 27CEC67_after=%u", static_cast<unsigned>(byte_after));
	}
}

void hook_G_InitGame(int levelTime, unsigned int randomSeed, int restart, int registerDvars, int savegame) {
    Console::print("------- Game Initialization -------");
    Console::print("gamename: S2");
    Console::print("gamedate: Jan 11 2026");
    _G_InitGame(levelTime, randomSeed, restart, registerDvars, savegame);
}

void hook_Online_PatchStreamer_va(const char* label, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char cleanFmt[1024];
    size_t j = 0;
    for (size_t i = 0; fmt[i] != '\0' && j < sizeof(cleanFmt) - 1; ++i) {
        if (fmt[i] != '\n' && fmt[i] != '\r') {
            cleanFmt[j++] = fmt[i];
        }
    }
    cleanFmt[j] = '\0';

    std::string cleanerFmt = GameUtil::sanitizeFormatWidths(cleanFmt); //it was looking awful in both consoles

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), cleanerFmt.c_str(), args);
    Console::printf("%s: %s\n", label, buffer);

    va_end(args);
}

void hook_Com_WriteConfig_f(int localClientNum, const char* name) {
    dvar_t* fs_debug = Functions::_Dvar_FindVar("1467");
    if (fs_debug) {
        if (fs_debug->current.integer) { //fs_debug
            Console::printf("Writing to config file '%s' for local client %d", name, localClientNum);
        }
    }
    
    
    _Com_WriteConfig_f(localClientNum, name);
}

void hook_DB_FileExists(const char* zoneName, FF_DIR source) {
    dvar_t* fs_debug = Functions::_Dvar_FindVar("1467");
    if (fs_debug) {
        if (fs_debug->current.integer) { //fs_debug
            Console::printf("Checking if file '%s' exists in %s", zoneName, source ? "usermaps directory" : "default directory");
        }
    }
    _DB_FileExists(zoneName, source);
}

void hook_LUI_Error(const char* error, void* luiVm) {
    const char* rawError = GameUtil::safeCString(error);
    const char* translatedError = error;

    if (Functions::_SEH_SafeTranslateString && error) {
        translatedError = GameUtil::safeCString(Functions::_SEH_SafeTranslateString(error));
    }

    Console::printf("LUI Error (raw): %s", rawError);

    if (translatedError && std::strcmp(rawError, translatedError) != 0) {
        Console::printf("LUI Error (translated): %s", translatedError);
    }

    logLuiVmState(luiVm);
    _LUI_Error(error, luiVm);
}

void hook_printf(const char* const Format, ...) {
    if (!Format) {
        Console::print("format is NULL");
        return;
    }
    constexpr size_t BUFSZ = 4096;
    char buf[BUFSZ];

    va_list args;
    va_start(args, Format);
    std::vsnprintf(buf, BUFSZ, Format, args);
    va_end(args);

    Console::printf(buf);
}


void printGfxBuildInfo(uintptr_t addr) {
    GfxBuildInfo** p = reinterpret_cast<GfxBuildInfo**>(addr);
    GfxBuildInfo* info = *p;
    Console::printf("bspCommandline   : %s", GameUtil::safeCString(info->bspCommandline));
    Console::printf("lightCommandline : %s", GameUtil::safeCString(info->lightCommandline));
    Console::printf("bspTimestamp     : %s", GameUtil::safeCString(info->bspTimestamp));
    Console::printf("lightTimestamp   : %s", GameUtil::safeCString(info->lightTimestamp));
}



void hook_Load_GfxBuildInfo(bool atStreamStart) {
    _Load_GfxBuildInfo(atStreamStart);
    dvar_t* printWorldInfo = Functions::_Dvar_FindVar("printWorldInfo");
    if (printWorldInfo) {
        if (printWorldInfo->current.enabled) {
            printGfxBuildInfo(0x98E17D0_b);
        }
    }
    
}

void hook_LUI_BeginEvent(LocalClientNum_t localClientNum, const char* eventName, void* luaVM) {
    std::string s = std::string(eventName);

    if (s != "mousemove" && s != "mouseup" && s != "mousedown") {
        Console::printf("LUI_BeginEvent: %s", eventName);
    }
    

    _LUI_BeginEvent(localClientNum, eventName, luaVM);
}

void CG_NotifyVirtualLobbySceneLoaded_hookfunc(const char* name) {
    Console::printf("Virtual Lobby: Loaded BSP '%s'", name);
    _CG_NotifyVirtualLobbySceneLoaded(name);
}

int Scr_IsValidGameType_hookfunc(const char* gametype) {
    int result = _Scr_IsValidGameType(gametype);
    if (!result) {
        Console::printf("g_gametype %s is not a valid gametype, defaulting to dm", gametype);
    }
    return result;
}

void PrintPatches::init() {
	Console::infoPrint(__FUNCTION__);

    //Virtual Lobby: Loaded BSP '%s'
    Hook::create("CG_NotifyVirtualLobbySceneLoaded", 0x41BED0_b, &CG_NotifyVirtualLobbySceneLoaded_hookfunc, &_CG_NotifyVirtualLobbySceneLoaded);

    //Loading Map:
    Hook::create("CM_LoadMap", 0x63C300_b, &hook_CM_LoadMap, &_CM_LoadMap);

    //----- FS_Startup -----
    Hook::create("FS_Startup", 0x756330_b, &hook_FS_Startup, &_FS_Startup);

    //----- Server Shutdown -----
    Hook::create("SV_Shutdown", 0x6DAF50_b, &hook_SV_Shutdown, &_SV_Shutdown);
    
    //------ Server Initialization ------
    Hook::create("SV_SpawnServer", 0x6DB350_b, &hook_SV_SpawnServer, &_SV_SpawnServer);
    
    //LUI: Loading LUA File \"%s\""
    Hook::create("LUI_LoadLuaFile", 0xC5150_b, &hook_LUI_LoadLuaFile, &_LUI_LoadLuaFile);
    
    //LUI: Starting up...
    Hook::create("LUI_Init", 0xC3460_b, &hook_LUI_Init, &_LUI_Init);
    
    //Loading Zone:
    Hook::create("DB_TryLoadXFileInternal", 0xABE30_b, &hook_DB_TryLoadXFileInternal, &_DB_TryLoadXFileInternal);
    
    //Adding fastfile '%s' to queue
    Hook::create("DB_LoadXZone", 0xA48D0_b, &hook_DB_LoadXZone, &_DB_LoadXZone);
    
    //------- Game Initialization -------
    Hook::create("G_InitGame", 0x55EC90_b, &hook_G_InitGame, &_G_InitGame);
    
    //Online_PatchStreamer printing
    Hook::create("Online_PatchStreamer_va", 0x2900F0_b, &hook_Online_PatchStreamer_va, &_Online_PatchStreamer_va);
    
    //fs_debug required
    //Writing to config file '%s' for local client %d
    Hook::create("Com_WriteConfig_f", 0x9C8E0_b, &hook_Com_WriteConfig_f, &_Com_WriteConfig_f);
    
    //fs_debug required
    //Checking if file '%s' exists in %s
    Hook::create("DB_FileExists", 0x9FE50_b, &hook_DB_FileExists, &_DB_FileExists);
    
    
    //LUI_Error
    Hook::create("LUI_Error", 0xBD900_b, &hook_LUI_Error, &_LUI_Error);
    
    //GfxWorld build info
    Hook::create("Load_GfxBuildInfo", 0x4AD840_b, &hook_Load_GfxBuildInfo, &_Load_GfxBuildInfo);

    //Scr_IsValidGameType
    //g_gametype %s is not a valid gametype, defaulting to dm
    Hook::create("Scr_IsValidGameType", 0x5AE670_b, &Scr_IsValidGameType_hookfunc, &_Scr_IsValidGameType);
}
