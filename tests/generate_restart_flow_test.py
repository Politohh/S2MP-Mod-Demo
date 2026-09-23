from pathlib import Path
source=Path('src/demo/demo_native.cpp').read_text(encoding='utf-8-sig')
def function(name):
 start=source.index(name+'(');start=source.rfind('\n',0,start)+1
 a=source.index('{',start);depth=1;i=a+1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[start:i]
defs=source[source.index('        enum class RestartStage'):source.index('        bool begin_restart_seek')]
pre=r'''#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <filesystem>
#include <format>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <iostream>
struct Console { static void printf(const char*,...) {} };
constexpr int LOCAL_CLIENT_0=0;
unsigned long long clock_ms=100;
unsigned long long GetTickCount64(){return clock_ms;}
std::atomic<bool> g_native_playing{true};
bool ready=true,paused_state=true,seek_ok=true;
std::atomic<unsigned> g_playback_generation{7};
std::filesystem::path g_playing_path="test.demo";
int camera=2,time_ms=18000,seek_calls=0;
float speed=0.05f;
bool engine_paused(){return paused_state;}
float engine_timescale(){return speed;}
int camera_mode(){return camera;}
void set_timescale(float v){speed=v;}
void set_camera_mode(int v){camera=v;}
void toggle_pause(){paused_state=!paused_state;}
int demo_time_smooth(){return time_ms;}
bool cgame_active(){return ready;}
char clc[262756]{};
bool have_clc=true, have_connection=true, g_saw_demo_state=false, g_eof_seen=false;
int connection=10;
int* connstate_ptr(int){return have_connection?&connection:nullptr;}
const char* clc_native(int){return have_clc?clc:nullptr;}
bool readable(const void*,size_t){return true;}
bool seek_absolute_now(int t,bool){++seek_calls;if(seek_ok)time_ms=t;return seek_ok;}
namespace demo_player { bool result=true;void report_seek_result(bool r){result=r;} }
struct Args {int nesting=0;int argc[1]={2};const char* argv[1][2]={{"demo_finish_restart","1"}};} args;
struct GameUtil {
 static inline bool accept=true;
 static inline std::vector<std::string> queued;
 static bool Cbuf_AddText(int,const std::string& s){if(!accept)return false;queued.push_back(s);return true;}
 static const Args* getCmdArgs(){return &args;}
};
'''
body='\n'.join(function(n) for n in ['begin_restart_seek','poll_restart_seek','finish_restart_seek','cancel_restart_seek','seek_in_progress','poll_session'])
tests=r'''
void setup(){
 have_clc=true;have_connection=true;g_saw_demo_state=false;g_eof_seen=false;connection=10;
 g_restart_stage=RestartStage::Idle;g_restart_id=0;GameUtil::queued.clear();GameUtil::accept=true;
 g_native_playing=true;ready=true;paused_state=true;seek_ok=true;seek_calls=0;speed=.05f;camera=2;
 time_ms=18000;clock_ms=100;g_playback_generation=7;g_playing_path="test.demo";
 int state=2;std::memcpy(clc+262752,&state,4);args.argv[0][1]="1";demo_player::result=true;
}
void ready_new(){
 g_native_playing=false;poll_restart_seek();assert(g_restart_stage==RestartStage::WaitingPlayback);
 g_native_playing=true;g_playback_generation++;time_ms=1000;poll_restart_seek();
 assert(g_restart_stage==RestartStage::Queued);
}
int main(){
 setup();assert(begin_restart_seek(5000,true));assert(seek_in_progress());
 poll_restart_seek();assert(g_restart_stage==RestartStage::WaitingClose);assert(GameUtil::queued.size()==1);
 g_native_playing=false;poll_restart_seek();assert(g_restart_stage==RestartStage::WaitingPlayback);
 g_native_playing=true;poll_restart_seek();assert(GameUtil::queued.size()==1); // old generation
 g_playback_generation++;ready=false;poll_restart_seek();assert(GameUtil::queued.size()==1);
 ready=true;GameUtil::accept=false;poll_restart_seek();assert(g_restart_stage==RestartStage::WaitingPlayback);
 GameUtil::accept=true;poll_restart_seek();assert(g_restart_stage==RestartStage::Queued);
 finish_restart_seek();assert(!seek_in_progress());assert(time_ms==5000);assert(!paused_state);assert(speed==.05f);assert(camera==2);
 setup();assert(begin_restart_seek(6000,false));ready_new();finish_restart_seek();assert(paused_state&&time_ms==6000);
 setup();paused_state=false;assert(begin_restart_seek(6000,false));ready_new();finish_restart_seek();assert(!paused_state);
 setup();GameUtil::accept=false;assert(!begin_restart_seek(5000,true));assert(!seek_in_progress());
 setup();assert(begin_restart_seek(5000,true));assert(!begin_restart_seek(9000,true));ready_new();
 cancel_restart_seek();finish_restart_seek();assert(seek_calls==0);assert(!seek_in_progress());
 setup();assert(begin_restart_seek(5000,true));clock_ms+=60001;poll_restart_seek();assert(!seek_in_progress());assert(!demo_player::result);
 setup();assert(begin_restart_seek(5000,true));ready_new();seek_ok=false;finish_restart_seek();assert(paused_state&&!demo_player::result);
 setup();assert(begin_restart_seek(5000,true));g_native_playing=false;poll_restart_seek();
 g_native_playing=true;g_playback_generation++;g_playing_path="other.demo";poll_restart_seek();assert(!seek_in_progress());
 // Exercise actual session observation, not a manually cleared native flag.
 setup();poll_session();assert(g_saw_demo_state);assert(begin_restart_seek(5000,true));
 have_clc=false;connection=0;ready=false;poll_session();
 assert(!g_native_playing);assert(g_restart_stage==RestartStage::WaitingPlayback);
 have_clc=true;connection=10;ready=true;g_native_playing=true;++g_playback_generation;
 poll_session();assert(g_restart_stage==RestartStage::Queued);finish_restart_seek();assert(time_ms==5000&&!paused_state);
 // A retained stale demoState=2 must not mask an observed disconnect.
 setup();poll_session();assert(begin_restart_seek(5000,true));connection=0;poll_session();assert(!g_native_playing);
 // Loading has not reached playback yet: missing clc is not a finished session.
 setup();have_clc=false;connection=0;poll_session();assert(g_native_playing);
 // Temporary memory unavailability without a disconnected state is inconclusive.
 setup();poll_session();have_clc=false;have_connection=false;poll_session();assert(g_native_playing);
 // The ordinary retained state=0 shutdown path remains supported.
 setup();poll_session();int stopped=0;std::memcpy(clc+262752,&stopped,4);poll_session();assert(!g_native_playing);
 // Paused/EOF playback remains loaded while its engine state is active.
 setup();poll_session();g_eof_seen=true;poll_session();assert(g_native_playing);
 std::cout<<"Restart lifecycle regression scenarios passed\n";
}
'''
Path('obj/test_restart_flow.cpp').write_text(pre+defs+body+tests,encoding='utf-8')
