from pathlib import Path

source = Path('src/demo/demo_camera.cpp').read_text(encoding='utf-8-sig')
start = source.index('        void __fastcall angles_to_axis_stub(')
opening = source.index('{', start)
end, depth = opening + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1

prefix = r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
uintptr_t return_address=100, g_freecam_axis_return=100;
#define _ReturnAddress() reinterpret_cast<void*>(return_address)
float g_roll=30;
std::atomic<uint64_t> g_final_roll_calls{0};
std::atomic<float> g_final_roll_input{0},g_final_roll_applied{0};
namespace demo_native {
bool playing=true,active=true,seeking=false; int mode=2;
bool native_playing(){return playing;}
bool cgame_active(){return active;}
bool seek_in_progress(){return seeking;}
int camera_mode(){return mode;}
}
bool custom_playing=false;
bool demo_is_playing(){return demo_native::playing || custom_playing;}
namespace theater_camera {
constexpr int THEATER_CAMERA_FREECAM=2;
int get_mode(){return demo_native::mode;}
}
bool accessible=true;
bool readable(const void*,size_t){return accessible;}
int calls=0;
float received[4];
const float* received_pointer=nullptr;
void engine(const float* angles,float* axis){
 ++calls; received_pointer=angles; memcpy(received,angles,12);
 axis[0]=angles[2];
}
auto g_angles_to_axis_orig=&engine;
'''
tests = r'''
int main(){
 float angles[4]={12,45,0,99}, axis[9]={};
 for(float roll: {30.f,-30.f,0.f}){
  g_roll=roll; calls=0; angles_to_axis_stub(angles,axis);
  assert(calls==1 && received[2]==roll && axis[0]==roll);
  assert(received[0]==12 && received[1]==45);
  assert(angles[2]==0 && received_pointer!=angles);
 }
 assert(g_final_roll_calls==3 && g_final_roll_applied==0);
 demo_native::playing=false;custom_playing=true;g_roll=30;
 calls=0;angles_to_axis_stub(angles,axis);
 assert(calls==1 && received[2]==30 && axis[0]==30);
 custom_playing=false;
 for(int gate=0;gate<7;++gate){
  return_address=100;demo_native::playing=true;demo_native::active=true;
  demo_native::seeking=false;demo_native::mode=2;accessible=true;g_roll=30;
  switch(gate){
   case 0:return_address=101;break;
   case 1:demo_native::playing=false;break;
   case 2:demo_native::active=false;break;
   case 3:demo_native::seeking=true;break;
   case 4:demo_native::mode=1;break;
   case 5:accessible=false;break;
   case 6:g_roll=std::numeric_limits<float>::quiet_NaN();break;
  }
  calls=0;angles_to_axis_stub(angles,axis);
  assert(calls==1 && received_pointer==angles && received[2]==0);
  assert(g_final_roll_calls==4);
 }
 std::cout<<"Actual roll callback: native and custom playback, original input preservation, single dispatch and seven bypass gates passed\n";
}
'''
Path('obj').mkdir(exist_ok=True)
Path('obj/test_final_roll.cpp').write_text(prefix + source[start:end] + tests)
