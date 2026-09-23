#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace demo_engine_probe
{
    constexpr bool analysis_section(std::string_view name, std::uint32_t flags)
    {
        return !(flags & 0x80000000u) // IMAGE_SCN_MEM_WRITE
            && (name == ".text" || name == ".rdata" || name == ".pdata" || name == "_RDATA");
    }
    constexpr bool image_range(std::size_t offset, std::size_t size, std::size_t limit)
    {
        return offset <= limit && size <= limit - offset;
    }
}
