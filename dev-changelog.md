## Add accounts with hashed, peppered passwords

### Login Server

**Added**
- `account_service` creates accounts (names 1-10 alphanumeric, case-insensitive via `name_key`; passwords 4-32) and checks logins, returning `create_result` / `login_result`.
- `password_hasher` hashes with HMAC-SHA-256 under a server pepper, then Argon2id (OpenSSL 3.5 `EVP_KDF` "ARGON2ID") with a 16-byte random salt, verifies in constant time, and rehashes at login when the cost or pepper id changes.
- `pepper_store` loads `accounts.pepper_file` (default `data/pepper.json`), creating it owner-only and atomically only while no accounts exist; ids are range-checked and key text is wiped after use.
- `platform::create_private_file` creates the pepper file with an owner-only protected DACL on Windows (`private_file_win32.cpp`) or `O_EXCL` 0600 elsewhere (`private_file_posix.cpp`).
- Startup fails when a pepper's fingerprint (HMAC of a fixed label) does not match the one stored in `pepper_checks`, or when accounts use a pepper id with no stored fingerprint (the error prints the INSERT to record it).
- Unknown names, unknown pepper ids and unusable rows spend a decoy hash so timing does not reveal which accounts exist; stored Argon2 costs are bounds-checked (`min_argon2_cost` / `max_argon2_cost`).
- `login_server_tests` project: 47 checks over the hasher, pepper store and account service against a throwaway database.

**Changed**
- `login_application` opens the database before the pepper file, counts accounts, then verifies the peppers.
- Config: `accounts.pepper_file`, `database.path` (`data/players.db`), `database.pool_size` (1-64); `allows_client_account_creation` now defaults to true.

### SQL

**Added**
- `accounts`, `login_events` and `pepper_checks` tables in `players.db` through Schemata (`entities.h`, `database.cpp`); the Login model declares only its own tables, leaving the World server's alone.
- Schemata v0.1.0-rc.24 and SQLite 3.53.4 vendored as prebuilt x64 archives in `third_party/schemata` (`refresh.md` holds the pin and hashes); OpenSSL linked through `third_party/openssl.props`.

Summary: Login Server and SQL areas gain account storage in players.db with Argon2id-peppered passwords, a guarded pepper key file and a 47-check test program (2026-10-26)
