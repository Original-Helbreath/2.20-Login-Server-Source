#pragma once

#include "common_types.h"
#include "config.h"
#include "connection.h"
#include "server.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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
	};

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

	private:
		struct peer;

		void on_accept(const std::shared_ptr<hb::net::connection>& link);
		void on_frame(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void on_register(const std::shared_ptr<peer>& state, hb::net::connection& link, const hb::net::frame& received);
		void on_close(const std::shared_ptr<peer>& state, hb::net::connection& link, hb::net::close_reason reason,
			std::string_view detail);
		const allowed_world* find_world(std::string_view name) const;

		world_link_options m_options;
		std::shared_ptr<hb::net::server> m_server;
		mutable std::mutex m_mutex;  // guards m_links: each link runs on its own strand
		std::map<std::string, world_link_status, std::less<>> m_links;
		std::map<std::string, std::weak_ptr<hb::net::connection>, std::less<>> m_live;
	};
}
