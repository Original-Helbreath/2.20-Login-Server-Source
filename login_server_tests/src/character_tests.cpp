// Character brokering tests (LOGIN-007): a real client_listener, authenticator, world_link_service and players.db
// over loopback, with a scripted World (world_stub.h) on the other end of the link. Covers the world check at
// login (no_such_world, world_unavailable), the character list after login, create (ok, name taken, slots full,
// format checks that never reach the World), delete, a World that is down, lost or silent giving
// world_unavailable, late and wrong answers from the World, and that no request is left pending.
#include "net_fixture.h"
#include "test_runner.h"
#include "character_rules.h"

namespace tests
{
	namespace
	{
		using namespace support;

		void rule_tests(runner& t)
		{
			t.check(login::is_valid_character_name("Zed") && login::is_valid_character_name("Abcdefghij") && login::is_valid_character_name("a1"),
				"character names of 1..10 letters and digits are valid");
			t.check(!login::is_valid_character_name("") && !login::is_valid_character_name("Abcdefghijk") && !login::is_valid_character_name("a b")
					&& !login::is_valid_character_name("a_b") && !login::is_valid_character_name("a-b"),
				"empty, long and punctuated character names are refused");
			t.check(login::is_valid_character_stats({ 10, 10, 10, 10, 10, 20 }) == false, "a stat of 20 is refused even when the total is 70");
			t.check(login::is_valid_character_stats({ 14, 14, 14, 14, 14, 0 }) == false, "a stat of 0 is refused");
			t.check(login::is_valid_character_stats({ 12, 12, 12, 12, 11, 11 }), "stats 10..14 summing to 70 are valid");
			t.check(!login::is_valid_character_stats({ 12, 12, 12, 12, 11, 12 }), "stats summing to 71 are refused");
			t.check(!login::is_valid_character_stats({ 9, 14, 14, 11, 11, 11 }), "a stat of 9 is refused");
			t.check(login::check_create_format("Zed", 2, 3, { 12, 12, 12, 12, 11, 11 }) == proto::character_result::ok, "sex 2, skin 3 pass");
			t.check(login::check_create_format("Zed", 0, 1, { 12, 12, 12, 12, 11, 11 }) == proto::character_result::failed, "sex 0 fails");
			t.check(login::check_create_format("Zed", 1, 4, { 12, 12, 12, 12, 11, 11 }) == proto::character_result::failed, "skin 4 fails");
		}

		void flow_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture f(folder, {});
			t.check(f.ready, "character fixture ready");
			if (!f.ready)
				return;
			t.check(f.accounts->create("Bob", "secret1") == login::create_result::created, "character: account Bob created");
			f.world.characters = { summary_of("Ann"), summary_of("Cid") };

			// login_request.world must be a configured world.
			peer client;
			t.check(f.open(client), "character client connected");
			t.check(login_to(f, client, "NOPE") == proto::login_result::no_such_world, "an unknown world answers no_such_world");
			t.check(login_to(f, client, "") == proto::login_result::no_such_world, "an empty world name answers no_such_world");
			t.check(login_to(f, client, "ws1") == proto::login_result::no_such_world, "world names are matched exactly");
			pump_for(f.io, 50ms);
			t.check(!client.closed && f.world.list_requests == 0 && f.auth->authenticated_count() == 0,
				"no_such_world leaves the session open, asks the World nothing and logs nobody in");

			// login ok comes with the World's character list.
			t.check(login_to(f, client, "WS1") == proto::login_result::ok, "login to a live world answers ok");
			proto::character_list head{};
			std::vector<hb::net::character_summary> list;
			t.check(receive_list(f.io, client, head, list), "login ok is followed by a character_list");
			t.check(head.max_slots == 4 && head.count == 2 && names_are(list, { "Ann", "Cid" }), "the list is the World's: 2 of 4 slots, Ann and Cid");
			t.check(list.size() == 2 && list[0].level == 5 && list[0].map_name.view() == "default", "the list carries the summary fields");
			t.check(f.world.list_requests == 1 && f.world.last_account_id != 0, "the World was asked once, for the account's id");
			const std::uint64_t account_id = f.world.last_account_id;
			t.check(f.auth->authenticated_count() == 1 && f.worlds->pending_count() == 0, "logged in with nothing pending");

			// create: ok, then the fresh list.
			client.send(create_of("Zed"));
			proto::create_character_response created{};
			t.check(receive(f.io, client, created) && created.result == proto::character_result::ok, "create answers ok");
			t.check(receive_list(f.io, client, head, list) && head.count == 3 && names_are(list, { "Ann", "Cid", "Zed" }),
				"create ok is followed by the fresh list with Zed");
			t.check(f.world.create_requests == 1 && f.world.last_account_id == account_id && f.world.list_requests == 2,
				"the World saw one create for the same account, then a list request");
			const std::optional<hb::net::character_summary> zed = list.size() == 3 ? std::optional(list[2]) : std::nullopt;
			t.check(zed && zed->sex == 1 && zed->skin == 1 && zed->appearance.hair_style == 2 && zed->appearance.hair_colour == 3
					&& zed->appearance.underwear_colour == 4 && zed->stats.magic == 11,
				"the create request reached the World with its fields intact");

			// create: name taken, no list follows.
			client.send(create_of("Zed"));
			t.check(receive(f.io, client, created) && created.result == proto::character_result::name_taken, "a taken name answers name_taken");
			t.check(stays_quiet(f, client) && f.world.list_requests == 2, "name_taken is followed by no list");

			// create: slots full.
			client.send(create_of("Amy"));
			t.check(receive(f.io, client, created) && created.result == proto::character_result::ok && receive_list(f.io, client, head, list)
					&& head.count == 4,
				"the fourth character is created");
			client.send(create_of("Bea"));
			t.check(receive(f.io, client, created) && created.result == proto::character_result::slots_full, "a fifth character answers slots_full");
			t.check(stays_quiet(f, client) && !f.world.has("Bea"), "slots_full is followed by no list and creates nothing");

			// Format checks stop a request before it reaches the World.
			const int creates = f.world.create_requests;
			client.send(create_of("bad name!"));
			t.check(receive(f.io, client, created) && created.result == proto::character_result::invalid_name, "a bad name answers invalid_name");
			client.send(create_of(""));
			t.check(receive(f.io, client, created) && created.result == proto::character_result::invalid_name, "an empty name answers invalid_name");
			client.send(create_of("Abcdefghijk"));
			t.check(receive(f.io, client, created) && created.result == proto::character_result::invalid_name, "an 11-character name answers invalid_name");
			proto::create_character_request bad_stats = create_of("Dan");
			bad_stats.stats = { 12, 12, 12, 12, 11, 12 };
			client.send(bad_stats);
			t.check(receive(f.io, client, created) && created.result == proto::character_result::invalid_stats, "stats summing to 71 answer invalid_stats");
			bad_stats.stats = { 20, 10, 10, 10, 10, 10 };
			client.send(bad_stats);
			t.check(receive(f.io, client, created) && created.result == proto::character_result::invalid_stats, "a stat of 20 answers invalid_stats");
			proto::create_character_request bad_skin = create_of("Dan");
			bad_skin.skin = 9;
			client.send(bad_skin);
			t.check(receive(f.io, client, created) && created.result == proto::character_result::failed, "a skin of 9 answers failed");
			t.check(f.world.create_requests == creates && stays_quiet(f, client), "refused formats never reached the World and sent no list");

			// delete: ok then the fresh list; unknown; bad name; pass-through of the World's own refusal.
			client.send(delete_of("Zed"));
			proto::delete_character_response deleted{};
			t.check(receive(f.io, client, deleted) && deleted.result == proto::character_result::ok, "delete answers ok");
			t.check(receive_list(f.io, client, head, list) && head.count == 3 && names_are(list, { "Ann", "Cid", "Amy" }),
				"delete ok is followed by the fresh list without Zed");
			client.send(delete_of("Zed"));
			t.check(receive(f.io, client, deleted) && deleted.result == proto::character_result::no_such_character, "deleting a missing character answers no_such_character");
			t.check(stays_quiet(f, client), "no_such_character is followed by no list");
			const int deletes = f.world.delete_requests;
			client.send(delete_of("bad name!"));
			t.check(receive(f.io, client, deleted) && deleted.result == proto::character_result::invalid_name, "a bad name on delete answers invalid_name");
			t.check(f.world.delete_requests == deletes, "a bad delete name never reached the World");
			f.world.forced = true;
			f.world.forced_result = proto::character_result::level_too_high;
			client.send(delete_of("Ann"));
			t.check(receive(f.io, client, deleted) && deleted.result == proto::character_result::level_too_high, "the World's level_too_high reaches the client");
			f.world.forced = false;
			t.check(f.worlds->pending_count() == 0, "nothing is left pending after the flows");

			// An answer to a request that is not pending is dropped; the link and the session carry on.
			(void)f.world.link->send(hb::net::link::character_change_response{ 424242, proto::character_result::ok });
			pump_for(f.io, 100ms);
			t.check(!f.world.closed && f.worlds->is_live("WS1"), "an answer to an unknown request id is dropped, the link stays up");
			client.send(delete_of("Cid"));
			t.check(receive(f.io, client, deleted) && deleted.result == proto::character_result::ok && receive_list(f.io, client, head, list),
				"requests still work after a stray answer");

			client.stop();

			// The wrong kind of answer is a protocol error: the link is closed and the client told world_unavailable.
			peer other;
			t.check(f.open(other) && login_to(f, other, "WS1") == proto::login_result::ok && receive_list(f.io, other, head, list),
				"second client logs in");
			f.world.wrong_kind = true;
			other.send(create_of("Eli"));
			t.check(receive(f.io, other, created) && created.result == proto::character_result::world_unavailable,
				"a list answering a create ends the request as world_unavailable");
			t.check(pump_until(f.io, [&] { return f.world.closed; }), "the Login server closes a link that answers with the wrong message");
			t.check(!f.worlds->is_live("WS1") && f.worlds->pending_count() == 0, "the world is lost and nothing is pending");
			other.stop();
		}

		void down_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture_options options;
			options.register_world = false;
			fixture f(folder, options);
			t.check(f.ready, "down fixture ready");
			if (!f.ready)
				return;
			(void)f.accounts->create("Bob", "secret1");

			// The World has never registered.
			peer early;
			t.check(f.open(early), "down client connected");
			t.check(login_to(f, early, "WS1") == proto::login_result::world_unavailable, "a configured world that never registered answers world_unavailable");
			t.check(login_to(f, early, "NOPE") == proto::login_result::no_such_world, "an unknown world is still no_such_world");
			t.check(f.auth->authenticated_count() == 0 && !early.closed, "world_unavailable logs nobody in and leaves the session open");

			// It registers; the same session can now log in.
			t.check(f.connect_world(), "the World registers");
			proto::character_list head{};
			std::vector<hb::net::character_summary> list;
			t.check(login_to(f, early, "WS1") == proto::login_result::ok && receive_list(f.io, early, head, list), "login works once the World is live");

			// The link is lost: a logged-in client's request, and a new login, both answer world_unavailable.
			f.world.stop();
			t.check(pump_until(f.io, [&] { return !f.worlds->is_live("WS1"); }), "the lost link is noticed");
			early.send(create_of("Zed"));
			proto::create_character_response created{};
			t.check(receive(f.io, early, created) && created.result == proto::character_result::world_unavailable, "create with the World down answers world_unavailable");
			early.send(delete_of("Zed"));
			proto::delete_character_response deleted{};
			t.check(receive(f.io, early, deleted) && deleted.result == proto::character_result::world_unavailable, "delete with the World down answers world_unavailable");
			t.check(!early.closed && f.worlds->pending_count() == 0, "the session stays open and nothing is pending");
			peer later;
			t.check(f.open(later) && login_to(f, later, "WS1") == proto::login_result::world_unavailable, "a new login with the World down answers world_unavailable");
			early.stop();
			later.stop();
		}

		void timeout_tests(runner& t, const std::filesystem::path& folder)
		{
			fixture_options options;
			options.request_timeout = 300ms;
			fixture f(folder, options);
			t.check(f.ready, "timeout fixture ready");
			if (!f.ready)
				return;
			(void)f.accounts->create("Bob", "secret1");

			// The World stays silent on the list request at login.
			f.world.answers = false;
			peer client;
			t.check(f.open(client), "timeout client connected");
			const auto started = std::chrono::steady_clock::now();
			t.check(login_to(f, client, "WS1") == proto::login_result::world_unavailable, "a silent World gives world_unavailable at login");
			const auto waited = std::chrono::steady_clock::now() - started;
			t.check(waited >= 250ms && waited < 3000ms, "the login waited about the 300 ms request timeout");
			t.check(f.auth->authenticated_count() == 0 && f.worlds->pending_count() == 0 && !client.closed,
				"the timed-out login logged nobody in, left nothing pending and kept the session");

			// Its late answer is dropped; the link and the session are fine.
			f.world.answers = true;
			f.world.release();
			t.check(stays_quiet(f, client) && !f.world.closed && f.worlds->is_live("WS1"), "a late answer is dropped without harming the link or the client");
			proto::character_list head{};
			std::vector<hb::net::character_summary> list;
			t.check(login_to(f, client, "WS1") == proto::login_result::ok && receive_list(f.io, client, head, list) && head.count == 0,
				"the session logs in once the World answers again");

			// A create the World does not answer in time.
			f.world.answers = false;
			client.send(create_of("Zed"));
			proto::create_character_response created{};
			const auto create_started = std::chrono::steady_clock::now();
			t.check(receive(f.io, client, created) && created.result == proto::character_result::world_unavailable, "a silent World gives world_unavailable on create");
			t.check(std::chrono::steady_clock::now() - create_started >= 250ms, "create waited about the request timeout");
			t.check(f.world.create_requests == 1 && f.worlds->pending_count() == 0 && !client.closed, "the create reached the World, nothing is pending, the session is open");

			// The World acts on it late; the late answer is dropped, later requests work.
			f.world.answers = true;
			f.world.release();
			t.check(stays_quiet(f, client) && f.world.has("Zed"), "the late create is applied by the World and its answer dropped");
			client.send(create_of("Yan"));
			t.check(receive(f.io, client, created) && created.result == proto::character_result::ok && receive_list(f.io, client, head, list)
					&& names_are(list, { "Zed", "Yan" }),
				"the next create after a timeout works");
			client.stop();
		}
	}

	void character_tests(runner& t, const std::filesystem::path& folder)
	{
		rule_tests(t);
		flow_tests(t, folder / "character_flow");
		down_tests(t, folder / "character_down");
		timeout_tests(t, folder / "character_timeout");
	}
}
