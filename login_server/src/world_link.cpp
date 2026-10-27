#include "world_link.h"

#include "hex.h"
#include "link_messages.h"
#include "log.h"
#include "message_codec.h"
#include "message_id.h"
#include "periodic_timer.h"

#include <algorithm>
#include <utility>

namespace hb::server::login
{
	namespace
	{
		namespace link = hb::net::link;
		using hb::net::close_reason;
		using hb::net::message_id;

		// Constant time over the whole key, so a wrong key reveals nothing through timing.
		bool same_key(const hb::net::link_key& a, const hb::net::link_key& b)
		{
			std::uint8_t difference = 0;
			for (std::size_t i = 0; i < a.size(); ++i)
				difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
			return difference == 0;
		}

		bool same_address(const asio::ip::address& remote, std::string_view allowed)
		{
			asio::error_code error;
			const asio::ip::address parsed = asio::ip::make_address(allowed, error);
			return !error && parsed == remote;
		}
	}

	std::string_view to_string(link_state state)
	{
		switch (state)
		{
		case link_state::never_registered:
			return "never registered";
		case link_state::live:
			return "live";
		case link_state::lost:
			return "lost";
		}
		return "unknown";
	}

	bool make_world_link_options(const login_config& config, world_link_options& options, std::string& error)
	{
		options.listen.bind_address = config.listen.bind_address;
		options.listen.port = config.listen.world_port;
		options.listen.connection.max_body_bytes = config.protocol.max_body_bytes;
		options.listen.max_connections = config.listen.max_world_connections;
		options.heartbeat_interval = std::chrono::seconds(config.protocol.heartbeat_seconds);
		options.loss_timeout = options.heartbeat_interval * heartbeats_before_loss;
		options.worlds.clear();
		for (const world_entry& entry : config.worlds)
		{
			std::vector<std::uint8_t> key;
			if (!from_hex(entry.link_key, key) || key.size() != hb::net::link_key{}.size())
			{
				error = std::format("world '{}': link_key is not 64 hex digits", entry.name);
				return false;
			}
			allowed_world world{ entry.name, entry.allowed_address, {} };
			std::ranges::copy(key, world.key.begin());
			options.worlds.push_back(std::move(world));
		}
		return true;
	}

	// One accepted link; touched only on that connection's strand.
	struct world_link_service::peer
	{
		std::weak_ptr<hb::net::connection> link;
		std::string world;  // set once registered
		bool registered = false;
		std::shared_ptr<hb::net::periodic_timer> heartbeat;
		std::uint32_t sequence = 0;
	};

	world_link_service::world_link_service(asio::io_context& context, world_link_options options)
		: m_options(std::move(options))
		, m_server(hb::net::server::create(context))
	{
		for (const allowed_world& world : m_options.worlds)
			m_links.emplace(world.name, world_link_status{});
	}

	bool world_link_service::start(std::string& error)
	{
		const bool listening = m_server->listen(m_options.listen,
			[this](const std::shared_ptr<hb::net::connection>& link) { on_accept(link); }, error);
		if (listening)
			log::info("world link listening on {}:{}", m_options.listen.bind_address, m_server->local_port());
		return listening;
	}

	void world_link_service::stop()
	{
		m_server->stop();
	}

	world_link_status world_link_service::status(std::string_view world) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_links.find(world);
		return found == m_links.end() ? world_link_status{} : found->second;
	}

	const allowed_world* world_link_service::find_world(std::string_view name) const
	{
		const auto found = std::ranges::find(m_options.worlds, name, &allowed_world::name);
		return found == m_options.worlds.end() ? nullptr : &*found;
	}

	void world_link_service::on_accept(const std::shared_ptr<hb::net::connection>& link)
	{
		const bool allowed = std::ranges::any_of(m_options.worlds,
			[&](const allowed_world& world) { return same_address(link->remote_ip(), world.allowed_address); });
		if (!allowed)
		{
			// Not a World host at all: no reply, nothing learned.
			log::warning("world link from {}:{} refused: address not in the allow-list", link->remote_address(),
				link->remote_port());
			link->close(close_reason::protocol_error, "address not allowed");
			return;
		}
		auto state = std::make_shared<peer>();
		state->link = link;
		// world_register must arrive within one loss timeout of accept, however the bytes are paced.
		link->set_deadline(m_options.loss_timeout);
		link->set_idle_timeout(m_options.loss_timeout);
		link->start(
			[this, state](hb::net::connection& from, const hb::net::frame& received) { on_frame(state, from, received); },
			[this, state](hb::net::connection& from, close_reason reason, std::string_view detail) {
				on_close(state, from, reason, detail);
			});
	}

	void world_link_service::on_frame(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received)
	{
		if (!state->registered)
		{
			on_register(state, link, received);
			return;
		}
		const std::uint16_t id = received.header.message_id;
		switch (static_cast<message_id>(id))
		{
		case message_id::link_heartbeat_from_world:
		{
			link::link_heartbeat beat{};
			if (!hb::net::decode_heartbeat(received, message_id::link_heartbeat_from_world, beat))
				link.close(close_reason::protocol_error, "malformed heartbeat");
			return;
		}
		case message_id::link_world_status:
		{
			link::world_status reported{};
			if (!hb::net::decode(received, reported))
			{
				link.close(close_reason::protocol_error, "malformed world_status");
				return;
			}
			std::scoped_lock lock(m_mutex);
			world_link_status& entry = m_links[state->world];
			entry.is_accepting_players = reported.is_accepting_players;
			entry.is_shutting_down = reported.is_shutting_down;
			entry.player_count = reported.player_count;
			entry.max_players = reported.max_players;
			return;
		}
		case message_id::link_character_list_response:
		case message_id::link_character_change_response:
		case message_id::link_session_grant_response:
		case message_id::link_session_started:
		case message_id::link_session_ended:
			// Character and session traffic is handled from LOGIN-006/007 on.
			log::debug("world {}: message 0x{:04x} not handled yet", state->world, id);
			return;
		default:
			link.close(close_reason::protocol_error, std::format("unexpected message 0x{:04x} on the world link", id));
			return;
		}
	}

	void world_link_service::on_register(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received)
	{
		link::world_register request{};
		if (!hb::net::decode(received, request))
		{
			link.close(close_reason::protocol_error,
				std::format("first frame must be a well-formed world_register (got 0x{:04x})", received.header.message_id));
			return;
		}
		const std::string name(request.world.view());
		const auto refuse = [&](link::register_result result, std::string_view why) {
			log::warning("world link from {}: '{}' refused: {}", link.remote_address(), name, why);
			(void)link.send(link::world_register_response{ result });
			link.close_after_send(close_reason::protocol_error, why);
		};
		const allowed_world* world = find_world(name);
		if (world == nullptr)
		{
			refuse(link::register_result::unknown_world, "unknown world");
			return;
		}
		// Another world's address, or the wrong key: one answer for both, so neither is confirmed.
		if (!same_address(link.remote_ip(), world->allowed_address))
		{
			refuse(link::register_result::bad_key, "address does not match this world");
			return;
		}
		if (!same_key(request.key, world->key))
		{
			refuse(link::register_result::bad_key, "wrong link key");
			return;
		}
		{
			std::scoped_lock lock(m_mutex);
			std::weak_ptr<hb::net::connection>& live = m_live[name];
			if (const auto existing = live.lock(); existing && existing->is_open())
			{
				refuse(link::register_result::world_name_taken, "already registered on another link");
				return;
			}
			live = state->link;
			world_link_status& entry = m_links[name];
			entry = world_link_status{};
			entry.state = link_state::live;
			entry.max_players = request.max_players;
		}
		link.set_deadline(std::chrono::milliseconds::zero());  // registered: heartbeats keep it alive now
		state->registered = true;
		state->world = name;
		(void)link.send(link::world_register_response{ link::register_result::ok });
		log::info("world {} registered from {} (clients at {}:{}, max {} players)", name, link.remote_address(),
			request.client_host.view(), request.client_port, request.max_players);

		state->heartbeat = hb::net::periodic_timer::create(link.executor());
		std::weak_ptr<peer> weak_state = state;
		state->heartbeat->start(m_options.heartbeat_interval, [weak_state, max_body = m_options.listen.connection.max_body_bytes] {
			const auto alive = weak_state.lock();
			if (!alive)
				return;
			const auto open = alive->link.lock();
			if (!open)
				return;
			std::vector<std::byte> bytes;
			if (hb::net::encode_heartbeat(message_id::link_heartbeat_from_login, link::link_heartbeat{ ++alive->sequence }, max_body, bytes))
				open->send_encoded(std::move(bytes));
		});
	}

	void world_link_service::on_close(const std::shared_ptr<peer>& state, hb::net::connection& link, close_reason reason,
		std::string_view detail)
	{
		if (state->heartbeat)
			state->heartbeat->stop();
		if (!state->registered)
		{
			log::info("world link from {}:{} closed before registering: {}{}{}", link.remote_address(), link.remote_port(),
				hb::net::to_string(reason), detail.empty() ? "" : ", ", detail);
			return;
		}
		{
			std::scoped_lock lock(m_mutex);
			const auto live = m_live.find(state->world);
			const auto current = live == m_live.end() ? nullptr : live->second.lock();
			if (current && current.get() != &link)
				return;  // an older link closing after the world re-registered on a new one
			m_links[state->world].state = link_state::lost;
			if (live != m_live.end())
				m_live.erase(live);
		}
		const auto write = reason == close_reason::shutdown ? log::level::info : log::level::warning;
		log::write(write, "world {} link lost: {}{}{}", state->world, hb::net::to_string(reason), detail.empty() ? "" : ", ",
			detail);
	}
}
