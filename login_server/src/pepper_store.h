#pragma once

#include "password_hasher.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

// The pepper key file (plan sect. 4.2): JSON with comments, kept outside players.db and the repo.
//   { "current": 1, "peppers": [ { "id": 1, "key": "<64 hex digits>" } ] }
// Old peppers stay listed so rows made with them still verify; they move to the current one at next login.
namespace hb::server::login
{
	class pepper_store
	{
	public:
		pepper_store() = default;
		pepper_store(const pepper_store&) = delete;
		pepper_store& operator=(const pepper_store&) = delete;
		// Wipes the keys.
		~pepper_store();

		// Loads the file, or, when it does not exist and may_create is set (no accounts yet), creates it
		// owner-only with one fresh random pepper.
		bool load_or_create(const std::filesystem::path& path, bool may_create, std::string& error);

		const std::map<std::int32_t, pepper_key>& all() const
		{
			return m_peppers;
		}

		std::int32_t current_id() const
		{
			return m_current_id;
		}

		const pepper_key& current() const
		{
			return m_peppers.at(m_current_id);
		}

		// Null when the id is unknown (a row made with a pepper since removed from the file).
		const pepper_key* find(std::int32_t id) const;

	private:
		bool load(const std::filesystem::path& path, std::string& error);
		bool create(const std::filesystem::path& path, std::string& error);

		std::int32_t m_current_id = 0;
		std::map<std::int32_t, pepper_key> m_peppers;
	};
}
