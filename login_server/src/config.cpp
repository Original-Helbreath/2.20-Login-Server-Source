#include "config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>

namespace hb::server::login
{
	// Private to this file: it drags <nlohmann/json.hpp> into every includer (sect. 4 exception).
	class config_reader
	{
	public:
		explicit config_reader(login_config& config)
			: m_config(config)
		{
		}

		bool read(const nlohmann::json& root)
		{
			allow_keys(root, "the config", { "listen", "worlds", "protocol", "accounts", "session", "database", "log" });
			read_listen(root);
			read_worlds(root);
			read_protocol(root);
			read_accounts(root);
			read_session(root);
			read_database_and_log(root);
			check_rules();
			return m_error.empty();
		}

		const std::string& error() const
		{
			return m_error;
		}

	private:
		void fail(std::string message)
		{
			if (m_error.empty())
				m_error = std::move(message);
		}

		// A misspelt key would otherwise be silently ignored and its default used.
		void allow_keys(const nlohmann::json& object, std::string_view where, std::initializer_list<std::string_view> keys)
		{
			for (const auto& [key, value] : object.items())
			{
				if (std::find(keys.begin(), keys.end(), key) == keys.end())
					fail(std::format("unknown key '{}' in {}", key, where));
			}
		}

		const nlohmann::json* section(const nlohmann::json& parent, std::string_view key, std::initializer_list<std::string_view> keys)
		{
			const auto found = parent.find(key);
			if (found == parent.end())
				return nullptr;
			if (!found->is_object())
			{
				fail(std::format("'{}' must be an object", key));
				return nullptr;
			}
			allow_keys(*found, std::format("'{}'", key), keys);
			return &*found;
		}

		void read_string(const nlohmann::json& parent, std::string_view key, std::string& value)
		{
			const auto found = parent.find(key);
			if (found == parent.end())
				return;
			if (!found->is_string() || found->get_ref<const std::string&>().empty())
			{
				fail(std::format("'{}' must be a non-empty string", key));
				return;
			}
			value = found->get<std::string>();
		}

		void read_bool(const nlohmann::json& parent, std::string_view key, bool& value)
		{
			const auto found = parent.find(key);
			if (found == parent.end())
				return;
			if (!found->is_boolean())
			{
				fail(std::format("'{}' must be true or false", key));
				return;
			}
			value = found->get<bool>();
		}

		template <typename T>
		void read_number(const nlohmann::json& parent, std::string_view key, T& value, std::uint64_t minimum)
		{
			const auto found = parent.find(key);
			if (found == parent.end())
				return;
			const std::uint64_t maximum = std::numeric_limits<T>::max();
			if (!found->is_number_unsigned() || found->get<std::uint64_t>() < minimum || found->get<std::uint64_t>() > maximum)
			{
				fail(std::format("'{}' must be a whole number from {} to {}", key, minimum, maximum));
				return;
			}
			value = static_cast<T>(found->get<std::uint64_t>());
		}

		void read_listen(const nlohmann::json& root)
		{
			const nlohmann::json* listen = section(root, "listen",
				{ "bind_address", "client_port", "world_port", "max_client_connections", "max_world_connections" });
			if (!listen)
				return;
			read_string(*listen, "bind_address", m_config.listen.bind_address);
			read_number(*listen, "client_port", m_config.listen.client_port, min_port);
			read_number(*listen, "world_port", m_config.listen.world_port, min_port);
			read_number(*listen, "max_client_connections", m_config.listen.max_client_connections, min_connections);
			read_number(*listen, "max_world_connections", m_config.listen.max_world_connections, min_connections);
		}

		void read_worlds(const nlohmann::json& root)
		{
			const auto found = root.find("worlds");
			if (found == root.end())
				return;
			if (!found->is_array())
			{
				fail("'worlds' must be an array");
				return;
			}
			std::set<std::string> names;
			for (const nlohmann::json& item : *found)
			{
				if (!item.is_object())
				{
					fail("each 'worlds' entry must be an object");
					return;
				}
				allow_keys(item, "a 'worlds' entry", { "name", "allowed_address", "link_key" });
				world_entry world;
				read_string(item, "name", world.name);
				read_string(item, "allowed_address", world.allowed_address);
				read_string(item, "link_key", world.link_key);
				if (!m_error.empty())
					return;
				if (world.name.empty() || world.allowed_address.empty())
					fail("each world needs 'name' and 'allowed_address'");
				else if (!is_hex_key(world.link_key))
					fail(std::format("world '{}': 'link_key' must be {} hex digits", world.name, link_key_hex_digits));
				else if (!names.insert(world.name).second)
					fail(std::format("world '{}' is listed twice", world.name));
				if (!m_error.empty())
					return;
				if (world.link_key.find_first_not_of('0') == std::string::npos)
					log::warning("world '{}' uses the all-zero sample link_key; set a real key before deploying", world.name);
				m_config.worlds.push_back(std::move(world));
			}
		}

		void read_protocol(const nlohmann::json& root)
		{
			const nlohmann::json* protocol = section(root, "protocol",
				{ "max_body_bytes", "world_request_timeout_seconds", "heartbeat_seconds" });
			if (!protocol)
				return;
			read_number(*protocol, "max_body_bytes", m_config.protocol.max_body_bytes, min_body_bytes);
			read_number(*protocol, "world_request_timeout_seconds", m_config.protocol.world_request_timeout_seconds, min_seconds);
			read_number(*protocol, "heartbeat_seconds", m_config.protocol.heartbeat_seconds, min_seconds);
		}

		void read_accounts(const nlohmann::json& root)
		{
			const nlohmann::json* accounts = section(root, "accounts",
				{ "allows_client_account_creation", "password_hash_cost", "pepper_file" });
			if (!accounts)
				return;
			read_bool(*accounts, "allows_client_account_creation", m_config.accounts.allows_client_account_creation);
			read_string(*accounts, "password_hash_cost", m_config.accounts.password_hash_cost);
			read_string(*accounts, "pepper_file", m_config.accounts.pepper_file);
		}

		void read_session(const nlohmann::json& root)
		{
			const nlohmann::json* session = section(root, "session",
				{ "ticket_lifetime_seconds", "idle_timeout_before_login_seconds", "idle_timeout_after_login_seconds" });
			if (!session)
				return;
			read_number(*session, "ticket_lifetime_seconds", m_config.session.ticket_lifetime_seconds, min_seconds);
			read_number(*session, "idle_timeout_before_login_seconds", m_config.session.idle_timeout_before_login_seconds, min_seconds);
			read_number(*session, "idle_timeout_after_login_seconds", m_config.session.idle_timeout_after_login_seconds, min_seconds);
		}

		void read_database_and_log(const nlohmann::json& root)
		{
			if (const nlohmann::json* database = section(root, "database", { "path", "pool_size" }))
			{
				read_string(*database, "path", m_config.database.path);
				read_number(*database, "pool_size", m_config.database.pool_size, min_pool_size);
				if (m_config.database.pool_size > max_pool_size)
					fail(std::format("'pool_size' must be at most {}", max_pool_size));
			}
			const nlohmann::json* log_section = section(root, "log", { "level" });
			if (!log_section)
				return;
			std::string level_name = "info";
			read_string(*log_section, "level", level_name);
			if (m_error.empty() && !hb::log::parse_level(level_name, m_config.log_level))
				fail("'level' must be debug, info, warning or error");
		}

		void check_rules()
		{
			if (!m_error.empty())
				return;
			const std::string& cost = m_config.accounts.password_hash_cost;
			if (std::find(password_hash_costs.begin(), password_hash_costs.end(), cost) == password_hash_costs.end())
				fail("'password_hash_cost' must be interactive, moderate or sensitive");
			else if (m_config.listen.client_port == m_config.listen.world_port)
				fail("'client_port' and 'world_port' must differ");
			else if (m_config.worlds.empty())
				fail("'worlds' must list at least one world");
		}

		static bool is_hex_key(std::string_view text)
		{
			return text.size() == link_key_hex_digits
				&& text.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
		}

		login_config& m_config;
		std::string m_error;
	};

	bool load_config(const std::filesystem::path& path, login_config& config, std::string& error)
	{
		error.clear();
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			error = std::format("cannot open {}", path.string());
			return false;
		}
		std::stringstream text;
		text << file.rdbuf();
		const nlohmann::json root = nlohmann::json::parse(text.str(), nullptr, false, true);
		if (root.is_discarded() || !root.is_object())
		{
			error = std::format("{} is not valid JSON (comments are allowed)", path.string());
			return false;
		}
		login_config loaded;
		config_reader reader(loaded);
		if (!reader.read(root))
		{
			error = std::format("{}: {}", path.string(), reader.error());
			return false;
		}
		config = std::move(loaded);
		return true;
	}
}
