#pragma once

#include "common_types.h"
#include "config.h"
#include "connection.h"
#include "link_messages.h"
#include "login_messages.h"
#include "online_accounts.h"
#include "server.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The Login side of the World link (plan sect. 4.5, 5.3): an hb::net::server on world_port that admits a
// World server only from its allow-listed address and with its link key, then keeps the link alive with
// heartbeats both ways. Character, grant and session messages arrive with LOGIN-006/007.
namespace hb::server::login
{
	enum class link_state : std::uint8_t
	{
		never_registered,
		live,
		lost,
	};

	std::string_view to_string(link_state state);

	struct allowed_world
	{
		std::string name;
		std::string allowed_address;
		hb::net::link_key key{};
	};

	struct world_link_options
	{
		hb::net::server_options listen;
		std::vector<allowed_world> worlds;
		std::chrono::milliseconds heartbeat_interval{ 5000 };
		// No frame for this long marks the link lost (and bounds the wait for world_register).
		std::chrono::milliseconds loss_timeout{ 15000 };
		// How long a client request waits for the World's answer before it counts as unavailable.
		std::chrono::milliseconds request_timeout{ 5000 };
		// How long an enter-game ticket stays valid (config session.ticket_lifetime_seconds).
		std::chrono::milliseconds ticket_lifetime{ 20000 };
	};

	// How a request to a World server ended: it answered, or it could not (no live link, the link was
	// lost, the answer did not arrive within request_timeout, or the request could not be sent).
	enum class world_request_status : std::uint8_t
	{
		answered,
		unavailable,
	};

	struct character_list_reply
	{
		std::uint8_t max_slots = 0;
		std::vector<hb::net::character_summary> characters;
	};

	// Called exactly once, on the executor given with the request.
	using character_list_done = std::function<void(world_request_status, character_list_reply)>;
	using character_change_done = std::function<void(world_request_status, hb::net::login::character_result)>;
	// The grant_result is meaningful only when the status is answered.
	using session_grant_done = std::function<void(world_request_status, hb::net::link::grant_result)>;

	// Heartbeats every heartbeat_seconds; the link is lost after three missed intervals.
	constexpr std::uint32_t heartbeats_before_loss = 3;

	// From the validated config (link keys are already 64 hex digits). False only on an unparsable key.
	bool make_world_link_options(const login_config& config, world_link_options& options, std::string& error);

	struct world_link_status
	{
		link_state state = link_state::never_registered;
		std::uint8_t is_accepting_players = 0;
		std::uint8_t is_shutting_down = 0;
		std::uint16_t player_count = 0;
		std::uint16_t max_players = 0;
		// Where clients connect to this World, from its world_register.
		std::string client_host;
		std::uint16_t client_port = 0;
	};

	class world_link_service
	{
	public:
		world_link_service(asio::io_context& context, world_link_options options);

		bool start(std::string& error);
		void stop();

		std::uint16_t local_port() const { return m_server->local_port(); }

		// never_registered for a name not in the allow-list.
		world_link_status status(std::string_view world) const;

		// True for a world name in the allow-list (the worlds a client may ask for), live or not.
		bool knows_world(std::string_view world) const;

		// True while the world's link is registered and open.
		bool is_live(std::string_view world) const;

		// Relay a client request to the named World and report the outcome on reply_on (the client
		// session's strand); done is called exactly once, also when the World is down, the link is lost
		// or request_timeout passes. The service chooses the request_id; the one in request is ignored.
		// A late answer to a request that already timed out is dropped.
		void request_character_list(std::string_view world, std::uint64_t account_id, const asio::any_io_executor& reply_on,
			character_list_done done);
		void request_create_character(std::string_view world, hb::net::link::create_character_request request,
			const asio::any_io_executor& reply_on, character_change_done done);
		void request_delete_character(std::string_view world, hb::net::link::delete_character_request request,
			const asio::any_io_executor& reply_on, character_change_done done);

		// Ask the named World to hold a one-time session for the account (session_grant). The service chooses
		// the request_id and sets expires_in_seconds from the registry's ticket lifetime; done gets the World's
		// grant_result, or unavailable. The caller reserved the account with online().begin_enter first.
		void request_session_grant(std::string_view world, hb::net::link::session_grant grant, const asio::any_io_executor& reply_on,
			session_grant_done done);

		// Order the named World to drop the account's session or pending grant. False when its link is not live.
		bool send_kick(std::string_view world, std::uint64_t account_id, hb::net::link::kick_reason reason);

		// Which accounts are offline, handing off or in a World; updated by the World's session messages.
		online_registry& online() { return m_online; }
		const online_registry& online() const { return m_online; }

		// Requests waiting for a World's answer.
		std::size_t pending_count() const;

	private:
		struct peer;

		// What a waiting request is to call; exactly one is set.
		struct request_callbacks
		{
			character_list_done list;
			character_change_done change;
			session_grant_done grant;
		};

		// How a request ended; the member matching the callback is the one that is read.
		struct request_outcome
		{
			world_request_status status = world_request_status::unavailable;
			character_list_reply list;
			hb::net::login::character_result change = hb::net::login::character_result::world_unavailable;
			hb::net::link::grant_result grant = hb::net::link::grant_result::shutting_down;
		};

		// One client request waiting for its answer.
		struct pending_request
		{
			std::string world;
			const hb::net::connection* link = nullptr;
			hb::net::message_id answer = hb::net::message_id::link_character_list_response;
			asio::any_io_executor reply_on;
			std::shared_ptr<asio::steady_timer> timer;
			request_callbacks done;
		};

		template <class Request>
		void submit(std::string_view world, Request request, hb::net::message_id answer, const asio::any_io_executor& reply_on,
			request_callbacks done);
		void finish(std::uint32_t request_id, request_outcome outcome);
		void on_grant_response(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void on_session_started(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void on_session_ended(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void finish_all(const hb::net::connection& link);
		void on_list_response(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void on_change_response(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);

		// What an answer from a World says about the request it names: no such request (it timed out or was
		// already answered, harmless), a request that was sent to another link or expects another answer
		// (a protocol error), or the request it answers.
		enum class answer_check : std::uint8_t
		{
			unknown,
			mismatched,
			matched,
		};
		answer_check check_answer(const hb::net::connection& link, std::uint32_t request_id, hb::net::message_id answer) const;

		void on_accept(const std::shared_ptr<hb::net::connection>& link);
		void on_frame(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void on_register(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void on_close(const std::shared_ptr<peer>& state, hb::net::connection& link, hb::net::close_reason reason,
			std::string_view detail);
		const allowed_world* find_world(std::string_view name) const;

		world_link_options m_options;
		online_registry m_online;
		std::shared_ptr<hb::net::server> m_server;
		mutable std::mutex m_mutex;  // guards m_links: each link runs on its own strand
		std::map<std::string, world_link_status, std::less<>> m_links;
		std::map<std::string, std::weak_ptr<hb::net::connection>, std::less<>> m_live;
		std::map<std::uint32_t, pending_request> m_pending;  // under m_mutex
		std::uint32_t m_next_request_id = 0;                 // under m_mutex
	};
}
