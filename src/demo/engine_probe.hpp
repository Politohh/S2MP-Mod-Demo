#pragma once
#include "ModBuild.hpp"
#include "Console.hpp"
#include "game.h"
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>
#include <cstring>

namespace demo_engine_probe
{
    // Leaf SEH boundary: protect the explicit diagnostic from a page becoming
    // inaccessible while copied. Only module code ranges are requested below.
    inline bool copy_code(void* dest, const void* source, std::size_t size)
    {
        __try { std::memcpy(dest, source, size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    inline bool executable_range(std::uintptr_t address, std::size_t size)
    {
        const auto end = address + size;
        if (end < address) return false;
        while (address < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))
                || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
            const DWORD protection = mbi.Protect & 0xff;
            if (protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE
                && protection != PAGE_EXECUTE_WRITECOPY) return false;
            const auto next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= address) return false;
            address = next;
        }
        return true;
    }

    inline void write()
    {
        try
        {
            wchar_t module_path[32768]{};
            const auto length = GetModuleFileNameW(nullptr, module_path, 32768);
            if (!length || length >= 32768) { Console::printf("[engine-probe] cannot resolve game path"); return; }
            const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return;
            const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
            const auto folder = std::filesystem::path(module_path).parent_path() / "main";
            std::filesystem::create_directories(folder);
            const auto path = folder / std::format("s2mp-build-{}-engine-probe.txt", mod_build::NUMBER);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) { Console::printf("[engine-probe] cannot write %s", path.string().c_str()); return; }
            out << "S2MP targeted engine code probe\nbuild=" << mod_build::NUMBER
                << std::format(" module=0x{:X} imageSize=0x{:X} peTimestamp=0x{:X}\n", module, image_size, nt->FileHeader.TimeDateStamp);
            out << "Runtime code may contain installed hook branches. No gameplay buffers or user data are included.\n";
            struct Range { const char* name; std::uintptr_t literal; std::size_t size; };
            // _b literals are RVAs minus 0x1000 on Steam, as in the existing
            // hooks. Restrict resolved ranges to this module's executable pages.
            const Range ranges[] = {
                {"CG_CalcViewValues", 0x8B9C0, 0x4000},
                {"CL_Demo_ProcessKeyFrameJump", 0x916ED0, 0x1400},
                {"CL_Demo_WriteKeyFrame", 0x913790, 0x540},
                {"CL_Demo_ShouldGenerateKeyFrame", 0x9193A0, 0x200},
                {"CL_Demo_FreeCameraMove", 0x912AE0, 0x600},
                {"GetAvailableCommandBufferIndex", 0x4A05C0, 0x300},
                // Follow the restore's packet replay and existing reset path.
                // Addresses come from build-16 runtime calls / existing source;
                // names remain descriptive until their full role is verified.
                {"RestoreReplay_9188C0", 0x9178C0, 0xB00},
                {"ResetReplay_919CE0", 0x918CE0, 0x700},
                {"GenerationGate_7DFD0", 0x7CFD0, 0x100}
            };
            int written = 0;
            for (const auto& range : ranges)
            {
                const auto address = _b(range.literal);
                out << std::format("\nRANGE {} address=0x{:X} size=0x{:X}\n", range.name, address, range.size);
                if (address < module || address - module > image_size
                    || range.size > image_size - (address - module)
                    || !executable_range(address, range.size))
                { out << "SKIPPED: outside module or not readable executable memory\n"; continue; }
                std::vector<unsigned char> bytes(range.size);
                if (!copy_code(bytes.data(), reinterpret_cast<void*>(address), bytes.size()))
                { out << "SKIPPED: copy fault\n"; continue; }
                for (std::size_t i = 0; i < bytes.size(); i += 32)
                {
                    out << std::format("{:X}:", address + i);
                    for (std::size_t j = i; j < bytes.size() && j < i + 32; ++j)
                        out << std::format(" {:02X}", bytes[j]);
                    out << '\n';
                }
                ++written;
            }
            out.flush();
            if (!out) { Console::printf("[engine-probe] write failed: %s", path.string().c_str()); return; }
            Console::printf("[engine-probe] exported %d/%zu code ranges to %s; send this file with s2mp_console.log",
                written, std::size(ranges), path.string().c_str());
        }
        catch (const std::exception& e) { Console::printf("[engine-probe] failed: %s", e.what()); }
    }
}
