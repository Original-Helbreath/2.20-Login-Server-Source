#pragma once

#include "database.h"
#include "password_hasher.h"
#include "pepper_store.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Account rules for the Login server: create an account, check a login. No roles: who is a GM is the
// World server's business, and making an administrator is a direct SQL edit.
namespace hb::server::login
{
	constexpr std::size_t min_password_length = 4;
	constexpr std::size_t max_password_length = 32;

	enum class create_result
	{
		created,
		invalid_name,
		invalid_password,
		name_taken,
		failed
	};

	enum class login_result
	{
		accepted,
		no_account,
		wrong_password,
		locked,
		failed
	};

	std::string_view to_string(create_result result);
	std::string_view to_string(login_result result);

	// Decides whether a missing pepper file may be created (only while there are no accounts).
	bool count_accounts(database& db, std::int64_t& count, std::string& error);

	class account_service
	{
	public:
		account_service(database& db, const pepper_store& peppers, argon2_cost cost);

		// Blocking (Argon2id runs here): callers run it on a worker thread, never the network thread.
		create_result create(std::string_view name, std::string_view password);
		login_result check_login(std::string_view name, std::string_view password, std::string_view address);

		// Startup check: each pepper's fingerprint against the one stored when it was first used; records new ones.
		bool verify_peppers(std::string& error);

	private:
		bool name_exists(const std::string& name_key);
		static std::string printable(std::string_view text);
		static bool is_valid_name(std::string_view name);
		static std::string lowercase(std::string_view text);
		static schemata::timestamp now();
		void record_event(std::optional<std::int64_t> account_id, std::string_view name, std::string_view address, login_result result);

		database& m_database;
		const pepper_store& m_peppers;
		password_hasher m_hasher;
	};
}
