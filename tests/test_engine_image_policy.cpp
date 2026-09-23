#include "../src/demo/engine_image_policy.hpp"
#include <limits>
using namespace demo_engine_probe;
static_assert(analysis_section(".text", 0x60000020));
static_assert(analysis_section(".rdata", 0x40000040));
static_assert(analysis_section(".pdata", 0x40000040));
static_assert(analysis_section("_RDATA", 0x40000040));
static_assert(!analysis_section(".data", 0xC0000040));
static_assert(!analysis_section(".text", 0xE0000020));
static_assert(!analysis_section(".heap", 0x40000040));
static_assert(image_range(8, 8, 16));
static_assert(!image_range(8, 9, 16));
static_assert(!image_range(17, 0, 16));
static_assert(!image_range(8, std::numeric_limits<std::size_t>::max(), 16));
int main() {}
