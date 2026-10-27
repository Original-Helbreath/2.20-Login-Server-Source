#pragma once

#include <filesystem>
#include <string>

// Platform-specific (Standards sect. 8): one implementation file per platform.
namespace hb::server::platform
{
	// Limits an existing file to the current user: an owner-only DACL on Windows, mode 0600 elsewhere.
	bool restrict_to_owner(const std::filesystem::path& path, std::string& error);
}
