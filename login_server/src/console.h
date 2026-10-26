#pragma once

// The only platform-specific code (Standards sect. 8): one implementation file per platform.
namespace hb::server::platform
{
	bool stdout_is_terminal();

	// Turns on ANSI colour sequences; false when the console cannot show them.
	bool enable_console_colors();
}
