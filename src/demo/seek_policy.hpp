#pragma once
#include <cstdint>
namespace demo_seek_policy
{
    struct Pick { int index = -1; int time = -1; };
    template<class Slots, class Eligible>
    Pick choose(const Slots& slots, const int target, Eligible eligible)
    {
        Pick result;
        for (const auto& slot : slots)
            if (slot.time >= 0 && slot.time <= target && slot.time > result.time
                && eligible(slot.index))
                result = {slot.index, slot.time};
        return result;
    }
    inline bool reached(const int target, const int actual)
    {
        const auto error = static_cast<std::int64_t>(actual) - target;
        return actual >= 0 && error >= -5 && error <= 5;
    }
}
