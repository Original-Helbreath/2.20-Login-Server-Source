// Network tests (LOGIN-005): a fake World server, built on hb::net::client, registers over loopback with
// the right key and is refused with a wrong key or from a wrong address; heartbeat loss marks the link
// lost; a malformed frame or an idle client closes the peer.
#include "client.h"
#include "client_listener.h"
#include "connection.h"
#include "frame_header.h"
#include "link_messages.h"
#include "message_codec.h"
#include "message_id.h"
#include "periodic_timer.h"
#include "span_writer.h"
#include "test_runner.h"
#include "world_link.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tests
{
	namespace
	{
		using namespace std::chrono_literals;
		using hb::net::close_reason;
		using hb::net::message_id;
		namespace link = hb::net::link;
		namespace login = hb::server::login;

		constexpr auto test_heartbeat = 50ms;
		constexpr auto test_loss = 150ms;

		// Runs the io_context on this thread until done() holds or the time is up.
		bool pump_until(asio::io_context& io, const std::function<bool()>& done, std::chrono::milliseconds limit = 3000ms)
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

		void pump_for(asio::io_context& io, std::chrono::milliseconds time)
		{
			pump_until(io, [] { return false; }, time);
		}

		hb::net::link_key key_of(std::uint8_t seed)
		{
			hb::net::link_key key{};
			for (std::size_t i = 0; i < key.size(); ++i)
				key[i] = static_cast<std::uint8_t>(seed + i);
			return key;
		}

		login::world_link_options link_options(std::string allowed_address)
		{
			login::world_link_options options;
			options.listen.bind_address = "127.0.0.1";
			options.listen.port = 0;
			options.worlds.push_back({ "WS1", std::move(allowed_address), key_of(1) });
			options.heartbeat_interval = test_heartbeat;
			options.loss_timeout = test_loss;
			return options;
		}

		// A stand-in World server on the shared client role. Everything runs on the test thread.
		struct fake_world
		{
			std::shared_ptr<hb::net::client> role;
			std::shared_ptr<hb::net::connection> link;
			bool connected = false;
			bool closed = false;
			close_reason closed_by = close_reason::local_close;
			int frames = 0;
			int login_heartbeats = 0;
			bool has_response = false;
			link::register_result response = link::register_result::ok;
			bool echoes_heartbeats = true;

			void connect(asio::io_context& io, std::uint16_t port, const link::world_register& request)
			{
				hb::net::client_options options;
				options.host = "127.0.0.1";
				options.port = port;
				role = hb::net::client::create(io, options);
				role->start(
					[this, request](const std::shared_ptr<hb::net::connection>& opened) {
						link = opened;
						connected = true;
						opened->start([this](hb::net::connection& from, const hb::net::frame& in) { on_frame(from, in); },
							[this](hb::net::connection&, close_reason reason, std::string_view) {
								closed = true;
								closed_by = reason;
							});
						(void)opened->send(request);
					},
					[](std::string_view) {});
			}

			void on_frame(hb::net::connection& from, const hb::net::frame& in)
			{
				++frames;
				link::world_register_response reply{};
				link::link_heartbeat beat{};
				if (hb::net::decode(in, reply))
				{
					has_response = true;
					response = reply.result;
				}
				else if (hb::net::decode_heartbeat(in, message_id::link_heartbeat_from_login, beat))
				{
					++login_heartbeats;
					std::vector<std::byte> bytes;
					if (echoes_heartbeats
						&& hb::net::encode_heartbeat(message_id::link_heartbeat_from_world, link::link_heartbeat{ beat.sequence }, 65536, bytes))
						from.send_encoded(std::move(bytes));
				}
			}

			void stop()
			{
				if (role)
					role->stop();
			}
		};

		link::world_register register_request(const hb::net::link_key& key, std::string_view name = "WS1")
		{
			link::world_register request{};
			request.world.assign(name);
			request.client_host.assign("127.0.0.1");
			request.client_port = 2849;
			request.key = key;
			request.max_players = 500;
			return request;
		}

		void good_key_tests(runner& t)
		{
			asio::io_context io;
			login::world_link_service links(io, link_options("127.0.0.1"));
			std::string error;
			t.check(links.start(error) && links.local_port() != 0, "world link listens on a free port");
			t.check(links.status("WS1").state == login::link_state::never_registered, "world starts never registered");

			fake_world world;
			world.connect(io, links.local_port(), register_request(key_of(1)));
			t.check(pump_until(io, [&] { return world.has_response; }), "fake World gets a register response");
			t.check(world.response == link::register_result::ok, "right key and address: register ok");
			t.check(pump_until(io, [&] { return links.status("WS1").state == login::link_state::live; }), "link marked live");
			t.check(links.status("WS1").max_players == 500, "max_players taken from world_register");

			// Kept alive by heartbeats both ways for several loss timeouts.
			pump_for(io, test_loss * 4);
			t.check(world.login_heartbeats >= 3, "Login sends heartbeats every interval");
			t.check(!world.closed && links.status("WS1").state == login::link_state::live, "answered heartbeats keep the link live");

			link::world_status status{ 1, 0, 42, 500 };
			(void)world.link->send(status);
			t.check(pump_until(io, [&] { return links.status("WS1").player_count == 42; }), "world_status updates the player count");

			// A second World under the same name is refused while the first is live.
			fake_world twin;
			twin.connect(io, links.local_port(), register_request(key_of(1)));
			t.check(pump_until(io, [&] { return twin.has_response && twin.closed; }), "second link for a live world answered and closed");
			t.check(twin.response == link::register_result::world_name_taken, "second link refused as world_name_taken");

			// Heartbeat loss: the World stops answering and the link is marked lost.
			world.echoes_heartbeats = false;
			t.check(pump_until(io, [&] { return links.status("WS1").state == login::link_state::lost; }), "heartbeat loss marks the link lost");
			t.check(pump_until(io, [&] { return world.closed; }), "lost link is closed");

			// After loss the World may register again.
			fake_world again;
			again.connect(io, links.local_port(), register_request(key_of(1)));
			t.check(pump_until(io, [&] { return again.has_response; }) && again.response == link::register_result::ok,
				"world re-registers after loss");
			t.check(pump_until(io, [&] { return links.status("WS1").state == login::link_state::live; }), "re-registered link is live");

			again.stop();
			twin.stop();
			world.stop();
			links.stop();
			pump_for(io, 50ms);
		}

		void refusal_tests(runner& t)
		{
			{
				asio::io_context io;
				login::world_link_service links(io, link_options("127.0.0.1"));
				std::string error;
				t.check(links.start(error), "world link listens (wrong key)");
				fake_world world;
				world.connect(io, links.local_port(), register_request(key_of(2)));
				t.check(pump_until(io, [&] { return world.has_response && world.closed; }), "wrong key: answered, then closed");
				t.check(world.response == link::register_result::bad_key, "wrong key refused as bad_key");
				t.check(links.status("WS1").state == login::link_state::never_registered, "wrong key leaves the world unregistered");

				fake_world stranger;
				stranger.connect(io, links.local_port(), register_request(key_of(1), "WS9"));
				t.check(pump_until(io, [&] { return stranger.has_response && stranger.closed; }), "unknown world: answered, then closed");
				t.check(stranger.response == link::register_result::unknown_world, "unknown world refused as unknown_world");

				fake_world rude;
				rude.connect(io, links.local_port(), register_request(key_of(1)));
				t.check(pump_until(io, [&] { return rude.connected; }), "fake World connected");
				t.check(pump_until(io, [&] { return rude.has_response; }), "registered before the rude frame");
				rude.link->send_encoded([] {
					std::vector<std::byte> bytes;
					(void)hb::net::encode_frame(hb::net::to_wire(message_id::login_response), {}, 65536, bytes);
					return bytes;
				}());
				t.check(pump_until(io, [&] { return rude.closed; }), "a Login -> client id on the world link closes it");
				t.check(pump_until(io, [&] { return links.status("WS1").state == login::link_state::lost; }),
					"protocol error marks the link lost");

				world.stop();
				stranger.stop();
				rude.stop();
				links.stop();
				pump_for(io, 50ms);
			}
			{
				// The only allowed address is 127.0.0.2, so a loopback World at 127.0.0.1 is a stranger.
				asio::io_context io;
				login::world_link_service links(io, link_options("127.0.0.2"));
				std::string error;
				t.check(links.start(error), "world link listens (wrong address)");
				fake_world world;
				world.connect(io, links.local_port(), register_request(key_of(1)));
				t.check(pump_until(io, [&] { return world.closed; }), "wrong address: connection closed");
				t.check(!world.has_response && world.frames == 0, "wrong address: no reply at all");
				t.check(links.status("WS1").state == login::link_state::never_registered, "wrong address leaves the world unregistered");
				world.stop();
				links.stop();
				pump_for(io, 50ms);
			}
		}

		// A raw peer on the shared client role that sends bytes and records whether the far side hung up.
		struct raw_peer
		{
			std::shared_ptr<hb::net::client> role;
			std::shared_ptr<hb::net::connection> link;
			bool closed = false;
			close_reason closed_by = close_reason::local_close;
			int frames = 0;

			void connect(asio::io_context& io, std::uint16_t port, std::vector<std::byte> first_bytes)
			{
				hb::net::client_options options;
				options.host = "127.0.0.1";
				options.port = port;
				role = hb::net::client::create(io, options);
				role->start(
					[this, first_bytes](const std::shared_ptr<hb::net::connection>& opened) {
						link = opened;
						opened->start([this](hb::net::connection&, const hb::net::frame&) { ++frames; },
							[this](hb::net::connection&, close_reason reason, std::string_view) {
								closed = true;
								closed_by = reason;
							});
						if (!first_bytes.empty())
							opened->send_encoded(first_bytes);
					},
					[](std::string_view) {});
			}
		};

		std::vector<std::byte> header_only(hb::net::frame_header header)
		{
			hb::net::span_writer writer;
			writer.write(header);
			return writer.take();
		}

		void client_listener_tests(runner& t)
		{
			asio::io_context io;
			login::client_listener_options options;
			options.listen.bind_address = "127.0.0.1";
			options.listen.port = 0;
			options.idle_before_login = 200ms;
			login::client_listener clients(io, options);
			std::string error;
			t.check(clients.start(error) && clients.local_port() != 0, "client listener listens on a free port");

			raw_peer reserved;
			reserved.connect(io, clients.local_port(), header_only({ 0, 0x0101, hb::net::protocol_version, 1 }));
			t.check(pump_until(io, [&] { return reserved.closed; }, 1000ms), "malformed frame (reserved byte set) closes the peer");
			t.check(reserved.closed_by == close_reason::peer_closed && reserved.frames == 0, "the server hung up without replying");

			raw_peer version;
			version.connect(io, clients.local_port(), header_only({ 0, 0x0101, static_cast<std::uint8_t>(hb::net::protocol_version + 1), 0 }));
			t.check(pump_until(io, [&] { return version.closed; }, 1000ms), "wrong protocol_version closes the peer");

			raw_peer oversize;
			oversize.connect(io, clients.local_port(), header_only({ 0x7FFFFFFF, 0x0101, hb::net::protocol_version, 0 }));
			t.check(pump_until(io, [&] { return oversize.closed; }, 1000ms), "oversize body_size closes the peer");

			raw_peer wrong_way;
			std::vector<std::byte> reply;
			(void)hb::net::encode_frame(hb::net::to_wire(message_id::login_response), {}, 65536, reply);
			wrong_way.connect(io, clients.local_port(), reply);
			t.check(pump_until(io, [&] { return wrong_way.closed; }, 1000ms), "a Login -> client id sent by a client closes it");

			raw_peer quiet;
			std::vector<std::byte> request;
			(void)hb::net::encode(hb::net::login::delete_character_request{}, 65536, request);
			quiet.connect(io, clients.local_port(), request);
			pump_for(io, 100ms);
			t.check(!quiet.closed, "a well-formed client message keeps the session open");
			t.check(pump_until(io, [&] { return quiet.closed; }, 1000ms), "idle session closed after the idle timeout");

			clients.stop();
			pump_until(io, [&] { return clients.session_count() == 0; }, 500ms);
			t.check(clients.session_count() == 0, "every session released after stop");
			pump_for(io, 20ms);
		}

		// Sends bytes one at a time, one per interval, on the peer's connection (a slowloris).
		std::shared_ptr<hb::net::periodic_timer> trickle(const std::shared_ptr<hb::net::connection>& link, std::vector<std::byte> bytes,
			std::chrono::milliseconds interval)
		{
			auto timer = hb::net::periodic_timer::create(link->executor());
			auto next = std::make_shared<std::size_t>(0);
			std::weak_ptr<hb::net::connection> weak = link;
			timer->start(interval, [weak, bytes, next] {
				const auto open = weak.lock();
				if (!open || *next >= bytes.size())
					return;
				open->send_encoded({ bytes[(*next)++] });
			});
			return timer;
		}

		void trickle_and_cap_tests(runner& t)
		{
			{
				asio::io_context io;
				login::client_listener_options options;
				options.listen.bind_address = "127.0.0.1";
				options.idle_before_login = 200ms;
				options.listen.max_connections = 2;
				login::client_listener clients(io, options);
				std::string error;
				t.check(clients.start(error), "client listener listens (trickle and cap)");

				// One byte every 40 ms of a valid frame never completes it inside the 200 ms deadline.
				std::vector<std::byte> request;
				(void)hb::net::encode(hb::net::login::login_request{}, 65536, request);
				raw_peer slow;
				slow.connect(io, clients.local_port(), {});
				t.check(pump_until(io, [&] { return slow.link != nullptr; }), "trickling client connected");
				const auto started = std::chrono::steady_clock::now();
				auto dripping = trickle(slow.link, request, 40ms);
				t.check(pump_until(io, [&] { return slow.closed; }, 2000ms), "trickling client closed by the deadline");
				const auto lasted = std::chrono::steady_clock::now() - started;
				t.check(lasted >= 150ms && lasted < 1000ms, "trickling client lasted about the 200 ms deadline");
				dripping->stop();
				pump_until(io, [&] { return clients.session_count() == 0; }, 500ms);

				// The cap: two held open, the third closed at once.
				raw_peer first;
				raw_peer second;
				first.connect(io, clients.local_port(), {});
				second.connect(io, clients.local_port(), {});
				t.check(pump_until(io, [&] { return clients.session_count() == 2; }, 1000ms), "two clients held at a cap of 2");
				raw_peer third;
				third.connect(io, clients.local_port(), {});
				t.check(pump_until(io, [&] { return third.closed; }, 1000ms), "third client closed at the cap");
				t.check(!first.closed && !second.closed && clients.session_count() == 2, "the first two stay open at the cap");
				t.check(pump_until(io, [&] { return first.closed && second.closed; }, 1000ms), "the first two still meet their deadline");
				clients.stop();
				pump_for(io, 20ms);
			}
			{
				// A World trickling world_register is closed by the registration deadline, unregistered.
				asio::io_context io;
				login::world_link_service links(io, link_options("127.0.0.1"));
				std::string error;
				t.check(links.start(error), "world link listens (trickle)");
				std::vector<std::byte> request;
				(void)hb::net::encode(register_request(key_of(1)), 65536, request);
				raw_peer slow;
				slow.connect(io, links.local_port(), {});
				t.check(pump_until(io, [&] { return slow.link != nullptr; }), "trickling World connected");
				auto dripping = trickle(slow.link, request, 30ms);
				t.check(pump_until(io, [&] { return slow.closed; }, 2000ms), "trickling World closed by the registration deadline");
				t.check(links.status("WS1").state == login::link_state::never_registered, "trickling World never registered");
				dripping->stop();
				links.stop();
				pump_for(io, 20ms);
			}
		}

		void shared_role_tests(runner& t)
		{
			asio::io_context io;
			// Nothing listens on this port once the server is stopped; the client reports the failure and retries.
			auto probe = hb::net::server::create(io);
			std::string error;
			hb::net::server_options options;
			options.bind_address = "127.0.0.1";
			t.check(probe->listen(options, [](const std::shared_ptr<hb::net::connection>&) {}, error), "probe server listens");
			const std::uint16_t port = probe->local_port();
			probe->stop();
			pump_for(io, 20ms);

			hb::net::client_options client_options;
			client_options.host = "127.0.0.1";
			client_options.port = port;
			client_options.reconnects = true;
			client_options.retry_delay = 30ms;
			auto role = hb::net::client::create(io, client_options);
			int failures = 0;
			bool connected = false;
			role->start([&](const std::shared_ptr<hb::net::connection>& opened) {
				connected = true;
				opened->start([](hb::net::connection&, const hb::net::frame&) {}, [](hb::net::connection&, close_reason, std::string_view) {});
			}, [&](std::string_view) { ++failures; });
			// Windows takes about 2 s to report a refused loopback connect, so one failure is waited for.
			t.check(pump_until(io, [&] { return failures >= 1; }, 6000ms), "client reports a refused connect");

			// The server comes up on that port; the retrying client connects to it.
			auto later = hb::net::server::create(io);
			options.port = port;
			int accepted = 0;
			t.check(later->listen(options, [&](const std::shared_ptr<hb::net::connection>& in) {
				++accepted;
				in->start([](hb::net::connection&, const hb::net::frame&) {}, [](hb::net::connection&, close_reason, std::string_view) {});
			}, error), "server listens on the same port");
			t.check(pump_until(io, [&] { return connected && accepted == 1; }, 6000ms), "client retries and connects once the server is up");
			t.check(later->connection_count() == 1, "server owns the accepted connection");
			role->stop();
			t.check(pump_until(io, [&] { return later->connection_count() == 0; }), "server releases a connection the client closed");
			later->stop();
			pump_for(io, 20ms);
		}
	}

	void network_tests(runner& t)
	{
		good_key_tests(t);
		refusal_tests(t);
		client_listener_tests(t);
		trickle_and_cap_tests(t);
		shared_role_tests(t);
	}
}
