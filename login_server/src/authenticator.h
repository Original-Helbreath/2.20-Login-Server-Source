#pragma once

#include "account_service.h"
#include "config.h"
#include "connection.h"
#include "worker_pool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Client authentication (LOGIN-006, plan sect. 5.3): login_request, create_account_request and
// change_password_request. Each runs on the connection's strand; the Argon2 and database work is posted to
// the worker pool and its verdict is posted back to the strand, so the network thread never hashes.
// A session handles one account request at a time: a second one while the first is pending, a second
// login on an authenticated session, or any character message before login closes the connection.
namespace hb::server::login
{
	struct authenticator_options
	{
		bool allow_account_creation = false;
		std::chrono::milliseconds idle_after_login{ 600000 };
		// Wrong passwords (login or change_password) a connection may send before it is closed.
		std::uint32_t max_failed_attempts = 3;
		// create_account_request that reached the database per connection.
		std::uint32_t max_account_creations = 3;
	};

	authenticator_options make_authenticator_options(const login_config& config);

	class authenticator
	{
	public:
		// accounts and workers must outlive the authenticator and every connection that calls it.
		authenticator(account_service& accounts, worker_pool& workers, authenticator_options options);

		authenticator(const authenticator&) = delete;
		authenticator& operator=(const authenticator&) = delete;

		// A frame from a client session (direction already checked). Closes the session on a protocol error.
		void handle(hb::net::connection& from, const hb::net::frame& received);

		// The session ended: forget its state and, if it was the account's live session, the account's entry.
		void on_closed(const hb::net::connection& session);

		bool is_authenticated(const hb::net::connection& session) const;
		std::size_t authenticated_count() const;

	private:
		struct session_state
		{
			bool busy = false;
			bool authenticated = false;
			std::int64_t account_id = 0;
			std::uint32_t failures = 0;
			std::uint32_t creations = 0;
		};

		void on_login(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		void on_create_account(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		void on_change_password(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);

		// False (the session was closed) when a request is already pending or the session is unknown.
		bool begin_request(const std::shared_ptr<hb::net::connection>& session);
		void finish_login(hb::net::connection& session, const login_outcome& outcome, const std::string& address);
		void finish_create(hb::net::connection& session, create_result result);
		void finish_change(hb::net::connection& session, password_change_result result);
		void audit_refusal(const std::shared_ptr<hb::net::connection>& session, std::string name, std::string_view outcome);
		// Counts a wrong password; true when the connection has used up its attempts (the caller closes it after the reply).
		bool count_failure(hb::net::connection& session);

		account_service& m_accounts;
		worker_pool& m_workers;
		authenticator_options m_options;
		mutable std::mutex m_mutex;
		std::unordered_map<const hb::net::connection*, session_state> m_sessions;
		std::unordered_map<std::int64_t, std::weak_ptr<hb::net::connection>> m_live;
	};
}
