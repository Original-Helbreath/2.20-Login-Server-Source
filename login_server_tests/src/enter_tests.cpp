// Enter-game tests (LOGIN-008): the account online registry and session tickets on their own, then a real
// client_listener, authenticator and world_link_service over loopback with a scripted World (world_stub.h):
// grant ok giving the client a ticket and the World's address, ticket expiry setting the account offline, the
// World's confirmation keeping it online, already_playing, force login sending kick_account, refusals from the
// World, and link loss setting that World's accounts offline.
#include "net_fixture.h"
#include "online_accounts.h"
#include "session_tickets.h"
#include "test_runner.h"

#include <format>
#include <set>

namespace tests
{
	namespace
	{
		using namespace support;
		using login::online_state;
		namespace link = hb::net::link;

		constexpr std::uint64_t account_a = 11;
		constexpr std::uint64_t account_b = 12;

		void ticket_tests(runner& t)
		{
			hb::net::session_ticket first{};
			hb::net::session_ticket second{};
			t.check(login::is_blank(first), "a fresh ticket is blank");
			t.check(login::generate_ticket(first) && login::generate_ticket(second), "tickets are generated");
			t.check(!login::is_blank(first) && !login::is_blank(second) && first != second, "tickets are non-blank and differ");
			std::set<hb::net::session_ticket> seen;
			for (int i = 0; i < 64; ++i)
			{
				hb::net::session_ticket next{};
				(void)login::generate_ticket(next);
				seen.insert(next);
			}
			t.check(seen.size() == 64, "64 tickets are all distinct");
			login::wipe_ticket(first);
			t.check(login::is_blank(first), "a wiped ticket is blank");
		}

		void registry_tests(runner& t)
		{
			asio::io_context io;
			login::online_registry registry(io.get_executor(), 150ms);
			using login::enter_decision;

			// offline -> handoff_pending, and a second normal enter is refused.
			t.check(registry.state(account_a) == online_state::offline, "an unknown account is offline");
			const login::enter_outcome first = registry.begin_enter(account_a, "WS1", "Ann", false);
			t.check(first.decision == enter_decision::reserved && first.generation != 0, "an offline account is reserved");
			t.check(registry.state(account_a) == online_state::handoff_pending && registry.world_of(account_a) == "WS1", "reserved means handoff_pending on that World");
			t.check(registry.begin_enter(account_a, "WS1", "Ann", false).decision == enter_decision::already_playing, "a pending account is already_playing");
			t.check(registry.is_pending(account_a, first.generation) && !registry.is_pending(account_a, first.generation + 1), "is_pending names the generation");

			// release: only the reservation it names.
			registry.release(account_a, first.generation + 1);
			t.check(registry.state(account_a) == online_state::handoff_pending, "releasing another generation changes nothing");
			registry.release(account_a, first.generation);
			t.check(registry.state(account_a) == online_state::offline, "release sets the account offline");

			// expiry: pending -> offline on its own.
			const login::enter_outcome second = registry.begin_enter(account_a, "WS1", "Ann", false);
			t.check(pump_until(io, [&] { return registry.state(account_a) == online_state::offline; }, 2000ms), "an unconfirmed ticket expires to offline");
			t.check(!registry.is_pending(account_a, second.generation), "an expired reservation is no longer pending");

			// confirmation: session_started keeps it online past the expiry time.
			const login::enter_outcome third = registry.begin_enter(account_a, "WS1", "Ann", false);
			registry.on_started("WS1", account_a, "Ann");
			t.check(registry.state(account_a) == online_state::in_world && !registry.is_pending(account_a, third.generation), "session_started puts the account in_world");
			pump_for(io, 300ms);
			t.check(registry.state(account_a) == online_state::in_world, "a confirmed account stays online after the ticket time");

			// in_world: normal is refused, force names the holder and leaves it until session_ended.
			t.check(registry.begin_enter(account_a, "WS1", "Ann", false).decision == enter_decision::already_playing, "an in_world account is already_playing");
			const login::enter_outcome forced = registry.begin_enter(account_a, "WS2", "Ann", true);
			t.check(forced.decision == enter_decision::kick_holder && forced.holder_world == "WS1", "force login names the holding World");
			t.check(registry.state(account_a) == online_state::in_world, "force login leaves an in_world account until the World reports the end");
			registry.on_ended("WS2", account_a);
			t.check(registry.state(account_a) == online_state::in_world, "session_ended from another World is ignored");
			registry.on_ended("WS1", account_a);
			t.check(registry.state(account_a) == online_state::offline, "session_ended sets the account offline");

			// force on a pending reservation revokes it at once.
			(void)registry.begin_enter(account_b, "WS1", "Bob", false);
			const login::enter_outcome revoked = registry.begin_enter(account_b, "WS1", "Bob", true);
			t.check(revoked.decision == enter_decision::kick_holder && registry.state(account_b) == online_state::offline, "force login revokes a pending reservation");
			t.check(registry.begin_enter(account_b, "WS1", "Bob", true).decision == enter_decision::reserved, "force login for an offline account just reserves");

			// rebuild after a restart: session_started for an unknown account.
			registry.on_ended("WS1", account_b);
			registry.on_started("WS1", account_b, "Bob");
			t.check(registry.state(account_b) == online_state::in_world, "session_started for an unknown account puts it in_world");

			// Another World claiming an account the first holds is refused; the holder keeps it.
			using verdict = login::online_registry::start_verdict;
			t.check(registry.on_started("WS2", account_b, "Bob") == verdict::held_elsewhere, "a World claiming an account another World holds is refused");
			t.check(registry.state(account_b) == online_state::in_world && registry.world_of(account_b) == "WS1", "the holder keeps an in_world account");
			registry.on_ended("WS1", account_b);
			const login::enter_outcome held = registry.begin_enter(account_b, "WS1", "Bob", false);
			t.check(registry.on_started("WS2", account_b, "Bob") == verdict::held_elsewhere && registry.is_pending(account_b, held.generation)
					&& registry.world_of(account_b) == "WS1",
				"a World claiming a pending account is refused and the grant stays pending");

			// Pending on the same World with another character: accepted, the grant's character is kept.
			registry.release(account_b, held.generation);
			(void)registry.begin_enter(account_b, "WS1", "Bob", false);
			t.check(registry.on_started("WS1", account_b, "Zed") == verdict::accepted && registry.state(account_b) == online_state::in_world
					&& registry.character_of(account_b) == "Bob",
				"a confirmation naming another character keeps the grant's character");
			registry.on_ended("WS1", account_b);
			t.check(registry.on_started("WS1", account_b, "Bob") == verdict::accepted && registry.state(account_b) == online_state::in_world,
				"an unknown account is still accepted (restart rebuild)");

			// link loss: every account of that World goes offline, another World's stay.
			registry.on_started("WS2", account_a, "Ann");
			t.check(registry.world_lost("WS1") == 1 && registry.state(account_b) == online_state::offline && registry.state(account_a) == online_state::in_world,
				"world_lost drops only that World's accounts");
			t.check(registry.count(online_state::in_world) == 1 && registry.count(online_state::handoff_pending) == 0, "counts follow the states");
		}

		// Logs the account in on the client and returns its id (0 when the login failed).
		std::uint64_t login_as(fixture& f, peer& client, std::string_view name)
		{
			client.send(login_of(name, "secret1", "WS1"));
			proto::login_response reply{};
			proto::character_list head{};
			std::vector<hb::net::character_summary> list;
			if (!receive(f.io, client, reply) || reply.result != proto::login_result::ok || !receive_list(f.io, client, head, list))
				return 0;
			return f.world.last_account_id;
		}

		proto::enter_game_request enter_of(std::string_view character, proto::enter_kind kind = proto::enter_kind::normal)
		{
			proto::enter_game_request request{};
			request.character.assign(character);
			request.kind = kind;
			return request;
		}

		// Sends enter_game_request and reads the answer; nullopt when none came.
		std::optional<proto::enter_game_response> enter(fixture& f, peer& client, std::string_view character,
			proto::enter_kind kind = proto::enter_kind::normal)
		{
			client.send(enter_of(character, kind));
			proto::enter_game_response reply{};
			if (!receive(f.io, client, reply))
				return std::nullopt;
			return reply;
		}

		bool prepare(fixture& f, runner& t, std::string_view label)
		{
			t.check(f.ready, std::format("{} fixture ready", label));
			if (!f.ready)
				return false;
			t.check(f.accounts->create("Bob", "secret1") == login::create_result::created
					&& f.accounts->create("Cat", "secret1") == login::create_result::created,
				std::format("{}: accounts Bob and Cat created", label));
			f.world.characters = { summary_of("Ann"), summary_of("Cid") };
			return true;
		}

		void grant_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture f(folder, {});
			if (!prepare(f, t, "grant"))
				return;
			std::set<hb::net::session_ticket> tickets;

			// grant ok: the client gets the World's address and a ticket, the World gets the same ticket.
			peer client;
			t.check(f.open(client), "enter client connected");
			const std::uint64_t bob = login_as(f, client, "Bob");
			t.check(bob != 0, "Bob logged in");
			const auto granted = enter(f, client, "Ann");
			t.check(granted && granted->result == proto::enter_result::ok, "enter_game_request answers ok");
			t.check(granted && granted->world_host.view() == "127.0.0.1" && granted->world_port == 2848, "the response carries the World's client address and port from its registration");
			t.check(granted && !login::is_blank(granted->ticket), "the response carries a ticket");
			t.check(f.world.grants == 1 && f.world.last_grant.account_id == bob && f.world.last_grant.character.view() == "Ann",
				"the World was sent one session_grant for the account and character");
			t.check(granted && f.world.last_grant.ticket == granted->ticket, "the World and the client hold the same ticket");
			t.check(f.world.last_grant.expires_in_seconds == 20, "the grant carries the ticket lifetime");
			t.check(f.world.last_grant.request_id != 0, "the grant carries a request id");
			t.check(f.worlds->online().state(bob) == online_state::handoff_pending && f.worlds->online().world_of(bob) == "WS1",
				"the account is handoff_pending on the World");
			t.check(pump_until(f.io, [&] { return client.closed; }), "the client session is closed after the response (handed_off)");
			t.check(f.worlds->pending_count() == 0, "nothing is left pending");
			if (granted)
				tickets.insert(granted->ticket);
			client.stop();

			// The World confirms the ticket: in_world.
			f.world.send_started(bob, "Ann");
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::in_world; }), "session_started puts the account in_world");

			// already_playing for a normal enter, from a new login.
			peer again;
			t.check(f.open(again) && login_as(f, again, "Bob") == bob, "Bob logs in again");
			const auto refused = enter(f, again, "Cid");
			t.check(refused && refused->result == proto::enter_result::already_playing, "a normal enter while in_world answers already_playing");
			t.check(refused && login::is_blank(refused->ticket) && refused->world_host.view().empty() && refused->world_port == 0,
				"already_playing carries no address and no ticket");
			t.check(f.world.grants == 1 && f.world.kicks.empty() && !again.closed, "already_playing sends nothing to the World and keeps the session");

			// force login: kick_account to the holding World, force_disconnect_sent to the client.
			const auto forced = enter(f, again, "Ann", proto::enter_kind::force_login);
			t.check(forced && forced->result == proto::enter_result::force_disconnect_sent, "force_login answers force_disconnect_sent");
			t.check(pump_until(f.io, [&] { return !f.world.kicks.empty(); }), "the World was sent kick_account");
			t.check(f.world.kicks.size() == 1 && f.world.kicks[0].account_id == bob && f.world.kicks[0].reason == link::kick_reason::forced_login_elsewhere,
				"the kick names the account and forced_login_elsewhere");
			t.check(f.worlds->online().state(bob) == online_state::in_world && f.world.grants == 1, "the account stays in_world until the World reports the end");

			// The World ends the session as ordered; the retry is granted a new ticket.
			f.world.send_ended(bob, link::session_end_reason::kicked);
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::offline; }), "session_ended sets the account offline");
			const auto retried = enter(f, again, "Ann");
			t.check(retried && retried->result == proto::enter_result::ok && f.world.grants == 2, "the retry after the kick is granted");
			t.check(retried && tickets.insert(retried->ticket).second, "the retry's ticket differs from the first");
			again.stop();

			// logout: session_ended while handoff is complete.
			f.world.send_started(bob, "Ann");
			f.world.send_ended(bob, link::session_end_reason::logout);
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::offline; }), "a logout sets the account offline");
			t.check(f.worlds->online().count(online_state::in_world) == 0 && f.worlds->online().count(online_state::handoff_pending) == 0,
				"no account is left online");
		}

		void pending_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture f(folder, {});
			if (!prepare(f, t, "pending"))
				return;

			peer first;
			t.check(f.open(first), "pending client connected");
			const std::uint64_t bob = login_as(f, first, "Bob");
			const auto granted = enter(f, first, "Ann");
			t.check(granted && granted->result == proto::enter_result::ok, "the first enter is granted");
			first.stop();

			// A second client while the ticket is outstanding: already_playing.
			peer second;
			t.check(f.open(second) && login_as(f, second, "Bob") == bob, "Bob logs in again while the ticket is outstanding");
			const auto refused = enter(f, second, "Ann");
			t.check(refused && refused->result == proto::enter_result::already_playing && f.world.grants == 1,
				"a normal enter while handoff_pending answers already_playing and sends no second grant");

			// Force login revokes the pending grant: kick, offline, and the retry gets a fresh ticket.
			const auto forced = enter(f, second, "Ann", proto::enter_kind::force_login);
			t.check(forced && forced->result == proto::enter_result::force_disconnect_sent, "force_login on a pending handoff answers force_disconnect_sent");
			t.check(pump_until(f.io, [&] { return f.world.kicks.size() == 1; }) && f.world.kicks[0].account_id == bob,
				"the World was told to drop the pending grant");
			t.check(f.worlds->online().state(bob) == online_state::offline, "a revoked handoff is offline at once");
			const auto retried = enter(f, second, "Ann");
			t.check(retried && retried->result == proto::enter_result::ok && f.world.grants == 2 && granted && retried->ticket != granted->ticket,
				"the retry is granted a different ticket");
			second.stop();
		}

		void expiry_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture_options options;
			options.ticket_lifetime = 400ms;
			fixture f(folder, options);
			if (!prepare(f, t, "expiry"))
				return;

			peer client;
			t.check(f.open(client), "expiry client connected");
			const std::uint64_t bob = login_as(f, client, "Bob");
			const auto granted = enter(f, client, "Ann");
			t.check(granted && granted->result == proto::enter_result::ok, "the enter is granted");
			t.check(f.world.last_grant.expires_in_seconds == 1, "a sub-second lifetime is sent as one second");
			t.check(f.worlds->online().state(bob) == online_state::handoff_pending, "handoff_pending while the ticket is valid");
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::offline; }, 3000ms),
				"an unconfirmed ticket sets the account offline when it expires");

			// A new login and enter after the expiry is granted again.
			peer again;
			t.check(f.open(again) && login_as(f, again, "Bob") == bob, "Bob logs in after the expiry");
			const auto second = enter(f, again, "Ann");
			t.check(second && second->result == proto::enter_result::ok && f.world.grants == 2, "an enter after the expiry is granted");

			// The confirmation keeps the account online past the ticket time.
			f.world.send_started(bob, "Ann");
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::in_world; }), "the confirmation puts the account in_world");
			pump_for(f.io, 700ms);
			t.check(f.worlds->online().state(bob) == online_state::in_world, "a confirmed account stays online after the ticket time");
			client.stop();
			again.stop();
		}

		void refusal_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture_options options;
			options.request_timeout = 300ms;
			fixture f(folder, options);
			if (!prepare(f, t, "refusal"))
				return;

			// Before login an enter closes the connection.
			peer early;
			t.check(f.open(early), "refusal: early client connected");
			early.send(enter_of("Ann"));
			t.check(pump_until(f.io, [&] { return early.closed; }), "enter_game_request before login closes the connection");

			peer client;
			t.check(f.open(client), "refusal client connected");
			const std::uint64_t bob = login_as(f, client, "Bob");

			// A name that cannot be a character never reaches the World.
			const auto bad = enter(f, client, "bad name!");
			t.check(bad && bad->result == proto::enter_result::no_such_character && f.world.grants == 0, "an invalid character name answers no_such_character without asking the World");

			// The World's own refusals map to the client's results and leave the account offline.
			f.world.grant_answer = link::grant_result::world_full;
			const auto full = enter(f, client, "Ann");
			t.check(full && full->result == proto::enter_result::world_full && login::is_blank(full->ticket), "world_full from the World answers world_full without a ticket");
			t.check(f.worlds->online().state(bob) == online_state::offline, "a refused grant leaves the account offline");
			f.world.grant_answer = link::grant_result::shutting_down;
			const auto closing = enter(f, client, "Ann");
			t.check(closing && closing->result == proto::enter_result::world_shutting_down, "shutting_down answers world_shutting_down");
			f.world.grant_answer = link::grant_result::no_such_character;
			const auto unknown = enter(f, client, "Zed");
			t.check(unknown && unknown->result == proto::enter_result::no_such_character, "no_such_character from the World answers no_such_character");
			t.check(f.worlds->online().state(bob) == online_state::offline && !client.closed && f.worlds->pending_count() == 0,
				"after refusals the account is offline, the session open and nothing pending");

			// A silent World: world_unavailable after the request timeout, nothing left reserved.
			f.world.answers = false;
			const auto silent = enter(f, client, "Ann");
			t.check(silent && silent->result == proto::enter_result::world_unavailable, "a silent World answers world_unavailable");
			t.check(f.worlds->online().state(bob) == online_state::offline && f.worlds->pending_count() == 0, "a timed-out grant leaves the account offline");
			f.world.answers = true;
			f.world.release();
			pump_for(f.io, 100ms);
			client.inbox.clear();

			// The success path still works afterwards.
			f.world.grant_answer = link::grant_result::ok;
			const auto fine = enter(f, client, "Ann");
			t.check(fine && fine->result == proto::enter_result::ok, "a grant after the refusals is answered ok");
			client.stop();
		}

		void claim_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture_options options;
			options.second_world = true;
			fixture f(folder, options);
			if (!prepare(f, t, "claim"))
				return;
			world_stub other;
			other.connect(f.io, f.worlds->local_port(), "WS2");
			t.check(pump_until(f.io, [&] { return f.worlds->is_live("WS2") && other.registered; }), "a second World registers");

			peer client;
			t.check(f.open(client), "claim client connected");
			const std::uint64_t bob = login_as(f, client, "Bob");
			const auto granted = enter(f, client, "Ann");
			t.check(granted && granted->result == proto::enter_result::ok, "Bob is granted on WS1");
			f.world.send_started(bob, "Ann");
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::in_world; }), "Bob is in_world on WS1");

			// WS2 claims Bob: refused and kicked; WS1 keeps him.
			other.send_started(bob, "Ann");
			t.check(pump_until(f.io, [&] { return !other.kicks.empty(); }), "the claiming World is sent kick_account");
			t.check(other.kicks.size() == 1 && other.kicks[0].account_id == bob && other.kicks[0].reason == link::kick_reason::forced_login_elsewhere,
				"the kick names the account and forced_login_elsewhere");
			t.check(f.worlds->online().state(bob) == online_state::in_world && f.worlds->online().world_of(bob) == "WS1" && f.world.kicks.empty(),
				"the holding World keeps the account and is not kicked");
			t.check(!other.closed && f.worlds->is_live("WS2"), "the claiming World's link stays up");
			client.stop();
			other.stop();
			pump_for(f.io, 50ms);
		}

		void link_loss_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture f(folder, {});
			if (!prepare(f, t, "link loss"))
				return;

			peer one;
			peer two;
			t.check(f.open(one) && f.open(two), "link loss clients connected");
			const std::uint64_t bob = login_as(f, one, "Bob");
			const std::uint64_t cat = login_as(f, two, "Cat");
			t.check(bob != 0 && cat != 0 && bob != cat, "two accounts logged in");

			// Bob is in the World, Cat holds an outstanding ticket.
			const auto bob_ticket = enter(f, one, "Ann");
			t.check(bob_ticket && bob_ticket->result == proto::enter_result::ok, "Bob is granted");
			f.world.send_started(bob, "Ann");
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::in_world; }), "Bob is in_world");
			const auto cat_ticket = enter(f, two, "Cid");
			t.check(cat_ticket && cat_ticket->result == proto::enter_result::ok, "Cat is granted");
			t.check(f.worlds->online().state(cat) == online_state::handoff_pending, "Cat is handoff_pending");

			// The link drops: both are offline, and a client asking now gets world_unavailable.
			f.world.drop_link();
			t.check(pump_until(f.io, [&] { return !f.worlds->is_live("WS1"); }), "the Login server notices the lost link");
			t.check(f.worlds->online().state(bob) == online_state::offline && f.worlds->online().state(cat) == online_state::offline,
				"link loss sets every account of that World offline");
			t.check(f.worlds->online().count(online_state::in_world) == 0 && f.worlds->online().count(online_state::handoff_pending) == 0,
				"nothing is left online");
			peer late;
			t.check(f.open(late), "a late client connects");
			late.send(login_of("Bob", "secret1", "WS1"));
			proto::login_response down{};
			t.check(receive(f.io, late, down) && down.result == proto::login_result::world_unavailable, "login to the lost World answers world_unavailable");
			late.stop();

			// The World comes back and re-announces Bob: the state is rebuilt.
			f.world.closed = false;
			f.world.registered = false;
			f.world.stop();
			t.check(f.connect_world(), "the World registers again");
			f.world.send_started(bob, "Ann");
			t.check(pump_until(f.io, [&] { return f.worlds->online().state(bob) == online_state::in_world; }), "the re-announced account is in_world again");
			t.check(f.worlds->online().state(cat) == online_state::offline, "the account that was only pending stays offline");
			one.stop();
			two.stop();
		}
	}

	void enter_tests(runner& t, const std::filesystem::path& folder)
	{
		ticket_tests(t);
		registry_tests(t);
		grant_tests(t, folder / "enter_grant");
		pending_tests(t, folder / "enter_pending");
		expiry_tests(t, folder / "enter_expiry");
		refusal_tests(t, folder / "enter_refusal");
		claim_tests(t, folder / "enter_claim");
		link_loss_tests(t, folder / "enter_loss");
	}
}
