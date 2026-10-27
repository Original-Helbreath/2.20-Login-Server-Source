#if !defined(_WIN32)

#include "private_file.h"

#include <cerrno>
#include <cstring>
#include <format>

#include <sys/stat.h>

namespace hb::server::platform
{
	bool restrict_to_owner(const std::filesystem::path& path, std::string& error)
	{
		if (chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0)
			return true;
		error = std::format("cannot restrict {}: {}", path.string(), std::strerror(errno));
		return false;
	}
}

#endif
