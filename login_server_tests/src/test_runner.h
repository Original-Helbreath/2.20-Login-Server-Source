#pragma once

#include "log.h"

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
}
