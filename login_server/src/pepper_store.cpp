#include "pepper_store.h"

#include "hex.h"
#include "log.h"
#include "private_file.h"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace hb::server::login
{
	bool pepper_store::load_or_create(const std::filesystem::path& path, bool may_create, std::string& error)
	{
		error.clear();
		std::error_code exists_error;
		if (std::filesystem::exists(path, exists_error))
			return load(path, error);
		if (exists_error)
		{
			error = std::format("cannot check {}: {}", path.string(), exists_error.message());
			return false;
		}
		if (!may_create)
		{
			error = std::format("{} is missing but accounts exist; restore it from backup (a new pepper would lock every account out)", path.string());
			return false;
		}
		return create(path, error);
	}

	const pepper_key* pepper_store::find(std::int32_t id) const
	{
		const auto found = m_peppers.find(id);
		return found == m_peppers.end() ? nullptr : &found->second;
	}

	bool pepper_store::load(const std::filesystem::path& path, std::string& error)
	{
		std::ifstream file(path, std::ios::binary);
		std::stringstream text;
		text << file.rdbuf();
		const nlohmann::json root = nlohmann::json::parse(text.str(), nullptr, false, true);
		if (!file || root.is_discarded() || !root.is_object() || !root.contains("current") || !root.contains("peppers")
			|| !root["current"].is_number_integer() || !root["peppers"].is_array())
		{
			error = std::format("{} must hold 'current' (a number) and 'peppers' (an array)", path.string());
			return false;
		}
		for (const nlohmann::json& entry : root["peppers"])
		{
			std::vector<std::uint8_t> bytes;
			std::string key_text;
			if (entry.is_object() && entry.contains("key") && entry["key"].is_string())
				key_text = entry["key"].get<std::string>();
			const bool valid = entry.is_object() && entry.contains("id") && entry["id"].is_number_integer()
				&& from_hex(key_text, bytes) && bytes.size() == pepper_bytes;
			pepper_key key{};
			if (valid)
				std::copy(bytes.begin(), bytes.end(), key.begin());
			OPENSSL_cleanse(key_text.data(), key_text.size());
			OPENSSL_cleanse(bytes.data(), bytes.size());
			if (!valid)
			{
				error = std::format("{}: each pepper needs an integer 'id' and a 'key' of {} hex digits", path.string(), pepper_bytes * 2);
				return false;
			}
			if (!m_peppers.emplace(entry["id"].get<std::int32_t>(), key).second)
			{
				error = std::format("{}: pepper id {} is listed twice", path.string(), entry["id"].get<std::int32_t>());
				return false;
			}
		}
		m_current_id = root["current"].get<std::int32_t>();
		if (!m_peppers.contains(m_current_id))
		{
			error = std::format("{}: 'current' is {}, which no pepper has", path.string(), m_current_id);
			return false;
		}
		log::info("loaded {} pepper(s) from {}, current id {}", m_peppers.size(), path.string(), m_current_id);
		return true;
	}

	bool pepper_store::create(const std::filesystem::path& path, std::string& error)
	{
		pepper_key key{};
		if (!password_hasher::random_bytes(key))
		{
			error = "could not generate a pepper";
			return false;
		}
		std::error_code directory_error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), directory_error);
		if (directory_error)
		{
			error = std::format("cannot create {}: {}", path.parent_path().string(), directory_error.message());
			return false;
		}
		// Written beside the target, limited to this user before the key goes in, then renamed into place,
		// so a crash never leaves a half-written key file.
		std::filesystem::path temporary = path;
		temporary += ".tmp";
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		{
			std::ofstream empty(temporary, std::ios::binary);
			if (!empty)
			{
				error = std::format("cannot write {}", temporary.string());
				return false;
			}
		}
		if (!platform::restrict_to_owner(temporary, error))
		{
			std::filesystem::remove(temporary, ignored);
			return false;
		}
		std::string text = nlohmann::json{ { "current", 1 }, { "peppers", { { { "id", 1 }, { "key", to_hex(key) } } } } }.dump(1, '\t') + "\r\n";
		bool written = false;
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			file << text;
			file.flush();
			written = static_cast<bool>(file);
		}
		OPENSSL_cleanse(text.data(), text.size());
		std::error_code rename_error;
		if (written)
			std::filesystem::rename(temporary, path, rename_error);
		if (!written || rename_error)
		{
			std::filesystem::remove(temporary, ignored);
			error = std::format("cannot write {}{}", path.string(), rename_error ? ": " + rename_error.message() : std::string());
			return false;
		}
		m_peppers.emplace(1, key);
		m_current_id = 1;
		log::warning("created a new pepper file {}; back it up: without it no existing password can be verified", path.string());
		return true;
	}
}
