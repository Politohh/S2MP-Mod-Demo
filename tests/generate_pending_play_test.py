from pathlib import Path
s=Path('src/demo/demo_player.cpp').read_text()
def fn(name):
 start=s.index(name+'(');start=s.rfind('\n',0,start)+1
 a=s.index('{',start);i=a+1;depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
# poll and command names only occur at their definitions before registration.
pre=r'''#include <mutex>
#include <string>
#include <filesystem>
#include <format>
#include <cstdlib>
#include <cassert>
#include <iostream>
struct Console{static void printf(const char*,...) {}};
constexpr int LOCAL_CLIENT_0=0;
std::mutex g_pending_lock;
std::string g_pending_name;
std::filesystem::path g_pending_path;
unsigned g_pending_id=0;
bool g_pending_dispatched=false;
unsigned long long g_pending_queued=0,g_pending_clear_since=0,clock_ms=100;
constexpr unsigned long long PENDING_SETTLE_MS=1500,PENDING_GIVE_UP_MS=30000;
bool active=false,play_ok=true;
int plays=0,queues=0;
std::filesystem::path last_path;
unsigned long long GetTickCount64(){return clock_ms;}
bool playing(){return active;}
bool play(const std::filesystem::path& p){++plays;last_path=p;return play_ok;}
struct Args {int nesting=0;int argc[1]={2};const char* argv[1][2]={{"demo_play_pending","1"}};} args;
struct GameUtil{
 static inline bool accept=true;
 static const Args* getCmdArgs(){return &args;}
 static bool Cbuf_AddText(int,const std::string&){if(!accept)return false;++queues;return true;}
};
'''
test=r'''
void reset(){g_pending_name="test.demo";g_pending_path="full/test.demo";g_pending_id=1;
 g_pending_dispatched=false;g_pending_queued=100;g_pending_clear_since=0;
 clock_ms=100;plays=queues=0;active=false;play_ok=true;GameUtil::accept=true;args.argv[0][1]="1";}
int main(){
 reset();active=true;poll_pending();assert(queues==0);
 active=false;poll_pending();clock_ms+=1499;poll_pending();assert(queues==0);
 ++clock_ms;GameUtil::accept=false;poll_pending();assert(!g_pending_dispatched&&!g_pending_name.empty());
 GameUtil::accept=true;poll_pending();poll_pending();assert(queues==1&&g_pending_dispatched);
 args.argv[0][1]="0";cmd_play_pending();assert(plays==0&&!g_pending_name.empty());
 args.argv[0][1]="1";cmd_play_pending();assert(plays==1&&last_path=="full/test.demo"&&g_pending_name.empty());
 reset();poll_pending();clock_ms+=1500;poll_pending();
 g_pending_name.clear();++g_pending_id;g_pending_dispatched=false;cmd_play_pending();assert(plays==0);
 reset();poll_pending();clock_ms+=1500;poll_pending();active=true;cmd_play_pending();assert(plays==0);
 reset();clock_ms+=30001;poll_pending();assert(g_pending_name.empty()&&queues==0);
 reset();poll_pending();clock_ms+=1500;poll_pending();play_ok=false;cmd_play_pending();assert(!g_pending_name.empty()&&!g_pending_dispatched);
 std::cout<<"Deferred reopen timing, retry and cancellation tests passed\n";
}
'''
Path('obj/test_pending_play.cpp').write_text(pre+fn('poll_pending')+'\n'+fn('cmd_play_pending')+test,encoding='utf-8')
