#if !defined(_WIN32)

#include "private_file.h"

#include <cerrno>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hb::server::platform
{
	bool create_private_file(const std::filesystem::path& path, std::string& error)
	{
		const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (file < 0)
		{
			error = std::format("cannot create {}: {}", path.string(), std::strerror(errno));
			return false;
		}
		// The umask can only remove bits, so 0600 holds; set it anyway in case the mode was widened.
		const bool restricted = fchmod(file, S_IRUSR | S_IWUSR) == 0;
		close(file);
		if (!restricted)
		{
			error = std::format("cannot restrict {}: {}", path.string(), std::strerror(errno));
			return false;
		}
		return true;
	}
}

#endif
