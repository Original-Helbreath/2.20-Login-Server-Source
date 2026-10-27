#include "hex.h"

namespace hb::server::login
{
	std::string to_hex(std::span<const std::uint8_t> bytes)
	{
		static constexpr std::string_view digits = "0123456789abcdef";
		std::string text;
		text.reserve(bytes.size() * 2);
		for (const std::uint8_t byte : bytes)
		{
			text.push_back(digits[byte >> 4]);
			text.push_back(digits[byte & 0x0f]);
		}
		return text;
	}

	bool from_hex(std::string_view text, std::vector<std::uint8_t>& bytes)
	{
		bytes.clear();
		if (text.size() % 2 != 0)
			return false;
		const auto nibble = [](char c) -> int
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			return -1;
		};
		bytes.reserve(text.size() / 2);
		for (std::size_t i = 0; i < text.size(); i += 2)
		{
			const int high = nibble(text[i]);
			const int low = nibble(text[i + 1]);
			if (high < 0 || low < 0)
			{
				bytes.clear();
				return false;
			}
			bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
		}
		return true;
	}
}
