#include "account_service.h"

#include "entities.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <string>

namespace hb::server::login
{
	std::string_view to_string(create_result result)
	{
		switch (result)
		{
		case create_result::created:
			return "created";
		case create_result::invalid_name:
			return "invalid_name";
		case create_result::invalid_password:
			return "invalid_password";
		case create_result::name_taken:
			return "name_taken";
		case create_result::failed:
			return "failed";
		}
		return "unknown";
	}

	std::string_view to_string(login_result result)
	{
		switch (result)
		{
		case login_result::accepted:
			return "accepted";
		case login_result::no_account:
			return "no_account";
		case login_result::wrong_password:
			return "wrong_password";
		case login_result::locked:
			return "locked";
		case login_result::failed:
			return "failed";
		}
		return "unknown";
	}

	account_service::account_service(database& db, const pepper_store& peppers, argon2_cost cost)
		: m_database(db)
		, m_peppers(peppers)
		, m_hasher(cost, peppers.current_id(), peppers.current())
	{
	}

	create_result account_service::create(std::string_view name, std::string_view password)
	{
		if (!is_valid_name(name))
			return create_result::invalid_name;
		if (password.size() < min_password_length || password.size() > max_password_length)
			return create_result::invalid_password;

		account fresh;
		fresh.name = std::string(name);
		fresh.name_key = lowercase(name);
		try
		{
			// Checked before hashing, so a taken name costs no Argon2 run.
			if (name_exists(fresh.name_key))
				return create_result::name_taken;
		}
		catch (const std::exception& failure)
		{
			log::error("account create for {} failed: {}", name, failure.what());
			return create_result::failed;
		}
		password_record record;
		if (!m_hasher.hash(password, record))
			return create_result::failed;
		try
		{
			schemata::ConnectionPool::Lease lease = m_database.pool().acquire();
			fresh.password_hash = record.hash;
			fresh.password_salt = record.salt;
			fresh.hash_iterations = static_cast<std::int32_t>(record.cost.iterations);
			fresh.hash_memory_kib = static_cast<std::int32_t>(record.cost.memory_kib);
			fresh.hash_lanes = static_cast<std::int32_t>(record.cost.lanes);
			fresh.pepper_id = record.pepper_id;
			fresh.created_at = now();
			schemata::ChangeTracker tracker(m_database.model());
			tracker.add(fresh);
			tracker.save_changes(*lease);
			log::info("account {} created (id {})", fresh.name, fresh.id);
			return create_result::created;
		}
		catch (const std::exception& failure)
		{
			// The unique key refuses a name taken between the check and the insert; report that as taken.
			try
			{
				if (name_exists(fresh.name_key))
					return create_result::name_taken;
			}
			catch (const std::exception&)
			{
			}
			log::error("account create for {} failed: {}", name, failure.what());
			return create_result::failed;
		}
	}

	login_result account_service::check_login(std::string_view name, std::string_view password, std::string_view address)
	{
		try
		{
			schemata::ConnectionPool::Lease lease = m_database.pool().acquire();
			std::optional<account> found;
			if (is_valid_name(name))
				found = schemata::from<account>(*lease, m_database.model()).where(schemata::col(&account::name_key) == lowercase(name)).first();
			lease.release();
			if (!found)
			{
				// Spend the same hashing time as a real check, so timing does not tell which names exist.
				password_record decoy;
				m_hasher.hash(password, decoy);
				record_event(std::nullopt, name, address, login_result::no_account);
				return login_result::no_account;
			}

			const bool cost_in_range = found->hash_iterations > 0 && found->hash_memory_kib > 0 && found->hash_lanes > 0;
			password_record record{ found->password_hash, found->password_salt,
				{ static_cast<std::uint32_t>(found->hash_iterations), static_cast<std::uint32_t>(found->hash_memory_kib),
					static_cast<std::uint32_t>(found->hash_lanes) },
				found->pepper_id };
			const pepper_key* pepper = m_peppers.find(record.pepper_id);
			const bool row_usable = cost_in_range && is_supported(record.cost)
				&& record.hash.size() == password_hash_bytes * 2 && record.salt.size() == password_salt_bytes * 2;
			if (!pepper || !row_usable)
			{
				if (!pepper)
					log::error("account {} uses pepper id {}, which the pepper file no longer has", found->name, record.pepper_id);
				else
					log::error("account {} has an unusable password row (cost {}/{}/{}, hash {} chars, salt {} chars)", found->name,
						found->hash_iterations, found->hash_memory_kib, found->hash_lanes, record.hash.size(), record.salt.size());
				password_record decoy;
				m_hasher.hash(password, decoy);
				record_event(found->id, name, address, login_result::failed);
				return login_result::failed;
			}
			if (!m_hasher.verify(password, record, *pepper))
			{
				record_event(found->id, name, address, login_result::wrong_password);
				return login_result::wrong_password;
			}
			if (found->locked)
			{
				record_event(found->id, name, address, login_result::locked);
				return login_result::locked;
			}

			// Rehash before taking a connection, so none is held through an Argon2 run.
			schemata::ChangeTracker tracker(m_database.model());
			tracker.track(*found);
			if (m_hasher.needs_rehash(record) && m_hasher.hash(password, record))
			{
				found->password_hash = record.hash;
				found->password_salt = record.salt;
				found->hash_iterations = static_cast<std::int32_t>(record.cost.iterations);
				found->hash_memory_kib = static_cast<std::int32_t>(record.cost.memory_kib);
				found->hash_lanes = static_cast<std::int32_t>(record.cost.lanes);
				found->pepper_id = record.pepper_id;
				log::info("account {} rehashed to the current cost and pepper", found->name);
			}
			found->last_login_at = now();
			schemata::ConnectionPool::Lease update_lease = m_database.pool().acquire();
			tracker.save_changes(*update_lease);
			update_lease.release();
			record_event(found->id, name, address, login_result::accepted);
			return login_result::accepted;
		}
		catch (const std::exception& failure)
		{
			log::error("login check for {} failed: {}", printable(name), failure.what());
			return login_result::failed;
		}
	}

	bool account_service::is_valid_name(std::string_view name)
	{
		if (name.empty() || name.size() > max_account_name_length)
			return false;
		return std::all_of(name.begin(), name.end(), [](char c)
			{
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
			});
	}

	std::string account_service::lowercase(std::string_view text)
	{
		std::string lowered(text);
		for (char& c : lowered)
		{
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}
		return lowered;
	}

	schemata::timestamp account_service::now()
	{
		return std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
	}

	void account_service::record_event(std::optional<std::int64_t> account_id, std::string_view name, std::string_view address, login_result result)
	{
		try
		{
			login_event event;
			event.account_id = account_id;
			event.name_attempted = printable(name);
			event.address = printable(address);
			event.outcome = std::string(to_string(result));
			event.at = now();
			schemata::ConnectionPool::Lease lease = m_database.pool().acquire();
			schemata::ChangeTracker tracker(m_database.model());
			tracker.add(event);
			tracker.save_changes(*lease);
		}
		catch (const std::exception& failure)
		{
			log::warning("could not record login event for {}: {}", printable(name), failure.what());
		}
	}

	bool account_service::name_exists(const std::string& name_key)
	{
		schemata::ConnectionPool::Lease lease = m_database.pool().acquire();
		return schemata::from<account>(*lease, m_database.model()).where(schemata::col(&account::name_key) == name_key).count() > 0;
	}

	std::string account_service::printable(std::string_view text)
	{
		// Untrusted client text: printable ASCII only, so it cannot forge log lines or split a character.
		std::string clean(text.substr(0, 64));
		for (char& c : clean)
		{
			if (c < 0x20 || c > 0x7e)
				c = '?';
		}
		return clean;
	}

	bool account_service::verify_peppers(std::string& error)
	{
		try
		{
			schemata::ConnectionPool::Lease lease = m_database.pool().acquire();
			for (const auto& [id, key] : m_peppers.all())
			{
				const std::string fingerprint = password_hasher::pepper_fingerprint(key);
				if (fingerprint.empty())
				{
					error = "could not fingerprint the pepper keys";
					return false;
				}
				const std::optional<pepper_check> stored = schemata::from<pepper_check>(*lease, m_database.model())
					.where(schemata::col(&pepper_check::pepper_id) == id).first();
				if (!stored)
				{
					// A first fingerprint is trusted only for a pepper no account uses yet; otherwise this key
					// cannot be told apart from a replaced one.
					const std::int64_t users = schemata::from<account>(*lease, m_database.model())
						.where(schemata::col(&account::pepper_id) == id).count();
					if (users > 0)
					{
						error = std::format("pepper id {} is used by {} account(s) but has no recorded fingerprint. If this pepper file "
							"is the original, record it with: INSERT INTO pepper_checks (pepper_id, fingerprint) VALUES ({}, '{}');",
							id, users, id, fingerprint);
						return false;
					}
					pepper_check fresh;
					fresh.pepper_id = id;
					fresh.fingerprint = fingerprint;
					// The tracker holds a reference to the row, so it saves while the row is alive.
					schemata::ChangeTracker tracker(m_database.model());
					tracker.add(fresh);
					tracker.save_changes(*lease);
				}
				else if (stored->fingerprint != fingerprint)
				{
					error = std::format("pepper id {} does not match the key the accounts were hashed with; restore the original pepper file", id);
					return false;
				}
			}
			return true;
		}
		catch (const std::exception& failure)
		{
			error = std::format("pepper check failed: {}", failure.what());
			return false;
		}
	}

	bool count_accounts(database& db, std::int64_t& count, std::string& error)
	{
		try
		{
			schemata::ConnectionPool::Lease lease = db.pool().acquire();
			count = schemata::from<account>(*lease, db.model()).count();
			return true;
		}
		catch (const std::exception& failure)
		{
			error = std::format("cannot count accounts: {}", failure.what());
			return false;
		}
	}
}
