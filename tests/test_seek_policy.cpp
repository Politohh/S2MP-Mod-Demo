#include "../src/demo/seek_policy.hpp"
#include <vector>
#include <cassert>
#include <iostream>
int main()
{
    using namespace demo_seek_policy;
    const auto any = [](int) { return true; };
    // Actual log 9: only slot 3 at 309200; neither J nor left-arrow may
    // replace the requested time with that later frame.
    const std::vector<Pick> log9{{3, 309200}};
    assert(choose(log9, 307380, any).index == -1);
    assert(choose(log9, 297100, any).index == -1);
    assert(choose(log9, 309200, any).index == 3);
    const std::vector<Pick> mixed{{5,310000},{2,306000},{8,299000}};
    assert(choose(mixed,307380,any).index == 2);
    assert(choose(mixed,307380,[](int i){return i != 2;}).index == 8);
    assert(choose(mixed,297100,any).index == -1);
    assert(!reached(307380,309500));
    assert(!reached(307380,-1));
    assert(reached(307380,307380));
    assert(reached(307380,307385));
    assert(!reached(307380,307386));
    assert(!reached(2147483647,0));
    std::cout << "Seek policy regression checks passed\n";
}
