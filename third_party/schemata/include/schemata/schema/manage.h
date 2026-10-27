#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <schemata/driver/driver.h>
#include <schemata/model/model.h>
#include <schemata/schema/differ.h>
#include <schemata/schema/migrate.h>

namespace schemata {

/// A foreign key `ensure_schema()` held back from a run, to be added by the
/// next one. See `EnsureResult::deferred_foreign_keys`.
struct DeferredForeignKey {
    /// The table the key is declared on, which already had rows.
    std::string table;

    /// The key's name.
    std::string name;

    /// The table it references: created by this run where `waits_for_seed`.
    std::string referenced_table;

    /// Under `DiffOptions::hold_back_for_backfill`, the key's columns this
    /// run adds (SCH-0044), where the rows already
    /// there would reference nothing at the new column's default: the key
    /// waits for an `after_columns` migration to backfill them. Empty when
    /// the key names no new column.
    std::vector<std::string> new_columns{};

    /// True where the key waits for the table it references, which this run
    /// created, to be seeded by an `after_schema` migration (SCH-0039).
    /// False for a key held back only for its new columns.
    bool waits_for_seed = true;

    /// The schema of the table it references; empty for the connection's own.
    std::string referenced_schema{};
};

/// A CHECK `ensure_schema()` held back from a run, to be added by the next
/// one. See `EnsureResult::deferred_checks`.
struct DeferredCheck {
    /// The table it is declared on, which already had rows.
    std::string table;

    /// The CHECK's name: a table CHECK's own, or `column_check_name()` for a
    /// column's.
    std::string name;

    /// The expression, as the model wrote it.
    std::string expression;
};

/// What `ensure_schema()` did.
struct EnsureResult {
    /// The differences found. Empty when the database already matched.
    SchemaDiff diff;

    /// Statements actually executed.
    std::size_t applied = 0;

    /// True when the live schema was never read because the recorded
    /// fingerprint already matched the model.
    bool skipped_by_fingerprint = false;

    /// The schema the run copied the target into before its first statement
    /// (`DiffOptions::backup_schema`, `DiffOptions::backup`), as the server
    /// has it; empty when it made no copy.
    std::string backup_schema{};

    /// New foreign keys this run did not add, because the table each
    /// references was created by this same run -- empty, so every row of a
    /// table that already had rows with a value in the key referenced nothing
    /// yet (`DeferredForeignKey::waits_for_seed`) -- or because the key names
    /// a column this run adds, whose default the rows would reference nothing
    /// with (`DeferredForeignKey::new_columns`; see `deferred_checks`, which
    /// such a key waits with for the `after_columns` backfill). Empty when
    /// nothing was deferred.
    ///
    /// The rest of the model is applied as usual, and the fingerprint is not
    /// recorded: the database is not yet in line with the model. The keys are
    /// added by the next `ensure_schema()`, which puts each to the rows then,
    /// as any new key is put -- refused, naming the rows, if any still
    /// reference nothing. So the phased startup that seeds the new table runs
    /// `ensure_schema()` once more after the seed, which is the post-seed
    /// constraint pass and costs one query when nothing was deferred:
    ///
    ///     migrate(db, all, {.phase = MigrationPhase::before_schema});
    ///     ensure_schema(db, model);
    ///     migrate(db, all, {.phase = MigrationPhase::after_schema});
    ///     ensure_schema(db, model);  // adds what the first one deferred
    ///
    /// Each deferral is in `diff.notices` as well. That and this list are the
    /// only report: the library writes no log. A caller that runs a bare
    /// `ensure_schema()` must check this list and run the post-seed pass when
    /// it is not empty -- or call `migrate_and_ensure()`, which does.
    std::vector<DeferredForeignKey> deferred_foreign_keys{};

    /// New CHECKs this run did not add (SCH-0044), under
    /// `DiffOptions::hold_back_for_backfill`: each names a column this same
    /// run adds to a table that already had rows, and the rows, given the
    /// column's default, would break it -- a column added `DEFAULT 0` under
    /// `CHECK (usage NOT IN (1,2,3) OR blend_mode = 1)`. Always empty with the
    /// option off, which refuses such a CHECK on the first run with nothing
    /// applied. Empty when nothing was held back.
    ///
    /// The column goes in; the CHECK waits, so an `after_columns` migration
    /// can backfill the rows before it applies. A new foreign key on such a
    /// column waits the same way, in `deferred_foreign_keys` with its
    /// `new_columns`. The next `ensure_schema()` adds them, put to the rows
    /// as any new constraint is: refused, naming the rows, if any still break
    /// one. Nothing is weakened -- a held-back constraint is only ever added
    /// later or refused. It is held back under `allow_destructive` too: that
    /// option does not get a CHECK past rows that break it, and the engine
    /// refuses it in the later run if a row still does. As for a deferred key, the fingerprint is not
    /// recorded, each is in `diff.notices` too, and the library writes no
    /// log: a caller that runs a bare `ensure_schema()` must check this list,
    /// or call `migrate_and_ensure()`, which runs the `after_columns` pass
    /// and the `ensure_schema()` after it.
    ///
    /// A CHECK on a column already there is never held back, nor one that
    /// takes the place of a live CHECK under its name, nor one the rows keep
    /// at the default. On SQLite, where the CHECK comes with a table rebuild,
    /// its own probe decides, not the rebuild: one destructive for another
    /// reason does not hold back a CHECK the rows keep.
    std::vector<DeferredCheck> deferred_checks{};
};

/// Build and heal: bring the database in line with the model.
///
/// Additive and widening operations are applied. Destructive ones are
/// refused, with a `schema_error` naming every one of them, unless
/// `DiffOptions::allow_destructive` is set for this call. Nothing is applied
/// when the diff is refused -- it is all or nothing, not best effort.
///
/// **Startup cost.** The model's fingerprint is recorded in
/// `__schemata_meta`. When it still matches, the live schema is not read at
/// all and this costs one query, which is what makes it reasonable to leave
/// on in production rather than something to reason about disabling.
///
/// **Atomicity.** On SQLite the whole diff runs inside a transaction, so a
/// failure leaves nothing half-applied. On MySQL and MariaDB it cannot:
/// DDL there causes an implicit commit, so statements land one at a time and
/// a failure partway leaves the earlier ones in place. The fingerprint is
/// only recorded once every statement has succeeded, so the next run
/// re-reads the live schema and finishes the job.
///
/// \throws schema_error when the model is invalid, or when the diff contains
///         destructive operations that were not opted into.
EnsureResult ensure_schema(driver::Connection& connection, const Model& model,
                           const DiffOptions& options = {});

/// What `migrate_and_ensure()` did, pass by pass.
struct StartupResult {
    /// The `before_schema` pass.
    MigrateResult before;

    /// The first `ensure_schema()`.
    EnsureResult schema;

    /// The `after_schema` pass.
    MigrateResult after;

    /// The post-seed `ensure_schema()`, run only when the `ensure_schema()`
    /// before the `after_schema` pass deferred a foreign key; it adds them.
    /// Unset when nothing was deferred.
    std::optional<EnsureResult> post_seed{};

    /// The `after_columns` pass (SCH-0044), run after the first
    /// `ensure_schema()`.
    MigrateResult columns{};

    /// The `ensure_schema()` after the `after_columns` pass, run only when the
    /// first one held back a CHECK or a foreign key on a column it added; it
    /// adds them. Unset when nothing was held back.
    std::optional<EnsureResult> post_columns{};
};

/// The caller hook `migrate_and_ensure()` runs between the schema pass and the
/// seed (SCH-0046). See `StartupOptions::before_seed`.
using BeforeSeedHook = std::function<void(driver::Connection&, const EnsureResult&)>;

/// What `migrate_and_ensure()` is given beyond the migrations and the model.
struct StartupOptions {
    /// Used for every pass, its `phase` set for each and its `dry_run`
    /// refused.
    MigrateOptions migrate{};

    /// Given to every `ensure_schema()` call.
    DiffOptions schema{};

    /// Called once per `migrate_and_ensure()` call, whenever it is set --
    /// fresh build, heal or nothing to do alike -- after every model table
    /// exists: after the first `ensure_schema()`, the `after_columns` pass and
    /// the `ensure_schema()` that follows it, and before the first
    /// `after_schema` migration. It is given the connection and the first
    /// `ensure_schema()`'s result (`StartupResult::schema`: what was created,
    /// what was healed, the notices).
    ///
    /// The place for a write the seed depends on that SQL migrations cannot
    /// make -- an environment stamp through the model, say -- instead of
    /// running the phases by hand around it.
    ///
    /// It runs outside any transaction, as the passes leave the connection.
    /// It may open and commit its own; it must not leave one open.
    ///
    /// An exception from it stops the startup as a failing step does and
    /// propagates as is: the steps before it stay done, a key still waiting
    /// for the seed stays deferred with no fingerprint recorded, and nothing
    /// of the `after_schema` pass has run, so the next call takes it all up
    /// again and calls the hook again.
    BeforeSeedHook before_seed{};
};

/// The phased startup in one call:
///
/// 1. `migrate()` the `before_schema` pass;
/// 2. `ensure_schema()`, with `DiffOptions::hold_back_for_backfill` on only
///    when an `after_columns` migration is pending -- otherwise as given, so
///    a CHECK on a new column the rows break is refused here, as in rc.20;
/// 3. `migrate()` the `after_columns` pass;
/// 4. only when step 2 held back a CHECK or a foreign key on a column it
///    added (`EnsureResult::deferred_checks`), `ensure_schema()` again, which
///    adds them now the backfill has set the rows, or refuses one, naming the
///    rows, if any still break it. A key step 2 deferred into a table it
///    created keeps waiting here for the seed, though that table is no
///    longer new;
/// 5. `StartupOptions::before_seed`, when set;
/// 6. `migrate()` the `after_schema` pass;
/// 7. only when the last `ensure_schema()` deferred a foreign key into a table
///    step 2 created, `ensure_schema()` once more, which adds that key now the
///    seed has filled the table, or refuses it, naming the rows, if any still
///    reference nothing.
///
/// Steps 4 and 7 are skipped when nothing was held back, so a startup with
/// nothing to wait for costs the three passes and one `ensure_schema()`.
/// For a model that holds back only one kind, the same as by hand:
///
///     DiffOptions hold;
///     hold.hold_back_for_backfill = true;  // only with a backfill to run
///     migrate(db, all, {.phase = MigrationPhase::before_schema});
///     ensure_schema(db, model, hold);
///     migrate(db, all, {.phase = MigrationPhase::after_columns});
///     // WARNING: if the run above also deferred a key for the seed, this
///     // call refuses it -- use migrate_and_ensure() for that model.
///     ensure_schema(db, model);  // only when a CHECK was held back
///     migrate(db, all, {.phase = MigrationPhase::after_schema});
///     ensure_schema(db, model);  // only when a key was deferred
///
/// A write the seed depends on goes in the hook rather than in a hand-run
/// sequence around the phases:
///
///     StartupOptions startup;
///     startup.before_seed = [](driver::Connection& db, const EnsureResult& first) {
///         write_environment_stamp(db, first);  // the seed reads it
///     };
///     migrate_and_ensure(db, all, model, startup);
///
/// With an `after_schema` migration pending, step 2 also keeps waiting for
/// the seed a new key into a table that already exists whose rows it
/// orphans -- the table empty or partly written -- as on the startup that
/// created it: a startup that stopped before its seed (a throwing hook, a
/// failing seed) resumes on the next call rather than refusing the key.
/// This holds for both overloads. Against rc.21 it means:
///
/// - such a key is refused by the post-seed `ensure_schema()` (step 7),
///   after the seed, rather than by step 2; the seed is recorded on that
///   failing startup, and with no seed left pending the next startup refuses
///   the key at step 2, as rc.21 did;
/// - the startup reads the migration ledger once more, to ask whether an
///   `after_schema` migration is pending.
///
/// Where both are held back at once, only this call keeps step 2's
/// waiting-for-the-seed keys back in step 4; a bare `ensure_schema()` there
/// would put them to the empty table and refuse them.
///
/// `migrate_options` is used for every pass, its `phase` set for each and its
/// `dry_run` refused: a dry run of a startup cannot know what the schema step
/// would leave for the seed. `schema_options` is given to every
/// `ensure_schema()` call.
///
/// \throws schema_error for `migrate_options.dry_run`, before anything runs.
/// \throws whatever the step that fails throws -- a `migrate()` pass or an
///         `ensure_schema()` (a CHECK still broken after the backfill, or a
///         key still orphaned after the seed, is refused there as
///         `schema_error`). The steps before it stay done: their migrations
///         recorded, their schema applied. A held-back constraint stays held
///         back, with no fingerprint recorded, so the next call or
///         `ensure_schema()` takes it up again.
/// 	hrows whatever `StartupOptions::before_seed` throws, as is, with the
///         same guarantees: nothing of the `after_schema` pass has run.
/// 	hrows schema_error when `StartupOptions::before_seed` returns with a
///         transaction open, before the `after_schema` pass.
StartupResult migrate_and_ensure(driver::Connection& connection,
                                 const std::vector<Migration>& migrations, const Model& model,
                                 const StartupOptions& options);

/// The same, with no `before_seed` hook: `migrate_options` and
/// `schema_options` as `StartupOptions::migrate` and `StartupOptions::schema`.
StartupResult migrate_and_ensure(driver::Connection& connection,
                                 const std::vector<Migration>& migrations, const Model& model,
                                 const MigrateOptions& migrate_options = {},
                                 const DiffOptions&    schema_options  = {});

/// Attest only: report drift without changing anything.
///
/// Always reads the live schema. Unlike `ensure_schema()` there is no
/// fingerprint shortcut, because the point of attestation is to check the
/// database rather than to trust a hash that says it was checked before.
///
/// Reads rows too, while there is drift of a kind the rows decide: a new
/// CHECK or foreign key, a collation move, a NOT NULL column with no default.
/// Each is put to the table as `ensure_schema()` puts it (see
/// `SchemaOperation::probes`), so the report says what a run would do -- and
/// each is a scan of the table, or of both tables for a key, paid for as long
/// as that drift is pending. A question the server will not answer is not a
/// failure: the operation is reported at its worst, saying why.
///
/// Validates the model first, and throws if it does not hold up -- the same
/// gate `ensure_schema()` applies. An invalid model is not drift and cannot
/// honestly be reported as none: a gate that passes what the deploy will
/// then refuse is worse than one that fails.
///
/// \throws schema_error if the model is invalid for this dialect.
///
/// **On SQLite, the default options write to the file.** Not here -- this
/// function only reads -- but `schemata::sqlite(path)` opens with
/// `SqliteOptions::wal` set, and switching a database into write-ahead logging
/// is itself a write: it creates a `-wal` and a `-shm` file beside the
/// database and leaves the mode changed after the connection closes. To
/// attest a database without changing anything about it at all, open it
/// read-only, which cannot create the file, cannot switch the journal mode and
/// cannot write a row:
///
///     schemata::SqliteOptions options;
///     options.path      = "app.db";
///     options.read_only = true;
///     const auto drift = schemata::verify_schema(*schemata::sqlite(options), model);
///
/// Suitable as a deploy gate or a startup assertion:
///
///     const auto drift = schemata::verify_schema(db, model);
///     if (!drift.empty()) {
///         std::fputs(drift.report().c_str(), stderr);
///     }
///
/// A model whose foreign keys reach into other schemas needs the same
/// `bindings` `ensure_schema()` was given in `DiffOptions::schema_bindings`,
/// and is refused, naming the alias, without them.
///
/// `schema` attests another schema than the session's own, as
/// `DiffOptions::schema` ensures one: a physical name or an alias in
/// `bindings`, and the session is where it was afterwards. Naming one needs
/// the connection to have a default schema of its own to go back to.
///
/// `validation` holds the model to the rules `DiffOptions::validation` would
/// in `ensure_schema()`; give both the same.
[[nodiscard]] SchemaDiff verify_schema(driver::Connection& connection, const Model& model,
                                       const SchemaBindings&  bindings   = {},
                                       std::string_view       schema     = {},
                                       const ValidateOptions& validation = {});

/// What `ensure_database()` did.
struct EnsureDatabaseResult {
    /// The database was not there and has been created.
    bool created = false;

    /// It was there with another default collation, and has this one now.
    bool default_changed = false;
};

/// Make sure a database -- a schema, in MySQL's own words -- exists on the
/// server, with the default collation given.
///
/// For a deployment that creates its schemas rather than being handed them,
/// which is what `ensure_schema()` with keys into another schema needs first.
/// Run it on a connection opened without a database, or with any other one.
///
/// `collation` is the server's own name, `utf8mb4_0900_bin` say, and the
/// character set is the one it belongs to. Empty creates the database
/// `CHARACTER SET utf8mb4` under that set's default collation, and leaves an
/// existing one's default alone. A database already there under another
/// default is altered to this one, which changes only the default -- no table
/// in it and no column is touched.
///
/// **The database default does not reach the tables `ensure_schema()`
/// creates.** Each is created `DEFAULT CHARSET=utf8mb4`, and MySQL gives a
/// table that names its character set that set's default collation, not the
/// database's. `Model::default_collation()` is what sets theirs.
///
/// \throws schema_error on SQLite, where a database is a file and opening it
///         creates it; when the name cannot be an identifier; when the server
///         has no such collation; and inside a transaction the caller opened,
///         which the implicit commit of CREATE DATABASE would end.
EnsureDatabaseResult ensure_database(driver::Connection& connection, std::string_view name,
                                     std::string_view collation = {});

/// What `copy_schema()` copied.
struct CopySchemaResult {
    /// The base tables created in the new schema.
    std::size_t tables = 0;

    /// The rows written into them, every table's together.
    std::int64_t rows = 0;
};

/// Copy a schema into a new one: every base table, with its rows, as one
/// consistent snapshot of `from` had them. MySQL and MariaDB only.
///
/// `to` is created with `from`'s default character set and collation, and each
/// base table of `from` in it with `CREATE TABLE to.t LIKE from.t` -- all of
/// them first, since each commits implicitly. Then, in one transaction at
/// REPEATABLE READ whatever the session's own level, the snapshot is opened
/// and each table's rows are read from it -- plain SELECTs, consistent reads
/// that take no lock, so the live tables' writers are neither blocked nor
/// seen: a row written after the snapshot opened is not in the copy -- and
/// written into the copy through the client, as `insert_rows()` writes, a page
/// of at most 2,000 rows at a time. A table with a primary key of numbers,
/// strings, dates or times -- or MariaDB's UUID, INET4 and INET6 -- is read in
/// pages in key order; one without, or keyed on an ENUM or a SET, or on a
/// prefix of any column (as a TEXT or BLOB key always is), is read whole, and
/// holds its rows in the client while they are written. The session's
/// `sql_mode` and `time_zone` are set for the copy and put back after it:
/// `STRICT_ALL_TABLES`, so a value the copy cannot hold as it was fails the
/// copy rather than be truncated into it, with `ALLOW_INVALID_DATES` and
/// without `NO_ZERO_DATE`, so a zero date or a zero in a date an older mode
/// let in is copied as it is; `NO_AUTO_VALUE_ON_ZERO`, so a key of 0 is copied
/// as 0; and UTC, so a TIMESTAMP is read and written as itself. One value no
/// strict insert takes is an ENUM's error value -- '', index 0, which a
/// non-strict insert stores for a value the ENUM does not list, and which
/// neither server takes back strictly as '', as 0 or otherwise. A row holding
/// one is written on its own, without strict mode for that statement alone
/// (MySQL's `SET_VAR` hint, MariaDB's `SET STATEMENT`) and with the integer 0
/// -- the error value's index, where '' may be a member -- and the copy fails
/// unless the statement drew exactly one warning for each error value in it
/// (`Connection::warning_count()`). A SET's '' is its empty set, which copies
/// strictly.
///
/// Not `INSERT INTO to.t SELECT * FROM from.t`, which reads `from.t` with
/// shared locks on every row it passes -- the live server's writers wait for
/// the copy -- and from no snapshot but each statement's own, so no two tables
/// agree.
///
/// **What the copy holds.** Each table's columns -- a generated one as its
/// expression, recomputed from the copied columns -- its indexes, its CHECK
/// constraints (on MySQL under names of the server's making, `t_chk_1`; MariaDB
/// keeps them), its engine, character set, collation and comment, and its
/// rows. **Not** its foreign keys, which `LIKE` leaves out (the index each
/// stood on is copied), nor its triggers, nor the AUTO_INCREMENT counter: the
/// copy's is the next after its highest key, as a reload's would be. Views,
/// MariaDB sequences, routines and events are not copied; a MariaDB
/// system-versioned table is refused, its history being the server's to write.
/// So, by name and before `to` is created, is a table of an engine other than
/// InnoDB, MyISAM, Aria, MEMORY and ARCHIVE -- every other engine, RocksDB,
/// TokuDB, S3 and ColumnStore among them, and one the catalog does not name:
/// `LIKE` of a MERGE table keeps its union of the source's own tables, so the
/// copy's rows would be written into the live ones; a FEDERATED, CONNECT or
/// SPIDER table keeps its rows elsewhere, and a BLACKHOLE one none.
///
/// **A table no transaction covers has no snapshot.** One of MyISAM, Aria,
/// MEMORY or ARCHIVE is copied as it is when it is read, not as it was when
/// the snapshot opened, and its rows need not agree with the InnoDB tables'.
/// A table altered after the snapshot opened fails the copy (MySQL error
/// 1412), rather than be copied as neither version. The rows of each table
/// are held by the transaction until it commits: a long copy keeps the undo of
/// every write since on the server, as `mysqldump --single-transaction` does,
/// and an ALTER on a table it has read waits for it. **The copy is one
/// transaction**: with the binary log on, every row written is logged at the
/// commit as one transaction, which a large copy can take past
/// `max_binlog_cache_size` -- failing the copy -- and which reaches a replica
/// as one transaction to apply.
///
/// A failure after `to` was created drops it again -- unless the connection is
/// what failed: a connection lost mid-copy cannot drop anything, and leaves
/// the part of `to` it had made, which is the caller's to drop. Values travel
/// through the client in the connection's character set, which is utf8mb4
/// unless `MySqlOptions::charset` says otherwise.
///
/// \throws schema_error on SQLite -- whose database is one file, copied whole
///         by the online backup API (`sqlite3_backup_init()`) or `VACUUM INTO`
///         -- inside a transaction the caller opened, for a name that cannot be
///         a schema's, for a `from` the server does not have, and for a `to`
///         it already has; whatever the server raises otherwise.
CopySchemaResult copy_schema(driver::Connection& connection, std::string_view from,
                             std::string_view to);

/// Whether the server has a database -- a schema -- by this name.
///
/// For a deployment that must know before it moves anything: whether a
/// schema was created by an earlier start, say, or has yet to be. Compared as
/// the server compares database names, so under `lower_case_table_names` = 1
/// the case does not matter. Allowed inside a transaction; it only reads.
///
/// \throws schema_error on SQLite, where a connection is one database.
[[nodiscard]] bool schema_exists(driver::Connection& connection, std::string_view name);

/// Whether a table by this name exists.
///
/// A base table only -- on MariaDB a system-versioned one counts. A view and
/// a session's temporary table answer no: the one is not a table, and the
/// other belongs to the session rather than to the schema. Schemata's own
/// bookkeeping tables are base tables, and answer yes, as does SQLite's
/// `sqlite_sequence`.
///
/// Names compare as the engine compares them: without regard to case on
/// SQLite, and on the MySQL family as `lower_case_table_names` says. One
/// catalog query, allowed inside a transaction.
///
/// \param schema the schema to look in, MySQL and MariaDB only. Empty is the
///        session's own; a connection with no default schema needs one named.
/// \throws schema_error for a schema on SQLite, or for none on a MySQL-family
///         connection that has no default schema.
[[nodiscard]] bool table_exists(driver::Connection& connection, std::string_view table,
                                std::string_view schema = {});

/// The table holding Schemata's own bookkeeping.
///
/// Excluded from every snapshot, so the differ never proposes dropping it.
///
/// Internal whole: its keys, its values and whether it exists at all are the
/// library's, and nothing outside it should read or write the table.
/// `recorded_fingerprint()` reads what it records of the model. Clearing it is
/// not a supported way to have `ensure_schema()` read the live schema -- set
/// `DiffOptions::ignore_fingerprint`.
inline constexpr std::string_view meta_table = "__schemata_meta";

/// Read the recorded model fingerprint, or an empty string when absent.
///
/// Once `ensure_schema()` has brought a schema in line, this is the model's
/// `Model::fingerprint(connection.dialect())` -- a model with keys into other
/// schemas included. The dialect is part of it because some properties are
/// hashed only on the engines that emit them -- STRICT, the collation
/// mappings and default, and on SQLite the integer widths, `real32`, a date's
/// type, a timestamp's digits and a blob's length and padding; see
/// `Model::fingerprint()`. So compare this with a fingerprint taken for the
/// same engine and not with one taken for another.
/// Where those schemas were bound is recorded as a row of its own in
/// `__schemata_meta`, never folded into this, so one model bound in two
/// environments of the one engine records one fingerprint in both.
///
/// `schema` reads another schema's than the session's own, leaving the session
/// where it was. Naming one needs the connection to have a default schema of
/// its own to go back to.
///
/// Empty after a run that deferred a foreign key or a CHECK
/// (`EnsureResult::deferred_foreign_keys`, `EnsureResult::deferred_checks`):
/// such a run clears what was
/// recorded, since the database is in line with no model until the key is
/// added.
[[nodiscard]] std::string recorded_fingerprint(driver::Connection& connection,
                                               std::string_view    schema = {});

} // namespace schemata
