#pragma once
#include "ModBuild.hpp"
#include "demo/engine_image_policy.hpp"
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
    // Explicit offline-analysis export. Keep initialized writable sections from
    // disk; never copy the live .data section or arbitrary process memory.
    inline void write_image()
    {
        try
        {
            wchar_t module_path[32768]{};
            const auto length = GetModuleFileNameW(nullptr, module_path, 32768);
            if (!length || length >= 32768) return;
            const std::filesystem::path source(module_path);
            std::ifstream input(source, std::ios::binary | std::ios::ate);
            const auto file_size = input.tellg();
            if (!input || file_size < 4096 || file_size > 512LL * 1024 * 1024)
            { Console::printf("[engine-image] invalid input size"); return; }
            std::vector<unsigned char> bytes(static_cast<std::size_t>(file_size));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0
                || static_cast<std::size_t>(dos->e_lfanew) > bytes.size() - sizeof(IMAGE_NT_HEADERS64)) return;
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
                || nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return;
            const auto section_offset = static_cast<std::size_t>(dos->e_lfanew) + 24 + nt->FileHeader.SizeOfOptionalHeader;
            if (section_offset > bytes.size() || nt->FileHeader.NumberOfSections >
                (bytes.size() - section_offset) / sizeof(IMAGE_SECTION_HEADER)) return;
            const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto* live_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            const auto* live_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + live_dos->e_lfanew);
            if (live_nt->FileHeader.TimeDateStamp != nt->FileHeader.TimeDateStamp
                || live_nt->OptionalHeader.SizeOfImage != nt->OptionalHeader.SizeOfImage)
            { Console::printf("[engine-image] disk/runtime image mismatch"); return; }
            auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(bytes.data() + section_offset);
            unsigned copied = 0;
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                const auto& section = sections[i];
                char name[9]{};
                std::memcpy(name, section.Name, 8);
                // Only these non-writable image sections are needed for code,
                // cross references, strings and function boundaries.
                if (!analysis_section(name, section.Characteristics)) continue;
                const auto size = static_cast<std::size_t>((std::min)(section.SizeOfRawData, section.Misc.VirtualSize));
                if (!size) continue;
                if (!image_range(section.PointerToRawData, size, bytes.size())
                    || !image_range(section.VirtualAddress, size, nt->OptionalHeader.SizeOfImage)
                    || !copy_code(bytes.data() + section.PointerToRawData,
                        reinterpret_cast<const void*>(module + section.VirtualAddress), size))
                { Console::printf("[engine-image] failed to read section %s; no image written", name); return; }
                ++copied;
                Console::printf("[engine-image] copied %s RVA=%X bytes=%zu", name, section.VirtualAddress, size);
            }
            if (!copied) { Console::printf("[engine-image] no eligible sections"); return; }
            // Loaded absolute addresses refer to the ASLR base. This is a mixed
            // analysis image, NOT an executable for use as a game replacement.
            nt->OptionalHeader.ImageBase = module;
            const auto folder = source.parent_path() / "main";
            std::filesystem::create_directories(folder);
            const auto output = folder / std::format("s2mp-build-{}-engine.analysis.bin", mod_build::NUMBER);
            std::ofstream out(output, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            out.close();
            if (!out) { Console::printf("[engine-image] output write failed; do not send partial file"); return; }
            Console::printf("[engine-image] exported %u sections to %s (analysis only; writable data remains from disk)",
                copied, output.string().c_str());
        }
        catch (const std::exception& e) { Console::printf("[engine-image] failed: %s", e.what()); }
    }

}
