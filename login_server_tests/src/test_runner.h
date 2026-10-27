#pragma once

#include "log.h"

#include <filesystem>
#include <string_view>

namespace tests
{
	class runner
	{
	public:
		void check(bool condition, std::string_view name)
		{
			++m_total;
			if (condition)
				return;
			++m_failed;
			hb::log::error("FAIL: {}", name);
		}

		bool passed() const
		{
			return m_failed == 0;
		}

		int total() const
		{
			return m_total;
		}

		int failed() const
		{
			return m_failed;
		}

	private:
		int m_total = 0;
		int m_failed = 0;
	};

	// protocol_tests.cpp
	void protocol_tests(runner& t);

	// message_tests.cpp
	void message_tests(runner& t);

	// network_tests.cpp
	void network_tests(runner& t);

	// auth_tests.cpp: each case uses its own subfolder of the given folder
	void auth_tests(runner& t, const std::filesystem::path& folder);

	// character_tests.cpp: character brokering against a scripted World; subfolders of the given folder
	void character_tests(runner& t, const std::filesystem::path& folder);
}
