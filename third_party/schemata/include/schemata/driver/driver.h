#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <schemata/core/error.h>
#include <schemata/sql/dialect.h>

namespace schemata::driver {
class Connection;
} // namespace schemata::driver

namespace schemata::detail {
class TargetSchema;

/// Internal: what a connection has learned from its server, kept until its
/// statement cache is next emptied. Defined where the library uses it.
struct SessionFacts;
[[nodiscard]] SessionFacts& session_facts(driver::Connection& connection);

/// Internal: the most statements a connection's cache keeps, zero where it is
/// off; the most of them that may be a multi-row write's partial statements
/// (`StatementCacheOptions::max_partial_bulk_statements`, and no more than
/// half the cache); whether it keeps one for `sql` right now; and closing
/// that one, if it does.
[[nodiscard]] std::size_t statement_cache_capacity(
    const driver::Connection& connection) noexcept;
[[nodiscard]] std::size_t partial_statement_capacity(
    const driver::Connection& connection) noexcept;
[[nodiscard]] bool statement_is_cached(const driver::Connection& connection,
                                       std::string_view          sql) noexcept;
void drop_cached_statement(driver::Connection& connection, std::string_view sql) noexcept;

/// Internal: whether the server says a transaction is open on this session,
/// whatever the connection's own count holds. False where the driver cannot
/// say.
[[nodiscard]] bool server_has_transaction(const driver::Connection& connection);
} // namespace schemata::detail

namespace schemata::driver {

class Connection;

/// What a statement is, as far as the transaction around it is concerned.
///
/// Read from the leading keyword, past whitespace and comments -- and *into* a
/// MySQL executable comment (`/*!40101 SET autocommit=0 */`), which the server
/// reads as code rather than skipping. Nothing here parses SQL. A kind is only
/// ever used to decide what to tell the caller about a transaction, or to
/// refuse a statement that would take one out of the library's hands.
enum class StatementKind : unsigned char {
    /// SELECT, INSERT, UPDATE, DELETE and the rest of the data statements.
    /// None of them can commit, so where one ends a transaction, the engine
    /// rolled it back.
    rows,

    /// DDL, and the rest of what the MySQL family commits implicitly before
    /// running it: GRANT, LOCK TABLES, FLUSH, ANALYZE and their kin.
    schema,

    /// CALL, and MariaDB's `BEGIN NOT ATOMIC` block. What runs inside is out
    /// of sight, and may commit, roll back, or start a transaction of its own.
    routine,

    /// COMMIT, ROLLBACK, BEGIN, START TRANSACTION, XA, and a SET of this
    /// session's autocommit. `ROLLBACK TO SAVEPOINT` is not one of these: it
    /// leaves the transaction open.
    transaction_control,

    /// Anything else, SAVEPOINT and every other SET among it.
    other,
};

/// Classify `sql` by its leading keyword.
[[nodiscard]] StatementKind statement_kind(std::string_view sql) noexcept;

/// Whether `sql` is a `USE`, read as `statement_kind()` reads its keyword.
///
/// A statement prepared on the MySQL family keeps the default schema it was
/// prepared under, so a connection switching schema empties its statement
/// cache; see `Connection::prepare_cached()`.
[[nodiscard]] bool switches_schema(std::string_view sql) noexcept;

/// Whether `sql` is an ALTER, CREATE, DROP, RENAME or TRUNCATE, read as
/// `statement_kind()` reads its keyword.
///
/// A statement prepared on the MySQL family keeps the names its columns had
/// when it was prepared, so a connection sending DDL empties its statement
/// cache; see `Connection::prepare_cached()`. Narrower than
/// `StatementKind::schema`, which counts GRANT, LOCK TABLES, FLUSH and the rest
/// of what commits implicitly: none of those renames a column.
[[nodiscard]] bool changes_schema(std::string_view sql) noexcept;

/// Whether `sql` is a SELECT, TABLE or VALUES statement: true when its first
/// keyword, past whitespace and comments, is one of the three. A `WITH ...
/// SELECT` or a parenthesised SELECT is not one.
///
/// What a driver may send a second time when the first run's result could
/// not be read. Nothing in one writes to a table, but the server has run it
/// once already, so it runs twice: a SELECT that assigns a user variable,
/// calls a function that writes, or takes `GET_LOCK()` does that twice.
[[nodiscard]] bool is_select(std::string_view sql) noexcept;

/// Whether `sql` is an UPDATE: true when its first keyword, past whitespace
/// and comments, is `UPDATE`. A `WITH ... UPDATE` is not one.
///
/// What a MySQL-family driver reads the server's info line for, to answer
/// `Connection::matched_rows()`.
[[nodiscard]] bool is_update(std::string_view sql) noexcept;

/// What the current row holds in one column, as the engine hands it back.
///
/// For a reader with no model to say what a column is: `select_rows()` and
/// `for_each_row()` in `<schemata/query/rows.h>` build a `Value` of this kind
/// for a table the model does not hold. SQLite answers by the value, which
/// its column's affinity leaves free to differ from row to row; the MySQL
/// family by the column's type, the same for every row but a NULL.
enum class CellKind : unsigned char {
    null,

    /// A signed integer, read with `get_int64()`.
    integer,

    /// An integer of an UNSIGNED column, or a BIT column's value: its bit
    /// pattern is what `get_int64()` returns.
    unsigned_integer,

    /// A FLOAT, DOUBLE or REAL, read with `get_double()`.
    real,

    /// Anything read as text: a string, and on the MySQL family a DECIMAL
    /// (its digits), a date or time, a JSON document, an ENUM or a SET.
    text,

    /// A binary string or a BLOB: bytes, read with `get_blob()`.
    blob,
};

/// How a statement the engine refused left the transaction it ran in.
///
/// Asked only once the server has said the transaction is gone, so this says
/// which way it went rather than whether it went.
enum class TransactionEnding : unsigned char {
    /// The engine rolled it back: a deadlock, a lock-wait timeout under
    /// `innodb_rollback_on_timeout`, MariaDB's snapshot-isolation write
    /// conflict, a trigger's `RAISE(ROLLBACK)`. Nothing in it survived, and
    /// the work can be retried from the start.
    rolled_back,

    /// A DDL statement on the MySQL family committed it before it ran. What
    /// the transaction held is committed, whether the statement then failed
    /// or not.
    implicit_commit,

    /// Something out of the library's sight ended it -- a stored procedure's
    /// own COMMIT, or a script whose earlier statements already ran. Which
    /// way it went cannot be told from here.
    unknown,
};

/// A prepared statement.
///
/// Parameter and column indices are both **zero-based**. SQLite numbers its
/// parameters from one and its columns from zero, in the same API; that
/// inconsistency is normalised away here rather than being passed on.
///
/// Bound values are copied or otherwise made safe by the implementation, so
/// a caller's buffer need not outlive the bind call.
class Statement {
public:
    Statement()                            = default;
    Statement(const Statement&)            = delete;
    Statement& operator=(const Statement&) = delete;
    virtual ~Statement();

    // -- binding ----------------------------------------------------------

    virtual void bind_null(int index)                          = 0;
    virtual void bind(int index, bool value)                   = 0;
    virtual void bind(int index, std::int64_t value)           = 0;

    /// Bind an unsigned 64-bit value.
    ///
    /// Separate from the signed binding because above 2^63 the two disagree
    /// about what the top bit means, and only the driver knows whether its
    /// engine has anywhere to put the difference. The MySQL family does;
    /// SQLite's INTEGER is signed 64-bit and its driver throws rather than
    /// storing a value that would read back negative.
    virtual void bind(int index, std::uint64_t value)          = 0;
    virtual void bind(int index, double value)                 = 0;
    virtual void bind(int index, std::string_view value)       = 0;
    virtual void bind(int index, std::span<const std::byte> value) = 0;

    /// Number of parameters the statement expects.
    [[nodiscard]] virtual int parameter_count() const = 0;

    // -- execution --------------------------------------------------------

    /// Advance to the next row.
    ///
    /// \returns true when a row is available and the column accessors below
    ///          are valid; false when the result is exhausted.
    /// \throws  query_error or constraint_violation on failure.
    [[nodiscard]] virtual bool step() = 0;

    /// Return the statement to its pre-execution state, keeping the
    /// compiled plan. Bindings are cleared.
    virtual void reset() = 0;

    /// Whether the most recent `step()` threw.
    ///
    /// Cleared by the next `step()` that returns. A statement cache closes a
    /// statement that failed rather than keeping it: the error may have left
    /// it in a state no later caller should inherit.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // -- results ----------------------------------------------------------

    [[nodiscard]] virtual int         column_count() const      = 0;
    /// On the MySQL family, the name the column had when the statement was
    /// prepared; see `Connection::prepare_cached()` for one renamed since,
    /// by this connection or by another.
    [[nodiscard]] virtual std::string column_name(int index) const = 0;

    [[nodiscard]] virtual bool         is_null(int index) const    = 0;
    [[nodiscard]] virtual bool         get_bool(int index) const   = 0;
    [[nodiscard]] virtual std::int64_t get_int64(int index) const  = 0;
    [[nodiscard]] virtual double       get_double(int index) const = 0;
    [[nodiscard]] virtual std::string  get_string(int index) const = 0;
    [[nodiscard]] virtual std::vector<std::byte> get_blob(int index) const = 0;

    /// The column's text, as `get_string()` reads it, without a copy.
    ///
    /// **The view lives as long as the row does**: until the next `step()`,
    /// `reset()`, or the statement's destruction -- for one lent by the
    /// statement cache, until the `CachedStatement` is given back, which
    /// resets it. Copy what has to outlive the row. Views of several columns
    /// of one row may be held side by side, and reading the column again, as
    /// text or as anything else, leaves a view of it as it was.
    ///
    /// Empty for NULL, as `get_string()` is; ask `is_null()` to tell the two
    /// apart. SQLite's view is the engine's own text for the row, converted in
    /// place from a number where the column holds one. The MySQL family's is
    /// the buffer the row was fetched into, and for a number column a buffer
    /// of the statement's own that it is formatted into.
    ///
    /// The default, for a driver that does not say, reads `get_string()` into
    /// a buffer the statement keeps per column, sized to every column at once,
    /// and keeps it while a read of the column gives the same text -- which
    /// within one row it always does -- so it lives as long as the row. It
    /// copies, which a driver's own does not have to. An index outside the
    /// columns throws what the driver's `get_string()` throws for it, or
    /// `std::out_of_range`.
    [[nodiscard]] virtual std::string_view get_text_view(int index) const;

    /// The column's bytes, as `get_blob()` reads them, without a copy. Lives
    /// as `get_text_view()` does, and is empty for NULL and for an empty
    /// blob alike.
    [[nodiscard]] virtual std::span<const std::byte> get_blob_view(int index) const;

    /// What the current row holds in the column; see `CellKind`.
    ///
    /// The default, for a driver that does not say, answers `null` or `text`.
    [[nodiscard]] virtual CellKind cell_kind(int index) const;

protected:
    // A statement runs inside its connection's transaction, so it reports to
    // the same watch the connection's own `execute()` does. These forward to
    // `Connection`'s hooks of the same names; see those for what each checks.
    // Call `before_statement` before the statement first goes to the engine,
    // `after_statement` once the engine has finished it, and
    // `after_failed_statement` when the engine refuses it, appending what it
    // returns to the error raised.
    static void before_statement(Connection& connection);
    static void after_statement(Connection& connection, std::string_view sql);
    [[nodiscard]] static std::string_view after_failed_statement(Connection& connection,
                                                                 TransactionEnding ending) noexcept;

    /// For a driver's `step()`: set on the way in, cleared on the way out, so
    /// that a step which throws leaves it set. See `failed()`.
    void set_failed(bool failed) noexcept { failed_ = failed; }

private:
    bool failed_ = false;

    /// Where the default `get_text_view()` and `get_blob_view()` keep what
    /// they read, one per column; empty for a driver that says itself.
    mutable std::vector<std::string>            text_views_;
    mutable std::vector<std::vector<std::byte>> blob_views_;
};

/// How a connection keeps statements prepared between uses.
///
/// Off unless `max_statements` is set. See `Connection::prepare_cached()`.
/// It lives in `schemata::driver`, beside `Connection`, and is spelled so from
/// outside it -- on a connection, or for every connection of a pool through
/// `PoolOptions::statement_cache`:
///
///     schemata::driver::StatementCacheOptions cache;
///     cache.max_statements = 64;
///     connection->set_statement_cache(cache);
///
///     schemata::PoolOptions pool_options;
///     pool_options.statement_cache = cache;
struct StatementCacheOptions {
    /// Most statements kept prepared at once; the least recently used goes
    /// first. Zero turns the cache off and closes what it holds.
    ///
    /// Each one is a server-side object on the MySQL family, counted against
    /// `max_prepared_stmt_count` -- a limit across every connection to the
    /// server, 16382 by default -- so this times the connections a process
    /// holds is what it can take of that.
    std::size_t max_statements = 0;

    /// How long a statement may go unused before it is closed. Checked
    /// whenever the connection is next asked for a cached statement, when the
    /// pool hands it out, and on `evict_idle_statements()`; a connection that
    /// is never used again holds its statements until it closes.
    std::chrono::seconds max_idle{300};

    /// Most partial statements of the multi-row writes kept prepared at once
    /// (`<schemata/query/bulk.h>`: `insert_many()`, `upsert_many()`,
    /// `insert_rows()`, `upsert_rows()`). 16 by default.
    ///
    /// A multi-row statement short of a full one -- the last of a call, or
    /// the only one of a call that fits in a statement -- has SQL of its own
    /// for each row count, so a caller sending many row counts sends many
    /// statements, and this is how many of them the cache holds at a time,
    /// each under its own row count and run as one Execute.
    ///
    /// Past it, on MySQL and MariaDB in a cache with 24 places beyond it
    /// (`max_statements` of 40 or more at the default of 16), a new row
    /// count is not kept and takes no other's place: it is sent as
    /// chunks of a few fixed sizes -- 16, 32, 64 rows and so on, and one of
    /// 1 to 15 -- which are statements every row count of the shape shares.
    /// So a caller whose row count varies prepares those and stops, at the
    /// cost of a few Executes a call where a row count kept is one; and a
    /// row count that then comes often -- 16 of the connection's last 64
    /// partial statements, and twice what the least sent of the kept ones
    /// is besides -- takes that one's place. One that repeats but is under
    /// a quarter of those statements stays chunks: raise this to cover the
    /// row counts that repeat. Size `max_statements` for the chunks: up to
    /// 24 places for each table written at varying row counts, beside the
    /// row counts kept here and the caller's own queries. A connection
    /// composes one shape for every 24 places the cache has past the ones
    /// it may keep; a shape past those is prepared and closed as in a
    /// smaller cache. Elsewhere
    /// -- SQLite, a smaller cache -- one that is kept takes the place of the
    /// least recently sent of the others, and a caller with more row counts
    /// than this prepares and closes a statement on most calls. See
    /// `BulkOptions` for the rest of what is kept and when.
    ///
    /// Never more than half of `max_statements`, whatever is set here: 128
    /// asks for a cache of 256. Zero keeps none under its own row count:
    /// every partial statement is sent as chunks where that is done, and
    /// prepared and closed on its own where it is not; full statements are
    /// kept either way. Each one kept is a prepared statement on the server
    /// like any other, of 4,096 parameters at most, and costs it memory by
    /// its parameters -- about 0.9 MiB for a hundred rows of eleven columns
    /// on MySQL 8.4 (measured) -- so raise it for the row counts that
    /// repeat, not to cover every count a caller might send.
    ///
    /// Changed on a live connection (`set_statement_cache()`), it holds from
    /// that connection's next multi-row write: lowered, the least recently
    /// sent beyond the new number are closed then; raised, more are kept
    /// from then on.
    std::size_t max_partial_bulk_statements = 16;
};

class Connection;

/// A statement lent by a connection's cache, given back when this goes.
///
/// Used as the `unique_ptr` from `prepare()` is: `stmt->bind(...)`,
/// `stmt->step()`, `*stmt`. On destruction the statement is reset -- its
/// parameters cleared, anything unread discarded -- and kept for the next
/// caller of the same SQL; where that fills the cache, the least recently
/// used goes instead. It is closed rather than kept where the cache is off
/// or gone, was emptied while this was lent, or already holds the same SQL;
/// where its last `step()` threw -- even if the caller caught that and went
/// on; and where it is given back while an exception unwinds past it, whose
/// caller stopped somewhere nobody can say.
///
/// **Hold it for the call that uses it, and no longer.** Three things follow
/// from holding it past that:
///
/// - It must not outlive the connection that lent it. Giving it back reaches
///   into that connection, and closing it needs the connection's handle, so
///   one destroyed after the connection is undefined behaviour -- as for any
///   statement from `prepare()`. Nor, from a pool, may it outlive the lease:
///   the connection is then another thread's, and a connection is not
///   thread-safe.
/// - While it is lent the cache does not have it: the same SQL asked for
///   meanwhile prepares a second statement.
/// - It is kept only if it comes back before the cache is next emptied --
///   `clear_statement_cache()`, a `USE` or DDL through the connection,
///   `reset_session()`. One given back after that is closed. Held across
///   `reset_session()` on the MySQL family it is also dead: the server has
///   dropped it, and its next run fails.
class CachedStatement {
public:
    CachedStatement() = default;
    ~CachedStatement();

    CachedStatement(CachedStatement&& other) noexcept;
    CachedStatement& operator=(CachedStatement&& other) noexcept;

    CachedStatement(const CachedStatement&)            = delete;
    CachedStatement& operator=(const CachedStatement&) = delete;

    [[nodiscard]] Statement& operator*() const { return *statement_; }
    [[nodiscard]] Statement* operator->() const { return statement_.get(); }
    [[nodiscard]] Statement* get() const noexcept { return statement_.get(); }
    explicit operator bool() const noexcept { return statement_ != nullptr; }

private:
    friend class Connection;
    CachedStatement(Connection* owner, std::unique_ptr<Statement> statement, std::string sql,
                    std::uint64_t generation) noexcept;

    void give_back() noexcept;

    Connection*                owner_ = nullptr;
    std::unique_ptr<Statement> statement_;
    std::string                sql_;

    /// The cache's generation when this was lent; one emptied since has moved
    /// on, and takes nothing back from before.
    std::uint64_t generation_ = 0;

    /// `std::uncaught_exceptions()` when this was lent. More at give-back
    /// means an exception is unwinding past the statement.
    int unwinding_ = 0;
};

/// A live connection to one database.
///
/// The only thing sch_core knows about a backend. Each engine project
/// supplies an implementation and a factory function; sch_core never
/// includes a vendor header and never links a vendor library.
///
/// A Connection is not thread-safe and must not be shared between threads.
/// That is not a restriction Schemata invents: the MySQL C API requires one
/// connection per thread, and SQLite's threading modes make sharing a handle
/// a runtime configuration question rather than a guarantee. Concurrency is
/// served by giving each thread its own connection from a pool, which is
/// ConnectionPool's job rather than this interface's.
class Connection {
public:
    Connection()                             = default;
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;
    virtual ~Connection();

    // -- identity ---------------------------------------------------------

    /// The dialect this connection actually speaks.
    ///
    /// Determined from the server at connect time, not from which archive
    /// was linked: sch_mysql pointed at a MariaDB server reports
    /// Dialect::mariadb, because the wire protocol is compatible but the DDL
    /// is not.
    [[nodiscard]] virtual Dialect dialect() const noexcept = 0;

    /// Raw server version string, for diagnostics and feature gating.
    [[nodiscard]] virtual std::string server_version() const = 0;

    // -- statements -------------------------------------------------------

    [[nodiscard]] virtual std::unique_ptr<Statement> prepare(std::string_view sql) = 0;

    /// The most parameters one statement may bind on this connection.
    ///
    /// SQLite's `SQLITE_LIMIT_VARIABLE_NUMBER` as this handle has it: 32766
    /// from 3.32, 999 before that and wherever a build lowered it. 65,535 on
    /// the MySQL family, whose protocol counts a statement's parameters in two
    /// bytes. What the multi-row writes of `<schemata/query/bulk.h>` size a
    /// statement by. A driver that does not say answers 999, the least any of
    /// the three has had.
    [[nodiscard]] virtual std::size_t max_parameters() const { return 999; }

    /// A prepared statement for `sql`, from this connection's cache.
    ///
    /// With the cache on, the first call for a piece of SQL prepares it and
    /// every later one reuses it: one Execute per run on the MySQL family,
    /// rather than a Prepare, an Execute and a Close. Keyed by the exact
    /// text. A statement still lent out when the same SQL is asked for again
    /// -- a query run inside the loop over another's rows -- is not shared;
    /// the second caller gets one of its own. With the cache off this is
    /// `prepare()`.
    ///
    /// The statement is lent, not given: hold it for the call that uses it.
    /// It must not outlive this connection (undefined behaviour) or, from a
    /// pool, the lease; and one whose `step()` threw, or that is given back
    /// while an exception unwinds, is closed rather than returned to the
    /// cache. See `CachedStatement`.
    ///
    /// Every statement Schemata runs for a query, a save or `execute_sql()`
    /// comes through here, so turning the cache on is all it takes.
    ///
    /// A statement prepared on the MySQL family keeps the default schema it
    /// was prepared under, and runs against it after a `USE`. So the cache is
    /// emptied whenever the connection switches schema where the library can
    /// see it: a `USE` sent through `execute()`, and every call aimed at
    /// another schema. One it cannot see -- a `USE` inside a stored
    /// procedure or a multi-statement string, `mysql_select_db()` on a raw
    /// handle -- has to be followed by `clear_statement_cache()`, or cached
    /// statements go on reading the schema before it.
    ///
    /// A statement kept this way runs again against its tables as they are
    /// then, which another connection may have changed; DDL through this one
    /// empties the cache, below. On the MySQL family a result that changed
    /// its number of columns since the statement was prepared -- a column
    /// added under `SELECT *` -- cannot be read through it, and by the time
    /// the server says so it has run the statement. One that `is_select()` is
    /// prepared again and sent again, so it runs twice: a SELECT that assigns
    /// a user variable, calls a function that writes, or takes `GET_LOCK()`
    /// does that twice. Anything else fails, run once, with an error saying
    /// the server ran it, and the statement is left fit to run again.
    ///
    /// A column renamed since the statement was prepared, with the number of
    /// columns unchanged, cannot be seen by the statement: both MySQL-family
    /// clients take a column's new type from the execute but keep its
    /// prepared name, so `column_name()` gives the old one, and `from_sql()`,
    /// which maps by that name, would read two columns that swapped names
    /// into each other's fields. So the cache is also emptied by DDL sent
    /// through this connection -- an ALTER, CREATE, DROP, RENAME or TRUNCATE
    /// (`changes_schema()`), through `execute()` or a prepared statement, on
    /// every engine -- which covers `ensure_schema()` and `migrate()`. CREATE
    /// and DROP TEMPORARY TABLE are DDL too, so code that makes a temporary
    /// table per request empties the whole cache each time -- on SQLite as
    /// well, which would not need it.
    ///
    /// `changes_schema()` reads only the first keyword, so DDL behind another
    /// one is not seen: inside a stored procedure (`CALL`) or a compound
    /// statement (`BEGIN NOT ATOMIC ... END`), in a server-side prepared
    /// statement run by `EXECUTE` or MariaDB's `EXECUTE IMMEDIATE`, or in a
    /// later statement of a multi-statement string. Nor is DDL from another
    /// connection: a statement cached here before it keeps its old column
    /// names, while a changed number of columns or type is caught by the
    /// driver as above. Follow any of these with `clear_statement_cache()`,
    /// or with `ConnectionPool::clear_statement_caches()` for every
    /// connection of a pool. A pool left at its default
    /// `reset_session_on_return = true` empties the cache on every return, so
    /// there it lasts one lease at most; set to false, DDL sent through one
    /// lease empties the other connections' caches, each at its first
    /// checkout after that lease is returned -- code that keeps the lease, or
    /// a `Context` over it, and wants them current sooner returns it or calls
    /// `ConnectionPool::clear_statement_caches()`; see
    /// `schema_changes_seen()`.
    [[nodiscard]] CachedStatement prepare_cached(std::string_view sql);

    /// Turn the statement cache on, resize it, or turn it off.
    ///
    /// A change to `max_partial_bulk_statements` holds from the connection's
    /// next multi-row write, as that field says.
    ///
    /// Emptied on `reset_session()` -- on the MySQL family the server drops
    /// every prepared statement then anyway -- and gone with the connection,
    /// so one opened to replace a dead one starts empty and prepares afresh.
    /// Shrinking closes the least recently used; `max_statements` = 0 closes
    /// them all.
    void set_statement_cache(const StatementCacheOptions& options);

    /// Statements kept prepared and not lent out right now.
    [[nodiscard]] std::size_t cached_statements() const noexcept;

    /// Close the cached statements unused for longer than the cache's
    /// `max_idle`. The pool calls this on every checkout.
    void evict_idle_statements() noexcept;

    /// Close every cached statement, keeping the cache's settings. One lent
    /// out now is closed when it is given back, rather than kept.
    ///
    /// Also forgets what the connection has learned of its server by asking,
    /// which goes stale by the same rules: the unique keys `upsert_rows()`
    /// read with `SHOW INDEX` for a table the model does not hold, the
    /// generated-key column `insert_row()` read for one (`SHOW COLUMNS`, or
    /// SQLite's `PRAGMA table_info`), and the target schemas the calls naming
    /// a table found (`TableTarget::schema`). Those are kept whether the
    /// statement cache is on or not, and forgotten wherever it is emptied --
    /// a `USE` or DDL through this connection, a pool's catch-up -- so the
    /// next call asks again. `reset_session()` empties the cache and forgets
    /// the unique keys and generated-key columns, a temporary table's going
    /// with the session, but keeps the schemas found: they are the server's,
    /// not the session's.
    ///
    /// One answer is not forgotten here: the session's `max_allowed_packet`,
    /// which the multi-row writes of `<schemata/query/bulk.h>` read once. Its
    /// session value is read only, so only a new session can hold another,
    /// and only `reset_session()` forgets it.
    void clear_statement_cache() noexcept;

    /// How many times this connection has emptied its statement cache for DDL
    /// it ran (`changes_schema()`), whether the cache is on or not. Not moved
    /// by a `USE`, which changes this session alone, nor by
    /// `clear_statement_cache()` or `reset_session()`.
    ///
    /// Other connections to the same server keep what they cached before the
    /// DDL. A pool that keeps sessions across a return compares this at the
    /// lease's start and end, and where it moved has every other connection
    /// empty its cache at its first checkout after that lease is returned.
    [[nodiscard]] std::uint64_t schema_changes_seen() const noexcept {
        return schema_changes_seen_;
    }

    /// Run a statement that returns no rows.
    ///
    /// For DDL and bookkeeping only. Everything carrying a value goes
    /// through prepare() and bound parameters: there is no interpolation
    /// path in the query API, and this is not one.
    virtual void execute(std::string_view sql) = 0;

    /// Rows affected by the most recent statement on this connection.
    [[nodiscard]] virtual std::int64_t affected_rows() const = 0;

    /// Rows the most recent statement's WHERE matched, where it was an UPDATE
    /// (`is_update()`); `affected_rows()` for any other statement.
    ///
    /// The two counts differ for an UPDATE on the MySQL family, whose
    /// affected rows are the rows it *changed*: a row that already held the
    /// values assigned is matched and not counted -- unless the connection
    /// was opened with `MySqlOptions::found_rows`, which makes the affected
    /// count the matched one. This answers the matched count either way. How
    /// each engine knows it:
    ///
    /// - SQLite: `sqlite3_changes()`, which is `affected_rows()`: SQLite
    ///   counts every row an UPDATE's WHERE finds, changed or not.
    /// - MySQL and MariaDB: the server's info line for the statement, read
    ///   with `mysql_info()` once it has run -- `Rows matched: 1  Changed: 0
    ///   Warnings: 0` -- whose first number is taken as the rows matched. A
    ///   statement with no info line, or none holding a number, answers
    ///   `affected_rows()`.
    ///
    /// The default, for a driver that does not say, is `affected_rows()`.
    [[nodiscard]] virtual std::int64_t matched_rows() const { return affected_rows(); }

    /// The warnings, errors and notes the most recent statement drew, as the
    /// server counts them: on MySQL and MariaDB the C API's
    /// `mysql_warning_count()`, read as soon as the statement has run. The
    /// whole count, where `SHOW WARNINGS` lists at most `max_error_count` of
    /// them. `SHOW WARNINGS` and `SELECT @@warning_count` cannot be prepared
    /// on MySQL (1295). Nothing for SQLite, which has no warnings, and for a
    /// driver that does not say.
    [[nodiscard]] virtual std::optional<std::int64_t> warning_count() const {
        return std::nullopt;
    }

    /// Value generated for the most recent identity insert.
    ///
    /// What it answers otherwise is each engine's own:
    ///
    /// - MySQL and MariaDB: the C API's `mysql_insert_id()`, not SQL's
    ///   `LAST_INSERT_ID()`. An insert that gives an AUTO_INCREMENT column a
    ///   value of its own answers that value; a multi-row insert answers the
    ///   first value generated; an insert into a table with no AUTO_INCREMENT
    ///   column answers 0, and so does an UPDATE after any insert.
    /// - SQLite: `sqlite3_last_insert_rowid()`, the rowid of the last row
    ///   inserted into a rowid table. An explicit INTEGER PRIMARY KEY value is
    ///   that rowid, and is answered; a table keyed on anything else still has
    ///   a rowid, and answers it; a multi-row insert answers the last row's. A
    ///   WITHOUT ROWID table, and a statement that inserts nothing, leave it
    ///   where it was.
    ///
    /// So read it straight after the insert, on the connection that ran it.
    [[nodiscard]] virtual std::int64_t last_insert_id() const = 0;

    // -- transactions -----------------------------------------------------
    //
    // Raw control. Prefer the RAII Transaction guard in <schemata/context>
    // over calling these directly.
    //
    // Note for the MySQL family: DDL causes an implicit commit, so schema
    // changes cannot participate in a transaction. `migrate()` records
    // progress per statement for exactly this reason.

    /// Whether a transaction Schemata opened is still counted as open.
    ///
    /// The count is what Schemata was asked to do: `begin()` sets it,
    /// `commit()` and `rollback()` clear it. It is what lets the RAII guard,
    /// the change tracker and the nesting refusal agree, and it is not the
    /// whole story -- a deadlock, a trigger's `RAISE(ROLLBACK)` or DDL's
    /// implicit commit can end the transaction on the server while the count
    /// still says open. `transaction_loss()` says when that has happened, and
    /// every statement sent while it has is refused before it goes.
    ///
    /// A caller who issues `BEGIN` through `execute()` goes around the count,
    /// which is their business; the documented way in is these three.
    [[nodiscard]] bool in_transaction() const noexcept { return in_transaction_; }

    /// A number no other transaction, on this or any other connection, has
    /// had or will have.
    ///
    /// `in_transaction()` cannot tell a second transaction from the first,
    /// and something that staged work inside one needs to. Pairing the two
    /// answers the question that actually matters: is this the same
    /// transaction my work is sitting in? Drawn from one process-wide
    /// sequence, because that question is asked across connections too -- a
    /// pool hands the same tracker a different connection -- and a count per
    /// connection gave the first transaction on each of them the same number.
    [[nodiscard]] std::uint64_t transaction_generation() const noexcept {
        return transaction_generation_;
    }

    /// How a transaction the count still calls open was ended on the server.
    enum class TransactionLoss : unsigned char {
        /// Nothing ended it behind the connection's back.
        none,

        /// The engine rolled it back while refusing a statement: InnoDB's
        /// deadlock victim, a lock-wait timeout under
        /// `innodb_rollback_on_timeout`, a trigger's `RAISE(ROLLBACK)`.
        /// Nothing in it survived.
        rolled_back,

        /// A statement that succeeded ended it -- DDL's implicit commit on the
        /// MySQL family, or a `COMMIT` or `ROLLBACK` sent as raw SQL -- or it
        /// ended where no statement was watching. Its work may or may not
        /// have been committed, and nothing here can tell which.
        outcome_unknown,
    };

    /// What ended the current or most recent transaction, if it was not
    /// `commit()` or `rollback()`.
    ///
    /// Kept until the next `begin()`, so code resolving work after the
    /// rollback -- `Context::Scope`, for one -- can still ask how it went.
    [[nodiscard]] TransactionLoss transaction_loss() const noexcept { return loss_; }

    /// Refuse to go on inside a transaction the server has already ended.
    ///
    /// For code about to join the caller's transaction: past this point every
    /// statement would run on its own, committed at once, while the caller
    /// still believed it could roll it back. Does nothing when no transaction
    /// is counted as open, which is the caller's own case to handle.
    ///
    /// \throws query_build_error naming `what` and how the transaction ended.
    void require_live_transaction(std::string_view what) const;

    /// Begin a transaction.
    ///
    /// Refuses to nest. Neither engine nests, and the two disagree about
    /// what happens if you try: SQLite raises, and MySQL's START TRANSACTION
    /// silently commits whatever was in flight, so the engine that destroys
    /// data is the one that says nothing. Refusing here means the check runs
    /// before the statement that would have done the damage.
    void begin();

    /// Commit.
    ///
    /// Refuses when nothing is open rather than sending a `COMMIT` the MySQL
    /// family accepts and quietly does nothing with -- which is how work gets
    /// reported as saved after a deadlock or an implicit commit has already
    /// thrown the transaction away. Refuses too when the server has already
    /// ended the transaction, saying how, since that decides whether its work
    /// survived.
    ///
    /// A `COMMIT` the server refuses while keeping the transaction open --
    /// SQLite's busy COMMIT in rollback-journal mode -- is rolled back before
    /// the error goes on, so the connection is not left holding a write lock
    /// nobody can release. On SQLite, where the commit happens in this
    /// process, a failed commit therefore does mean nothing happened.
    ///
    /// On MySQL and MariaDB it does not. The COMMIT runs on the server, so one
    /// that fails because the connection died may have been committed with the
    /// reply lost, or rolled back as the session ended, and no question asked
    /// from here distinguishes them -- reconnecting asks a different session.
    /// Such a commit records `TransactionLoss::outcome_unknown`, says so in
    /// the error it raises, and leaves any staged work as it stands for
    /// `ChangeTracker::commit_within()` or `rollback_within()` to resolve once
    /// the data has been checked.
    void commit();

    /// Roll back. Does nothing when nothing is open.
    ///
    /// Quiet rather than strict, because this is what destructors call while
    /// an exception is already unwinding. Sends `ROLLBACK` whenever the server
    /// still has the transaction open, and nothing once it has let go.
    void rollback();

    /// Roll back whatever transaction is open, however it was opened.
    ///
    /// For a connection changing hands: the pool calls this on every return,
    /// so the next borrower never inherits a transaction. Unlike `rollback()`
    /// it also catches one the count never saw -- a `BEGIN` sent as raw SQL,
    /// or a SQLite `SAVEPOINT` outside a transaction -- wherever the driver
    /// can ask the server.
    ///
    /// \throws whatever the `ROLLBACK` raises; a connection that cannot even
    ///         be rolled back is not one to hand to anybody else.
    void reset_transaction();

    /// Shed this session's state, so the next borrower inherits none of it.
    ///
    /// What the pool calls on every return. It rolls back first, exactly as
    /// `reset_transaction()` does -- an `autocommit = 0` left behind by this
    /// borrower would otherwise make the next borrower's ordinary writes part
    /// of a transaction nobody commits -- and then puts the session back the
    /// way this driver opened it.
    ///
    /// On the MySQL family that is the server's own connection reset, which
    /// drops temporary tables, user variables, prepared statements, table
    /// locks and session variables in one round trip and needs no privilege,
    /// followed by the character set the options asked for. On SQLite, which
    /// has no session to reset, it is the settings the driver applied at open:
    /// the busy timeout and `foreign_keys`. What that leaves behind is
    /// documented on `ConnectionPool::Lease::release()`.
    ///
    /// The statement cache is emptied first, and the unique keys `upsert_rows()`
    /// read and the generated-key columns `insert_row()` read are forgotten;
    /// the target schemas found are kept (see `clear_statement_cache()`). The
    /// `max_allowed_packet` a multi-row write read is forgotten too, and read
    /// again by the next one: the reset session takes the global value as it
    /// is by then.
    ///
    /// \throws whatever the reset raises; a connection that cannot be cleaned
    ///         is not one to hand to anybody else.
    void reset_session();

    /// Raw savepoints.
    ///
    /// Two things these do not do, both of which `Context::savepoint()` does.
    ///
    /// They do not coordinate with a change tracker: a `rollback_to()` takes
    /// the rows away, and a tracker that staged them goes on believing they
    /// were saved, so the commit baselines them and the work is gone with
    /// nothing raised.
    ///
    /// And they do not agree across engines about what happens with no
    /// transaction open. SQLite's `SAVEPOINT` quietly starts one, so the
    /// rollback works; the MySQL family's does not, so the rows are already
    /// committed and the `ROLLBACK TO` fails with "SAVEPOINT does not exist"
    /// having undone nothing. That divergence is the server's, and it is
    /// left intact here rather than papered over -- these are the raw calls.
    ///
    /// The name is the caller's, quoted as an identifier. An empty one, or
    /// one holding a NUL, is refused with `query_build_error` before anything
    /// is sent (`detail::require_quotable()`, which a driver of one's own
    /// calls too).
    virtual void savepoint(std::string_view name)    = 0;
    virtual void release_savepoint(std::string_view name) = 0;
    virtual void rollback_to(std::string_view name)  = 0;

    // -- health -----------------------------------------------------------

    /// Whether the connection is still usable.
    ///
    /// Checked by the pool on checkout: MySQL closes idle connections after
    /// wait_timeout, eight hours by default, and a pool that hands out a
    /// dead connection is worse than no pool at all.
    [[nodiscard]] virtual bool is_alive() = 0;

protected:
    // The statements themselves. The public trio above keeps the count and
    // then calls these, so a driver never has to remember to.
    virtual void do_begin()    = 0;
    virtual void do_commit()   = 0;
    virtual void do_rollback() = 0;

    /// What the server says about whether a transaction is open right now.
    ///
    /// The count above tracks what Schemata was asked to do, which is not
    /// always what happened: a deadlock or a trigger's `RAISE(ROLLBACK)` rolls
    /// the transaction back, DDL on the MySQL family commits it, and a caller
    /// can send `COMMIT` or `ROLLBACK` through `execute()`. Every one of those
    /// leaves the count saying "open" and the server saying otherwise, and a
    /// `COMMIT` sent into that gap succeeds while committing nothing.
    ///
    /// Both engines can answer. SQLite reads `sqlite3_get_autocommit`. The
    /// MySQL family reads `SERVER_STATUS_IN_TRANS` from the status flags of
    /// the last packet -- `MYSQL::server_status` on libmysqlclient,
    /// `mariadb_get_infov(MARIADB_CONNECTION_SERVER_STATUS)` on MariaDB
    /// Connector/C -- except that an error packet carries no flags, so after
    /// an error the first question costs one no-op round trip to refresh
    /// them. `std::nullopt` means this driver cannot say, or the connection
    /// is too far gone to ask, and the count stands alone.
    [[nodiscard]] virtual std::optional<bool> server_in_transaction() const {
        return std::nullopt;
    }

    // A driver empties the statement cache first thing in its destructor,
    // with `clear_statement_cache()`, while the handle its statements need in
    // order to close is still open.

    /// Forget any transaction this connection believed it had.
    ///
    /// For a driver that has just reconnected, or otherwise knows the
    /// server-side transaction is gone.
    void forget_transaction() noexcept { in_transaction_ = false; }

    /// Put the session back the way this driver opened it.
    ///
    /// Called by `reset_session()`, once the transaction is gone. The default
    /// does nothing, for a driver whose sessions carry nothing to shed.
    virtual void do_reset_session() {}

    // The transaction's watch over the statements run inside it. A driver
    // calls these around every statement it executes -- its Statement
    // implementations reach them through `Statement`'s forwarders -- and they
    // do nothing unless the count says a transaction is open. Checking at the
    // statement is what finds the first one that would run outside the
    // caller's transaction, rather than finding out at commit time, when all
    // of it has already committed on its own.

    /// Refuse to send a statement into a transaction the server has ended.
    ///
    /// \throws query_build_error, and nothing is sent.
    void before_statement();

    /// Check a statement the engine accepted did not end the transaction.
    ///
    /// DDL's implicit commit on the MySQL family shows here, on the DDL's own
    /// OK packet, and so does a `COMMIT` or `ROLLBACK` sent as raw SQL.
    ///
    /// \throws query_error naming `sql`.
    void after_statement(std::string_view sql);

    /// Record how a statement the engine refused ended the transaction.
    ///
    /// \param ending which way it went, decided by the driver from the error
    ///        code and the kind of statement it sent. Only a statement that
    ///        can commit leaves the outcome unknown -- DDL on the MySQL
    ///        family, or a routine running statements of its own. A SELECT or
    ///        a DML statement cannot, so one that ends a transaction ended it
    ///        by rollback, and the work is the caller's to retry.
    /// \returns a sentence to append to the error, empty when the transaction
    ///          is unaffected.
    [[nodiscard]] std::string_view after_failed_statement(TransactionEnding ending) noexcept;

private:
    friend class Statement;
    friend class CachedStatement;

    struct StatementCache;
    // Out of line, so a driver's constructor -- which may have to destroy
    // this member -- never needs the definition.
    struct StatementCacheDeleter {
        void operator()(StatementCache* cache) const noexcept;
    };
    void keep_statement(std::string sql, std::unique_ptr<Statement> statement,
                        std::uint64_t generation) noexcept;

    // Puts the session's default schema back after a call aimed at another.
    // `USE` is session state rather than transactional, so it is let past a
    // transaction the server has ended -- the one statement the watch above
    // must not refuse, since refusing it leaves every later statement in the
    // wrong schema.
    friend class detail::TargetSchema;

    // What the calls naming a table learned of its unique keys and its
    // generated key, and the schemas they found; emptied with the statement
    // cache.
    friend detail::SessionFacts& detail::session_facts(Connection& connection);
    // The multi-row writes bound the statements cut short they keep in the
    // cache, and close the oldest for the newest.
    friend std::size_t detail::statement_cache_capacity(const Connection& connection) noexcept;
    friend std::size_t detail::partial_statement_capacity(
        const Connection& connection) noexcept;
    friend bool        detail::statement_is_cached(const Connection& connection,
                                                   std::string_view  sql) noexcept;
    friend void        detail::drop_cached_statement(Connection&      connection,
                                                     std::string_view sql) noexcept;
    // A multi-row write joins a transaction opened behind the count's back.
    friend bool detail::server_has_transaction(const Connection& connection);
    struct SessionFactsDeleter {
        void operator()(detail::SessionFacts* facts) const noexcept;
    };

    bool            in_transaction_         = false;
    std::uint64_t   transaction_generation_ = 0;
    TransactionLoss loss_                   = TransactionLoss::none;
    std::uint64_t   schema_changes_seen_    = 0;

    /// Null until `set_statement_cache()` turns it on.
    std::unique_ptr<StatementCache, StatementCacheDeleter> statement_cache_;

    /// Null until something is learned.
    std::unique_ptr<detail::SessionFacts, SessionFactsDeleter> session_facts_;
};

inline void Statement::before_statement(Connection& connection) {
    connection.before_statement();
}

inline void Statement::after_statement(Connection& connection, std::string_view sql) {
    connection.after_statement(sql);
}

inline std::string_view Statement::after_failed_statement(Connection& connection,
                                                          TransactionEnding ending) noexcept {
    return connection.after_failed_statement(ending);
}

} // namespace schemata::driver
