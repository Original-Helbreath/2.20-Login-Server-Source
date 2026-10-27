// Authentication tests (LOGIN-006): a real client_listener, authenticator, worker pool and players.db over
// loopback, driven by a test client built on hb::net::client. Covers login (good, bad password, unknown
// account, locked after the password check), a second login closing the first, the pre-login deadline
// and after-login idle timeout, create_account (ok, taken, invalid, disabled), change_password, the audit
// rows, and the protocol-error closes.
#include "account_service.h"
#include "authenticator.h"
#include "client.h"
#include "client_listener.h"
#include "connection.h"
#include "database.h"
#include "entities.h"
#include "message_codec.h"
#include "message_id.h"
#include "pepper_store.h"
#include "test_runner.h"
#include "worker_pool.h"

#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tests
{
	namespace
	{
		using namespace std::chrono_literals;
		using hb::net::close_reason;
		namespace proto = hb::net::login;
		namespace login = hb::server::login;

		constexpr login::argon2_cost test_cost{ 1, 8192, 1 };

		bool pump_until(asio::io_context& io, const std::function<bool()>& done, std::chrono::milliseconds limit = 5000ms)
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

		// A game client stand-in: sends typed requests, keeps every frame it receives.
		struct peer
		{
			std::shared_ptr<hb::net::client> role;
			std::shared_ptr<hb::net::connection> link;
			std::deque<hb::net::frame> inbox;
			bool connected = false;
			bool closed = false;
			close_reason closed_by = close_reason::local_close;

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
							[this](hb::net::connection&, close_reason reason, std::string_view) {
								closed = true;
								closed_by = reason;
							});
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

		// Waits for the next frame and decodes it as T; false if none comes or it is another message.
		template <class T>
		bool receive(asio::io_context& io, peer& from, T& out)
		{
			if (!pump_until(io, [&] { return !from.inbox.empty(); }))
				return false;
			const hb::net::frame next = from.inbox.front();
			from.inbox.pop_front();
			return hb::net::decode(next, out);
		}

		proto::login_request login_of(std::string_view name, std::string_view password)
		{
			proto::login_request request{};
			request.account.assign(name);
			request.password.assign(password);
			request.world.assign("WS1");
			return request;
		}

		proto::create_account_request create_of(std::string_view name, std::string_view password)
		{
			proto::create_account_request request{};
			request.account.assign(name);
			request.password.assign(password);
			request.email.assign("player@example.com");
			return request;
		}

		proto::change_password_request change_of(std::string_view name, std::string_view old_password, std::string_view new_password)
		{
			proto::change_password_request request{};
			request.account.assign(name);
			request.old_password.assign(old_password);
			request.new_password.assign(new_password);
			return request;
		}

		struct fixture_options
		{
			bool allow_creation = true;
			std::chrono::milliseconds before_login{ 4000 };
			std::chrono::milliseconds after_login{ 8000 };
			std::uint32_t max_failures = 3;
		};

		struct fixture
		{
			asio::io_context io;
			login::pepper_store peppers;
			login::database db;
			std::unique_ptr<login::account_service> accounts;
			std::unique_ptr<login::worker_pool> workers;
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
				login::authenticator_options auth_options;
				auth_options.allow_account_creation = options.allow_creation;
				auth_options.idle_after_login = options.after_login;
				auth_options.max_failed_attempts = options.max_failures;
				auth = std::make_unique<login::authenticator>(*accounts, *workers, auth_options);
				login::client_listener_options listen;
				listen.listen.bind_address = "127.0.0.1";
				listen.listen.port = 0;
				listen.idle_before_login = options.before_login;
				clients = std::make_unique<login::client_listener>(io, listen, auth.get());
				ready = ready && clients->start(error);
			}

			~fixture()
			{
				if (clients)
					clients->stop();
				pump_for(io, 30ms);
				if (workers)
					workers->stop();
			}

			bool open(peer& client)
			{
				client.connect(io, clients->local_port());
				return pump_until(io, [&] { return client.connected; });
			}

			std::int64_t events(std::string_view outcome)
			{
				schemata::ConnectionPool::Lease lease = db.pool().acquire();
				return schemata::from<login::login_event>(*lease, db.model())
					.where(schemata::col(&login::login_event::outcome) == std::string(outcome)).count();
			}

			void set_locked(std::string_view name_key, bool locked)
			{
				schemata::ConnectionPool::Lease lease = db.pool().acquire();
				std::optional<login::account> row = schemata::from<login::account>(*lease, db.model())
					.where(schemata::col(&login::account::name_key) == std::string(name_key)).first();
				schemata::ChangeTracker tracker(db.model());
				tracker.track(*row);
				row->locked = locked;
				tracker.save_changes(*lease);
			}

			// Audit rows written by a worker after the reply: wait for them.
			bool wait_events(std::string_view outcome, std::int64_t expected)
			{
				return pump_until(io, [&] { return events(outcome) == expected; }, 2000ms);
			}

			void set_cost(std::string_view name_key, std::int32_t memory_kib)
			{
				schemata::ConnectionPool::Lease lease = db.pool().acquire();
				std::optional<login::account> row = schemata::from<login::account>(*lease, db.model())
					.where(schemata::col(&login::account::name_key) == std::string(name_key)).first();
				schemata::ChangeTracker tracker(db.model());
				tracker.track(*row);
				row->hash_memory_kib = memory_kib;
				tracker.save_changes(*lease);
			}

			std::int64_t account_count()
			{
				std::string error;
				std::int64_t count = -1;
				(void)login::count_accounts(db, count, error);
				return count;
			}
		};

		// Logs in over a fresh connection and returns the reply code; nullopt when no reply came.
		std::optional<proto::login_result> try_login(fixture& f, peer& client, std::string_view name, std::string_view password)
		{
			if (!client.connected && !f.open(client))
				return std::nullopt;
			client.send(login_of(name, password));
			proto::login_response reply{};
			if (!receive(f.io, client, reply))
				return std::nullopt;
			return reply.result;
		}

		void login_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture f(folder, {});
			t.check(f.ready, "auth fixture ready");
			if (!f.ready)
				return;
			t.check(f.accounts->create("Bob", "secret1") == login::create_result::created, "auth: account Bob created");

			peer good;
			const auto good_result = try_login(f, good, "bob", "secret1");
			t.check(good_result == proto::login_result::ok, "good login answers ok, any case of the name");
			pump_for(f.io, 50ms);
			t.check(!good.closed && f.auth->authenticated_count() == 1, "good login keeps the session open and registered");

			peer wrong;
			t.check(try_login(f, wrong, "Bob", "secret2") == proto::login_result::password_mismatch, "bad password answers password_mismatch");
			pump_for(f.io, 50ms);
			t.check(!wrong.closed, "one bad password leaves the session open to retry");

			peer unknown;
			t.check(try_login(f, unknown, "Nobody", "secret1") == proto::login_result::password_mismatch,
				"unknown account answers the same code as a bad password");
			t.check(try_login(f, unknown, "bad name!", "secret1") == proto::login_result::password_mismatch,
				"an invalid account name answers the same code too");

			// After a retry the right password still logs in.
			t.check(try_login(f, wrong, "Bob", "secret1") == proto::login_result::ok, "retry with the right password logs in");

			// Locked is told only after the password check.
			f.set_locked("bob", true);
			peer locked;
			t.check(try_login(f, locked, "Bob", "secret1") == proto::login_result::account_locked, "locked account with the right password answers account_locked");
			t.check(try_login(f, locked, "Bob", "wrong12") == proto::login_result::password_mismatch,
				"locked account with a wrong password answers password_mismatch (locked not leaked)");
			t.check(!locked.closed, "locked reply leaves the session open");
			f.set_locked("bob", false);

			// Audit rows: one per attempt, outcome names only, never a password.
			t.check(f.events("accepted") == 2 && f.events("wrong_password") == 2 && f.events("no_account") == 2 && f.events("locked") == 1,
				"login audit rows recorded per outcome");

			// A second login closes the first session.
			peer first;
			peer second;
			t.check(try_login(f, first, "Bob", "secret1") == proto::login_result::ok, "second-login test: first session logs in");
			t.check(try_login(f, second, "Bob", "secret1") == proto::login_result::ok, "second-login test: second session logs in");
			t.check(pump_until(f.io, [&] { return first.closed; }), "a second login closes the first session");
			pump_for(f.io, 50ms);
			t.check(!second.closed, "the second session stays open");
			// good, wrong (retried to ok), first (closed) and second: wrong and second hold the account now.
			t.check(f.auth->authenticated_count() == 1, "one live session per account after a second login");
			peer third;
			t.check(try_login(f, third, "Bob", "secret1") == proto::login_result::ok, "third login accepted");
			t.check(pump_until(f.io, [&] { return second.closed; }), "the third login closes the second session");
			good.stop();
			wrong.stop();
			unknown.stop();
			locked.stop();
			first.stop();
			second.stop();
			third.stop();
		}

		void deadline_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture_options options;
			options.before_login = 400ms;
			options.after_login = 1200ms;
			fixture f(folder, options);
			t.check(f.ready, "deadline fixture ready");
			if (!f.ready)
				return;
			(void)f.accounts->create("Ann", "secret1");

			peer idle;
			t.check(f.open(idle), "idle client connected");
			t.check(pump_until(f.io, [&] { return idle.closed; }, 2000ms), "a client that never logs in is closed by the pre-login deadline");

			peer client;
			t.check(f.open(client), "logging-in client connected");
			const auto started = std::chrono::steady_clock::now();
			client.send(login_of("Ann", "secret1"));
			proto::login_response reply{};
			t.check(receive(f.io, client, reply) && reply.result == proto::login_result::ok, "deadline test: login ok");
			// The 400 ms pre-login deadline would have fired by now if login had not cleared it.
			pump_until(f.io, [&] { return client.closed; }, 700ms);
			t.check(!client.closed, "login clears the pre-login deadline");
			t.check(pump_until(f.io, [&] { return client.closed; }, 3000ms), "after-login idle timeout closes a silent logged-in session");
			const auto lasted = std::chrono::steady_clock::now() - started;
			t.check(lasted >= 1000ms && lasted < 2800ms, "logged-in session lasted about the 1200 ms idle timeout");
			client.stop();
			idle.stop();
		}

		void account_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture f(folder, {});
			t.check(f.ready, "account fixture ready");
			if (!f.ready)
				return;

			peer client;
			t.check(f.open(client), "create client connected");
			proto::create_account_response created{};
			client.send(create_of("Cara", "secret1"));
			t.check(receive(f.io, client, created) && created.result == proto::account_result::ok, "create_account ok");
			client.send(create_of("cara", "other12"));
			t.check(receive(f.io, client, created) && created.result == proto::account_result::name_taken, "create_account name taken (case-insensitive)");
			client.send(create_of("bad name", "secret1"));
			t.check(receive(f.io, client, created) && created.result == proto::account_result::invalid_name, "create_account invalid name");
			t.check(f.account_count() == 1, "only the one account was stored");
			t.check(try_login(f, client, "Cara", "secret1") == proto::login_result::ok, "the created account logs in");
			t.check(f.events("created") == 1 && f.events("name_taken") == 1 && f.events("bad_name") == 1, "account creation audit rows recorded");

			// A fourth account request on one connection is over the creation cap.
			peer greedy;
			t.check(f.open(greedy), "greedy client connected");
			for (const char* name : { "Dan", "Eve", "Fay" })
			{
				greedy.send(create_of(name, "secret1"));
				receive(f.io, greedy, created);
			}
			t.check(created.result == proto::account_result::ok && f.account_count() == 4, "three creations allowed per connection");
			greedy.send(create_of("Gus", "secret1"));
			t.check(pump_until(f.io, [&] { return greedy.closed; }), "a fourth creation closes the connection");
			t.check(f.account_count() == 4, "the refused creation stored nothing");
			t.check(f.wait_events("create_capped", 1), "a creation over the cap is audited");

			// change_password, allowed before login.
			peer changer;
			t.check(f.open(changer), "change client connected");
			proto::change_password_response changed{};
			changer.send(change_of("Cara", "secret1", "newpass1"));
			t.check(receive(f.io, changer, changed) && changed.result == proto::account_result::ok, "change_password ok before login");
			peer old_pw;
			t.check(try_login(f, old_pw, "Cara", "secret1") == proto::login_result::password_mismatch, "the old password no longer works");
			t.check(try_login(f, old_pw, "Cara", "newpass1") == proto::login_result::ok, "the new password works");
			changer.send(change_of("Cara", "wrong12", "newpass2"));
			t.check(receive(f.io, changer, changed) && changed.result == proto::account_result::failed, "change_password with a wrong old password fails");
			changer.send(change_of("Nobody", "secret1", "newpass2"));
			t.check(receive(f.io, changer, changed) && changed.result == proto::account_result::failed,
				"change_password for an unknown account fails the same way");
			changer.send(change_of("Cara", "newpass1", "abc"));
			t.check(receive(f.io, changer, changed) && changed.result == proto::account_result::failed, "change_password to a too-short password fails");
			t.check(!changer.closed, "failed changes under the cap leave the session open");
			peer still_new;
			t.check(try_login(f, still_new, "Cara", "newpass1") == proto::login_result::ok, "a refused change leaves the password as it was");
			f.set_locked("cara", true);
			changer.send(change_of("Cara", "newpass1", "newpass3"));
			t.check(receive(f.io, changer, changed) && changed.result == proto::account_result::failed, "change_password on a locked account fails");
			f.set_locked("cara", false);
			t.check(f.events("pw_changed") == 1 && f.events("pw_change_fail") == 2 && f.events("pw_change_lock") == 1, "password change audit rows recorded");

			// Wrong passwords count toward the per-connection cap and close the connection after the reply.
			peer brute;
			t.check(f.open(brute), "brute client connected");
			proto::login_response reply{};
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				brute.send(login_of("Cara", "guess123"));
				t.check(receive(f.io, brute, reply) && reply.result == proto::login_result::password_mismatch, "wrong guess answered before the cap");
			}
			t.check(pump_until(f.io, [&] { return brute.closed; }), "the third wrong password closes the connection after the reply");

			client.stop();
			greedy.stop();
			changer.stop();
			old_pw.stop();
			still_new.stop();
			brute.stop();
		}

		void disabled_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture_options options;
			options.allow_creation = false;
			fixture f(folder, options);
			t.check(f.ready, "disabled fixture ready");
			if (!f.ready)
				return;
			peer client;
			t.check(f.open(client), "disabled-creation client connected");
			proto::create_account_response created{};
			client.send(create_of("Hal", "secret1"));
			t.check(receive(f.io, client, created) && created.result == proto::account_result::disabled, "create_account answers disabled when the config forbids it");
			t.check(f.account_count() == 0 && !client.closed, "nothing stored and the session stays open");
			t.check(f.wait_events("create_disabled", 1), "a creation while disabled is audited");
			client.stop();
		}

		void protocol_error_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture f(folder, {});
			t.check(f.ready, "protocol fixture ready");
			if (!f.ready)
				return;
			(void)f.accounts->create("Ivy", "secret1");

			peer early;
			t.check(f.open(early), "early client connected");
			early.send(proto::delete_character_request{});
			t.check(pump_until(f.io, [&] { return early.closed; }), "a character message before login closes the connection");

			peer twice;
			t.check(try_login(f, twice, "Ivy", "secret1") == proto::login_result::ok, "double-login test: first login ok");
			twice.send(login_of("Ivy", "secret1"));
			t.check(pump_until(f.io, [&] { return twice.closed; }), "a login_request on a logged-in session closes it");

			// Two logins sent back to back: the second arrives while the first is pending.
			peer pipelined;
			t.check(f.open(pipelined), "pipelining client connected");
			std::vector<std::byte> both;
			(void)hb::net::encode(login_of("Ivy", "secret1"), 65536, both);
			std::vector<std::byte> second;
			(void)hb::net::encode(login_of("Ivy", "secret1"), 65536, second);
			both.insert(both.end(), second.begin(), second.end());
			pipelined.link->send_encoded(std::move(both));
			t.check(pump_until(f.io, [&] { return pipelined.closed; }), "a request sent while another is pending closes the connection");

			// A well-formed character message after login is accepted (handled in LOGIN-007).
			peer logged;
			t.check(try_login(f, logged, "Ivy", "secret1") == proto::login_result::ok, "post-login message test: login ok");
			logged.send(proto::delete_character_request{});
			pump_for(f.io, 100ms);
			t.check(!logged.closed, "a character message after login keeps the session open");
			// An unusable password row answers service_unavailable and counts toward the failure cap.
			(void)f.accounts->create("Jon", "secret1");
			f.set_cost("jon", -1);
			peer broken;
			t.check(f.open(broken), "unusable-row client connected");
			proto::login_response broken_reply{};
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				broken.send(login_of("Jon", "secret1"));
				t.check(receive(f.io, broken, broken_reply) && broken_reply.result == proto::login_result::service_unavailable,
					"unusable row answers service_unavailable");
			}
			t.check(pump_until(f.io, [&] { return broken.closed; }), "service_unavailable replies count toward the failure cap");
			broken.stop();
			early.stop();
			twice.stop();
			pipelined.stop();
			logged.stop();
		}
	}

	void auth_tests(runner& t, const std::filesystem::path& folder)
	{
		// The options a live server gets from its config defaults, not the ones the fixtures force.
		const login::authenticator_options live = login::make_authenticator_options(login::login_config{});
		t.check(live.max_failed_attempts == 3 && live.max_account_creations == 3, "live authenticator caps are 3 failures and 3 creations");
		t.check(live.allow_account_creation && live.idle_after_login == 600s, "live authenticator takes creation and idle timeout from the config");
		login_tests(t, folder / "auth_login");
		deadline_tests(t, folder / "auth_deadline");
		account_tests(t, folder / "auth_accounts");
		disabled_tests(t, folder / "auth_disabled");
		protocol_error_tests(t, folder / "auth_protocol");
	}
}
