#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include <schemata/sql/dialect.h>

namespace schemata {

/// Base for every error Schemata raises.
///
/// Schemata reports failure through exceptions rather than a result type.
/// C++20 has no std::expected, so a result type would have to be
/// hand-rolled, and it would then appear in the return type of every
/// function that can fail -- which is most of them.
///
/// The usual argument against exceptions here is that a constraint
/// violation is an expected outcome rather than an exceptional one. That is
/// true, and the answer is API shape rather than return types: operations
/// whose failure is routine are spelled so that failure is not an error at
/// all. find() returns std::optional<T> rather than throwing when a row is
/// absent. Only genuine faults -- a dropped connection, a violated
/// constraint, a malformed migration -- raise.
///
/// The hierarchy is fine-grained so that catching precisely is possible
/// without inspecting strings or native codes.
class error : public std::runtime_error {
public:
    explicit error(const std::string& what) : std::runtime_error(what) {}
};

/// A failure reported by the database server or client library.
///
/// Carries the native error code and SQLSTATE alongside the message,
/// because the normalisation into the subclasses below is necessarily
/// incomplete: engines invent codes faster than any mapping can track them.
class database_error : public error {
public:
    database_error(const std::string& what,
                   Dialect            dialect,
                   int                native_code,
                   std::string        sqlstate = {})
        : error(what),
          dialect_(dialect),
          native_code_(native_code),
          sqlstate_(std::move(sqlstate)) {}

    /// Which engine produced this.
    [[nodiscard]] Dialect dialect() const noexcept { return dialect_; }

    /// The engine's own error number, unmapped.
    [[nodiscard]] int native_code() const noexcept { return native_code_; }

    /// Five-character SQLSTATE, when the engine supplied one. SQLite does
    /// not, so this is empty there.
    [[nodiscard]] const std::string& sqlstate() const noexcept { return sqlstate_; }

    /// Whether the same work, run again unchanged, may succeed: true for a
    /// `deadlock` and a `lock_timeout` -- except SQLite's plain
    /// `SQLITE_LOCKED`, a conflict within the connection itself that no wait
    /// ends (see `lock_timeout`) -- and false for every other failure, which a
    /// retry would meet again.
    ///
    /// Which work is not the same for the two. A `deadlock` rolled the whole
    /// transaction back: retry the transaction from its first statement, not
    /// the one that failed. A `lock_timeout` on the MySQL family failed the
    /// statement alone under the default `innodb_rollback_on_timeout=0`, and
    /// the transaction is still open with its earlier work in it -- retry the
    /// statement, or roll back and retry the transaction; under `=1` it
    /// rolled the transaction back, as a deadlock does. On SQLite the
    /// statement did not run: retry it, except inside a transaction that has
    /// read, whose snapshot another connection's commit can have made stale
    /// (`SQLITE_BUSY_SNAPSHOT`), where only rolling back and retrying the
    /// transaction can succeed. `driver::Connection::transaction_loss()` says
    /// whether a transaction `begin()` opened was rolled back.
    ///
    /// Outside a transaction a statement is its own, and either is cured by
    /// running the statement again. MariaDB's 1020, a write that lost to
    /// another under `innodb_snapshot_isolation`, rolls the transaction back
    /// too and is a plain `query_error`: not a lock the work waited for, and
    /// answered false here, though retrying the transaction may well succeed.
    [[nodiscard]] virtual bool retryable() const noexcept { return false; }

private:
    Dialect     dialect_;
    int         native_code_;
    std::string sqlstate_;
};

/// Could not establish or maintain a connection.
///
/// What connecting refuses -- a wrong password, a database the options name
/// that is not there, a certificate that does not verify -- and a connection
/// lost under a statement. A statement the server refuses for want of a
/// privilege, or for naming a database that is not there (MySQL's 1044, 1045
/// and 1049), is a `query_error` instead: the connection is still there to run
/// the next one, and reconnecting would only be refused again.
class connection_error : public database_error {
public:
    using database_error::database_error;
};

/// A statement failed for a reason that is not a constraint violation:
/// syntax, a missing table, a type mismatch, a permission problem.
class query_error : public database_error {
public:
    using database_error::database_error;
};

/// A statement waited for a lock another connection held, and gave up.
///
/// SQLite's `SQLITE_BUSY` and `SQLITE_LOCKED`, in every extended form -- the
/// first once the connection's busy timeout has run out, the second at once,
/// since no timeout applies to it -- and error 1205 on MySQL and MariaDB, once
/// `innodb_lock_wait_timeout` has. The statement did not run, so this is the
/// one failure that trying again unchanged can cure, which is why it has a
/// type of its own.
///
/// A subclass of `query_error`, which is what these were raised as before, so
/// a handler catching that still catches them. It says why the statement
/// failed and nothing about the transaction around it: under the MySQL
/// family's default `innodb_rollback_on_timeout=0` a 1205 leaves the
/// transaction open with its earlier work in it, under `=1` it rolls the
/// transaction back, and `Connection::transaction_loss()` is what tells the
/// two apart. A deadlock (1213) is not one of these; it is a `deadlock`.
///
/// SQLite has no deadlock of its own to report. Two transactions that each
/// read and then both write are its nearest case, and the one that cannot
/// have the write lock is refused at once with `SQLITE_BUSY` -- the busy
/// timeout is not waited out, since waiting could not end it -- which is one
/// of these.
///
/// `retryable()` is true for each, but one: SQLite's plain `SQLITE_LOCKED`
/// (native code 6, no extended form). That is a conflict within the
/// connection itself -- a `DROP TABLE` while a read of the table is still
/// open on it -- which no other connection will end, so running the
/// statement again meets it again until the caller finishes or resets the
/// read. `SQLITE_LOCKED_SHAREDCACHE`, a lock another connection of a shared
/// cache holds, is retryable.
class lock_timeout : public query_error {
public:
    using query_error::query_error;

    [[nodiscard]] bool retryable() const noexcept override {
        // SQLITE_LOCKED, which the connection itself holds.
        constexpr int sqlite_locked = 6;
        return !(dialect() == Dialect::sqlite && native_code() == sqlite_locked);
    }
};

/// The engine found two transactions each waiting for a lock the other held,
/// and ended this one to let the other go on: MySQL and MariaDB error 1213
/// (SQLSTATE 40001), InnoDB's deadlock victim.
///
/// **The whole transaction was rolled back**, not the statement alone: every
/// statement since `begin()` is undone, `Connection::transaction_loss()` says
/// `rolled_back`, and each statement sent now is refused until the connection
/// is rolled back. Retry the transaction from its start; `retryable()` says
/// true. Outside a transaction the statement alone was undone.
///
/// A subclass of `query_error`, which is what a deadlock was raised as before,
/// so a handler catching that still catches it, and one reading
/// `native_code() == 1213` still reads 1213. SQLite raises none: see
/// `lock_timeout`.
class deadlock : public query_error {
public:
    using query_error::query_error;

    [[nodiscard]] bool retryable() const noexcept override { return true; }
};

/// A write was rejected by a constraint.
///
/// Normalised across engines: the same logical violation raises the same
/// C++ type whether it came from SQLite, MySQL or MariaDB, even though the
/// three report entirely unrelated numbers for it.
class constraint_violation : public database_error {
public:
    using database_error::database_error;
};

class unique_violation : public constraint_violation {
public:
    using constraint_violation::constraint_violation;
};

class foreign_key_violation : public constraint_violation {
public:
    using constraint_violation::constraint_violation;
};

class not_null_violation : public constraint_violation {
public:
    using constraint_violation::constraint_violation;
};

class check_violation : public constraint_violation {
public:
    using constraint_violation::constraint_violation;
};

/// A query could not be built, so nothing was sent to the server.
///
/// Distinct from `query_error`, which is the server rejecting a statement:
/// this is Schemata rejecting one first, because a predicate named a member
/// the model does not map, an ordering named a table the query does not
/// reach, or the entity was never registered. It carries no native code
/// because no engine was involved.
class query_build_error : public error {
public:
    using error::error;
};

/// A value read from the database is not one the C++ type it maps to can
/// hold.
///
/// Not the query's fault, which ran, nor the server's, which returned what
/// it holds: the stored value itself cannot be read as asked -- a date column
/// holding `0000-00-00`, or a timestamp past the year 9999 that something
/// other than Schemata wrote. Raised by `parse_timestamp()` and `parse_date()`
/// on text that is not one, and by the materialiser, which adds the column
/// it was reading.
///
/// Its own type rather than `query_build_error`, because something did reach
/// the server, and rather than a `database_error`, because nothing there went
/// wrong. It carries no native code for the same reason.
class conversion_error : public error {
public:
    using error::error;
};

/// The live schema is not what the model expects, and healing it was not
/// permitted -- either because verify_schema() only reports, or because the
/// diff contained a destructive operation that was not opted into.
class schema_error : public error {
public:
    using error::error;
};

/// Which rule a migration list broke. See `migration_history_error`.
enum class MigrationRefusal {
    /// A migration that has run completely has different statements now.
    changed,
    /// A migration that stopped partway has different statements now.
    partial,
    /// A migration that has never run sorts before one that has.
    out_of_order,
    /// A migration that has never run has the id of one that has, in another
    /// case: almost always the one that ran, renamed.
    case_twin,
    /// Two migrations in the list share an id, or differ only in case.
    duplicate,
    /// A migration cannot be recorded or run as given: no id, an id too long
    /// for the history table, or a statement string holding two statements.
    malformed,
    /// The database has recorded a migration that nothing given names: its id
    /// is neither in the list nor in `MigrateOptions::known_ids`. Almost
    /// always a migration file deleted or renamed after it ran -- or a
    /// database a newer build has migrated. Raised only where
    /// `MigrateOptions::refuse_missing` asks for it.
    missing,
};

/// `migrate()` refused a migration list, before running any of it.
///
/// A `schema_error`, as every such refusal was before it had a type of its
/// own, so code that catches that still does. What a caller needs to act on
/// it -- name the file, wait, retry, or tell someone -- is here rather than
/// only in the text.
///
/// `MigrationRefusal` may gain an enumerator in a later release, as it gained
/// `missing`: a `switch` over `kind()` wants a `default`.
class migration_history_error : public schema_error {
public:
    migration_history_error(const std::string& what, MigrationRefusal kind, std::string id,
                            std::string recorded_checksum = {},
                            std::string current_checksum  = {})
        : schema_error(what),
          kind_(kind),
          id_(std::move(id)),
          recorded_checksum_(std::move(recorded_checksum)),
          current_checksum_(std::move(current_checksum)) {}

    [[nodiscard]] MigrationRefusal kind() const noexcept { return kind_; }

    /// The migration refused. Empty only for one with no id. For `missing`
    /// it is the recorded id nothing names -- the lowest, where there are
    /// several; `missing_migrations()` lists them all.
    [[nodiscard]] const std::string& id() const noexcept { return id_; }

    /// For `changed` and `partial`: the checksum the database recorded, and
    /// what the migration hashes to now. Empty otherwise.
    [[nodiscard]] const std::string& recorded_checksum() const noexcept {
        return recorded_checksum_;
    }
    [[nodiscard]] const std::string& current_checksum() const noexcept {
        return current_checksum_;
    }

private:
    MigrationRefusal kind_;
    std::string      id_;
    std::string      recorded_checksum_;
    std::string      current_checksum_;
};

/// `migrate()` was asked to run where it cannot run safely: inside a
/// transaction the caller opened, whose implicit DDL commit on the MySQL
/// family would end that transaction without a word.
///
/// Narrower than its name. A migration list or recorded history that does
/// not hold together is a `schema_error`, like every other refusal to change
/// a schema, and a statement the engine rejects is the engine's own
/// `query_error`.
///
/// Also what `migrations_from_directory()` raises for a directory or file it
/// cannot read, and what a migration it listed raises when loaded to run if
/// the file changed on disk since it was listed.
class migration_error : public error {
public:
    using error::error;
};

} // namespace schemata
