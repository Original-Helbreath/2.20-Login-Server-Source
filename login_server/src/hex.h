#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hb::server::login
{
	// Lowercase hex, two digits per byte.
	std::string to_hex(std::span<const std::uint8_t> bytes);

	// Accepts upper or lower case. False on an odd length or a non-hex digit.
	bool from_hex(std::string_view text, std::vector<std::uint8_t>& bytes);
}
