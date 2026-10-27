#include "online_accounts.h"

#include "log.h"

#include <utility>
#include <vector>

namespace hb::server::login
{
	std::string_view to_string(online_state state)
	{
		switch (state)
		{
		case online_state::offline:
			return "offline";
		case online_state::handoff_pending:
			return "handoff pending";
		case online_state::in_world:
			return "in world";
		}
		return "unknown";
	}

	online_registry::online_registry(const asio::any_io_executor& executor, std::chrono::milliseconds ticket_lifetime)
		: m_executor(executor)
		, m_lifetime(ticket_lifetime)
	{
	}

	online_registry::~online_registry()
	{
		std::scoped_lock lock(m_mutex);
		for (auto& item : m_entries)
			cancel(item.second);
	}

	void online_registry::cancel(entry& target)
	{
		if (target.expiry)
			target.expiry->cancel();
		target.expiry.reset();
	}

	void online_registry::arm_expiry(std::uint64_t account_id, entry& target)
	{
		target.expiry = std::make_shared<asio::steady_timer>(m_executor);
		target.expiry->expires_after(m_lifetime);
		// A cancelled timer reports an error and returns before touching the registry.
		target.expiry->async_wait([this, account_id, generation = target.generation](const asio::error_code& error) {
			if (!error)
				expire(account_id, generation);
		});
	}

	void online_registry::expire(std::uint64_t account_id, std::uint64_t generation)
	{
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_entries.find(account_id);
			if (found == m_entries.end() || found->second.generation != generation || found->second.state != online_state::handoff_pending)
				return;  // confirmed, released or replaced first
			m_entries.erase(found);
		}
		log::info("account {}: the ticket expired, offline", account_id);
	}

	enter_outcome online_registry::begin_enter(std::uint64_t account_id, std::string_view world, std::string_view character, bool force)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_entries.find(account_id);
		if (found != m_entries.end())
		{
			if (!force)
				return { enter_decision::already_playing, 0, {} };
			enter_outcome outcome{ enter_decision::kick_holder, 0, found->second.world };
			if (found->second.state == online_state::handoff_pending)
			{
				cancel(found->second);
				m_entries.erase(found);
			}
			return outcome;
		}
		entry& reserved = m_entries[account_id];
		reserved.state = online_state::handoff_pending;
		reserved.world = std::string(world);
		reserved.character = std::string(character);
		reserved.generation = ++m_next_generation;
		arm_expiry(account_id, reserved);
		return { enter_decision::reserved, reserved.generation, {} };
	}

	void online_registry::release(std::uint64_t account_id, std::uint64_t generation)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_entries.find(account_id);
		if (found == m_entries.end() || found->second.generation != generation || found->second.state != online_state::handoff_pending)
			return;
		cancel(found->second);
		m_entries.erase(found);
	}

	bool online_registry::is_pending(std::uint64_t account_id, std::uint64_t generation) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_entries.find(account_id);
		return found != m_entries.end() && found->second.generation == generation && found->second.state == online_state::handoff_pending;
	}

	online_registry::start_verdict online_registry::on_started(std::string_view world, std::uint64_t account_id, std::string_view character)
	{
		bool rebuilt = false;
		bool wrong_character = false;
		std::string holder;
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_entries.find(account_id);
			if (found != m_entries.end() && found->second.world != world)
			{
				holder = found->second.world;  // another World holds the account: it keeps it
			}
			else
			{
				rebuilt = found == m_entries.end();
				entry& target = m_entries[account_id];
				// A pending grant was made for one character; a different one in the confirmation does not replace it.
				wrong_character = !rebuilt && target.state == online_state::handoff_pending && target.character != character;
				cancel(target);
				target.state = online_state::in_world;
				target.world = std::string(world);
				if (!wrong_character)
					target.character = std::string(character);
				target.generation = ++m_next_generation;
			}
		}
		if (!holder.empty())
		{
			log::warning("world {}: session_started for account {} refused, world {} holds it", world, account_id, holder);
			return start_verdict::held_elsewhere;
		}
		if (wrong_character)
			log::warning("world {}: session_started for account {} names another character than its grant; the grant's is kept", world, account_id);
		else if (rebuilt)
			log::info("world {}: account {} announced in world (no grant on record)", world, account_id);
		return start_verdict::accepted;
	}

	void online_registry::on_ended(std::string_view world, std::uint64_t account_id)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_entries.find(account_id);
		if (found == m_entries.end() || found->second.world != world)
			return;
		cancel(found->second);
		m_entries.erase(found);
	}

	std::size_t online_registry::world_lost(std::string_view world)
	{
		std::size_t removed = 0;
		std::scoped_lock lock(m_mutex);
		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			if (it->second.world == world)
			{
				cancel(it->second);
				it = m_entries.erase(it);
				++removed;
			}
			else
			{
				++it;
			}
		}
		return removed;
	}

	online_state online_registry::state(std::uint64_t account_id) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_entries.find(account_id);
		return found == m_entries.end() ? online_state::offline : found->second.state;
	}

	std::string online_registry::world_of(std::uint64_t account_id) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_entries.find(account_id);
		return found == m_entries.end() ? std::string() : found->second.world;
	}

	std::string online_registry::character_of(std::uint64_t account_id) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_entries.find(account_id);
		return found == m_entries.end() ? std::string() : found->second.character;
	}

	std::size_t online_registry::count(online_state wanted) const
	{
		std::scoped_lock lock(m_mutex);
		std::size_t total = 0;
		for (const auto& item : m_entries)
		{
			if (item.second.state == wanted)
				++total;
		}
		return total;
	}
}
