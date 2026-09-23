#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <format>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <iostream>
#include "../src/demo/engine_image_policy.hpp"
std::vector<unsigned char> live(0x4000,0);
std::wstring input_path;
DWORD fake_path(HMODULE,wchar_t* out,DWORD){wcscpy_s(out,32768,input_path.c_str());return (DWORD)input_path.size();}
HMODULE fake_module(const wchar_t*){return reinterpret_cast<HMODULE>(live.data());}
#define GetModuleFileNameW fake_path
#define GetModuleHandleW fake_module
struct Console {static void printf(const char*,...) {}};
namespace mod_build {constexpr int NUMBER=20;}
namespace demo_engine_probe {
bool copy_code(void* d,const void* s,size_t n){std::memcpy(d,s,n);return true;}
// ACTUAL_EXPORT_FUNCTION
}
int main(){
 const auto dir=std::filesystem::absolute("obj/engine-image-fixture");std::filesystem::create_directories(dir);
 input_path=(dir/"input.bin").wstring();std::vector<unsigned char> disk(4096,0);
 auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(disk.data());dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=128;
 auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(disk.data()+128);nt->Signature=IMAGE_NT_SIGNATURE;
 nt->FileHeader.NumberOfSections=3;nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);
 nt->FileHeader.TimeDateStamp=123;nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=0x4000;
 auto* sec=IMAGE_FIRST_SECTION(nt);
 const char* names[]={".text",".data",".rdata"};
 for(int i=0;i<3;i++){
  std::memcpy(sec[i].Name,names[i],strlen(names[i]));sec[i].VirtualAddress=(i+1)*0x1000;
  sec[i].PointerToRawData=1024+i*512;sec[i].SizeOfRawData=256;sec[i].Misc.VirtualSize=256;
  sec[i].Characteristics=i==1?0xC0000040:0x40000040;
  std::memset(disk.data()+sec[i].PointerToRawData,10+i,256);
  std::memset(live.data()+sec[i].VirtualAddress,80+i,256);
 }
 std::memcpy(live.data(),disk.data(),1024);
 {std::ofstream f(input_path,std::ios::binary);f.write((char*)disk.data(),disk.size());}
 demo_engine_probe::write_image();
 std::ifstream f(dir/"main/s2mp-build-20-engine.analysis.bin",std::ios::binary);
 std::vector<unsigned char> out((std::istreambuf_iterator<char>(f)),{});assert(out.size()==disk.size());
 for(int i=0;i<3;i++)for(int j=0;j<256;j++)assert(out[sec[i].PointerToRawData+j]==(i==1?11:80+i));
 std::ifstream original(input_path,std::ios::binary);std::vector<unsigned char> after((std::istreambuf_iterator<char>(original)),{});
 assert(after==disk);assert(reinterpret_cast<IMAGE_NT_HEADERS64*>(out.data()+128)->OptionalHeader.ImageBase==(uintptr_t)live.data());
 std::cout<<"Actual image exporter: code/rdata copied; live writable data excluded; original unchanged\n";
}
