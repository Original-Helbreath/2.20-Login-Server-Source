#include "login_application.h"

#include "log.h"

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

		// Listeners arrive with the networking milestone; LOGIN-001 stops after the config.
		log::info("login server stopped");
		return true;
	}

	void login_application::log_config() const
	{
		log::info("listening on {} (client port {}, world port {})", m_config.listen.bind_address,
			m_config.listen.client_port, m_config.listen.world_port);
		for (const world_entry& world : m_config.worlds)
			log::info("world {} allowed from {}", world.name, world.allowed_address);
		log::info("database {}, client account creation {}, password cost {}", m_config.database.path,
			m_config.accounts.allows_client_account_creation ? "on" : "off", m_config.accounts.password_hash_cost);
	}
}
