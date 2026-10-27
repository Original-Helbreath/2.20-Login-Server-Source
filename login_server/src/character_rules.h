#pragma once

#include "common_types.h"
#include "login_messages.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Create-time format checks the Login server makes before it bothers the World (plan M13): they fail
// fast on requests no World would accept. They are format only; the World server is authoritative for
// whether a name is taken, whether a slot is free and every other rule, and answers for those itself.
namespace hb::server::login
{
	constexpr std::size_t max_character_name_length = 10;
	constexpr std::uint8_t min_character_stat = 10;
	constexpr std::uint8_t max_character_stat = 14;
	constexpr unsigned character_stat_total = 70;

	// 1..10 ASCII letters and digits, the same alphabet as account names.
	constexpr bool is_valid_character_name(std::string_view name)
	{
		if (name.empty() || name.size() > max_character_name_length)
			return false;
		for (const char c : name)
		{
			const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			const bool digit = c >= '0' && c <= '9';
			if (!letter && !digit)
				return false;
		}
		return true;
	}

	// Every stat 10..14 and the six adding up to 70.
	constexpr bool is_valid_character_stats(const hb::net::character_stats& stats)
	{
		const std::uint8_t values[] = { stats.strength, stats.vitality, stats.dexterity, stats.intelligence, stats.magic, stats.charisma };
		unsigned total = 0;
		for (const std::uint8_t value : values)
		{
			if (value < min_character_stat || value > max_character_stat)
				return false;
			total += value;
		}
		return total == character_stat_total;
	}

	// ok when the request is well formed enough to forward; otherwise the result the client is told:
	// invalid_name, invalid_stats, or failed for a sex or skin outside the select-screen values (sex 1 or 2,
	// skin 1..3).
	constexpr hb::net::login::character_result check_create_format(std::string_view name, std::uint8_t sex, std::uint8_t skin,
		const hb::net::character_stats& stats)
	{
		using hb::net::login::character_result;
		if (!is_valid_character_name(name))
			return character_result::invalid_name;
		if (!is_valid_character_stats(stats))
			return character_result::invalid_stats;
		if (sex < 1 || sex > 2 || skin < 1 || skin > 3)
			return character_result::failed;
		return character_result::ok;
	}
}
