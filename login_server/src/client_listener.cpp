#include "client_listener.h"

#include "log.h"
#include "message_id.h"

#include <format>
#include <utility>

namespace hb::server::login
{
	client_listener_options make_client_listener_options(const login_config& config)
	{
		client_listener_options options;
		options.listen.bind_address = config.listen.bind_address;
		options.listen.port = config.listen.client_port;
		options.listen.connection.max_body_bytes = config.protocol.max_body_bytes;
		options.listen.max_connections = config.listen.max_client_connections;
		options.idle_before_login = std::chrono::seconds(config.session.idle_timeout_before_login_seconds);
		return options;
	}

	client_listener::client_listener(asio::io_context& context, client_listener_options options)
		: m_options(std::move(options))
		, m_server(hb::net::server::create(context))
	{
	}

	bool client_listener::start(std::string& error)
	{
		const bool listening = m_server->listen(m_options.listen,
			[this](const std::shared_ptr<hb::net::connection>& session) { on_accept(session); }, error);
		if (listening)
			log::info("clients listening on {}:{}", m_options.listen.bind_address, m_server->local_port());
		return listening;
	}

	void client_listener::stop()
	{
		m_server->stop();
	}

	void client_listener::on_accept(const std::shared_ptr<hb::net::connection>& session)
	{
		log::debug("client {}:{} connected", session->remote_address(), session->remote_port());
		// Fixed from accept: trickled bytes or frames do not extend the time to log in (LOGIN-006 clears it).
		session->set_deadline(m_options.idle_before_login);
		session->set_idle_timeout(m_options.idle_before_login);
		session->start(
			[](hb::net::connection& from, const hb::net::frame& received) {
				const std::uint16_t id = received.header.message_id;
				if (hb::net::direction_of(id) != hb::net::message_direction::client_to_login)
				{
					from.close(hb::net::close_reason::protocol_error, std::format("unexpected message 0x{:04x}", id));
					return;
				}
				log::debug("client {}: message 0x{:04x} not handled yet (LOGIN-006)", from.remote_address(), id);
			},
			[](hb::net::connection& from, hb::net::close_reason reason, std::string_view detail) {
				const bool expected = reason == hb::net::close_reason::peer_closed || reason == hb::net::close_reason::shutdown;
				log::write(expected ? log::level::debug : log::level::info, "client {}:{} closed: {}{}{}", from.remote_address(),
					from.remote_port(), hb::net::to_string(reason), detail.empty() ? "" : ", ", detail);
			});
	}
}
