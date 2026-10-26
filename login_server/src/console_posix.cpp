#if !defined(_WIN32)

#include "console.h"

#include <cstdio>

#include <unistd.h>

namespace hb::server::platform
{
	bool stdout_is_terminal()
	{
		return isatty(fileno(stdout)) != 0;
	}

	bool enable_console_colors()
	{
		return true;
	}
}

#endif
