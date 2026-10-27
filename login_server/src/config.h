#pragma once

#include "log.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// login_server.json (Standards sect. 12: JSON with comments). Settings only; no game-data paths.
namespace hb::server::login
{
	constexpr std::size_t link_key_hex_digits = 64;
	constexpr std::uint64_t min_port = 1;
	constexpr std::uint64_t min_connections = 1;
	constexpr std::uint64_t min_seconds = 1;
	constexpr std::uint64_t min_body_bytes = 64;
	constexpr std::uint64_t min_pool_size = 1;
	constexpr std::uint64_t max_pool_size = 64;
	constexpr std::array<std::string_view, 3> password_hash_costs = { "interactive", "moderate", "sensitive" };

	struct listen_config
	{
		std::string bind_address = "0.0.0.0";
		std::uint16_t client_port = 2848;
		std::uint16_t world_port = 3840;
		// Open connections each listener holds at most; one more is closed on accept.
		std::uint32_t max_client_connections = 1000;
		std::uint32_t max_world_connections = 8;
	};

	struct world_entry
	{
		std::string name;
		std::string allowed_address;
		std::string link_key;
	};

	struct protocol_config
	{
		std::uint32_t max_body_bytes = 65536;
		std::uint32_t world_request_timeout_seconds = 5;
		std::uint32_t heartbeat_seconds = 5;
	};

	struct accounts_config
	{
		bool allows_client_account_creation = true;
		std::string password_hash_cost = "moderate";
		// The pepper key file: a secret kept outside players.db and the repo; created on first run.
		std::string pepper_file = "data/pepper.json";
	};

	struct session_config
	{
		std::uint32_t ticket_lifetime_seconds = 20;
		std::uint32_t idle_timeout_before_login_seconds = 60;
		std::uint32_t idle_timeout_after_login_seconds = 600;
	};

	struct database_config
	{
		// players.db is shared with the World server: Login owns accounts and login_events, World the
		// character tables. gamedata.db and world.db are the World server's alone.
		std::string path = "data/players.db";
		// Schemata pool size; the worker thread count matches it (plan sect. 5.1).
		std::uint32_t pool_size = 4;
	};

	struct login_config
	{
		listen_config listen;
		std::vector<world_entry> worlds;
		protocol_config protocol;
		accounts_config accounts;
		session_config session;
		database_config database;
		hb::log::level log_level = hb::log::level::info;
	};

	// Reads and validates the file. On failure returns false and says why in error.
	bool load_config(const std::filesystem::path& path, login_config& config, std::string& error);
}
