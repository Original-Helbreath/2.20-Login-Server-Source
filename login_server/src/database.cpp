#include "database.h"

#include "entities.h"
#include "log.h"

#include <schemata/sqlite.h>

#include <exception>
#include <filesystem>
#include <format>

namespace hb::server::login
{
	bool database::open(const std::string& path, std::size_t pool_size, std::string& error)
	{
		error.clear();
		try
		{
			const std::filesystem::path file(path);
			if (file.has_parent_path())
				std::filesystem::create_directories(file.parent_path());
			m_model.add<account>();
			m_model.add<login_event>();
			m_model.add<pepper_check>();
			schemata::PoolOptions options;
			options.max_size = pool_size;
			m_pool = std::make_unique<schemata::ConnectionPool>([path] { return schemata::sqlite(path); }, options);

			schemata::ConnectionPool::Lease lease = m_pool->acquire();
			const schemata::EnsureResult result = schemata::ensure_schema(*lease, m_model);
			if (result.applied > 0)
				log::info("schema updated: {} statement(s) applied to {}", result.applied, path);
			else
				log::info("schema verified ({})", path);
			return true;
		}
		catch (const std::exception& failure)
		{
			// A database-layer fault is reported, never worked around here (og-helbreath rule: Schemata owns it).
			error = std::format("{}: {}", path, failure.what());
			return false;
		}
	}
}
