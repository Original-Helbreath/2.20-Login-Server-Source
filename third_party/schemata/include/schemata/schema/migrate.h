#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <schemata/driver/driver.h>

namespace schemata {

/// One named, ordered change to the database.
///
/// Migrations exist for what a differ can never work out on its own. A differ
/// sees two schemas and no history, so a rename is indistinguishable from a
/// drop plus an add, and a backfill is invisible to it entirely. A column
/// rename the model can say itself: `ColumnBuilder::renamed_from` has
/// `ensure_schema()` rename the column in place, rows kept, with no migration
/// at all. A backfill, and any move of data between columns or tables, is
/// written by hand, run once, in a fixed order, and recorded.
///
/// **Moving data out of columns the model drops: expand, then contract.**
/// No single pass can both move the rows and drop the columns they come from,
/// since a `before_schema` migration runs before the new shape exists and an
/// `after_schema` one after the old columns are gone. So the move takes two
/// releases:
///
///   - Release N, *expand*: the model adds the new table or columns and keeps
///     the old ones. A CHECK the new rows would break is retired in the same
///     release (`EntityBuilder::retire_check`; dropping a CHECK is never
///     destructive). An `after_schema` migration backfills the new shape from
///     the old columns, which are still there to read.
///   - Release N+1, *contract*: the model drops the old columns. That is a
///     destructive change, opted into for that run once the backfill has run
///     everywhere.
///
/// Helbreath's shop prices, for example: `items.buy_price` and
/// `shop_items.gold_price` become rows of a new `shop_item_costs` table, gold
/// as `currency_item_id` 1. Release N declares `shop_item_costs`, keeps both
/// price columns and retires the CHECK `currency_item_id <> 1`; its
/// `after_schema` migration copies the prices across with `INSERT INTO
/// shop_item_costs (...) SELECT ..., 1, gold_price FROM shop_items`, and the
/// same from `items.buy_price`. Release N+1 removes `buy_price` and
/// `gold_price` from the model.
///
/// There is no hook for data writes in the middle of the schema pass, between
/// the add and the drop. It would put data writes inside the DDL pass, where a
/// failure leaves states no release describes, and on MySQL and MariaDB every
/// DDL statement commits on its own, so nothing could roll a half-moved table
/// back. The `after_columns` phase is not such a hook: it runs between two
/// whole `ensure_schema()` passes, the first of which holds back the CHECKs
/// and keys the backfill is for (see `MigrationPhase::after_columns`).
///
/// Everything a differ *can* work out stays with `ensure_schema()`, which is
/// why a migration here is plain SQL rather than a second schema definition.
/// The usual shape of a startup is both, in order: migrations first for the
/// steps only a person could know about, then `ensure_schema()` to build and
/// heal the rest. A migration that needs the schema to be there first -- a
/// seed -- says so with its `phase`, and runs in a second pass after it.
///
/// When a migration runs, relative to `ensure_schema()`.
enum class MigrationPhase {
    /// Whenever `migrate()` is called, in any pass, and on a new database
    /// like any other. What every migration was before phases existed.
    any,

    /// Before `ensure_schema()`, to carry an existing database's rows into the
    /// shape the model is about to take: a column replaced by one computed
    /// from a join, say, which no differ can do because it cannot know the
    /// join. On a database with no tables yet there are no rows to carry and
    /// no old shape to carry them from, so there it is recorded as run without
    /// running -- `ensure_schema()` builds the new shape directly. "No tables"
    /// is asked again once an earlier migration in the same call has run, so
    /// a list that builds the old shape and then reshapes it carries its rows
    /// as an upgrade would.
    before_schema,

    /// After `ensure_schema()`: seeds and backfills that need the model's
    /// tables and columns to exist.
    after_schema,

    /// Between the two: after `ensure_schema()` has added the model's new
    /// columns, and before the CHECKs and foreign keys on those columns that
    /// the rows already there would break at the column's default. A backfill
    /// sets those rows first: a column added `DEFAULT 0` that existing rows
    /// must hold 1 in, under a new `CHECK (usage NOT IN (1,2,3) OR
    /// blend_mode = 1)`. Under `DiffOptions::hold_back_for_backfill`, which
    /// `migrate_and_ensure()` sets when a migration of this phase is pending,
    /// `ensure_schema()` holds such a constraint back
    /// (`EnsureResult::deferred_checks`, `EnsureResult::deferred_foreign_keys`)
    /// rather than refusing it, and the next `ensure_schema()`, after this
    /// pass, adds it -- put to the
    /// rows then as any new constraint is, and refused if a row still breaks
    /// it. `migrate_and_ensure()` runs the passes in order: before_schema,
    /// the schema, after_columns, the schema again when something was held
    /// back, the caller's `StartupOptions::before_seed` hook when set,
    /// after_schema. On a new database it runs like any other, against
    /// tables that have no rows yet.
    ///
    /// Declared last so the values of the three before it stay what they
    /// were; the order of the passes is not the order of the values.
    after_columns,
};

struct Migration {
    /// Identity and order in one. Sorted as text, so a numeric prefix is the
    /// usual shape: "0001_initial", "0002_split_display_name".
    ///
    /// It is recorded in the database, so it must not change once a
    /// migration has run anywhere.
    std::string id;

    /// What this migration is for, in a form fit for a log. Not recorded.
    std::string description;

    /// Statements to run, in order.
    ///
    /// May be empty: the migration then runs nothing and is recorded as run.
    std::vector<std::string> statements;

    /// Where the migration came from, in a form fit for a message: a file
    /// path, usually. Not recorded and not hashed; quoted beside the id in
    /// every refusal about this migration, so the message names the file to
    /// open. Empty says nothing.
    std::string origin{};

    /// Which pass it runs in. See `MigrationPhase`; not hashed, so giving a
    /// migration that has run a phase does not make it an edit.
    MigrationPhase phase = MigrationPhase::any;

    /// A line for whoever reads the history table later: why it ran, the
    /// ticket it belongs to. Recorded when the migration is first started, at
    /// most `max_migration_note_length` bytes; not hashed, so a note may be
    /// added or reworded without the migration reading as edited.
    std::string note{};

    /// What the migration's content hashes to, worked out by the caller --
    /// a hash of the file it is read from, say. When set, this rather than the
    /// statements is what the history records and compares, so a migration
    /// that has run need never be read again to be checked: change the
    /// content, change the hash, and the edit is refused as ever.
    ///
    /// A migration recorded under its statements' hash, before it had one of
    /// these, is moved over quietly the first time: its statements are hashed
    /// once more, and where they match what was recorded the record takes the
    /// new hash instead of reading as edited.
    ///
    /// Hashed, never stored as given: the history records a hash of the id
    /// and this, so it may be of any length and hold any bytes -- a hex
    /// digest, a file's size and time, a version string. All that is ever
    /// asked of it is whether it is what it was when the migration ran.
    std::string content_hash{};

    /// Produces the statements, for a migration too large to hold in every
    /// list: called only when the migration has to run, or once to move a
    /// record over as above. Needs `content_hash`, since there are no
    /// statements to hash without calling it, and leaves `statements` empty.
    /// What it returns is held to the same rules as `statements`, before any
    /// migration of the run starts.
    ///
    /// It may return no statements, and the migration is then recorded as run
    /// having run nothing, as one with an empty `statements` is.
    ///
    /// If it throws, the exception leaves `migrate()` as it was thrown, and no
    /// migration of that call has run: every loader the run needs is called
    /// before the first migration starts -- by a dry run too -- so a failed
    /// load applies nothing and records no migration. (On the MySQL family the
    /// history table may have been created or brought up to date first.) The
    /// one loader called later is a `before_schema` migration's on a database
    /// that had no tables when the call began, which is loaded only once an
    /// earlier migration of the same call has built some; if that one throws,
    /// the earlier migrations stay applied on the MySQL family and are rolled
    /// back with the rest of the run on SQLite. Never called by
    /// `pending_migrations()` or `migration_checksum()`.
    std::function<std::vector<std::string>()> load_statements{};
};

/// A migration as the database has it.
struct AppliedMigration {
    std::string id;

    /// When it was first started, as `YYYY-MM-DD HH:MM:SS.ffffff` in UTC.
    std::string applied_at;

    /// The hash of the statements as they were when it ran.
    std::string checksum;

    /// How many statements have been recorded as run.
    ///
    /// Only ever 0 or all of them on SQLite, where the whole run is one
    /// transaction, and for a data-only migration on the MySQL family, which
    /// runs as one. Otherwise on the MySQL family DDL commits as it goes, so this is
    /// what lets a migration that died partway pick up where it stopped. It
    /// counts statements *recorded*, which trails statements *executed* by at
    /// most one -- see the note on `migrate()`.
    std::size_t steps = 0;

    bool complete = false;

    /// `Migration::note` and `MigrateOptions::applied_by` as they were when it
    /// was first started. Empty where none was given, and for a migration
    /// recorded before the history table had room for them.
    std::string note{};
    std::string applied_by{};
};

struct MigrateOptions {
    /// Report what would run without running any of it.
    bool dry_run = false;

    /// Accept a migration whose statements no longer hash to what was
    /// recorded.
    ///
    /// Off by default. A changed checksum means the database and the source
    /// have quietly diverged: the database holds what the migration used to
    /// say, and nothing will ever apply the difference.
    ///
    /// A migration that had stopped partway starts again from its first
    /// statement, because the recorded step count counted statements of a
    /// version that no longer exists.
    bool allow_changed = false;

    /// Accept a migration whose id sorts before one that has already run.
    ///
    /// Off by default. Two people branching at the same time is the ordinary
    /// way this happens: one writes `0005_add_index`, the other
    /// `0006_drop_column`, and whichever merges second would otherwise run
    /// out of order on a database the first has already changed -- leaving a
    /// state a fresh build would never produce.
    bool allow_out_of_order = false;

    /// How long to wait for another process that is already migrating.
    ///
    /// Migrations take a lock for the whole run, so two application replicas
    /// starting at once cannot both apply the same backfill. This is how long
    /// the second one waits before giving up. Zero fails immediately, which
    /// is what a deploy that would rather retry wants, and a negative value is
    /// read as zero -- passed through, MySQL would wait for ever on it and
    /// MariaDB would fail at once.
    ///
    /// The MySQL family only. SQLite's lock is the database's own write lock,
    /// so how long a second process waits for it is the connection's
    /// `busy_timeout` rather than anything settable here.
    int lock_timeout_seconds = 30;

    /// The schema to migrate, when it is not the session's own. Empty is the
    /// session's. A physical name: `migrate()` takes no bindings, so an alias
    /// is not resolved here.
    ///
    /// The history table, the lock and every statement are that schema's, and
    /// the session's default schema is what it was afterwards. A schema the
    /// server does not have is refused by name. MySQL and MariaDB only, and the
    /// connection needs a default schema of its own to go back to -- MySQL
    /// cannot put a session back to having none. Any schema will do: a
    /// process that creates every schema it uses may connect with
    /// `information_schema` as its default.
    std::string schema{};

    /// The pass to run: only pending migrations of this phase, and those of
    /// `MigrationPhase::any`. Unset, or `any`, runs every pending migration,
    /// whatever its phase, in id order.
    ///
    /// The phased startup is three calls, with the whole list each time:
    ///
    ///     migrate(db, all, {.phase = MigrationPhase::before_schema});
    ///     ensure_schema(db, model);
    ///     migrate(db, all, {.phase = MigrationPhase::after_schema});
    ///
    /// followed by a second `ensure_schema()` where the model adds a foreign
    /// key into a table the first one creates: that key waits for the seed
    /// (`EnsureResult::deferred_foreign_keys`).
    ///
    /// Where the model adds a column together with a CHECK or a foreign key
    /// on it that the rows already there would break at the column's
    /// default, an `after_columns` pass backfills the rows in between, and a
    /// second `ensure_schema()` adds what the first held back
    /// (`EnsureResult::deferred_checks`). The first must be asked to hold back
    /// (`DiffOptions::hold_back_for_backfill`); without it, it refuses:
    ///
    ///     migrate(db, all, {.phase = MigrationPhase::before_schema});
    ///     ensure_schema(db, model, hold);  // hold.hold_back_for_backfill = true
    ///     migrate(db, all, {.phase = MigrationPhase::after_columns});
    ///     // WARNING: refuses a key the first run deferred for the seed;
    ///     // migrate_and_ensure() keeps it waiting.
    ///     ensure_schema(db, model);  // adds what the first one held back
    ///     migrate(db, all, {.phase = MigrationPhase::after_schema});
    ///     ensure_schema(db, model);  // adds a key waiting for the seed
    ///
    /// `migrate_and_ensure()`, in `<schemata/schema/manage.h>`, is all of it
    /// in one call, each extra `ensure_schema()` only when something was held
    /// back -- and it is the way to run a model that holds back both kinds at
    /// once: its middle pass keeps waiting for the seed a key the first one
    /// deferred for it, which a bare `ensure_schema()` would put to the empty
    /// table and refuse. A write the seed depends on that is not a migration
    /// goes in its `StartupOptions::before_seed` hook, not between hand-run
    /// phases.
    ///
    /// The order rule -- a new migration may not sort before one that has
    /// run -- holds within a phase, since that is the order a new database
    /// runs them in: every before_schema one, then the schema, then every
    /// after_columns one, then every after_schema one. A recorded migration
    /// the list does not include counts for all of them.
    std::optional<MigrationPhase> phase{};

    /// Who or what is running this: a deploy id, a host, a user. Recorded
    /// beside each migration this call starts, at most
    /// `max_migration_note_length` bytes. Empty records nothing.
    std::string applied_by{};

    /// Refuse a database that has recorded a migration nothing given names.
    ///
    /// Off by default, and off `migrate()` is what it was: a recorded id the
    /// list leaves out is not looked at, which is what lets a baseline stand
    /// in for the migrations it replaced. It is also what lets a migration
    /// file be deleted after it ran with nothing noticing -- the more
    /// destructive half of breaking an append-only history. An edit is refused
    /// as `changed`; a delete, without this, leaves a database holding what
    /// the file did and a source tree that no longer builds it, and every run
    /// reports nothing pending.
    ///
    /// On, every row of the history table must be named: by a migration in the
    /// list, or by an id in `known_ids`. One that is neither is refused with
    /// `migration_history_error` of kind `MigrationRefusal::missing`, before
    /// anything runs, in a dry run as in a real one, and in every pass of a
    /// phased startup alike -- each pass is handed the whole list, and the
    /// check is of the list, not of the pass.
    /// An empty list is checked too, where otherwise it returns at once: a
    /// directory emptied of its files is the case in point. For a list that
    /// is not empty the check reads the history `migrate()` reads anyway, and
    /// adds no round trip; an empty list reads the history, which it
    /// otherwise would not.
    ///
    /// A listed migration that has run and since gained a `content_hash` may
    /// have its loader called before the refusal, as before any refusal of
    /// this kind: its record is moved to the new hash first. Nothing runs and
    /// nothing is written.
    ///
    /// **Which id.** `id()` is the lowest recorded id nothing names, and the
    /// message says how many more there are; `missing_migrations()` returns
    /// them all.
    ///
    /// **Bytes, not case.** Ids are compared byte for byte, as the history is
    /// keyed. A recorded `0004_seed` is not named by `0004_SEED` in
    /// `known_ids`: on a filesystem that tells the two apart they are two
    /// files, and a file renamed by case is a rename -- the message says so
    /// where that is what it found. In the *list*, an id that never ran and
    /// differs from a recorded one only in case is refused first, as
    /// `case_twin`.
    ///
    /// **A migration that stopped partway** and is named by nothing is missing
    /// like any other, and the message says it is unfinished: with no
    /// migration to resume it, the statements it had left will never run.
    ///
    /// **It also refuses a database ahead of the code** -- one a newer build
    /// has migrated, started by an older one -- since its newest ids are in
    /// no list the older build has. That is the rule, not a side effect: the
    /// older build's model is not the one that database was migrated for.
    bool refuse_missing = false;

    /// Recorded ids the caller vouches for though the list does not hold
    /// them, for `refuse_missing`: the migrations a baseline replaced, whose
    /// files are still there -- `DirectoryListing::superseded` -- or the ids a
    /// list squashed by hand used to hold. Read only when `refuse_missing` is
    /// set; otherwise ignored, whatever it holds.
    ///
    /// Only names: an id here is not run, not compared with what was recorded
    /// and not held to any rule of the list -- it may be one no database
    /// recorded, and may repeat one in the list.
    std::vector<std::string> known_ids{};
};

struct MigrateResult {
    /// The ids applied by this call, in the order they ran.
    std::vector<std::string> applied;

    /// Statements actually executed. Zero for a dry run.
    std::size_t statements = 0;

    /// `before_schema` migrations recorded as run without running, because
    /// the database had no tables when they came to run. Not in `applied`.
    /// A dry run judges that once, from the database as it is.
    std::vector<std::string> inert{};
};

/// The longest id the history table can hold, in bytes.
///
/// Ids are compared as bytes on every engine. On the MySQL family the key
/// column is `VARBINARY(760)`: a text column would take the database's
/// collation, under which `0001_cafe` and `0001_café` -- or, on MariaDB,
/// `0003_x` and `0003_x ` -- are one row, so the second of two migrations
/// failed with a duplicate key after the first one's DDL had committed. A
/// history table made by an earlier release, keyed on `VARCHAR(190)`, is
/// converted the next time `migrate()` runs; 760 bytes is what that column
/// could hold, so the conversion never truncates an id already recorded.
///
/// Converting needs the ALTER privilege. A user without it -- one allowed to
/// CREATE and write, which is all a data migration ever needed -- keeps the
/// old key and migrates as every earlier release did, and with it keeps the
/// old key's hazard: two ids the collation calls equal still collide. The
/// first run by a user who may ALTER converts it.
///
/// The limit itself is unchanged. A longer id is accepted by SQLite and
/// rejected by the MySQL family, which is a divergence worth catching before
/// anything has run.
inline constexpr std::size_t max_migration_id_length = 190;

/// The longest `Migration::note` or `MigrateOptions::applied_by` the history
/// table records, in bytes.
///
/// The columns holding them were added in rc.10. A history table made earlier
/// gains them the next time `migrate()` runs; on the MySQL family that needs
/// the ALTER privilege, and a user without it migrates as before, recording
/// neither, until a user who may ALTER runs.
inline constexpr std::size_t max_migration_note_length = 255;

/// The table recording which migrations have run.
///
/// Excluded from every snapshot, so the differ never proposes dropping it.
///
/// A reader outside the library may rely on five of its columns, by name:
/// `id`, `applied_at` (text, `YYYY-MM-DD HH:MM:SS.ffffff`, UTC), `complete`
/// (1 or 0), `note` and `applied_by` (NULL where none was given). Their types
/// may widen. `checksum` and `steps` are internal; `applied_migrations()`
/// reads them. Nothing outside the library should write to it: a row deleted
/// is a migration `migrate()` runs again.
inline constexpr std::string_view migrations_table = "__schemata_migrations";

/// The hash recorded for a migration: of its `content_hash` where it has one,
/// of its statements otherwise.
///
/// Exposed so a tool can show why a migration is reported as changed. Never
/// calls `load_statements`.
[[nodiscard]] std::string migration_checksum(const Migration& migration);

/// Every migration the database has a record of, in id order.
///
/// Returns an empty list when the history table does not exist yet, which is
/// simply the state before the first migration has ever run.
///
/// `schema` reads another schema's history than the session's own, leaving the
/// session where it was -- see `MigrateOptions::schema`. Naming one needs the
/// connection to have a default schema of its own to go back to.
[[nodiscard]] std::vector<AppliedMigration> applied_migrations(driver::Connection& connection,
                                                               std::string_view    schema = {});

/// The ids that have not completely run, in the order they would run in.
///
/// A migration that died partway is pending: it still has statements left.
/// So is one whose history row records no step, which a failed data-only
/// migration leaves on the MySQL family -- see `migrate()`.
///
/// Decided by the history rows alone. It never calls `load_statements`, and
/// it checks nothing `migrate()` would refuse: a migration that has run and
/// been edited since is not pending, and is refused only when `migrate()`
/// runs.
///
/// Ids rather than pointers into `migrations`, so that the commonest way to
/// call this -- passing a list built on the spot -- cannot hand back
/// references to a vector that has already gone.
///
/// `schema` as for `applied_migrations()`, and needing the same default
/// schema to go back to.
[[nodiscard]] std::vector<std::string> pending_migrations(
    driver::Connection& connection, const std::vector<Migration>& migrations,
    std::string_view schema = {});

/// The migrations the database has recorded that nothing given names: every
/// history row whose id is neither in `migrations` nor in `known_ids`, in id
/// order. Empty when every row is named, and when there is no history table.
///
/// The set `MigrateOptions::refuse_missing` refuses, with nothing refused: for
/// a caller that would rather warn than stop, or that wants every id where the
/// refusal names the lowest. It is the same test over the same rows -- ids
/// compared byte for byte, a row not `complete` counted like any other -- so
/// `migrate()` with `refuse_missing` and the same `known_ids` throws
/// `MigrationRefusal::missing` exactly when this is not empty, naming this
/// list's first id. (A list `migrate()` refuses for another reason first -- a
/// duplicate, a `case_twin` -- is not refused here: this checks nothing
/// else.)
///
/// The records rather than their ids, unlike `pending_migrations()`: a pending
/// migration is in the caller's list, which says the rest about it, and a
/// missing one is in nothing the caller has. Its row is all there is to say
/// what was lost -- when it ran, whether it finished, the note and who ran
/// it -- and it is a copy, so nothing here points into a vector that has gone.
///
/// A read, and only of the history: it takes no lock, creates nothing, never
/// calls `load_statements`, and holds the list to none of `migrate()`'s rules.
///
/// `schema` as for `applied_migrations()`, and needing the same default
/// schema to go back to.
[[nodiscard]] std::vector<AppliedMigration> missing_migrations(
    driver::Connection& connection, const std::vector<Migration>& migrations,
    const std::vector<std::string>& known_ids = {}, std::string_view schema = {});

/// Apply every pending migration, in id order.
///
/// **The list need not hold what has run.** Only the migrations given are
/// checked and applied; a recorded migration the list leaves out is neither
/// run nor looked at again, so a project may squash its history into a
/// baseline and hand over only the baseline onward -- which
/// `migrations_from_directory()` does for a directory of `.sql` files. The
/// baseline itself is new to a database that ran what it replaced, and runs
/// there: write it to be harmless where its content is already there. What a
/// recorded migration still does is anchor the order rule: a migration given
/// that has never run and sorts below the highest completed id on record -- in
/// the list or not -- is refused unless `MigrateOptions::allow_out_of_order`,
/// so a baseline is a new id, after every one any database has run. A
/// migration given that has run is compared with what it was, and an edit is
/// refused as ever.
///
/// The same latitude hides a delete: a migration removed from the list after
/// it ran reads exactly as one a baseline replaced. `MigrateOptions::
/// refuse_missing` takes the latitude back for a caller that wants a deleted
/// migration noticed. Every recorded id must then be named, by the list or by
/// `MigrateOptions::known_ids`, and one that is not is refused as
/// `MigrationRefusal::missing` before anything runs. A baseline still works
/// under it, on one condition: **what it replaced must stay nameable** -- the
/// replaced ids passed as `known_ids`, which for a directory means the
/// superseded files stay on disk, where `list_migration_directory()` finds
/// them and reports their ids. Delete a superseded file and every database
/// that ran it is refused. `missing_migrations()` is the same test as a read.
///
/// **Only one process at a time.** The run takes a lock for its whole
/// duration -- `GET_LOCK()` on the MySQL family, the write lock an immediate
/// transaction already holds on SQLite. Without it two replicas starting
/// together both read an unfinished migration, both decide it has no
/// progress, and both apply it: a backfill runs twice and the history looks
/// perfectly clean afterwards. A second caller waits
/// `MigrateOptions::lock_timeout_seconds` and then throws.
///
/// There is no call that asks whether another process holds the lock. On the
/// MySQL family it is a named lock the server keeps, one per schema and the
/// same one `ensure_schema()` takes: `schemata_migrate_` and then the MD5 of
/// the schema's name in hex -- on a server without MD5(), which MySQL 9
/// removed, the first 32 hex digits of its SHA2-256 instead. So an operator
/// can ask
///
///     SELECT IS_USED_LOCK(CONCAT('schemata_migrate_', MD5('hb_world')));
///
/// which answers the holding connection's id, or NULL when nobody holds it.
///
/// **Atomicity.** On SQLite the *entire run* is one transaction, so a failure
/// anywhere leaves the database exactly as it started, history row included.
///
/// On MySQL and MariaDB it cannot be: DDL there causes an implicit commit.
/// Progress is recorded after every statement, and a migration that failed
/// resumes from the statement after the last one *recorded* -- which is not
/// quite the same as the last one that succeeded. A process killed between
/// executing a statement and recording it will run that statement a second
/// time on the next attempt. **Write MySQL-family migration statements so
/// that running one twice is harmless** (`CREATE TABLE IF NOT EXISTS`,
/// `INSERT ... ON DUPLICATE KEY UPDATE`, an `UPDATE` with a guard in its
/// `WHERE`). There is no way to close that window from the client side: a
/// statement and the record of it are two round trips, and DDL will not sit
/// inside a transaction with them.
///
/// **A data-only migration is the exception.** When every statement of one
/// migration reads or writes rows -- INSERT, UPDATE, DELETE, REPLACE, SELECT,
/// WITH, VALUES, TABLE, DO -- nothing in it commits implicitly, so on the MySQL
/// family it runs in one transaction with its completion recorded inside it.
/// A seed that fails at statement 150 of 260 leaves none of its rows and no
/// progress claiming them, and a process killed partway leaves the same.
/// Detected per migration from each statement's leading keyword, read past
/// leading whitespace and comments (`-- ...`, `# ...`, `/* ... */`), so a
/// statement that opens with a comment is judged by the word after it. One
/// DDL statement, CALL, SET or executable comment (`/*! */`, `/*M! */`) puts
/// the whole migration back on the per-statement path. Keep seeds and
/// backfills in migrations of their own to get this, and keep their tables on
/// InnoDB -- a rollback cannot undo rows written to a MyISAM table.
///
/// **What a failed migration leaves in the history.** On the MySQL family a
/// migration's history row is written, and committed, before its first
/// statement runs. So a data-only migration that failed leaves that row
/// behind, `steps` 0 and `complete` false -- `applied_migrations()` lists it
/// and `pending_migrations()` names it -- where SQLite, whose run rolled back
/// whole, leaves no row at all. A re-run treats the two alike: a migration
/// that has recorded no step has no progress for an edit to contradict, so it
/// may be corrected and run again without `allow_changed`, and runs from its
/// first statement as the new version. The same goes for a migration of DDL
/// whose first statement failed. One that recorded a step before it failed
/// resumes after it, and an edit to it is refused -- see `allow_changed`.
///
/// Either way a migration is only marked complete once all of its statements
/// have run.
///
/// **One statement per string.** SQLite would run `"A; B"` as two and the
/// MySQL family would reject it, so it is refused here instead -- that
/// divergence is exactly the kind this library exists to remove. It also
/// keeps the recorded step count meaning what it says.
///
/// \throws migration_error when called inside a transaction the caller
///         opened, before anything is read or written.
/// \throws migration_history_error (a `schema_error`) when two migrations
///         share an id or differ only in case, when a migration has already
///         run with different statements or stopped partway and was edited
///         since, when a new migration sorts before one already applied, when
///         a pending id matches a recorded one except in case, when an id is
///         empty or too long to record, or when a statement string holds more
///         than one statement -- carrying `kind()`, `id()` and, for an edit,
///         both checksums, or when a note is longer than
///         `max_migration_note_length`. Each names the migration's `origin` if
///         it has one. With `MigrateOptions::refuse_missing`, also when the
///         database has recorded an id that neither the list nor `known_ids`
///         names (`MigrationRefusal::missing`, `id()` the lowest such) -- for
///         an empty list and a dry run too.
/// \throws schema_error when `applied_by` is longer than
///         `max_migration_note_length`, when another process holds the lock, when the target
///         schema is refused (see `MigrateOptions::schema`), or when a history
///         row vanishes while its migration runs.
/// \throws query_error (a `database_error`) when a statement fails, carrying
///         the engine's own code and message. Progress up to that statement
///         is kept on the MySQL family and rolled back on SQLite.
MigrateResult migrate(driver::Connection& connection, const std::vector<Migration>& migrations,
                      const MigrateOptions& options = {});

} // namespace schemata
