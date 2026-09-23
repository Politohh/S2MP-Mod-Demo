#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "demo/demo_library.hpp"

// =====================================================================
//  DEMO PLAYER — one list, one transport, two engines underneath
// =====================================================================
//
// S2MP-Mod can play two kinds of demo and they are NOT interchangeable:
//
//   .demo    the ENGINE's own format, driven by cl_demo_play. Recorded
//            automatically by the game, seeks by keyframe, has the
//            engine's own camera and timescale.
//   .dm_s2   our own container, replayed by feeding the recorded network
//            messages back through CL_ParseServerMessage.
//
// The user should not have to know which is which. Everything here
// dispatches on the file EXTENSION, so Play/Stop/Pause/Seek/Speed each
// do the right thing for whatever is loaded.
//
// This file adds no hooks and owns no engine state -- it is a router.

namespace demo_player
{
	enum class Kind
	{
		None,
		Engine,   // .demo   -> demo_native
		Custom,   // .dm_s2  -> demo_playback
	};

	struct Entry
	{
		std::filesystem::path path;
		std::string           name;      // filename, for display
		Kind                  kind = Kind::None;
		std::uintmax_t        size = 0;

		// Cheap metadata, read at refresh: two seeks per file. Only ENGINE
		// demos have it -- the custom container is a different format.
		demo_library::Info    info;
	};

	void init();

	// ---- library ----------------------------------------------------
	void refresh();
	const std::vector<Entry>& list();
	int  selected();
	void set_selected(int index);
	[[nodiscard]] const Entry* selected_entry();

	// ---- transport --------------------------------------------------
	[[nodiscard]] Kind active();                 // what is playing right now
	[[nodiscard]] bool playing();
	bool play(const std::filesystem::path& path);
	bool play_selected();
	void stop();

	[[nodiscard]] bool paused();
	void toggle_pause();

	// -1 when the active system cannot report a position.
	[[nodiscard]] float progress();
	[[nodiscard]] float timescale();
	void set_timescale(float value);

	// The active demo's own clock in ms (mid-seek: the pending target). -1 when
	// nothing is playing.
	[[nodiscard]] std::int32_t current_time();
	// Absolute seek; lands on `ms` and keeps the pause state. CLIENT THREAD:
	// from the GUI, queue `demo_seek_to <ms>` instead.
	bool seek_absolute(std::int32_t ms);
	[[nodiscard]] bool last_seek_failed();
	// Positive skips forward, negative rewinds. Both engines honour it the same
	// way. CLIENT THREAD, like seek_absolute.
	void seek_relative(std::int32_t ms);

	// Starts a demo that was asked for while another was still loaded, once
	// that one has closed. Call once a frame.
	void poll_pending();

	// ---- recording --------------------------------------------------
	// Automatic engine recording -- the normal way demos are made.
	[[nodiscard]] bool auto_record();
	void set_auto_record(bool on);
	// True while the manual .dm_s2 capture is running.
	[[nodiscard]] bool capturing();

	// Why the active/selected demo cannot be played, or nullptr.
	[[nodiscard]] const char* blocked_reason();

	// ---- library management ------------------------------------------
	// Duration needs a walk of the whole packet stream, so it is done ONCE
	// for the selected demo and cached rather than for every row.
	[[nodiscard]] const demo_library::Info& selected_details();
	// Rename every engine demo that still has its generated hex name.
	int tidy_names(std::string& summary);
}
