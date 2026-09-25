#pragma once

namespace reshade_capture
{
	// Called on the render thread. Lazily registers an optional ReShade add-on
	// callback; ordinary capture remains available when ReShade is absent.
	void tick();
	[[nodiscard]] bool active();
}
