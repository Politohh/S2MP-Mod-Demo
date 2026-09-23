/////////////////////////////////////////
//           Game Util
//	Utility functions for the mod
/////////////////////////////////////////
#include "pch.h"
#include "GameUtil.hpp"

#include <sstream>
#include <algorithm>
#include <list>
#include "resource.h"

typedef unsigned int uint32;
char** commandTextBuffers;

/**
 * @brief Sanitizes a string for safe use as a file name.
 *
 * Replaces characters that are invalid in file names with underscores.
 *
 * @param name The input file name.
 * @return A sanitized file name string.
 */
std::string GameUtil::sanitizeFileName(const std::string& name) {
    std::string safe = name;
    for (char& c : safe) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return safe;
}

bool GameUtil::areWeHost() {
    dvar_t* sv_running = *(dvar_t**)0x1BD2778_b;
    return sv_running->current.enabled;
}

bool GameUtil::isReadablePtr(const void* ptr, size_t bytes = 1) {
    if (!ptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT) return false;

    const DWORD protect = mbi.Protect & 0xFF;
    const bool readable =
        protect == PAGE_READONLY ||
        protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;

    if (!readable) {
        return false;
    }
    if (mbi.Protect & PAGE_GUARD) {
        return false;
    }
    if (mbi.Protect & PAGE_NOACCESS) {
        return false;
    }

    //ensure [ptr, ptr+bytes) stays in region
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t end = base + mbi.RegionSize;
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    if (p < base || p + bytes > end) {
        return false;
    }

    return true;
}

const char* GameUtil::safeCString(const char* s, size_t maxLen) {
    if (!s) {
        return "<null>";
    }


    //common sentinel
    const uintptr_t v = reinterpret_cast<uintptr_t>(s);
    if (v == 0xFFFFFFFFFFFFFFFFull) {
        return "<invalid>";
    }

    if (!isReadablePtr(s, 1)) {
        return "<unreadable>";
    }

    for (size_t i = 0; i < maxLen; ++i) {
        if (!isReadablePtr(s + i, 1)) {
            return "<unterminated/unreadable>";
        }
        if (s[i] == '\0') {
            return s;
        }
    }
    return "<unterminated>";
}

/**
 * @brief Compares a memory region against a byte pattern.
 *
 * Checks whether the bytes at the given address match the provided
 * initializer-list pattern exactly.
 *
 * @param addr Pointer to the memory to compare.
 * @param pattern The byte pattern to match.
 * @return true if all bytes match; false otherwise.
 */
bool GameUtil::bytesMatch(const uint8_t* addr, const std::initializer_list<uint8_t>& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (addr[i] != *(pattern.begin() + i)) {
            return false;
        }
    }
    return true;
}


void GameUtil::setCustomSplashScreen() {

}

/**
 * @brief Enables or disables game input processing (keyCatchers).
 *
 * Used primarily by the internal console to block game input
 * while accepting keyboard input.
 *
 * @param b true to block game input; false to restore input.
 */
void GameUtil::blockGameInput(bool b) {
    if (b) {
        int* pKeyCatchers = (int*)0x1BAE4E0_b;
        *pKeyCatchers |= 1;
    }
    else {
        int* pKeyCatchers = (int*)0x1BAE4E0_b;
        *pKeyCatchers &= ~1;
    }
}


/**
 * @brief Converts a color array to a normalized string representation.
 *
 * Converts RGBA color values (0�255) to normalized floats (0.0�1.0)
 * and formats them as a space-separated string.
 *
 * @param color RGBA color array.
 * @return A formatted string representation of the color.
 */
std::string GameUtil::colorToString(const unsigned __int8 color[4]) {
    std::ostringstream oss;
    for (int i = 0; i < 4; ++i) {
        oss << std::fixed << std::setprecision(2) << (color[i] * (1.0f / 255.0f));
        if (i < 3) {
            oss << " ";
        }
    }
    return oss.str();
}

bool GameUtil::decodeDvarSecureBool(const dvar_t* dvar) {
    if (!dvar || dvar->type != DVAR_TYPE_BOOL_SECURE) {
        Console::printf("non secure bool '%s' passed into decodeDvarSecureBool", dvar->name);
        return false;
    }

    const uint32_t K0 = *reinterpret_cast<uint32_t*>(0xD8F124_b);
    const uint32_t K1 = *reinterpret_cast<uint32_t*>(0xD8F148_b);
    const uint32_t K2 = *reinterpret_cast<uint32_t*>(0xD8F14C_b);
    const uint32_t K3 = *reinterpret_cast<uint32_t*>(0xD8F150_b);

    const uint32_t BOOL_OFFSET = *reinterpret_cast<uint32_t*>(0x14DA5E0_b);

    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dvar) + 0x10);

    const uint32_t e0 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x10);
    const uint32_t e1 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x14);
    const uint32_t e2 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x18);
    const uint32_t e3 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x1C);

    uint32_t decoded[4];

    decoded[0] = ~K0 ^ p ^ e2;
    decoded[1] = p ^ e0 ^ e2 ^ K1;
    decoded[2] = p ^ e3 ^ e0 ^ K2;
    decoded[3] = p ^ e1 ^ e3 ^ K3;
    return *(reinterpret_cast<const uint8_t*>(decoded) + BOOL_OFFSET) != 0;
}
void GameUtil::setDvarSecureBool(dvar_t* dvar, bool newValue) {
    if (!dvar || dvar->type != DVAR_TYPE_BOOL_SECURE) {
        return;
    }

    const uint32_t K0 = *reinterpret_cast<uint32_t*>(0xD8F124_b);
    const uint32_t K1 = *reinterpret_cast<uint32_t*>(0xD8F148_b);
    const uint32_t K2 = *reinterpret_cast<uint32_t*>(0xD8F14C_b);
    const uint32_t K3 = *reinterpret_cast<uint32_t*>(0xD8F150_b);

    const uint32_t BOOL_OFFSET = *reinterpret_cast<uint32_t*>(0x14DA5E0_b);

    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dvar) + 0x10);

    uint32_t e0 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x10);
    uint32_t e1 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x14);
    uint32_t e2 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x18);
    uint32_t e3 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x1C);

    uint32_t decoded[4];

    decoded[0] = ~K0 ^ p ^ e2;
    decoded[1] = p ^ e0 ^ e2 ^ K1;
    decoded[2] = p ^ e3 ^ e0 ^ K2;
    decoded[3] = p ^ e1 ^ e3 ^ K3;

    const uint8_t encodedBoolValue = newValue ? 1 : 0;

    std::memcpy(reinterpret_cast<uint8_t*>(decoded) + BOOL_OFFSET, &encodedBoolValue, sizeof(encodedBoolValue));

    e2 = decoded[0] ^ ~K0 ^ p;
    e0 = decoded[1] ^ p ^ e2 ^ K1;
    e3 = decoded[2] ^ p ^ e0 ^ K2;
    e1 = decoded[3] ^ p ^ e3 ^ K3;

    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x10) = e0;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x14) = e1;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x18) = e2;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x1C) = e3;

    dvar->modified = true;
}

float decodeDvarSecureFloat(const dvar_t* dvar) {
    const uint32_t K0 = *reinterpret_cast<uint32_t*>(0xD8F164_b);
    const uint32_t K1 = *reinterpret_cast<uint32_t*>(0xD8F168_b);
    const uint32_t K2 = *reinterpret_cast<uint32_t*>(0xD8F16C_b);
    const uint32_t K3 = *reinterpret_cast<uint32_t*>(0xD8F170_b);
    const uint32_t FLOAT_OFFSET = *reinterpret_cast<uint32_t*>(0x14DA5E8_b);
    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dvar) + 0x10);
    const uint32_t e0 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x10);
    const uint32_t e1 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x14);
    const uint32_t e2 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x18);
    const uint32_t e3 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dvar) + 0x1C);

    uint32_t decoded[4];

    decoded[0] = ~K0 ^ p ^ e2;
    decoded[1] = p ^ e0 ^ e2 ^ K1;
    decoded[2] = p ^ e3 ^ e0 ^ K2;
    decoded[3] = p ^ e1 ^ e3 ^ K3;

    uint32_t raw;
    std::memcpy(&raw, reinterpret_cast<const uint8_t*>(decoded) + FLOAT_OFFSET, sizeof(raw));

    float out;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
}

// The decode is the exact inverse of Dvar_SetVariant's case 0xB (IDA 0xB30C0):
// same keys (IDA 0xD90164..0xD90170), same per-session slot offset
// (dword_14DB5E8), same XOR chain run backwards.
float GameUtil::getDvarSecureFloat(const dvar_t* dvar) {
    return decodeDvarSecureFloat(dvar);
}

void GameUtil::setDvarSecureFloat(dvar_t* dvar, float newValue) {
    if (!dvar || dvar->type != DVAR_TYPE_FLOAT_SECURE) {
        return;
    }

    const float minValue = dvar->domain.value.min;
    const float maxValue = dvar->domain.value.max;

    if (newValue < minValue) {
        newValue = minValue;
    }
    else if (newValue > maxValue) {
        newValue = maxValue;
    }

    const uint32_t K0 = *reinterpret_cast<uint32_t*>(0xD8F164_b);
    const uint32_t K1 = *reinterpret_cast<uint32_t*>(0xD8F168_b);
    const uint32_t K2 = *reinterpret_cast<uint32_t*>(0xD8F16C_b);
    const uint32_t K3 = *reinterpret_cast<uint32_t*>(0xD8F170_b);

    const uint32_t FLOAT_OFFSET = *reinterpret_cast<uint32_t*>(0x14DA5E8_b);

    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dvar) + 0x10);

    uint32_t e0 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x10);
    uint32_t e1 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x14);
    uint32_t e2 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x18);
    uint32_t e3 = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x1C);

    uint32_t decoded[4];

    decoded[0] = ~K0 ^ p ^ e2;
    decoded[1] = p ^ e0 ^ e2 ^ K1;
    decoded[2] = p ^ e3 ^ e0 ^ K2;
    decoded[3] = p ^ e1 ^ e3 ^ K3;

    std::memcpy(reinterpret_cast<uint8_t*>(decoded) + FLOAT_OFFSET, &newValue, sizeof(newValue));

    e2 = decoded[0] ^ ~K0 ^ p;
    e0 = decoded[1] ^ p ^ e2 ^ K1;
    e3 = decoded[2] ^ p ^ e0 ^ K2;
    e1 = decoded[3] ^ p ^ e3 ^ K3;

    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x10) = e0;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x14) = e1;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x18) = e2;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dvar) + 0x1C) = e3;

    dvar->modified = true;
}

std::string GameUtil::getDvarDomainAsString(const dvar_t* dvar) {
    if (!dvar) {
        return "Domain is <null dvar>";
    }

    auto floatToString = [](float value) -> std::string {
        std::ostringstream stream;
        stream << std::setprecision(9) << value;
        return stream.str();
        };

    auto floatRangeToString = [&](const char* typeName, const DvarLimits& domain) -> std::string {
        return std::string("Domain is ") + typeName + ": " +
            floatToString(domain.value.min) + " to " + floatToString(domain.value.max);
        };

    auto vectorRangeToString = [&](const char* typeName, const DvarLimits& domain) -> std::string {
        return std::string("Domain is ") + typeName + ": each component " +
            floatToString(domain.vector.min) + " to " + floatToString(domain.vector.max);
        };

    auto intRangeToString = [](const char* typeName, const DvarLimits& domain) -> std::string {
        return std::string("Domain is ") + typeName + ": " +
            std::to_string(domain.integer.min) + " to " + std::to_string(domain.integer.max);
        };

    switch (dvar->type)
    {
    case DVAR_TYPE_BOOL:
        return "Domain is bool: 0 or 1";

    case DVAR_TYPE_BOOL_SECURE:
        return "Domain is secure bool: 0 or 1";

    case DVAR_TYPE_FLOAT:
        return floatRangeToString("float", dvar->domain);

    case DVAR_TYPE_FLOAT_SECURE:
        return floatRangeToString("secure float", dvar->domain);

    case DVAR_TYPE_FLOAT_2:
        return vectorRangeToString("vec2", dvar->domain);

    case DVAR_TYPE_FLOAT_3:
        return vectorRangeToString("vec3", dvar->domain);

    case DVAR_TYPE_FLOAT_4:
        return vectorRangeToString("vec4", dvar->domain);

    case DVAR_TYPE_INT:
        return intRangeToString("int", dvar->domain);

    case DVAR_TYPE_INT_SECURE:
        return intRangeToString("secure int", dvar->domain);

    case DVAR_TYPE_ENUM:
    {
        const int maxIndex = dvar->domain.enumeration.stringCount - 1;
        if (maxIndex < 0) {
            return "Domain is enum: <empty>";
        }

        return "Domain is enum: 0 to " + std::to_string(maxIndex);
    }

    case DVAR_TYPE_STRING:
        return "Domain is string: any text";

    case DVAR_TYPE_COLOR:
        return vectorRangeToString("color", dvar->domain);

    case DVAR_TYPE_VEC3_ALT:
        return vectorRangeToString("vec3", dvar->domain);

    default:
        return "Domain is unsupported type: " + std::to_string(static_cast<int>(dvar->type));
    }
}
/**
 * @brief Converts a dvar value to string.
 *
 * Handles all supported dvar types and formats the output
 *
 * @param dvar Pointer to the dvar.
 * @param showQuotesAroundStrings Whether string values should be quoted.
 * @param truncateFloats Whether floating-point values should be truncated.
 * @return A string representation of the dvar's current value.
 */
std::string GameUtil::dvarValueToString(const dvar_t* dvar, bool showQuotesAroundStrings, bool truncateFloats) {
    if (!dvar) {
        return "<null dvar>";
    }
    auto floatToString = [truncateFloats](float value) -> std::string {
        if (truncateFloats) {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(1) << value;
            return stream.str();
        }

        return std::to_string(value);
        };

    switch (dvar->type)
    {
    case DVAR_TYPE_BOOL:
        return dvar->current.enabled ? "1" : "0";

    case DVAR_TYPE_BOOL_SECURE:
        return decodeDvarSecureBool(dvar) ? "1" : "0";

    case DVAR_TYPE_FLOAT:
        return floatToString(dvar->current.value);

    case DVAR_TYPE_FLOAT_2:
        return floatToString(dvar->current.vector[0]) + " " +
            floatToString(dvar->current.vector[1]);

    case DVAR_TYPE_FLOAT_3:
        return floatToString(dvar->current.vector[0]) + " " +
            floatToString(dvar->current.vector[1]) + " " +
            floatToString(dvar->current.vector[2]);

    case DVAR_TYPE_FLOAT_4:
        return floatToString(dvar->current.vector[0]) + " " +
            floatToString(dvar->current.vector[1]) + " " +
            floatToString(dvar->current.vector[2]) + " " +
            floatToString(dvar->current.vector[3]);

    case DVAR_TYPE_INT:
        return std::to_string(dvar->current.integer);

    case DVAR_TYPE_INT_SECURE:
        return "UNKNOWN TYPE. Send this dvar to Rattpak please!";

    case DVAR_TYPE_FLOAT_SECURE:
        return floatToString(decodeDvarSecureFloat(dvar));

    case DVAR_TYPE_ENUM:
    {
        // Best placeholder until your DvarLimits enum layout is confirmed.
        return "PLACEHOLDER: ENUM";

        /*
        if (!dvar->domain.enumeration.stringCount)
            return "";

        const int index = dvar->current.integer;

        if (index < 0 || index >= dvar->domain.enumeration.stringCount)
            return "";

        return dvar->domain.enumeration.strings[index];
        */
    }

    case DVAR_TYPE_STRING:
    {
        const char* str = dvar->current.string ? dvar->current.string : "";

        if (showQuotesAroundStrings)
            return "\"" + std::string(str) + "\"";

        return str;
    }

    case DVAR_TYPE_COLOR:
        return colorToString(dvar->current.color);

    case DVAR_TYPE_VEC3_ALT:
        // Type 0x9 used the same 3-float storage path as FLOAT_3 in RegisterNew.
        return floatToString(dvar->current.vector[0]) + " " +
            floatToString(dvar->current.vector[1]) + " " +
            floatToString(dvar->current.vector[2]);

    default:
        return "Unsupported type: " + std::to_string(static_cast<int>(dvar->type));
    }
}

/**
 * @brief Converts a string to lowercase.
 *
 * @param str The input string.
 * @return A lowercase version of the input string.
 */
std::string GameUtil::toLower(const std::string& str) {
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::tolower);
    return lowerStr;
}

/**
 * @brief Converts a string to uppercase.
 *
 * @param input The input string.
 * @return An uppercase version of the input string.
 */
std::string GameUtil::toUpper(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::toupper(c); });
    return result;
}

/**
 * @brief Checks whether a string contains only whitespace.
 *
 * @param str The string to check.
 * @return true if the string contains only whitespace; false otherwise.
 */
bool GameUtil::isOnlyWhitespace(const std::string& str) {
    std::string noSpaces;
    noSpaces.reserve(str.size()); //avoid reallocs

    std::remove_copy_if(
        str.begin(), str.end(),
        std::back_inserter(noSpaces),
        [](unsigned char c) { return std::isspace(c); }
    );

    return noSpaces.empty();
}

/**
 * @brief Retrieves text from the system clipboard.
 *
 * Reads text from the clipboard, truncating at the first newline
 * character if present. Basically only used for internal console.
 *
 * @return Clipboard text, or an empty string on failure.
 */
std::string GameUtil::getStringFromClipboard() {
    //open clipboard
    if (!OpenClipboard(nullptr)) {
        Console::print("Cannot open the clipboard");
        return "";
    }

    //make sure clipboard has text
    if (!IsClipboardFormatAvailable(CF_TEXT)) {
        Console::print("Clipboard does not contain text data");
        CloseClipboard();
        return "";
    }

    //get handle to clipboard data based text format
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (hData == nullptr) {
        Console::print("Cannot get clipboard data");
        CloseClipboard();
        return "";
    }

    //lock handle to get actual text ptr
    char* pszText = static_cast<char*>(GlobalLock(hData));
    if (pszText == nullptr) {
        Console::print("Cannot lock global handle");
        CloseClipboard();
        return "";
    }
    std::string text = pszText;
    GlobalUnlock(hData);
    CloseClipboard();

    //only paste upto newline
    size_t newlinePos = text.find('\n');
    if (newlinePos != std::string::npos) {
        text = text.substr(0, newlinePos);
    }
    //Console::print(text);

    return text;
}


/**
 * @brief Appends a command to the engine command buffer.
 *
 * Thread-safe wrapper for submitting commands to the engine.
 *
 * @param localClientNum The local client index.
 * @param command The command string to execute.
 */
bool GameUtil::Cbuf_AddText(LocalClientNum_t localClientNum, const std::string& command) {
    commandTextBuffers = reinterpret_cast<char**>(0xAA754A8_b);
    int bufferIndex = Functions::_GetAvailableCommandBufferIndex();
    // Probe RVA 0x4A15C0 scans two local-client active flags (stride 0x7B8).
    // It does NOT allocate command buffers. Menus legitimately return -1;
    // the caller's local client still owns a command text buffer there.
    if (bufferIndex == -1) bufferIndex = static_cast<int>(localClientNum);
    if (bufferIndex < 0 || bufferIndex >= 2) return false;

    Functions::_Sys_EnterCriticalSection(193);

    //Console::printf("[Cbuf_AddText] bufferIndex = %d", bufferIndex);
    //Console::printf("[Cbuf_AddText] commandTextBuffers = %p", static_cast<void*>(commandTextBuffers));

    char** commandBuffer = &commandTextBuffers[2 * bufferIndex];

    //Console::printf("[Cbuf_AddText] commandBuffer = %p", static_cast<void*>(commandBuffer));
    if (!isReadablePtr(commandBuffer, 16)) {
        Functions::_Sys_LeaveCriticalSection(193);
        return false;
    }
    uint32_t currentOffset = *(reinterpret_cast<uint32_t*>(commandBuffer) + 3);
    uint32_t bufferSize = *(reinterpret_cast<uint32_t*>(commandBuffer) + 2);

    std::string commandWithNewline = command + "\n";
    size_t commandLength = commandWithNewline.length();

    bool queued = false;
    if (*commandBuffer && bufferSize <= 0x100000 && currentOffset <= bufferSize
        && commandLength < bufferSize - currentOffset
        && isReadablePtr(*commandBuffer + currentOffset, commandLength + 1)) {
        strcpy_s(&(*commandBuffer)[currentOffset], bufferSize - currentOffset, commandWithNewline.c_str());
        *(reinterpret_cast<uint32_t*>(commandBuffer) + 3) += static_cast<uint32_t>(commandLength);
        queued = true;
    }
    else {
        // The other silent drop. Say so: a command that vanishes here is
        // indistinguishable from one that ran and did nothing.
        Console::printf("[Cbuf_AddText] command buffer full, dropped: %s", command.c_str());
    }

    Functions::_Sys_LeaveCriticalSection(193);
    return queued;
}


/**
 * @brief Safely converts a string to a float.
 *
 * Returns 0.0f if the conversion fails or if extra characters
 * are present.
 *
 * @param str The input string.
 * @return The converted float value, or 0.0f on failure.
 */
float GameUtil::safeStringToFloat(const std::string& str) {
    try {
        size_t pos;
        float result = std::stof(str, &pos);
        if (pos != str.length()) {
            return 0.0f;
        }
        return result;
    }
    catch (const std::exception&) {
        return 0.0f;
    }
}

/**
 * @brief Safely converts a string to an integer.
 *
 * Returns 0 if the conversion fails or if extra characters
 * are present.
 *
 * @param str The input string.
 * @return The converted integer value, or 0 on failure.
 */
int GameUtil::safeStringToInt(const std::string& str) {
    try {
        size_t pos;
        int result = std::stoi(str, &pos);
        if (pos != str.length()) {
            return 0;
        }
        return result;
    }
    catch (const std::exception&) {
        return 0;
    }
}

/**
 * @brief Converts a pointer address to a string.
 *
 * @param address Pointer to convert.
 * @return A string representation of the address.
 */
std::string GameUtil::getAddressAsString(void* address) {
    std::stringstream ss;
    ss << address;
    return ss.str();
}

/**
 * @brief Converts an ASCII character to lowercase.
 *
 * Intended for use with std::transform.
 *
 * @param in The input character.
 * @return The lowercase equivalent if applicable.
 */
char GameUtil::asciiToLower(char in) {
    if (in <= 'Z' && in >= 'A')
        return in - ('Z' - 'z');
    return in;
}

/**
 * @brief Converts a string to a boolean value.
 *
 * Accepts "1" or "true" (case-insensitive) as true.
 *
 * @param str The input string.
 * @return true if the string represents true; false otherwise.
 */
bool GameUtil::stringToBool(const std::string& str) {
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), GameUtil::asciiToLower);

    return (lowerStr == "1" || lowerStr == "true");
}


std::list<cmd_function_s> GameUtil::cmdHeap;

/**
 * @brief Registers a console command with the engine.
 *
 * @param name The command name.
 * @param func The function to execute.
 */
void GameUtil::addCommand(char const* name, void (*func)()) {
    cmdHeap.emplace_back();
    auto it = std::prev(cmdHeap.end());
    Functions::_Cmd_AddCommandInternal(name, func, &(*it));
}

/**
 * @brief Override an exsiting console command with the engine.
 *
 * @param name The command name to override.
 * @param func The function to execute.
 */
void GameUtil::overrideCommand(char const* name, void (*func)()) {
    Functions::_Cmd_RemoveCommand(name);
    GameUtil::addCommand(name, func);
}

/**
 * @brief Removes field width specifiers from printf-style format strings.
 *
 * Converts format specifiers like "%10d" into "%d". Very rare use case.
 *
 * @param fmt The input format string.
 * @return A sanitized format string.
 */
std::string GameUtil::sanitizeFormatWidths(const char* fmt) {
    std::string s(fmt);
    return std::regex_replace(s, std::regex("%(\\d+)([a-zA-Z])"), "%$2");
}


/**
 * @brief Retrieves the current command argument structure.
 *
 * @return Pointer to the CmdArgs structure, or nullptr if unavailable.
 */
CmdArgs* GameUtil::getCmdArgs() {
    CmdArgs* cmdArgs = reinterpret_cast<CmdArgs*>(0xAA752D0_b);
    if (!cmdArgs) {
        Console::printf("%s was called but cmdArgs was null!", __FUNCTION__);
        return nullptr;
    }

    return cmdArgs;
}



cg_t* GameUtil::CG_GetLocalClientGlobals() {
    const auto* rendererData = reinterpret_cast<const uintptr_t*>(0x8AFBB38_b);
    if (!GameUtil::isReadablePtr(rendererData, sizeof(*rendererData))) {
        return nullptr;
    }

    const uintptr_t rd = *rendererData;
    if (!rd) {
        return nullptr;
    }

    if (rd < 0x1E6C10) {
        return nullptr;
    }

    cg_t* cg = reinterpret_cast<cg_t*>(rd - 0x1E6C10);
    if (!GameUtil::isReadablePtr(cg, sizeof(cg->clientNum))) {
        return nullptr;
    }

    return cg;
}

unsigned char GameUtil::CG_GetLocalClientNum() {
    cg_t* cg = GameUtil::CG_GetLocalClientGlobals();
    if (!cg) {
        return 0xFF;
    }

    if (!GameUtil::isReadablePtr(&cg->clientNum, sizeof(cg->clientNum))) {
        return 0xFF;
    }

    return cg->clientNum;
}

gentity_s* GameUtil::getGentity(int entId) {
    if (entId < 0 || entId >= 2046) {
        return nullptr;
    }

    gentity_s* entity = reinterpret_cast<gentity_s*>(0x9ED3430_b + (entId * sizeof(gentity_s)));
    if (!GameUtil::isReadablePtr(entity, sizeof(gentity_s))) {
        return nullptr;
    }

    return entity;
}
gentity_s* GameUtil::G_getLocalPlayer() {
    unsigned char clientNum = GameUtil::CG_GetLocalClientNum();
    if (clientNum == 0xFF) {
        return nullptr;
    }
    return GameUtil::getGentity(clientNum);
}

bool GameUtil::getPlayerPosition(float* outPos) {
    if (!outPos) {
        return false;
    }

    unsigned char clientNum = GameUtil::CG_GetLocalClientNum();
    if (clientNum == 0xFF) {
        return false;
    }

    const auto* entityCount = reinterpret_cast<const int*>(0xA0E40D0_b);
    if (!GameUtil::isReadablePtr(entityCount, sizeof(*entityCount)) || *entityCount <= clientNum) {
        return false;
    }

    gentity_s* player = GameUtil::getGentity(clientNum);

    if (!player) {
        return false;
    }

    float* origin = player->origin;
    if (!GameUtil::isReadablePtr(origin, sizeof(float) * 3)) {
        return false;
    }

    outPos[0] = origin[0];
    outPos[1] = origin[1];
    outPos[2] = origin[2];

    return true;
}