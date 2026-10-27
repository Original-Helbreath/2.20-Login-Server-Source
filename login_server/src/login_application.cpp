#include "login_application.h"

#include "account_service.h"
#include "asio_config.h"
#include "authenticator.h"
#include "client_listener.h"
#include "worker_pool.h"
#include "world_link.h"

#include <asio/signal_set.hpp>

#include <csignal>
#include <string>
#include <utility>
#include "database.h"
#include "log.h"
#include "password_hasher.h"
#include "pepper_store.h"

namespace hb::server::login
{
	bool login_application::run(const std::vector<std::string>& arguments)
	{
		const std::string config_path = arguments.size() > 1 ? arguments[1] : std::string(default_config_path);
		log::info("login server starting, config {}", config_path);

		std::string error;
		if (!load_config(config_path, m_config, error))
		{
			log::error("config rejected: {}", error);
			return false;
		}
		log::set_level(m_config.log_level);
		log_config();

		database db;
		if (!db.open(m_config.database.path, m_config.database.pool_size, error))
		{
			log::error("database failed: {}", error);
			return false;
		}
		std::int64_t account_count = 0;
		if (!count_accounts(db, account_count, error))
		{
			log::error("database failed: {}", error);
			return false;
		}
		pepper_store peppers;
		if (!peppers.load_or_create(m_config.accounts.pepper_file, account_count == 0, error))
		{
			log::error("pepper file rejected: {}", error);
			return false;
		}
		// The config validated the cost name, so the preset exists.
		account_service accounts(db, peppers, *cost_from_name(m_config.accounts.password_hash_cost));
		if (!accounts.verify_peppers(error))
		{
			log::error("pepper file rejected: {}", error);
			return false;
		}

		return serve(accounts);
	}

	bool login_application::serve(account_service& accounts)
	{
		std::string error;
		world_link_options link_options;
		if (!make_world_link_options(m_config, link_options, error))
		{
			log::error("config rejected: {}", error);
			return false;
		}

		// One network io_context on this thread; slow jobs go to the workers (plan sect. 5.1).
		asio::io_context network;
		worker_pool workers(m_config.database.pool_size);
		world_link_service world_links(network, std::move(link_options));
		// Declared after the workers and before the listener: sessions closing at shutdown still call it.
		authenticator auth(accounts, workers, world_links, make_authenticator_options(m_config));
		client_listener clients(network, make_client_listener_options(m_config), &auth);
		if (!world_links.start(error) || !clients.start(error))
		{
			log::error("listen failed: {}", error);
			world_links.stop();
			clients.stop();
			return false;
		}

		asio::signal_set signals(network, SIGINT, SIGTERM);
		signals.async_wait([&](const asio::error_code& wait_error, int signal) {
			if (wait_error)
				return;
			log::info("signal {} received, shutting down", signal);
			clients.stop();
			world_links.stop();
			network.stop();
		});

		log::info("login server running with {} worker threads; Ctrl+C stops it", workers.size());
		network.run();
		// Run the closes the shutdown posted, so every session and link logs its end.
		network.restart();
		network.poll();
		workers.stop();
		log::info("login server stopped");
		return true;
	}

	void login_application::log_config() const
	{
		log::info("listening on {} (client port {}, world port {})", m_config.listen.bind_address,
			m_config.listen.client_port, m_config.listen.world_port);
		for (const world_entry& world : m_config.worlds)
			log::info("world {} allowed from {}", world.name, world.allowed_address);
		log::info("database {} (pool {}), client account creation {}, password cost {}", m_config.database.path,
			m_config.database.pool_size, m_config.accounts.allows_client_account_creation ? "on" : "off",
			m_config.accounts.password_hash_cost);
	}
}
