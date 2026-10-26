#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "console.h"

#include <cstdio>

#include <io.h>
#include <windows.h>

namespace hb::server::platform
{
	bool stdout_is_terminal()
	{
		return _isatty(_fileno(stdout)) != 0;
	}

	bool enable_console_colors()
	{
		HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode = 0;
		if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode))
			return false;
		return SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
	}
}

#endif
