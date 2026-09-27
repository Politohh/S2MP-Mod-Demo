#pragma once
#include <string>
#include <vector>
#include "structs.h"
#include "FuncPointers.h"
#include "Console.hpp"


struct AssetPoolStats {
	unsigned int count = 0;
	unsigned int totalSize = 0;
};


class GameUtil {
public:
	static std::string sanitizeFileName(const std::string& name);
	static bool areWeHost();
	static bool isReadablePtr(const void* ptr, size_t bytes);
	static const char* safeCString(const char* s, size_t maxLen = 4096);
	static bool bytesMatch(const uint8_t* addr, const std::initializer_list<uint8_t>& pattern);
	static void setCustomSplashScreen();
	static void blockGameInput(bool b);
	static std::string colorToString(const unsigned __int8 color[4]);
	static bool decodeDvarSecureBool(const dvar_t* dvar);
	static void setDvarSecureBool(dvar_t* dvar, bool newValue);
	static void setDvarSecureFloat(dvar_t* dvar, float newValue);
	// Reads a DVAR_TYPE_FLOAT_SECURE (0x0B) value. Such dvars store the float
	// XOR-encoded at +16..+31 (Dvar_SetVariant case 0xB), so reading +16 as a
	// float gives garbage. The caller must have checked the type and that
	// dvar+0x10..+0x1F is readable.
	static float getDvarSecureFloat(const dvar_t* dvar);
	// Uses the engine's secure-int getter; current.integer is encoded for type 0x0C.
	static int getDvarSecureInt(const dvar_t* dvar);
	static std::string dvarValueToString(const dvar_t* dvar, bool showQuotesAroundStrings, bool truncateFloats);
	static std::string getDvarDomainAsString(const dvar_t* dvar);
	static std::string toLower(const std::string& str);
	static std::string toUpper(const std::string& input);
	static bool isOnlyWhitespace(const std::string& str);
	static std::string getStringFromClipboard();
	//static void Cbuf_AddText(LocalClientNum_t localClientNum, std::string text);
	// Returns FALSE when the command was DROPPED -- either no command buffer was
	// free, or the one we got is full. Both used to fail silently, which is how a
	// deferred demo restart went missing and left the game sitting at the menu
	// (2026-09-22). Callers that must not lose a command have to retry.
	static bool Cbuf_AddText(LocalClientNum_t localClientNum, const std::string& command);
	static float safeStringToFloat(const std::string& str);
	static int safeStringToInt(const std::string& str);
	static std::string getAddressAsString(void* address);
	static char asciiToLower(char in);
	static bool stringToBool(const std::string& str);
	static void addCommand(char const* name, void (*func)());

	static void overrideCommand(char const* name, void(*func)());

	static std::string sanitizeFormatWidths(const char* fmt);

	static CmdArgs* getCmdArgs();

	static gentity_s* G_getLocalPlayer();

	static bool getPlayerPosition(float* outPos);

	static cg_t* CG_GetLocalClientGlobals();

	static unsigned char CG_GetLocalClientNum();

	static gentity_s* getGentity(int entId);

	static std::list<cmd_function_s> cmdHeap;

	template <typename Callback>
	static AssetPoolStats assetPoolTraverseHelper(XAssetType targetType, Callback&& callback) {
		AssetPoolStats stats{};

		auto db_hashTable = (uint32_t*)0x27D0110_b;
		auto g_assetEntryPool = (uint8_t*)0x50F50E0_b;

		constexpr uint32_t HASH_SIZE = 0x48800;
		constexpr uint32_t MAX_ENTRIES = 0x200000;
		constexpr uint32_t ENTRY_SIZE = 32;
		constexpr uint32_t NEXT_INDEX_OFFSET = 20;

		if (targetType > ASSET_TYPE_DLOGROUTES) {
			Console::printf("Invalid asset type %u", targetType);
			return stats;
		}

		for (uint32_t hash = 0; hash < HASH_SIZE; ++hash) {
			uint32_t index = db_hashTable[hash];

			while (index) {
				if (index >= MAX_ENTRIES) {
					break;
				}

				uint8_t* entry = g_assetEntryPool + (index * ENTRY_SIZE);

				XAssetType assetType = *(XAssetType*)(entry + 0);
				uint32_t nextIndex = *(uint32_t*)(entry + NEXT_INDEX_OFFSET);

				if (assetType == targetType) {
					const char* assetName = Functions::_DB_GetXAssetName(entry);

					if (assetName) {
						XAsset* asset = reinterpret_cast<XAsset*>(entry);

						if (!callback(asset, assetName)) {
							return stats;
						}
					}

					++stats.count;
					stats.totalSize += Functions::_DB_GetXAssetTypeSize(assetType);
				}

				if (nextIndex >= MAX_ENTRIES) {
					break;
				}

				index = nextIndex;
			}
		}

		return stats;
	}
};

