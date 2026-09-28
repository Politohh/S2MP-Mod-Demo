#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// =====================================================================
//  DEMO LIBRARY — read a .demo well enough to name and describe it
// =====================================================================
//
// The engine names its recordings `x0147_6a77b6ea.demo`, which tells the
// user nothing. Everything needed to do better is inside the file, and the
// format is fully proven:
//
//   header  +0x00 u32 version    == 29
//           +0x04 u32 headerSize == 79384, which is ALSO the body offset
//           +0x08 u32 recording client slot
//   footer  last 8 bytes: [u32 footerSize][u32 magic == 29]
//           body at (len - 8 - footerSize) begins [29, 3984]
//           and carries the MAP NAME at body + 272
//   body    a byte-tagged packet stream from +79384:
//           0 end | 1 archive (185 B) | 2 snapshot | 3 alt snapshot | 4 cmds
//
// The map name is two seeks. Duration needs a walk of the packet stream, but
// only of the TYPE BYTES and lengths -- the client archive (type 1) carries an
// uncompressed serverTime at packet offset 37, so first-to-last is the
// duration without decompressing anything.
//
// Nothing here touches the engine. It is pure file I/O and is safe to call
// from any thread.

namespace demo_library
{
	struct Info
	{
		bool ok = false;              // header + footer validated
		std::string map;              // "mp_forest_01", or empty
		std::int32_t duration_ms = -1;// -1 if not walked / not determinable
		std::uint32_t client = 0;     // recording client slot (header +0x08)
		std::uintmax_t size = 0;
		std::string date;             // "2026-08-31 17:45" from the file mtime
		std::string reason;           // why ok == false
	};

	// Cheap: header + footer only (two seeks). No duration.
	[[nodiscard]] Info describe(const std::filesystem::path& path);

	// Adds duration by walking the packet stream. Sequential, but it reads the
	// whole file, so call it for ONE demo (the selected row), not for a list.
	[[nodiscard]] Info describe_full(const std::filesystem::path& path);

	// "mp_forest_01_2026-08-31_1745" -- no extension, already filename-safe.
	// Falls back to the existing stem when the map cannot be read.
	[[nodiscard]] std::string suggest_name(const std::filesystem::path& path,
		const Info& info);

	// Rename in place, keeping the extension and refusing to clobber. Returns
	// the new path on success. `why` explains a failure.
	[[nodiscard]] std::optional<std::filesystem::path> rename(
		const std::filesystem::path& path, const std::string& new_stem,
		std::string& why);

	// Rename to suggest_name(). No-op (and success) if it already looks named.
	[[nodiscard]] std::optional<std::filesystem::path> auto_rename(
		const std::filesystem::path& path, std::string& why);

	[[nodiscard]] bool remove(const std::filesystem::path& path, std::string& why);

	// Our own .dm_s2 container. Reads only the first few KB: the map name comes
	// from its map_header chunk (id 1, right after the 51-byte gen_header), the
	// date from the file time. ok == true once the map header was found.
	[[nodiscard]] Info describe_custom(const std::filesystem::path& path);

	[[nodiscard]] std::string human_size(std::uintmax_t bytes);
	[[nodiscard]] std::string human_duration(std::int32_t ms);
}
