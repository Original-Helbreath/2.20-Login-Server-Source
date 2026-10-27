#pragma once

// Test support shared by the Login server's network tests: a loopback game-client stand-in, the server fixture
// (players.db, workers, World link, authenticator, client listener) with a scripted World, and helpers.
#include "account_service.h"
#include "authenticator.h"
#include "character_rules.h"
#include "client.h"
#include "client_listener.h"
#include "connection.h"
#include "database.h"
#include "message_codec.h"
#include "message_id.h"
#include "pepper_store.h"
#include "test_runner.h"
#include "worker_pool.h"
#include "world_link.h"
#include "world_stub.h"

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tests
{
	namespace support
	{
		using namespace std::chrono_literals;
		using hb::net::close_reason;
		namespace proto = hb::net::login;
		namespace login = hb::server::login;

		constexpr login::argon2_cost test_cost{ 1, 8192, 1 };

		inline bool pump_until(asio::io_context& io, const std::function<bool()>& done, std::chrono::milliseconds limit = 5000ms)
		{
			const auto deadline = std::chrono::steady_clock::now() + limit;
			while (!done())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				if (io.stopped())
					io.restart();
				io.run_for(5ms);
			}
			return true;
		}

		inline void pump_for(asio::io_context& io, std::chrono::milliseconds time)
		{
			pump_until(io, [] { return false; }, time);
		}

		// A game client stand-in: sends typed requests, keeps every frame it receives.
		struct peer
		{
			std::shared_ptr<hb::net::client> role;
			std::shared_ptr<hb::net::connection> link;
			std::deque<hb::net::frame> inbox;
			bool connected = false;
			bool closed = false;

			void connect(asio::io_context& io, std::uint16_t port)
			{
				hb::net::client_options options;
				options.host = "127.0.0.1";
				options.port = port;
				role = hb::net::client::create(io, options);
				role->start(
					[this](const std::shared_ptr<hb::net::connection>& opened) {
						link = opened;
						connected = true;
						opened->start([this](hb::net::connection&, const hb::net::frame& in) { inbox.push_back(in); },
							[this](hb::net::connection&, close_reason, std::string_view) { closed = true; });
					},
					[](std::string_view) {});
			}

			template <class T>
			void send(const T& message)
			{
				(void)link->send(message);
			}

			void stop()
			{
				if (role)
					role->stop();
			}
		};

		template <class T>
		bool receive(asio::io_context& io, peer& from, T& out)
		{
			if (!pump_until(io, [&] { return !from.inbox.empty(); }))
				return false;
			const hb::net::frame next = from.inbox.front();
			from.inbox.pop_front();
			return hb::net::decode(next, out);
		}

		inline bool receive_list(asio::io_context& io, peer& from, proto::character_list& head, std::vector<hb::net::character_summary>& list)
		{
			if (!pump_until(io, [&] { return !from.inbox.empty(); }))
				return false;
			const hb::net::frame next = from.inbox.front();
			from.inbox.pop_front();
			return hb::net::decode_character_list(next, head, list);
		}

		struct fixture_options
		{
			bool register_world = true;
			std::chrono::milliseconds request_timeout{ 2000 };
			std::chrono::milliseconds ticket_lifetime{ 20000 };
			bool second_world = false;  // also allow a World named WS2 (tests connect their own stub to it)
		};

		struct fixture
		{
			asio::io_context io;
			login::pepper_store peppers;
			login::database db;
			std::unique_ptr<login::account_service> accounts;
			std::unique_ptr<login::worker_pool> workers;
			std::unique_ptr<login::world_link_service> worlds;
			world_stub world;
			std::unique_ptr<login::authenticator> auth;
			std::unique_ptr<login::client_listener> clients;
			bool ready = false;

			fixture(const std::filesystem::path& folder, const fixture_options& options)
			{
				std::error_code ignored;
				std::filesystem::remove_all(folder, ignored);
				std::filesystem::create_directories(folder, ignored);
				std::string error;
				ready = peppers.load_or_create(folder / "pepper.json", true, error)
					&& db.open((folder / "players.db").string(), 2, error);
				if (!ready)
					return;
				accounts = std::make_unique<login::account_service>(db, peppers, test_cost);
				ready = accounts->verify_peppers(error);
				workers = std::make_unique<login::worker_pool>(2);
				login::world_link_options link_options;
				link_options.listen.bind_address = "127.0.0.1";
				link_options.listen.port = 0;
				link_options.worlds.push_back({ "WS1", "127.0.0.1", stub_key() });
				if (options.second_world)
					link_options.worlds.push_back({ "WS2", "127.0.0.1", stub_key() });
				link_options.request_timeout = options.request_timeout;
				link_options.ticket_lifetime = options.ticket_lifetime;
				worlds = std::make_unique<login::world_link_service>(io, link_options);
				ready = ready && worlds->start(error);
				if (!ready)
					return;
				login::authenticator_options auth_options;
				auth = std::make_unique<login::authenticator>(*accounts, *workers, *worlds, auth_options);
				login::client_listener_options listen;
				listen.listen.bind_address = "127.0.0.1";
				listen.listen.port = 0;
				clients = std::make_unique<login::client_listener>(io, listen, auth.get());
				ready = clients->start(error);
				if (ready && options.register_world)
					ready = connect_world();
			}

			~fixture()
			{
				if (clients)
					clients->stop();
				world.stop();
				if (worlds)
					worlds->stop();
				pump_for(io, 30ms);
				if (workers)
					workers->stop();
			}

			bool connect_world()
			{
				world.connect(io, worlds->local_port(), "WS1");
				return pump_until(io, [&] { return worlds->is_live("WS1") && world.registered; });
			}

			bool open(peer& client)
			{
				client.connect(io, clients->local_port());
				return pump_until(io, [&] { return client.connected; });
			}
		};

		inline proto::login_request login_of(std::string_view name, std::string_view password, std::string_view world)
		{
			proto::login_request request{};
			request.account.assign(name);
			request.password.assign(password);
			request.world.assign(world);
			return request;
		}

		// Sends login_request and reads login_response; nullopt when no reply came.
		inline std::optional<proto::login_result> login_to(fixture& f, peer& client, std::string_view world)
		{
			client.send(login_of("Bob", "secret1", world));
			proto::login_response reply{};
			if (!receive(f.io, client, reply))
				return std::nullopt;
			return reply.result;
		}

		inline proto::create_character_request create_of(std::string_view name)
		{
			proto::create_character_request request{};
			request.name.assign(name);
			request.sex = 1;
			request.skin = 1;
			request.hair_style = 2;
			request.hair_colour = 3;
			request.underwear_colour = 4;
			request.stats = { 12, 12, 12, 12, 11, 11 };  // 70
			return request;
		}

		inline proto::delete_character_request delete_of(std::string_view name)
		{
			proto::delete_character_request request{};
			request.name.assign(name);
			return request;
		}

		inline hb::net::character_summary summary_of(std::string_view name)
		{
			hb::net::character_summary summary{};
			summary.name.assign(name);
			summary.is_valid = 1;
			summary.sex = 1;
			summary.skin = 1;
			summary.level = 5;
			summary.map_name.assign("default");
			return summary;
		}

		inline bool names_are(const std::vector<hb::net::character_summary>& list, std::initializer_list<std::string_view> names)
		{
			if (list.size() != names.size())
				return false;
			std::size_t i = 0;
			for (const std::string_view name : names)
			{
				if (list[i++].name.view() != name)
					return false;
			}
			return true;
		}

		// True when nothing more arrives for a while: no stray list follows a refusal.
		inline bool stays_quiet(fixture& f, peer& client)
		{
			pump_for(f.io, 100ms);
			return client.inbox.empty() && !client.closed;
		}

	}
}
