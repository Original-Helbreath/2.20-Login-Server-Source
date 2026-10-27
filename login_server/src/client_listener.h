#pragma once

#include "authenticator.h"
#include "config.h"
#include "connection.h"
#include "server.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// The Login side of game-client connections (plan sect. 5.3): an hb::net::server on client_port. Each
// session is framed by hb::net::connection (a malformed frame closes it) and closed after the idle
// timeout. Message ids are checked here; login, account and password messages go to the authenticator.
namespace hb::server::login
{
	struct client_listener_options
	{
		hb::net::server_options listen;
		std::chrono::milliseconds idle_before_login{ 60000 };
	};

	client_listener_options make_client_listener_options(const login_config& config);

	class client_listener
	{
	public:
		// With an authenticator, login, account and password messages are handled by it; without one (the
		// network tests) only the message ids are checked. It must outlive the listener and its sessions.
		client_listener(asio::io_context& context, client_listener_options options, authenticator* auth = nullptr);

		bool start(std::string& error);
		void stop();

		std::uint16_t local_port() const { return m_server->local_port(); }
		std::size_t session_count() const { return m_server->connection_count(); }

	private:
		void on_accept(const std::shared_ptr<hb::net::connection>& session);

		client_listener_options m_options;
		authenticator* m_auth;
		std::shared_ptr<hb::net::server> m_server;
	};
}
