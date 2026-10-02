#pragma once

namespace demo_gui
{
	void init();
	void toggle();
	void on_demo_stop();
}

// Dummy export class referenced by legacy tooling.
class S2MPTheaterDummy
{
public:
	static void touch() {}
};
