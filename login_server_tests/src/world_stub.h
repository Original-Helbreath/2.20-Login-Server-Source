#pragma once

#include "client.h"
#include "connection.h"
#include "link_messages.h"
#include "login_messages.h"
#include "message_codec.h"
#include "message_id.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A scripted World server for the Login tests: registers over the real link, keeps a character table,
// applies the rules the real World owns (name taken, slots full, no such character) and answers the list,
// create and delete requests. Switches let a test make it silent, override a result, or answer with the
// wrong message. Everything runs on the test thread.
namespace tests
{
	inline hb::net::link_key stub_key()
	{
		hb::net::link_key key{};
		for (std::size_t i = 0; i < key.size(); ++i)
			key[i] = static_cast<std::uint8_t>(0x40 + i);
		return key;
	}

	struct world_stub
	{
		std::shared_ptr<hb::net::client> role;
		std::shared_ptr<hb::net::connection> link;
		bool connected = false;
		bool closed = false;
		bool registered = false;
		hb::net::close_reason closed_by = hb::net::close_reason::local_close;

		// Script.
		bool answers = true;       // false: requests are held, unanswered, until release()
		bool wrong_kind = false;   // answer a create or delete with a character list instead
		bool forced = false;       // answer every create and delete with forced_result
		hb::net::login::character_result forced_result = hb::net::login::character_result::ok;
		std::uint8_t max_slots = 4;
		std::vector<hb::net::character_summary> characters;

		// What it saw.
		int list_requests = 0;
		int create_requests = 0;
		int delete_requests = 0;
		std::uint64_t last_account_id = 0;
		std::uint32_t last_request_id = 0;
		std::vector<hb::net::frame> held;

		void connect(asio::io_context& io, std::uint16_t port, std::string_view world)
		{
			hb::net::client_options options;
			options.host = "127.0.0.1";
			options.port = port;
			role = hb::net::client::create(io, options);
			const std::string name(world);
			role->start(
				[this, name](const std::shared_ptr<hb::net::connection>& opened) {
					link = opened;
					connected = true;
					opened->start([this](hb::net::connection& from, const hb::net::frame& in) { on_frame(from, in); },
						[this](hb::net::connection&, hb::net::close_reason reason, std::string_view) {
							closed = true;
							closed_by = reason;
						});
					hb::net::link::world_register request{};
					request.world.assign(name);
					request.client_host.assign("127.0.0.1");
					request.client_port = 2848;
					request.key = stub_key();
					request.max_players = 500;
					(void)opened->send(request);
				},
				[](std::string_view) {});
		}

		void stop()
		{
			if (role)
				role->stop();
		}

		// Answers everything held while answers was false, in arrival order.
		void release()
		{
			std::vector<hb::net::frame> frames;
			frames.swap(held);
			for (const hb::net::frame& in : frames)
				answer(in);
		}

		void on_frame(hb::net::connection& from, const hb::net::frame& in)
		{
			namespace link_ns = hb::net::link;
			using hb::net::message_id;
			link_ns::world_register_response registration{};
			link_ns::link_heartbeat beat{};
			if (hb::net::decode(in, registration))
			{
				registered = registration.result == link_ns::register_result::ok;
				return;
			}
			if (hb::net::decode_heartbeat(in, message_id::link_heartbeat_from_login, beat))
			{
				std::vector<std::byte> bytes;
				if (hb::net::encode_heartbeat(message_id::link_heartbeat_from_world, link_ns::link_heartbeat{ beat.sequence }, 65536, bytes))
					from.send_encoded(std::move(bytes));
				return;
			}
			link_ns::character_list_request list{};
			link_ns::create_character_request create{};
			link_ns::delete_character_request remove{};
			if (hb::net::decode(in, list))
			{
				++list_requests;
				last_account_id = list.account_id;
				last_request_id = list.request_id;
			}
			else if (hb::net::decode(in, create))
			{
				++create_requests;
				last_account_id = create.account_id;
				last_request_id = create.request_id;
			}
			else if (hb::net::decode(in, remove))
			{
				++delete_requests;
				last_account_id = remove.account_id;
				last_request_id = remove.request_id;
			}
			else
			{
				return;
			}
			if (answers)
				answer(in);
			else
				held.push_back(in);
		}

		void send_list(std::uint32_t request_id)
		{
			hb::net::link::character_list_response head{};
			head.request_id = request_id;
			head.max_slots = max_slots;
			head.count = static_cast<std::uint8_t>(characters.size());
			std::vector<std::byte> bytes;
			if (hb::net::encode_character_list(head, characters, 65536, bytes))
				link->send_encoded(std::move(bytes));
		}

		void send_change(std::uint32_t request_id, hb::net::login::character_result result)
		{
			if (wrong_kind)
				send_list(request_id);
			else
				(void)link->send(hb::net::link::character_change_response{ request_id, result });
		}

		bool has(std::string_view name) const
		{
			return std::ranges::any_of(characters, [&](const hb::net::character_summary& c) { return c.name.view() == name; });
		}

		void answer(const hb::net::frame& in)
		{
			using hb::net::login::character_result;
			hb::net::link::character_list_request list{};
			hb::net::link::create_character_request create{};
			hb::net::link::delete_character_request remove{};
			if (hb::net::decode(in, list))
			{
				send_list(list.request_id);
			}
			else if (hb::net::decode(in, create))
			{
				character_result result = character_result::ok;
				if (forced)
					result = forced_result;
				else if (has(create.name.view()))
					result = character_result::name_taken;
				else if (characters.size() >= max_slots)
					result = character_result::slots_full;
				else
					characters.push_back(make_character(create));
				send_change(create.request_id, result);
			}
			else if (hb::net::decode(in, remove))
			{
				character_result result = character_result::ok;
				if (forced)
				{
					result = forced_result;
				}
				else
				{
					const auto found = std::ranges::find_if(characters,
						[&](const hb::net::character_summary& c) { return c.name.view() == remove.name.view(); });
					if (found == characters.end())
						result = character_result::no_such_character;
					else
						characters.erase(found);
				}
				send_change(remove.request_id, result);
			}
		}

		static hb::net::character_summary make_character(const hb::net::link::create_character_request& create)
		{
			hb::net::character_summary summary{};
			summary.name = create.name;
			summary.is_valid = 1;
			summary.sex = create.sex;
			summary.skin = create.skin;
			summary.level = 1;
			summary.stats = create.stats;
			summary.appearance.hair_style = create.hair_style;
			summary.appearance.hair_colour = create.hair_colour;
			summary.appearance.underwear_colour = create.underwear_colour;
			summary.map_name.assign("default");
			return summary;
		}
	};
}
