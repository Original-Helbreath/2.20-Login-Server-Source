#pragma once

#include "database.h"
#include "entities.h"
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

	enum class password_change_result
	{
		changed,
		wrong_credentials,  // unknown account and wrong old password look the same
		invalid_password,
		locked,
		failed
	};

	// What a login check found: the verdict and, when accepted, which account (the stored spelling of its name).
	struct login_outcome
	{
		login_result result = login_result::failed;
		std::int64_t account_id = 0;
		std::string name;
	};

	std::string_view to_string(create_result result);
	std::string_view to_string(login_result result);
	std::string_view to_string(password_change_result result);

	// Decides whether a missing pepper file may be created (only while there are no accounts).
	bool count_accounts(database& db, std::int64_t& count, std::string& error);

	class account_service
	{
	public:
		account_service(database& db, const pepper_store& peppers, argon2_cost cost);

		// Blocking (Argon2id runs here): callers run it on a worker thread, never the network thread.
		// address is the peer's address text, recorded in the audit (login_events) with the outcome.
		create_result create(std::string_view name, std::string_view password, std::string_view address = {});
		login_outcome authenticate(std::string_view name, std::string_view password, std::string_view address);
		login_result check_login(std::string_view name, std::string_view password, std::string_view address);
		// Checks the old password like a login (same decoy work for an unknown account), then stores the new one.
		password_change_result change_password(std::string_view name, std::string_view old_password, std::string_view new_password,
			std::string_view address);

		// Writes one audit row for an attempt refused before it reached the service (blocking).
		void audit(std::string_view name, std::string_view address, std::string_view outcome);

		// Startup check: each pepper's fingerprint against the one stored when it was first used; records new ones.
		bool verify_peppers(std::string& error);

	private:
		enum class credential_check
		{
			no_account,
			unusable,
			wrong_password,
			verified
		};

		// Looks the account up and verifies the password; a missing or unusable row still spends one Argon2 run.
		// found is set for every case but no_account.
		credential_check check_credentials(std::string_view name, std::string_view password, std::optional<account>& found,
			password_record& record);
		static void store_hash(account& row, const password_record& record);
		create_result create_row(std::string_view name, std::string_view password, std::int64_t& id);
		bool name_exists(const std::string& name_key);
		static std::string printable(std::string_view text);
		static bool is_valid_name(std::string_view name);
		static std::string lowercase(std::string_view text);
		static schemata::timestamp now();
		void record_event(std::optional<std::int64_t> account_id, std::string_view name, std::string_view address, std::string_view outcome);

		database& m_database;
		const pepper_store& m_peppers;
		password_hasher m_hasher;
	};
}
