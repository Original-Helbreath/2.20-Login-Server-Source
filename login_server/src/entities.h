#pragma once

#include <schemata/schemata.h>

#include <cstdint>
#include <optional>
#include <string>

// The Login server's tables in players.db (plan sect. 3). The World server owns the character tables in the
// same file; each server's model declares only its own tables, and Schemata leaves the other's alone.
// Making an account an administrator is a direct SQL edit, never a server command.
namespace hb::server::login
{
	constexpr std::size_t max_account_name_length = 10;

	struct account
	{
		std::int64_t id = 0;
		std::string name;
		// Lowercase name: the unique key, so "Bob" and "bob" are one account.
		std::string name_key;
		std::string password_hash;
		std::string password_salt;
		std::int32_t hash_iterations = 0;
		std::int32_t hash_memory_kib = 0;
		std::int32_t hash_lanes = 0;
		std::int32_t pepper_id = 0;
		// Set from the admin dashboard (future); checked at every login.
		bool locked = false;
		schemata::timestamp created_at{};
		std::optional<schemata::timestamp> last_login_at;
	};

	struct login_event
	{
		std::int64_t id = 0;
		std::optional<std::int64_t> account_id;
		std::string name_attempted;
		std::string address;
		std::string outcome;
		schemata::timestamp at{};
	};

	// One row per pepper id the accounts were hashed with: its fingerprint, so a pepper file lost and recreated,
	// or a key swapped under the same id, stops the server instead of failing every login as a wrong password.
	struct pepper_check
	{
		std::int32_t pepper_id = 0;
		std::string fingerprint;
	};
}

template <>
struct schemata::Entity<hb::server::login::pepper_check>
{
	static void describe(schemata::EntityBuilder<hb::server::login::pepper_check>& e)
	{
		using hb::server::login::pepper_check;
		e.table("pepper_checks");
		SCH_KEY(e, pepper_id);
		SCH_PROP(e, fingerprint).max_length(32);
	}
};

template <>
struct schemata::Entity<hb::server::login::account>
{
	static void describe(schemata::EntityBuilder<hb::server::login::account>& e)
	{
		using hb::server::login::account;
		e.table("accounts");
		SCH_KEY(e, id).identity();
		SCH_PROP(e, name).max_length(hb::server::login::max_account_name_length);
		SCH_PROP(e, name_key).max_length(hb::server::login::max_account_name_length).unique();
		SCH_PROP(e, password_hash).max_length(64);
		SCH_PROP(e, password_salt).max_length(32);
		SCH_PROP(e, hash_iterations);
		SCH_PROP(e, hash_memory_kib);
		SCH_PROP(e, hash_lanes);
		SCH_PROP(e, pepper_id);
		SCH_PROP(e, locked).default_sql("0");
		SCH_PROP(e, created_at);
		SCH_PROP(e, last_login_at);
	}
};

template <>
struct schemata::Entity<hb::server::login::login_event>
{
	static void describe(schemata::EntityBuilder<hb::server::login::login_event>& e)
	{
		using hb::server::login::login_event;
		e.table("login_events");
		SCH_KEY(e, id).identity();
		SCH_PROP(e, account_id);
		SCH_PROP(e, name_attempted).max_length(64);
		SCH_PROP(e, address).max_length(64);
		SCH_PROP(e, outcome).max_length(16);
		SCH_PROP(e, at);
	}
};
