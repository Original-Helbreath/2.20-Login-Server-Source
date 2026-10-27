#include "password_hasher.h"

#include "hex.h"
#include "log.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <memory>
#include <vector>

namespace hb::server::login
{
	std::optional<argon2_cost> cost_from_name(std::string_view name)
	{
		// The libsodium crypto_pwhash presets, so the names mean what they mean there.
		if (name == "interactive")
			return argon2_cost{ 2, 65536, 1 };
		if (name == "moderate")
			return argon2_cost{ 3, 262144, 1 };
		if (name == "sensitive")
			return argon2_cost{ 4, 1048576, 1 };
		return std::nullopt;
	}

	password_hasher::password_hasher(argon2_cost cost, std::int32_t pepper_id, const pepper_key& pepper)
		: m_cost(cost)
		, m_pepper_id(pepper_id)
		, m_pepper(pepper)
	{
	}

	password_hasher::~password_hasher()
	{
		OPENSSL_cleanse(m_pepper.data(), m_pepper.size());
	}

	bool password_hasher::hash(std::string_view password, password_record& record) const
	{
		std::array<std::uint8_t, password_salt_bytes> salt{};
		std::array<std::uint8_t, password_hash_bytes> digest{};
		if (!random_bytes(salt) || !derive(password, salt, m_cost, m_pepper, digest))
			return false;
		record.hash = to_hex(digest);
		record.salt = to_hex(salt);
		record.cost = m_cost;
		record.pepper_id = m_pepper_id;
		OPENSSL_cleanse(digest.data(), digest.size());
		return true;
	}

	bool password_hasher::verify(std::string_view password, const password_record& record, const pepper_key& record_pepper) const
	{
		std::vector<std::uint8_t> salt;
		std::vector<std::uint8_t> stored;
		if (!from_hex(record.salt, salt) || !from_hex(record.hash, stored) || stored.size() != password_hash_bytes)
			return false;
		std::array<std::uint8_t, password_hash_bytes> digest{};
		if (!derive(password, salt, record.cost, record_pepper, digest))
			return false;
		const bool matches = CRYPTO_memcmp(digest.data(), stored.data(), digest.size()) == 0;
		OPENSSL_cleanse(digest.data(), digest.size());
		return matches;
	}

	bool password_hasher::needs_rehash(const password_record& record) const
	{
		return record.cost != m_cost || record.pepper_id != m_pepper_id;
	}

	bool password_hasher::random_bytes(std::span<std::uint8_t> buffer)
	{
		if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) == 1)
			return true;
		log::error("RAND_bytes failed");
		return false;
	}

	std::string password_hasher::pepper_fingerprint(const pepper_key& pepper)
	{
		constexpr std::string_view label = "hb-login-pepper-check";
		std::array<std::uint8_t, 32> digest{};
		std::size_t digest_size = 0;
		if (!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, pepper.data(), pepper.size(),
				reinterpret_cast<const unsigned char*>(label.data()), label.size(), digest.data(), digest.size(), &digest_size)
			|| digest_size != digest.size())
		{
			log::error("HMAC-SHA-256 pepper fingerprint failed");
			return {};
		}
		return to_hex(std::span<const std::uint8_t>(digest.data(), 16));
	}

	bool is_supported(const argon2_cost& cost)
	{
		return cost.iterations >= min_argon2_cost.iterations && cost.iterations <= max_argon2_cost.iterations
			&& cost.memory_kib >= min_argon2_cost.memory_kib && cost.memory_kib <= max_argon2_cost.memory_kib
			&& cost.lanes >= min_argon2_cost.lanes && cost.lanes <= max_argon2_cost.lanes;
	}

	bool password_hasher::derive(std::string_view password, std::span<const std::uint8_t> salt, const argon2_cost& cost,
		const pepper_key& pepper, std::span<std::uint8_t> output) const
	{
		// Pepper first: HMAC-SHA-256(pepper, password) fixes the length and keeps a stolen database alone useless.
		std::array<std::uint8_t, password_hash_bytes> peppered{};
		std::size_t peppered_size = 0;
		if (!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, pepper.data(), pepper.size(),
				reinterpret_cast<const unsigned char*>(password.data()), password.size(),
				peppered.data(), peppered.size(), &peppered_size) || peppered_size != peppered.size())
		{
			log::error("HMAC-SHA-256 pepper step failed");
			return false;
		}

		const std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)> kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr), EVP_KDF_free);
		const std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)> context(kdf ? EVP_KDF_CTX_new(kdf.get()) : nullptr, EVP_KDF_CTX_free);
		if (!context)
		{
			log::error("OpenSSL has no ARGON2ID KDF (needs OpenSSL 3.2 or later)");
			OPENSSL_cleanse(peppered.data(), peppered.size());
			return false;
		}
		std::uint32_t iterations = cost.iterations;
		std::uint32_t memory_kib = cost.memory_kib;
		std::uint32_t lanes = cost.lanes;
		// OSSL_PARAM takes non-const pointers at the C boundary; nothing is written through them.
		const std::array<OSSL_PARAM, 6> params = {
			OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, peppered.data(), peppered.size()),
			OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<std::uint8_t*>(salt.data()), salt.size()),
			OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
			OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory_kib),
			OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
			OSSL_PARAM_construct_end(),
		};
		const bool derived = EVP_KDF_derive(context.get(), output.data(), output.size(), params.data()) == 1;
		OPENSSL_cleanse(peppered.data(), peppered.size());
		if (!derived)
			log::error("Argon2id derivation failed");
		return derived;
	}
}
