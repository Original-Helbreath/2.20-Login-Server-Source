// Message struct tests (LOGIN-004): every packet of plan sect. 4.4 and 4.5 round-trips through the frame
// codec; out-of-range enums, non-0/1 flags, non-zero-padded text and wrong ids are refused.
#include "common_types.h"
#include "frame_codec.h"
#include "link_messages.h"
#include "login_messages.h"
#include "message_codec.h"
#include "message_id.h"
#include "test_runner.h"
#include "world_messages.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <vector>

namespace tests
{
	namespace
	{
		using namespace hb::net;

		constexpr std::uint32_t test_max_body = 4096;

		template <wire_type T>
		bool same_bytes(const T& a, const T& b)
		{
			return std::ranges::equal(std::as_bytes(std::span<const T, 1>(&a, 1)), std::as_bytes(std::span<const T, 1>(&b, 1)));
		}

		// Encode under T::id, decode the frame, decode the body; true when every byte comes back.
		template <identified_message T>
		bool round_trips(const T& message)
		{
			std::vector<std::byte> bytes;
			if (!encode(message, test_max_body, bytes))
				return false;
			frame in;
			if (decode_frame(bytes, test_max_body, in) != frame_error::none)
				return false;
			T out{};
			return in.header.body_size == sizeof(T) && decode(in, out) && same_bytes(message, out);
		}

		// Encodes as-is (the sender's check is not the receiver's), then reports whether decode accepts it.
		template <identified_message T>
		bool accepted(const T& message)
		{
			std::vector<std::byte> bytes;
			frame in;
			T out{};
			return encode(message, test_max_body, bytes) && decode_frame(bytes, test_max_body, in) == frame_error::none
				&& decode(in, out);
		}

		template <std::size_t N>
		fixed_string<N> text(std::string_view value)
		{
			fixed_string<N> out;
			out.assign(value);
			return out;
		}

		template <std::size_t N>
		fixed_string<N> hidden_text(std::string_view value)
		{
			fixed_string<N> out = text<N>(value);
			out.chars[N - 1] = 'x';  // a byte behind the NUL
			return out;
		}

		std::array<std::uint8_t, 32> pattern(std::uint8_t seed)
		{
			std::array<std::uint8_t, 32> bytes{};
			for (std::size_t i = 0; i < bytes.size(); ++i)
				bytes[i] = static_cast<std::uint8_t>(seed + i * 7);
			return bytes;
		}

		character_summary summary(std::string_view name)
		{
			character_summary s{};
			s.name.assign(name);
			s.is_valid = 1;
			s.sex = 1;
			s.skin = 2;
			s.level = 99;
			s.experience = 123456;
			s.stats = { 10, 20, 30, 40, 50, 60 };
			s.appearance.weapon = 200;
			s.appearance.hides_body_armour = 1;
			s.map_name.assign("elvine");
			s.last_saved = { 2026, 10, 26, 12, 30 };
			return s;
		}

		void id_tests(runner& t)
		{
			t.check(direction_of(0x0101) == message_direction::client_to_login, "0x0101 is client -> Login");
			t.check(direction_of(0x0207) == message_direction::login_to_client, "0x0207 is Login -> client");
			t.check(direction_of(0x0308) == message_direction::world_to_login, "0x0308 is World -> Login");
			t.check(direction_of(0x0408) == message_direction::login_to_world, "0x0408 is Login -> World");
			t.check(direction_of(0x0501) == message_direction::client_to_world, "0x0501 is client -> World");
			t.check(direction_of(0x0502) == message_direction::world_to_client, "0x0502 is World -> client");
			t.check(is_known(0x0501) && is_known(0x0502), "the handoff ids are known");
			t.check(!is_known(0x0000) && !is_known(0x0107) && !is_known(0x0407) && !is_known(0x0500) && !is_known(0x0505) && !is_known(0xFFFF),
				"undefined ids are unknown");
			t.check(to_wire(world::world_enter_request::id) == 0x0501 && to_wire(world::world_enter_response::id) == 0x0502,
				"the handoff structs carry their plan ids");
			t.check(to_wire(login::enter_game_response::id) == 0x0207 && to_wire(link::kick_account::id) == 0x0406,
				"structs carry their plan ids");
		}

		void login_round_trip_tests(runner& t)
		{
			using namespace hb::net::login;

			login_request request{ text<16>("shadow"), text<32>("hunter2"), text<32>("Abaddon") };
			t.check(round_trips(request), "login_request round-trips");
			login_response response{ login_result::account_blocked, 1, { 2027, 1, 2 }, { 2026, 12, 31 } };
			t.check(round_trips(response), "login_response round-trips");
			t.check(round_trips(create_account_request{ text<16>("new"), text<32>("pw"), text<64>("a@b.c") }),
				"create_account_request round-trips");
			t.check(round_trips(create_account_response{ account_result::disabled }), "create_account_response round-trips");
			t.check(round_trips(change_password_request{ text<16>("shadow"), text<32>("old"), text<32>("new") }),
				"change_password_request round-trips");
			t.check(round_trips(change_password_response{ account_result::name_taken }), "change_password_response round-trips");
			t.check(round_trips(create_character_request{ text<16>("Hero"), 2, 3, 4, 5, 6, { 10, 11, 12, 13, 14, 15 } }),
				"create_character_request round-trips");
			t.check(round_trips(create_character_response{ character_result::world_unavailable }),
				"create_character_response round-trips");
			t.check(round_trips(delete_character_request{ text<16>("Hero") }), "delete_character_request round-trips");
			t.check(round_trips(delete_character_response{ character_result::no_such_character }),
				"delete_character_response round-trips");
			t.check(round_trips(enter_game_request{ text<16>("Hero"), enter_kind::force_login }), "enter_game_request round-trips");
			t.check(round_trips(enter_game_response{ enter_result::ok, text<46>("2001:db8::1"), 2848, pattern(3) }),
				"enter_game_response round-trips");
		}

		void link_round_trip_tests(runner& t)
		{
			using namespace hb::net::link;

			t.check(round_trips(world_register{ text<32>("Abaddon"), text<46>("127.0.0.1"), 2848, pattern(9), 1000 }),
				"world_register round-trips");
			t.check(round_trips(world_register_response{ register_result::version_mismatch }), "world_register_response round-trips");
			t.check(round_trips(world_status{ 1, 0, 321, 1000 }), "world_status round-trips");
			t.check(round_trips(character_list_request{ 7, 0x0102030405060708ull }), "character_list_request round-trips");
			t.check(round_trips(create_character_request{ 8, 42, text<16>("Hero"), 1, 1, 2, 3, 4, { 1, 2, 3, 4, 5, 6 } }),
				"link create_character_request round-trips");
			t.check(round_trips(delete_character_request{ 9, 42, text<16>("Hero") }), "link delete_character_request round-trips");
			t.check(round_trips(character_change_response{ 9, login::character_result::slots_full }),
				"character_change_response round-trips");
			t.check(round_trips(session_grant{ 10, 42, text<16>("Hero"), pattern(1), 30 }), "session_grant round-trips");
			t.check(round_trips(session_grant_response{ 10, grant_result::shutting_down }), "session_grant_response round-trips");
			t.check(round_trips(session_started{ 42, text<16>("Hero") }), "session_started round-trips");
			t.check(round_trips(session_ended{ 42, session_end_reason::world_shutdown }), "session_ended round-trips");
			t.check(round_trips(kick_account{ 42, kick_reason::admin }), "kick_account round-trips");

			const link_heartbeat beat{ 0xA1B2C3D4u };
			std::vector<std::byte> bytes;
			frame in;
			link_heartbeat out{};
			t.check(encode_heartbeat(message_id::link_heartbeat_from_world, beat, test_max_body, bytes)
					&& decode_frame(bytes, test_max_body, in) == frame_error::none
					&& decode_heartbeat(in, message_id::link_heartbeat_from_world, out) && out.sequence == beat.sequence,
				"heartbeat round-trips under 0x0308");
			t.check(!decode_heartbeat(in, message_id::link_heartbeat_from_login, out), "heartbeat under the other id refused");
			bytes.clear();
			t.check(!encode_heartbeat(message_id::login_request, beat, test_max_body, bytes) && bytes.empty(),
				"heartbeat refuses a non-heartbeat id");
		}

		void world_round_trip_tests(runner& t)
		{
			using namespace hb::net::world;

			t.check(round_trips(world_enter_request{ pattern(5), text<16>("Hero") }), "world_enter_request round-trips");
			t.check(round_trips(world_enter_response{ world_enter_result::shutting_down }), "world_enter_response round-trips");
			for (std::uint8_t r = 0; r <= 4; ++r)
				t.check(accepted(world_enter_response{ static_cast<world_enter_result>(r) }), std::format("world_enter_result {} accepted", r));
			t.check(!accepted(world_enter_response{ static_cast<world_enter_result>(5) }), "world_enter_result 5 refused");
			t.check(!accepted(world_enter_request{ pattern(5), hidden_text<16>("Hero") }), "world_enter_request with a non-zero-padded character refused");

			// Wrong size and wrong id.
			std::vector<std::byte> bytes;
			frame in;
			world_enter_request wrong_size{};
			t.check(encode(world_enter_response{ world_enter_result::ok }, test_max_body, bytes)
					&& decode_frame(bytes, test_max_body, in) == frame_error::none && !decode(in, wrong_size),
				"a one-byte body is not a world_enter_request");
			world_enter_response wrong_id{};
			bytes.clear();
			t.check(encode(world_enter_request{ pattern(5), text<16>("Hero") }, test_max_body, bytes) && decode_frame(bytes, test_max_body, in) == frame_error::none
					&& !decode(in, wrong_id),
				"a world_enter_request body is not a world_enter_response");
		}

		void rejection_tests(runner& t)
		{
			using namespace hb::net::login;

			// Out-of-range enums, one per enum type.
			t.check(!accepted(login_response{ static_cast<login_result>(9), 0, {}, {} }), "login_result 9 refused");
			t.check(accepted(login_response{ login_result::service_unavailable, 0, {}, {} }), "last login_result accepted");
			t.check(!accepted(create_account_response{ static_cast<account_result>(5) }), "account_result 5 refused");
			t.check(!accepted(create_character_response{ static_cast<character_result>(9) }), "character_result 9 refused");
			t.check(!accepted(enter_game_request{ text<16>("Hero"), static_cast<enter_kind>(2) }), "enter_kind 2 refused");
			t.check(!accepted(enter_game_response{ static_cast<enter_result>(7), {}, 0, {} }), "enter_result 7 refused");
			t.check(!accepted(link::world_register_response{ static_cast<link::register_result>(5) }), "register_result 5 refused");
			t.check(!accepted(link::session_grant_response{ 1, static_cast<link::grant_result>(4) }), "grant_result 4 refused");
			t.check(!accepted(link::session_ended{ 1, static_cast<link::session_end_reason>(4) }), "session_end_reason 4 refused");
			t.check(!accepted(link::kick_account{ 1, static_cast<link::kick_reason>(3) }), "kick_reason 3 refused");
			t.check(!accepted(link::character_change_response{ 1, static_cast<character_result>(0xFF) }),
				"character_result 255 refused on the link");

			// Flags other than 0 or 1.
			t.check(!accepted(login_response{ login_result::ok, 2, {}, {} }), "has_account_expiry 2 refused");
			t.check(!accepted(link::world_status{ 1, 0xFF, 0, 0 }), "is_shutting_down 255 refused");

			// Text with bytes hidden behind the NUL.
			t.check(!accepted(login_request{ hidden_text<16>("shadow"), text<32>("pw"), text<32>("w") }),
				"login_request with a non-zero-padded account refused");
			t.check(!accepted(login_request{ text<16>("shadow"), hidden_text<32>("pw"), text<32>("w") }),
				"login_request with a non-zero-padded password refused");
			t.check(!accepted(create_account_request{ text<16>("a"), text<32>("b"), hidden_text<64>("c") }),
				"create_account_request with a non-zero-padded email refused");
			t.check(!accepted(enter_game_response{ enter_result::ok, hidden_text<46>("10.0.0.1"), 1, {} }),
				"enter_game_response with a non-zero-padded host refused");
			t.check(!accepted(link::world_register{ text<32>("w"), hidden_text<46>("h"), 1, {}, 1 }),
				"world_register with a non-zero-padded host refused");
			t.check(!accepted(link::session_grant{ 1, 1, hidden_text<16>("Hero"), {}, 30 }),
				"session_grant with a non-zero-padded character refused");

			// Wrong id and wrong size.
			std::vector<std::byte> bytes;
			frame in;
			const delete_character_request request{ text<16>("Hero") };
			t.check(encode(request, test_max_body, bytes) && decode_frame(bytes, test_max_body, in) == frame_error::none,
				"delete request encodes");
			link::session_started wrong{};
			t.check(!decode(in, wrong), "a body under another struct's id is refused");
			in.body.push_back(std::byte{ 0 });
			in.header.body_size += 1;
			delete_character_request longer{};
			t.check(!decode(in, longer), "a body one byte long is refused");
			in.body.resize(sizeof(delete_character_request) - 1);
			t.check(!decode(in, longer), "a body one byte short is refused");
		}

		template <character_list_head Head>
		bool list_accepted(const Head& head, std::span<const character_summary> summaries)
		{
			span_writer body;
			body.write(head);
			body.write_array(summaries);
			std::vector<std::byte> bytes;
			frame in;
			Head out_head{};
			std::vector<character_summary> out;
			return encode_frame(to_wire(Head::id), body.take(), test_max_body, bytes)
				&& decode_frame(bytes, test_max_body, in) == frame_error::none && decode_character_list(in, out_head, out);
		}

		void character_list_tests(runner& t)
		{
			const std::array<character_summary, 2> two{ summary("Alpha"), summary("Beta") };

			std::vector<std::byte> bytes;
			frame in;
			login::character_list head{};
			std::vector<character_summary> out;
			t.check(encode_character_list(login::character_list{ 4, 2 }, std::span<const character_summary>(two), test_max_body, bytes)
					&& decode_frame(bytes, test_max_body, in) == frame_error::none && decode_character_list(in, head, out),
				"character_list with two summaries decodes");
			t.check(in.header.body_size == 2 + 2 * sizeof(character_summary), "character_list body is head + 2 x 73 bytes");
			t.check(head.max_slots == 4 && head.count == 2 && out.size() == 2 && same_bytes(out[0], two[0])
					&& same_bytes(out[1], two[1]),
				"character_list round-trips every byte");

			bytes.clear();
			link::character_list_response link_head{};
			t.check(encode_character_list(link::character_list_response{ 77, 4, 2 }, std::span<const character_summary>(two),
						test_max_body, bytes)
					&& decode_frame(bytes, test_max_body, in) == frame_error::none && decode_character_list(in, link_head, out)
					&& link_head.request_id == 77 && out.size() == 2,
				"link character_list_response round-trips");

			bytes.clear();
			t.check(encode_character_list(login::character_list{ 4, 0 }, {}, test_max_body, bytes)
					&& decode_frame(bytes, test_max_body, in) == frame_error::none && decode_character_list(in, head, out)
					&& out.empty(),
				"empty character_list decodes");

			bytes.clear();
			t.check(!encode_character_list(login::character_list{ 4, 3 }, std::span<const character_summary>(two), test_max_body, bytes)
					&& bytes.empty(),
				"encoder refuses a count that differs from the summaries");
			t.check(!encode_character_list(login::character_list{ 1, 2 }, std::span<const character_summary>(two), test_max_body, bytes),
				"encoder refuses count above max_slots");

			t.check(!list_accepted(login::character_list{ 1, 2 }, std::span<const character_summary>(two)),
				"count above max_slots refused");
			t.check(!list_accepted(login::character_list{ 4, 3 }, std::span<const character_summary>(two)),
				"count above the summaries sent refused");
			t.check(!list_accepted(login::character_list{ 4, 1 }, std::span<const character_summary>(two)),
				"summaries beyond count refused");
			std::array<character_summary, 2> bad = two;
			bad[1].is_valid = 2;
			t.check(!list_accepted(login::character_list{ 4, 2 }, std::span<const character_summary>(bad)),
				"summary with is_valid 2 refused");
			bad = two;
			bad[0].map_name.chars[15] = 'x';
			t.check(!list_accepted(login::character_list{ 4, 2 }, std::span<const character_summary>(bad)),
				"summary with a non-zero-padded map name refused");
			bad = two;
			bad[0].appearance.hides_body_armour = 3;
			t.check(!list_accepted(login::character_list{ 4, 2 }, std::span<const character_summary>(bad)),
				"summary with hides_body_armour 3 refused");

			// The encoder refuses what the decoder would refuse (review item 2).
			bytes.clear();
			bad = two;
			bad[1].is_valid = 2;
			t.check(!encode_character_list(login::character_list{ 4, 2 }, std::span<const character_summary>(bad), test_max_body, bytes)
					&& bytes.empty(),
				"encoder refuses a summary with is_valid 2");
			bad = two;
			bad[0].name.chars[15] = 'x';
			t.check(!encode_character_list(login::character_list{ 4, 2 }, std::span<const character_summary>(bad), test_max_body, bytes)
					&& bytes.empty(),
				"encoder refuses a summary with a non-zero-padded name");

			// The list under the wrong id; a refused decode leaves head reset and summaries empty (review item 3).
			bytes.clear();
			link_head = link::character_list_response{ 55, 9, 9 };
			out.assign(3, summary("Stale"));
			t.check(encode_character_list(login::character_list{ 4, 2 }, std::span<const character_summary>(two), test_max_body, bytes)
					&& decode_frame(bytes, test_max_body, in) == frame_error::none && !decode_character_list(in, link_head, out)
					&& out.empty(),
				"a client list read as a link list is refused");
			t.check(link_head.request_id == 0 && link_head.max_slots == 0 && link_head.count == 0,
				"refused decode (wrong id) resets the head");

			// A head that reads but fails is_well_formed (count above max_slots) must not survive either.
			span_writer raw;
			raw.write(login::character_list{ 1, 2 });
			raw.write_array(std::span<const character_summary>(two));
			bytes.clear();
			head = login::character_list{ 7, 7 };
			out.assign(1, summary("Stale"));
			t.check(encode_frame(to_wire(login::character_list::id), raw.take(), test_max_body, bytes)
					&& decode_frame(bytes, test_max_body, in) == frame_error::none && !decode_character_list(in, head, out)
					&& out.empty() && head.max_slots == 0 && head.count == 0,
				"refused decode (count above max_slots) resets the head and clears summaries");
		}
	}

	void message_tests(runner& t)
	{
		id_tests(t);
		login_round_trip_tests(t);
		link_round_trip_tests(t);
		world_round_trip_tests(t);
		rejection_tests(t);
		character_list_tests(t);
	}
}
