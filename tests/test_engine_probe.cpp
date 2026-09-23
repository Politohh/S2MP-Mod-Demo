#include <windows.h>
#include <format>
#include <cassert>
#include <iostream>
#include "../src/demo/engine_probe.hpp"
int main()
{
    using namespace demo_engine_probe;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    assert(memory);
    const auto addr = reinterpret_cast<std::uintptr_t>(memory);
    assert(!executable_range(addr,32));
    memory[0] = 0xAB;
    DWORD old = 0;
    assert(VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&old));
    assert(executable_range(addr,4096));
    assert(!executable_range(addr,4097));
    unsigned char copied=0;
    assert(copy_code(&copied,memory,1) && copied == 0xAB);
    assert(VirtualProtect(memory,4096,PAGE_NOACCESS,&old));
    assert(!executable_range(addr,1));
    assert(!copy_code(&copied,memory,1));
    assert(!executable_range(0,16));
    assert(!executable_range(UINTPTR_MAX-1,16));
    VirtualFree(memory,0,MEM_RELEASE);
    std::cout << "Engine probe memory guards passed\n";
}
