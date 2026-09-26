#pragma once

#include <algorithm>
#include <cmath>

// Native demo slow motion can advance cl.serverTime in game-tick-sized steps.
// Integrate the verified playback speed against a monotonic wall clock for the
// camera only, while leaving the engine's demo/seek clock untouched.
class dolly_slow_clock
{
public:
	void reset()
	{
		active_ = false;
		samples_ = 0;
		repeated_ = 0;
		max_engine_step_ = 0;
	}

	double sample(const int engine_ms, const double wall_ms, const float speed,
		const bool paused)
	{
		if (engine_ms < 0 || !std::isfinite(wall_ms) || !std::isfinite(speed)
			|| speed <= 0.0f || speed > 0.11f || paused)
		{
			reset();
			return engine_ms;
		}

		if (!active_)
		{
			start(engine_ms, wall_ms, speed);
			return smooth_ms_;
		}

		const double elapsed = wall_ms - last_wall_ms_;
		// A rewind, speed change or stalled process needs a new anchor. Ignore
		// ordinary ~16 ms engine steps: following those would restore the jolt.
		if (engine_ms < last_engine_ms_ - 4 || elapsed < 0.0 || elapsed > 500.0
			|| std::abs(speed - last_speed_) > 0.02f
			|| std::abs(static_cast<double>(engine_ms) - smooth_ms_) > 50.0)
		{
			start(engine_ms, wall_ms, speed);
			return smooth_ms_;
		}

		smooth_ms_ += elapsed * speed;
		last_wall_ms_ = wall_ms;
		last_speed_ = speed;
		++samples_;
		if (engine_ms == last_engine_ms_) ++repeated_;
		if (engine_ms > last_engine_ms_)
			max_engine_step_ = (std::max)(max_engine_step_, engine_ms - last_engine_ms_);
		last_engine_ms_ = engine_ms;
		return smooth_ms_;
	}

	[[nodiscard]] int samples() const { return samples_; }
	[[nodiscard]] int repeated() const { return repeated_; }
	[[nodiscard]] int max_engine_step() const { return max_engine_step_; }

private:
	void start(const int engine_ms, const double wall_ms, const float speed)
	{
		reset();
		active_ = true;
		smooth_ms_ = engine_ms;
		last_wall_ms_ = wall_ms;
		last_engine_ms_ = engine_ms;
		last_speed_ = speed;
	}

	bool active_ = false;
	double smooth_ms_ = 0.0;
	double last_wall_ms_ = 0.0;
	int last_engine_ms_ = 0;
	float last_speed_ = 1.0f;
	int samples_ = 0;
	int repeated_ = 0;
	int max_engine_step_ = 0;
};
