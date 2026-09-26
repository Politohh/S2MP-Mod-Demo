#include "../src/demo/dolly_clock.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
	dolly_slow_clock clock;
	constexpr int start = 53500;
	double previous = start;
	for (int frame = 0; frame < 300; ++frame)
	{
		const double wall_ms = frame * (1000.0 / 60.0);
		const double real_demo_ms = wall_ms * 0.05;
		// A 60 Hz demo tick arrives only every ~20 output frames at 0.05x.
		const int engine_ms = start + static_cast<int>(
			std::floor(real_demo_ms / (1000.0 / 60.0)) * (1000.0 / 60.0));
		const double smooth = clock.sample(engine_ms, wall_ms, 0.05f, false);
		if (frame > 0)
		{
			const double advance = smooth - previous;
			assert(advance > 0.8 && advance < 0.9);
		}
		previous = smooth;
	}
	assert(clock.repeated() > 250);
	assert(clock.max_engine_step() >= 16);

	// Rewind, pause, resume, and a speed change must re-anchor; none may
	// continue the previous shot's wall clock through a seek.
	assert(clock.sample(53000, 5010.0, 0.05f, false) == 53000.0);
	assert(clock.sample(53000, 5020.0, 0.05f, true) == 53000.0);
	assert(clock.sample(53000, 5030.0, 0.05f, false) == 53000.0);
	assert(clock.sample(53000, 5040.0, 0.10f, false) == 53000.0);
	assert(clock.sample(53100, 5050.0, 0.10f, false) == 53100.0);
	std::cout << "slow dolly clock: quantized-tick smoothing and resets passed\n";
}
