#pragma once

#include "BuildMap.hpp"
#include "Console.hpp"
#include "GameUtil.hpp"
#include "demo/demo_game.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

// Counters belong to existing hooks. No network calls, readiness overrides or
// client-state writes: a loading stall must preserve the evidence behind it.
namespace live_match_diagnostics
{
	inline std::atomic<unsigned int> write_enter{}, write_leave{}, parse_enter{}, parse_leave{}, create_commands{};
	inline std::array<std::atomic<unsigned int>, 16> opcodes{};

	template <typename T>
	inline T read(const void* pointer, T fallback)
	{
		if (!GameUtil::isReadablePtr(pointer, sizeof(T))) return fallback;
		T value;
		std::memcpy(&value, pointer, sizeof(value));
		return value;
	}

	inline int field(const void* object, std::size_t offset)
	{
		return object ? read(static_cast<const std::uint8_t*>(object) + offset, -1) : -1;
	}

	inline void report(int state)
	{
		if (state < demo_game::CA_CONNECTED) return;
		const auto* connection = demo_game::clc_for();
		Console::printf("[match] flow: txCalls=%u/%u rxCalls=%u/%u createCmd=%u serverSeq=%d reliable=%d/%d",
			write_enter.load(std::memory_order_relaxed), write_leave.load(std::memory_order_relaxed),
			parse_enter.load(std::memory_order_relaxed), parse_leave.load(std::memory_order_relaxed),
			create_commands.load(std::memory_order_relaxed),
			field(connection, demo_game::CLC_SERVER_MESSAGE_SEQUENCE),
			field(connection, demo_game::CLC_RELIABLE_SEQUENCE), field(connection, demo_game::CLC_RELIABLE_ACKNOWLEDGE));
		Console::printf("[match] decoded first opcodes (session totals): gs0=%u op1=%u cmd2=%u cmd3=%u table4=%u match5=%u snap6=%u end7=%u",
			opcodes[0].load(std::memory_order_relaxed), opcodes[1].load(std::memory_order_relaxed),
			opcodes[2].load(std::memory_order_relaxed), opcodes[3].load(std::memory_order_relaxed),
			opcodes[4].load(std::memory_order_relaxed), opcodes[5].load(std::memory_order_relaxed),
			opcodes[6].load(std::memory_order_relaxed), opcodes[7].load(std::memory_order_relaxed));

		// These extra layouts were read from the Steam native packet writer and
		// timeout checker. Do not apply them to another game build.
		if (build_map::current() != build_map::Build::Steam) return;
		const auto* paused = read(reinterpret_cast<const void*>(0x1BD27B0_b), static_cast<const std::uint8_t*>(nullptr));
		Console::printf("[match] engine: hub=%d cl_paused=%d demoState=%d netType=%d outgoingSeq=%d serverClock=%d",
			read(reinterpret_cast<const void*>(0x1BD26F8_b), std::uint8_t{0}), field(paused, 0x10),
			field(connection, 0x40260), field(connection, 0x54840), field(connection, 0x54830),
			read(reinterpret_cast<const void*>(0xC5FAA44_b), -1));
		const auto* clients = read(reinterpret_cast<const void*>(0xC5FAA58_b), static_cast<const std::uint8_t*>(nullptr));
		const int count = read(reinterpret_cast<const void*>(0xC5FAA50_b), std::uint8_t{0});
		if (!clients || count < 1 || count > 48) return;
		int printed = 0;
		for (int i = 0; i < count && printed < 4; ++i)
		{
			const auto* client = clients + static_cast<std::size_t>(i) * 0x11E870;
			const int client_state = field(client, 0);
			if (client_state <= 0) continue;
			Console::printf("[match] server client %d: state=%d testClient=%d lastPacketTime=%d",
				i, client_state, field(client, 0x42158), field(client, 0x41E20));
			++printed;
		}
	}
}
