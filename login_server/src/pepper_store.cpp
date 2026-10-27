#include "pepper_store.h"

#include "hex.h"
#include "log.h"
#include "private_file.h"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

#include <algorithm>
#include <format>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

namespace hb::server::login
{
	namespace
	{
		// A real file is well under 1 KiB per pepper; anything this large is not a pepper file.
		constexpr std::uintmax_t max_pepper_file_bytes = 64 * 1024;

		void cleanse(std::string& text)
		{
			OPENSSL_cleanse(text.data(), text.size());
		}

		// A pepper id is a positive number that fits the database column, never silently truncated.
		bool is_id(const nlohmann::json& value)
		{
			if (!value.is_number_integer())
				return false;
			const std::int64_t id = value.get<std::int64_t>();
			return id >= 1 && id <= std::numeric_limits<std::int32_t>::max();
		}
	}

	pepper_store::~pepper_store()
	{
		for (auto& [id, key] : m_peppers)
			OPENSSL_cleanse(key.data(), key.size());
	}

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
		// Every copy of the key text (the file contents and the parsed tree) is wiped on the way out.
		std::string text;
		nlohmann::json root;
		const auto wipe = [&]
		{
			if (root.is_object() && root.contains("peppers") && root["peppers"].is_array())
			{
				for (nlohmann::json& entry : root["peppers"])
				{
					if (entry.is_object() && entry.contains("key") && entry["key"].is_string())
						cleanse(entry["key"].get_ref<std::string&>());
				}
			}
			cleanse(text);
		};
		std::error_code size_error;
		const std::uintmax_t size = std::filesystem::file_size(path, size_error);
		std::ifstream file(path, std::ios::binary);
		if (!size_error && size <= max_pepper_file_bytes)
		{
			text.resize(static_cast<std::size_t>(size));
			file.read(text.data(), static_cast<std::streamsize>(text.size()));
		}
		if (size_error || size > max_pepper_file_bytes || !file)
		{
			cleanse(text);
			error = std::format("cannot read {}", path.string());
			return false;
		}
		root = nlohmann::json::parse(text, nullptr, false, true);
		if (root.is_discarded() || !root.is_object() || !root.contains("current") || !root.contains("peppers")
			|| !is_id(root["current"]) || !root["peppers"].is_array())
		{
			wipe();
			error = std::format("{} must hold 'current' (a pepper id) and 'peppers' (an array)", path.string());
			return false;
		}
		for (const nlohmann::json& entry : root["peppers"])
		{
			std::vector<std::uint8_t> bytes;
			const bool valid = entry.is_object() && entry.contains("id") && is_id(entry["id"]) && entry.contains("key")
				&& entry["key"].is_string() && from_hex(entry["key"].get_ref<const std::string&>(), bytes) && bytes.size() == pepper_bytes;
			pepper_key key{};
			if (valid)
				std::copy(bytes.begin(), bytes.end(), key.begin());
			OPENSSL_cleanse(bytes.data(), bytes.size());
			if (!valid)
			{
				wipe();
				error = std::format("{}: each pepper needs an 'id' (1 or more) and a 'key' of {} hex digits", path.string(), pepper_bytes * 2);
				return false;
			}
			const std::int32_t id = entry["id"].get<std::int32_t>();
			const bool added = m_peppers.emplace(id, key).second;
			OPENSSL_cleanse(key.data(), key.size());
			if (!added)
			{
				wipe();
				error = std::format("{}: pepper id {} is listed twice", path.string(), id);
				return false;
			}
		}
		m_current_id = root["current"].get<std::int32_t>();
		wipe();
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
		// Wipes the local key on every return path.
		const std::unique_ptr<pepper_key, decltype([](pepper_key* k) { OPENSSL_cleanse(k->data(), k->size()); })> key_wipe(&key);
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
		// Created owner-only beside the target, the key written in, then renamed into place, so no other user can
		// open it and a crash never leaves a half-written key file.
		std::filesystem::path temporary = path;
		temporary += ".tmp";
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		if (!platform::create_private_file(temporary, error))
			return false;
		nlohmann::json root = { { "current", 1 }, { "peppers", { { { "id", 1 }, { "key", to_hex(key) } } } } };
		std::string text = root.dump(1, '\t') + "\r\n";
		cleanse(root["peppers"][0]["key"].get_ref<std::string&>());
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
