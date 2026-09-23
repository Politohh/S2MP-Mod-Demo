from pathlib import Path
s=Path('src/GameUtil.cpp').read_text(encoding='utf-8-sig')
a=s.index('bool GameUtil::Cbuf_AddText(');b=s.index('\n}\n',a)+2
pre=r'''#include <windows.h>
#include <cstdint>
#include <string>
#include <cstring>
#include <cassert>
#include <iostream>
enum LocalClientNum_t { LOCAL_CLIENT_0=0, LOCAL_CLIENT_1=1 };
struct Buffer { char* text; uint32_t capacity; uint32_t used; } buffers[2];
char texts[2][128]{};
char** commandTextBuffers;
size_t operator"" _b(unsigned long long){return reinterpret_cast<size_t>(buffers);}
struct Console{static void printf(const char*,...) {}};
struct Functions{
 static inline int active=0,locks=0;
 static int _GetAvailableCommandBufferIndex(){return active;}
 static void _Sys_EnterCriticalSection(int){++locks;}
 static void _Sys_LeaveCriticalSection(int){--locks;}
};
struct GameUtil{
 static bool isReadablePtr(const void* p,size_t){return p!=nullptr;}
 static bool Cbuf_AddText(LocalClientNum_t,const std::string&);
};
'''
test=r'''
int main(){
 buffers[0]={texts[0],128,0};buffers[1]={texts[1],128,0};
 Functions::active=-1;assert(GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,"demo_play test"));
 assert(std::string(texts[0])=="demo_play test\n");assert(buffers[0].used==15);
 assert(GameUtil::Cbuf_AddText(LOCAL_CLIENT_1,"hello"));assert(std::string(texts[1])=="hello\n");
 Functions::active=0;assert(GameUtil::Cbuf_AddText(LOCAL_CLIENT_1,"next"));
 assert(std::string(texts[0])=="demo_play test\nnext\n");
 Functions::active=2;assert(!GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,"bad"));
 Functions::active=-1;assert(!GameUtil::Cbuf_AddText(static_cast<LocalClientNum_t>(2),"bad"));
 buffers[0].used=129;assert(!GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,"bad"));
 buffers[0].used=127;assert(!GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,"bad"));
 buffers[0].used=0;buffers[0].text=nullptr;assert(!GameUtil::Cbuf_AddText(LOCAL_CLIENT_0,"bad"));
 assert(Functions::locks==0);
 std::cout<<"Menu command routing and bounds tests passed\n";
}
'''
Path('obj/test_command_route.cpp').write_text(pre+s[a:b]+test,encoding='utf-8')
