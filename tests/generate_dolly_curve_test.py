from pathlib import Path

s = Path('src/demo/dolly.cpp').read_text(encoding='utf-8-sig')
start = s.index('[[nodiscard]] float timed_hermite(')
opening = s.index('{', start)
end, depth = opening + 1, 1
while depth:
    depth += (s[end] == '{') - (s[end] == '}')
    end += 1

head = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
'''
tail = r'''
int main() {
 // Serv's uneven marker intervals: 272, 241, 442 ms.
 constexpr int t0=35546,t1=35818,t2=36059,t3=36501;
 auto left=[&](float f){return timed_hermite(-97,-10,132,230,t0,t1,t2,t3,f);};
 auto right=[&](float f){return timed_hermite(-10,132,230,230,t1,t2,t3,t3,f);};
 assert(std::fabs(left(1)-right(0))<0.0001f);
 const float step=0.001f;
 const float left_velocity=(left(1)-left(1-step))/(step*(t2-t1));
 const float right_velocity=(right(step)-right(0))/(step*(t3-t2));
 assert(std::fabs(left_velocity-right_velocity)<0.003f);
 // Two markers remain a straight line; equal intervals stay finite.
 auto two=[&](float f){return timed_hermite(0,0,100,100,0,0,100,100,f);};
 assert(std::fabs(two(0.25f)-25)<0.001f);
 assert(std::fabs(two(0.75f)-75)<0.001f);
 std::cout<<"Timed dolly: unequal-spacing position and velocity continuity passed\n";
}
'''
Path('obj').mkdir(exist_ok=True)
Path('obj/test_dolly_curve.cpp').write_text(head+s[start:end]+tail)
