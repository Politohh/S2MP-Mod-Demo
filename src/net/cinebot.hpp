#pragma once

#include <vector>

namespace cinebot
{
	struct bot_view
	{
		int slot = -1;
		bool alive = false;
		bool frozen = false;
		float x = 0, y = 0, z = 0;
	};

	void init();
	void tick();
	[[nodiscard]] bool available();
	[[nodiscard]] int selected_slot();
	void select(int slot);
	[[nodiscard]] std::vector<bot_view> list();
	void spawn_at_crosshair();
	void move_selected_to_crosshair();
	void toggle_selected_freeze();
}
