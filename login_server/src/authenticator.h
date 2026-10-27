#pragma once

#include "account_service.h"
#include "config.h"
#include "connection.h"
#include "worker_pool.h"
#include "world_link.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Client authentication (LOGIN-006, plan sect. 5.3): login_request, create_account_request and
// change_password_request. Each runs on the connection's strand; the Argon2 and database work is posted to
// the worker pool and its verdict is posted back to the strand, so the network thread never hashes.
// A session handles one account request at a time: a second one while the first is pending, a second
// login on an authenticated session, or any character message before login closes the connection.
//
// Character brokering (LOGIN-007): login_request.world must name a configured World (else no_such_world)
// whose link is live (else world_unavailable). After the password check the character list is fetched
// from that World, and login_response ok is sent only together with it; the account counts as logged in
// only then. create_character_request and delete_character_request are format-checked here (plan M13),
// relayed to the World with a request id and answered with the World's result, followed on ok by a fresh
// character_list. A World that is down, lost or silent for world_request_timeout_seconds gives
// world_unavailable. The Login server reads no character table.
//
// Enter game (LOGIN-008): enter_game_request reserves the account (offline -> handoff_pending, see
// online_accounts.h), draws a ticket from the CSPRNG, sends session_grant to the session's World and, when the
// World answers ok, replies enter_game_response with the World's client address and port and the ticket, then
// closes the session (handed_off). A normal request for an account that is pending or in a World answers
// already_playing; force_login sends kick_account to the holding World and answers force_disconnect_sent.
// The ticket is never logged and is wiped from this server's own copies after it is sent.
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
		// Bound for the character_list frame the client gets (config protocol.max_body_bytes).
		std::uint32_t max_body_bytes = 65536;
	};

	authenticator_options make_authenticator_options(const login_config& config);

	class authenticator
	{
	public:
		// accounts, workers and worlds must outlive the authenticator and every connection that calls it.
		authenticator(account_service& accounts, worker_pool& workers, world_link_service& worlds, authenticator_options options);

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
			std::string world;  // from login_request; the World this session's character requests go to
			// Between begin_enter and the World's answer: the reservation and the ticket for the reply.
			std::uint64_t enter_generation = 0;
			hb::net::session_ticket ticket{};
		};

		enum class change_kind : std::uint8_t
		{
			create,
			remove,
		};

		void on_login(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		void on_create_account(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		void on_change_password(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		void on_create_character(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		void on_delete_character(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		void on_enter_game(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received);
		// The World answered the grant (or did not): reply to the client, with the ticket and the World's address on ok.
		void finish_enter(const std::shared_ptr<hb::net::connection>& session, std::uint64_t account_id, std::uint64_t generation,
			world_request_status status, hb::net::link::grant_result grant);
		void reply_enter(hb::net::connection& session, hb::net::login::enter_result result);
		// Moves the session's pending ticket out (wiping the stored one); false when the session is unknown.
		bool take_enter_ticket(const hb::net::connection& session, hb::net::session_ticket& ticket);

		// The session's account id and World; false when the session is unknown.
		bool session_target(const hb::net::connection& session, std::uint64_t& account_id, std::string& world) const;
		void end_request(const hb::net::connection& session);
		void reply_login(hb::net::connection& session, hb::net::login::login_result result);
		// The password check passed: fetch the character list, then complete or refuse the login.
		void begin_login_list(hb::net::connection& session, const login_outcome& outcome, const std::string& address);
		void finish_login_list(const std::shared_ptr<hb::net::connection>& session, const login_outcome& outcome,
			const std::string& address, world_request_status status, character_list_reply list);
		// The World answered a create or delete; on ok the fresh list is fetched before the client is told.
		void finish_character_change(const std::shared_ptr<hb::net::connection>& session, change_kind kind, world_request_status status,
			hb::net::login::character_result result);
		void finish_change_list(const std::shared_ptr<hb::net::connection>& session, change_kind kind, world_request_status status,
			character_list_reply list);
		void send_change_reply(hb::net::connection& session, change_kind kind, hb::net::login::character_result result);
		// Encodes the character_list frame; false when it cannot be encoded.
		bool encode_list(const character_list_reply& list, std::vector<std::byte>& bytes) const;

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
		world_link_service& m_worlds;
		authenticator_options m_options;
		mutable std::mutex m_mutex;
		std::unordered_map<const hb::net::connection*, session_state> m_sessions;
		std::unordered_map<std::int64_t, std::weak_ptr<hb::net::connection>> m_live;
	};
}
