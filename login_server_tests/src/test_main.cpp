// Tests for the Login server's account and password code, run against a throwaway database.
// Exit code 0 when every check passes.
#include "account_service.h"
#include "database.h"
#include "entities.h"
#include "log.h"
#include "password_hasher.h"
#include "pepper_store.h"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace tests
{
	using namespace hb::server::login;

	// Cheap enough to run many times; the production presets are checked separately.
	constexpr argon2_cost test_cost{ 1, 8192, 1 };
	constexpr argon2_cost raised_cost{ 2, 8192, 1 };

	class runner
	{
	public:
		void check(bool condition, std::string_view name)
		{
			++m_total;
			if (condition)
				return;
			++m_failed;
			hb::log::error("FAIL: {}", name);
		}

		bool passed() const
		{
			return m_failed == 0;
		}

		int total() const
		{
			return m_total;
		}

		int failed() const
		{
			return m_failed;
		}

	private:
		int m_total = 0;
		int m_failed = 0;
	};

	pepper_key make_pepper(std::uint8_t fill)
	{
		pepper_key key{};
		key.fill(fill);
		return key;
	}

	void hasher_tests(runner& t)
	{
		const pepper_key pepper = make_pepper(0x11);
		const password_hasher hasher(test_cost, 1, pepper);
		password_record first;
		password_record second;
		t.check(hasher.hash("correct horse", first), "hash succeeds");
		t.check(hasher.hash("correct horse", second), "second hash succeeds");
		t.check(first.hash.size() == password_hash_bytes * 2 && first.salt.size() == password_salt_bytes * 2, "hex sizes");
		t.check(first.salt != second.salt && first.hash != second.hash, "fresh salt per hash");
		t.check(hasher.verify("correct horse", first, pepper), "right password verifies");
		t.check(!hasher.verify("correct horsf", first, pepper), "wrong password refused");
		t.check(!hasher.verify("", first, pepper), "empty password refused");
		t.check(!hasher.verify("correct horse", first, make_pepper(0x22)), "wrong pepper refused");
		t.check(!hasher.needs_rehash(first), "current record needs no rehash");
		t.check(password_hasher(raised_cost, 1, pepper).needs_rehash(first), "raised cost needs rehash");
		t.check(password_hasher(test_cost, 2, make_pepper(0x22)).needs_rehash(first), "new pepper needs rehash");
		password_record tampered = first;
		tampered.hash[0] = tampered.hash[0] == '0' ? '1' : '0';
		t.check(!hasher.verify("correct horse", tampered, pepper), "tampered hash refused");
		tampered = first;
		tampered.salt = "zz";
		t.check(!hasher.verify("correct horse", tampered, pepper), "bad salt hex refused");
		t.check(cost_from_name("moderate") == argon2_cost{ 3, 262144, 1 }, "moderate preset");
		t.check(!cost_from_name("weak").has_value(), "unknown preset refused");
	}

	void set_cost(database& db, std::string_view name_key, std::int32_t memory_kib)
	{
		// Stands in for a corrupt row.
		schemata::ConnectionPool::Lease lease = db.pool().acquire();
		std::optional<account> row = schemata::from<account>(*lease, db.model()).where(schemata::col(&account::name_key) == std::string(name_key)).first();
		schemata::ChangeTracker tracker(db.model());
		tracker.track(*row);
		row->hash_memory_kib = memory_kib;
		tracker.save_changes(*lease);
	}

	void set_locked(database& db, std::string_view name_key, bool locked)
	{
		// Stands in for the admin dashboard's lock.
		schemata::ConnectionPool::Lease lease = db.pool().acquire();
		std::optional<account> row = schemata::from<account>(*lease, db.model()).where(schemata::col(&account::name_key) == std::string(name_key)).first();
		schemata::ChangeTracker tracker(db.model());
		tracker.track(*row);
		row->locked = locked;
		tracker.save_changes(*lease);
	}

	std::optional<account> load(database& db, std::string_view name_key)
	{
		schemata::ConnectionPool::Lease lease = db.pool().acquire();
		return schemata::from<account>(*lease, db.model()).where(schemata::col(&account::name_key) == std::string(name_key)).first();
	}

	void account_tests(runner& t, const std::filesystem::path& folder)
	{
		std::string error;
		pepper_store refused;
		t.check(!refused.load_or_create(folder / "pepper.json", false, error) && !std::filesystem::exists(folder / "pepper.json"),
			"missing pepper file not created while accounts exist");
		pepper_store peppers;
		t.check(peppers.load_or_create(folder / "pepper.json", true, error), "pepper file created");
		t.check(!std::filesystem::exists(folder / "pepper.json.tmp"), "no temporary pepper file left");
		pepper_store reloaded;
		t.check(reloaded.load_or_create(folder / "pepper.json", false, error) && reloaded.current() == peppers.current(), "pepper file reloads the same key");

		database db;
		t.check(db.open((folder / "players.db").string(), 2, error), std::format("database opens ({})", error));
		account_service accounts(db, peppers, test_cost);
		t.check(accounts.verify_peppers(error), std::format("pepper fingerprint recorded ({})", error));
		t.check(accounts.verify_peppers(error), std::format("pepper fingerprint matches on the next start ({})", error));

		t.check(accounts.create("Bob", "secret1") == create_result::created, "create Bob");
		t.check(accounts.create("bob", "other12") == create_result::name_taken, "name is case-insensitive");
		t.check(accounts.create("", "secret1") == create_result::invalid_name, "empty name refused");
		t.check(accounts.create("ElevenChars", "secret1") == create_result::invalid_name, "name over 10 refused");
		t.check(accounts.create("bad name", "secret1") == create_result::invalid_name, "space in name refused");
		t.check(accounts.create("Ann", "abc") == create_result::invalid_password, "short password refused");
		t.check(accounts.create("Ann", std::string(33, 'x')) == create_result::invalid_password, "long password refused");
		std::int64_t count = 0;
		t.check(count_accounts(db, count, error) && count == 1, "only Bob was stored");

		const std::optional<account> bob = load(db, "bob");
		t.check(bob && bob->password_hash.size() == 64 && bob->password_hash.find("secret1") == std::string::npos, "stored as a hash");
		t.check(bob && bob->hash_iterations == 1 && bob->hash_memory_kib == 8192 && bob->pepper_id == peppers.current_id(), "cost and pepper stored");

		t.check(accounts.check_login("BOB", "secret1", "127.0.0.1") == login_result::accepted, "login accepted, any case");
		t.check(accounts.check_login("Bob", "secret2", "127.0.0.1") == login_result::wrong_password, "wrong password refused");
		t.check(accounts.check_login("Nobody", "secret1", "127.0.0.1") == login_result::no_account, "unknown account refused");
		t.check(accounts.check_login("bad name!", "secret1", "127.0.0.1") == login_result::no_account, "invalid name refused");
		t.check(load(db, "bob")->last_login_at.has_value(), "last login recorded");

		set_locked(db, "bob", true);
		t.check(accounts.check_login("Bob", "secret1", "127.0.0.1") == login_result::locked, "locked account refused");
		t.check(accounts.check_login("Bob", "wrong12", "127.0.0.1") == login_result::wrong_password, "locked does not leak to a wrong password");
		set_locked(db, "bob", false);

		account_service stronger(db, peppers, raised_cost);
		t.check(stronger.check_login("Bob", "secret1", "127.0.0.1") == login_result::accepted, "login under raised cost");
		const std::optional<account> rehashed = load(db, "bob");
		t.check(rehashed && rehashed->hash_iterations == 2 && rehashed->password_hash != bob->password_hash, "rehashed at the raised cost");
		t.check(stronger.check_login("Bob", "secret1", "127.0.0.1") == login_result::accepted, "login after rehash");

		set_cost(db, "bob", -1);
		t.check(stronger.check_login("Bob", "secret1", "127.0.0.1") == login_result::failed, "out-of-range stored cost refused");
		set_cost(db, "bob", 8192);

		// A pepper file recreated under the same id: the fingerprint no longer matches.
		std::filesystem::remove(folder / "pepper.json");
		pepper_store recreated;
		t.check(recreated.load_or_create(folder / "pepper.json", true, error), "recreated pepper file");
		account_service swapped(db, recreated, test_cost);
		t.check(!swapped.verify_peppers(error), "swapped pepper key detected");

		schemata::ConnectionPool::Lease lease = db.pool().acquire();
		const std::int64_t events = schemata::from<login_event>(*lease, db.model()).count();
		t.check(events == 9, std::format("every login attempt recorded ({} events)", events));
	}
}

int main()
{
	hb::log::initialize("login_server_tests", "tests");
	tests::runner t;
	const std::filesystem::path folder = "test_data";
	std::error_code ignored;
	std::filesystem::remove(folder / "players.db", ignored);
	std::filesystem::remove(folder / "players.db-wal", ignored);
	std::filesystem::remove(folder / "players.db-shm", ignored);
	std::filesystem::remove(folder / "pepper.json", ignored);
	try
	{
		tests::hasher_tests(t);
		tests::account_tests(t, folder);
	}
	catch (const std::exception& failure)
	{
		hb::log::report_crash(failure.what());
		return 1;
	}
	hb::log::info("{} of {} checks passed", t.total() - t.failed(), t.total());
	hb::log::shutdown();
	return t.passed() ? 0 : 1;
}
