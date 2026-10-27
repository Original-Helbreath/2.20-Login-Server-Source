#pragma once

#include "asio_config.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Account online state (plan sect. 5.3), in memory and keyed by account id; never written to a database.
//
//   offline -> handoff_pending   begin_enter reserves the account for one grant
//   handoff_pending -> offline   the ticket expires, the grant is refused or lost (release), the World
//                                link is lost, or a force login revokes it
//   handoff_pending -> in_world  on_started: the World confirmed the ticket
//   in_world -> offline          on_ended, or the World link is lost
//
// An account the registry does not know is offline. on_started for such an account puts it in_world: that is
// how the state is rebuilt after a restart (the World re-announces its players with session_started).
// Thread safe: client sessions, World links and the expiry timers all call it.
namespace hb::server::login
{
	enum class online_state : std::uint8_t
	{
		offline,
		handoff_pending,
		in_world,
	};

	std::string_view to_string(online_state state);

	enum class enter_decision : std::uint8_t
	{
		reserved,         // the account is now handoff_pending; send the grant (generation names it)
		already_playing,  // a normal enter for an account already pending or in a World
		kick_holder,      // a force login: send kick_account to holder_world and tell the client to retry
	};

	struct enter_outcome
	{
		enter_decision decision = enter_decision::already_playing;
		std::uint64_t generation = 0;  // set when reserved
		std::string holder_world;      // set when kick_holder: the World that holds the account
	};

	class online_registry
	{
	public:
		// A reservation not confirmed within ticket_lifetime goes back to offline.
		online_registry(const asio::any_io_executor& executor, std::chrono::milliseconds ticket_lifetime);
		~online_registry();

		online_registry(const online_registry&) = delete;
		online_registry& operator=(const online_registry&) = delete;

		std::chrono::milliseconds ticket_lifetime() const { return m_lifetime; }

		// enter_game_request. Offline: reserve. Pending or in_world: already_playing, or with force the holder
		// is to be kicked (a pending reservation is revoked at once, so the client's retry can be granted; an
		// in_world one stays until the World reports session_ended).
		enter_outcome begin_enter(std::uint64_t account_id, std::string_view world, std::string_view character, bool force);

		// The grant did not succeed: the reservation goes back to offline (no-op if it already changed).
		void release(std::uint64_t account_id, std::uint64_t generation);

		// True while the account is still the handoff_pending one that generation names.
		bool is_pending(std::uint64_t account_id, std::uint64_t generation) const;

		// session_started from a World. Accepted (the account is in_world there) when the account is unknown, or
		// pending or in_world on that same World. A pending grant keeps the character it was made for. Refused when
		// a different World holds the account: the holder stays, and the caller kicks the claiming World.
		// An unknown account is accepted without a grant because the restart rebuild needs it, so this trusts
		// registered Worlds (link key and address already checked) to announce only their own players.
		enum class start_verdict : std::uint8_t
		{
			accepted,
			held_elsewhere,
		};
		start_verdict on_started(std::string_view world, std::uint64_t account_id, std::string_view character);

		// session_ended from a World: offline, when that World is the one holding the account.
		void on_ended(std::string_view world, std::uint64_t account_id);

		// The World's link is lost: every account it holds is offline. Returns how many.
		std::size_t world_lost(std::string_view world);

		online_state state(std::uint64_t account_id) const;
		// The World holding the account; empty when offline.
		std::string world_of(std::uint64_t account_id) const;
		// The character the account is entering or playing; empty when offline.
		std::string character_of(std::uint64_t account_id) const;
		std::size_t count(online_state state) const;

	private:
		struct entry
		{
			online_state state = online_state::offline;
			std::string world;
			std::string character;
			std::uint64_t generation = 0;
			std::shared_ptr<asio::steady_timer> expiry;  // while handoff_pending
		};

		void arm_expiry(std::uint64_t account_id, entry& target);  // under m_mutex
		void expire(std::uint64_t account_id, std::uint64_t generation);
		static void cancel(entry& target);

		asio::any_io_executor m_executor;
		std::chrono::milliseconds m_lifetime;
		mutable std::mutex m_mutex;
		std::map<std::uint64_t, entry> m_entries;
		std::uint64_t m_next_generation = 0;
	};
}
