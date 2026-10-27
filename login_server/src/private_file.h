#pragma once

#include <filesystem>
#include <string>

// Platform-specific (Standards sect. 8): one implementation file per platform.
namespace hb::server::platform
{
	// Creates a new, empty file that only the current user can open: an owner-only DACL on Windows, mode 0600
	// elsewhere, set at creation so no other handle can open it first. Fails if the file already exists.
	bool create_private_file(const std::filesystem::path& path, std::string& error);
}
