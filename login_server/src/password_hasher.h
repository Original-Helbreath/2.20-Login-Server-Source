#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Password storage (plan sect. 4.2): HMAC-SHA-256 with a server pepper, then Argon2id with a random salt.
// Hashed, never encrypted. All primitives come from OpenSSL 3.5 (libcrypto).
namespace hb::server::login
{
	constexpr std::size_t password_hash_bytes = 32;
	constexpr std::size_t password_salt_bytes = 16;
	constexpr std::size_t pepper_bytes = 32;

	using pepper_key = std::array<std::uint8_t, pepper_bytes>;

	// Argon2id cost: passes over memory, memory in KiB, parallel lanes.
	struct argon2_cost
	{
		std::uint32_t iterations = 3;
		std::uint32_t memory_kib = 262144;
		std::uint32_t lanes = 1;

		bool operator==(const argon2_cost&) const = default;
	};

	// Bounds for a cost read back from the database, so a corrupt row cannot ask Argon2 for terabytes.
	constexpr argon2_cost min_argon2_cost{ 1, 8192, 1 };
	constexpr argon2_cost max_argon2_cost{ 10, 1048576, 4 };
	bool is_supported(const argon2_cost& cost);

	// The libsodium-named presets the config offers: "interactive", "moderate", "sensitive".
	std::optional<argon2_cost> cost_from_name(std::string_view name);

	// What a row stores. Hash and salt are lowercase hex.
	struct password_record
	{
		std::string hash;
		std::string salt;
		argon2_cost cost;
		std::int32_t pepper_id = 0;
	};

	class password_hasher
	{
	public:
		password_hasher(argon2_cost cost, std::int32_t pepper_id, const pepper_key& pepper);
		password_hasher(const password_hasher&) = delete;
		password_hasher& operator=(const password_hasher&) = delete;
		// Wipes its copy of the pepper.
		~password_hasher();

		// A new record with a fresh random salt. False if OpenSSL fails (logged).
		bool hash(std::string_view password, password_record& record) const;

		// Constant-time check of a password against a stored record, peppered with the record's own pepper.
		bool verify(std::string_view password, const password_record& record, const pepper_key& record_pepper) const;

		// True when the record was made with another cost or pepper than the current ones.
		bool needs_rehash(const password_record& record) const;

		// Fills buffer from OpenSSL's CSPRNG (seeded by the OS). False on failure.
		static bool random_bytes(std::span<std::uint8_t> buffer);

		// HMAC of a fixed label under the pepper, as hex: stored beside the accounts so a pepper key swapped or
		// recreated under the same id is caught at startup. Reveals nothing about the key. Empty on failure.
		static std::string pepper_fingerprint(const pepper_key& pepper);

	private:
		bool derive(std::string_view password, std::span<const std::uint8_t> salt, const argon2_cost& cost,
			const pepper_key& pepper, std::span<std::uint8_t> output) const;

		argon2_cost m_cost;
		std::int32_t m_pepper_id = 0;
		pepper_key m_pepper{};
	};
}
