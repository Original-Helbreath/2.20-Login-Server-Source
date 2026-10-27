#pragma once

#include <schemata/schemata.h>

#include <cstddef>
#include <memory>
#include <string>

// players.db through Schemata. Every access borrows a connection from the pool for one unit of work.
namespace hb::server::login
{
	class database
	{
	public:
		// Opens the pool and creates or heals the schema. False (with the reason) on any database fault.
		bool open(const std::string& path, std::size_t pool_size, std::string& error);

		schemata::ConnectionPool& pool()
		{
			return *m_pool;
		}

		const schemata::Model& model() const
		{
			return m_model;
		}

	private:
		schemata::Model m_model;
		std::unique_ptr<schemata::ConnectionPool> m_pool;
	};
}
