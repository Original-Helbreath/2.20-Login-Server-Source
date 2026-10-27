#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <schemata/driver/driver.h>

namespace schemata {

/// How a new connection is opened.
///
/// Taking a factory rather than connection options is what keeps the pool in
/// sch_core: it never names an engine, so it needs no vendor header and
/// works with whichever archive the caller linked.
///
///     ConnectionPool pool([] { return schemata::sqlite("app.db"); });
using ConnectionFactory = std::function<std::unique_ptr<driver::Connection>()>;

struct PoolOptions {
    /// Most connections open at once. A checkout beyond this waits.
    std::size_t max_size = 8;

    /// Connections kept open when nothing is using them.
    ///
    /// Held open past `max_idle`, unlike the rest, so a bursty workload does
    /// not pay the handshake every time it goes quiet.
    std::size_t min_size = 0;

    /// How long an idle connection above `min_size` is kept before closing.
    std::chrono::seconds max_idle{300};

    /// How long `acquire()` waits for one to come free before giving up.
    std::chrono::milliseconds checkout_timeout{5000};

    /// Skip the liveness check for a connection returned less than this long
    /// ago.
    ///
    /// The check is `is_alive()`, which on MySQL and MariaDB is a ping: one
    /// round trip on every checkout. Zero, the default, pings every one. A
    /// connection skipped this way that the server has closed after all --
    /// killed, or the server restarted -- fails on its first statement, as a
    /// held lease would; what the check exists for, the `wait_timeout` of a
    /// quiet night, is hours long.
    std::chrono::milliseconds validate_after{0};

    /// After the factory fails, how long `acquire()` refuses to try it again.
    ///
    /// While the server is down every attempt waits out the connect timeout,
    /// so a caller retrying in a loop spends all its time connecting. Inside
    /// this window an `acquire()` that would have to open a connection
    /// rethrows, at once, the exception the factory threw -- its type,
    /// dialect, native code and SQLSTATE as they were; an idle connection is
    /// still handed out. Zero, the default, tries every time.
    ///
    /// It is the same failure, rethrown: each caller inside the window gets
    /// the one failure, no connect is attempted for it, and a log of those
    /// callers shows the original error repeated, with the message of the
    /// attempt that failed rather than of a new one.
    std::chrono::milliseconds connect_backoff{0};

    /// Reset the session on every return. True by default.
    ///
    /// False keeps what the borrower left on the connection -- temporary
    /// tables, user variables, session variables, and prepared statements,
    /// the statement cache's among them -- and only rolls back a transaction
    /// still open. It saves the reset's round trips (see
    /// `Lease::release()`) and is what lets a statement cache outlive a
    /// lease. It is for a pool whose every borrower is code that changes no
    /// session state it does not also put back.
    ///
    /// The default schema is such state. A `USE` sent through `execute()`
    /// empties the statement cache; a switch the library cannot see -- a
    /// `USE` inside a stored procedure or a multi-statement string,
    /// `mysql_select_db()` on a raw handle -- has to be followed by
    /// `clear_statement_cache()`, or the next borrower's cached statements
    /// read the schema before it.
    ///
    /// DDL empties it too. DDL sent through one lease empties that
    /// connection's cache at once, and every other connection's at its first
    /// checkout after that lease is returned: a lease that returns with its
    /// connection's `schema_changes_seen()` moved moves the pool on. Until
    /// then the others go on as they were, so code that keeps a lease -- or a
    /// `Context` over one -- after DDL and wants them current sooner returns
    /// it, or calls `ConnectionPool::clear_statement_caches()`. DDL the
    /// factory runs as it opens a connection -- a CREATE TABLE IF NOT EXISTS,
    /// an `ensure_schema()` or a migration -- moves the pool on as it opens,
    /// and the new connection, which cached nothing before it, is not emptied
    /// for it. CREATE and DROP TEMPORARY TABLE count, so borrowers that make
    /// a temporary table per request empty every connection's cache each
    /// time. DDL the pool cannot see -- from another process, a migration
    /// tool or a connection opened without the pool, run by a factory that
    /// then fails (throws or returns nothing), or inside a `CALL`, a compound
    /// statement, an `EXECUTE` or a later statement of a multi-statement string
    /// -- has to be followed by `ConnectionPool::clear_statement_caches()`, or
    /// a column renamed there keeps its old name in statements cached here.
    /// With this true none of that arises: the reset empties the cache on
    /// every return.
    bool reset_session_on_return = true;

    /// The statement cache each connection the pool opens gets. Off by
    /// default. The type is the driver's, in `schemata::driver`:
    ///
    ///     schemata::driver::StatementCacheOptions cache;
    ///     cache.max_statements = 64;
    ///     options.statement_cache = cache;  // or options.statement_cache.max_statements = 64
    ///
    /// See `driver::Connection::prepare_cached()`, which says why a
    /// switch of schema or DDL the library cannot see must be followed by
    /// `clear_statement_cache()` -- for a pool, by
    /// `ConnectionPool::clear_statement_caches()` -- and, above, what a pool
    /// that keeps sessions does with the DDL it can see.
    driver::StatementCacheOptions statement_cache{};
};

/// A pool of database connections, safe to share between threads.
///
/// Per D9, Schemata owns connections rather than threads. An async or
/// thread-pool API would infect every signature for no benefit, because the
/// real concurrency problem is which connection a statement runs on, not
/// where the threads come from.
///
/// Liveness is validated on checkout, and that is what makes the pool
/// correct rather than merely convenient: MySQL closes an idle connection
/// after `wait_timeout` (eight hours by default) without telling anyone, so
/// a pool that handed one back unchecked would fail on the first statement
/// after a quiet night. On checkout only: a lease held for hours is not
/// checked again, and is subject to the same `wait_timeout` -- hold one for a
/// unit of work, not for the life of a thread.
///
/// What it costs, on MySQL and MariaDB: a checkout of an idle connection is
/// one round trip, the ping (`PoolOptions::validate_after` skips it for one
/// returned recently); opening a new one is the handshake. A return is the
/// reset described on `Lease::release()`. When the factory fails nothing is
/// remembered unless `PoolOptions::connect_backoff` says so: the next
/// `acquire()` tries again, and waits out the connect timeout again.
///
/// The pool itself is thread-safe. A `Connection` is not, which is the whole
/// point: a lease gives one thread exclusive use of one connection.
class ConnectionPool {
public:
    explicit ConnectionPool(ConnectionFactory factory, PoolOptions options = {});

    ConnectionPool(const ConnectionPool&)            = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    ~ConnectionPool();

    /// Exclusive use of one connection, returned to the pool on destruction.
    ///
    /// A lease may outlive the pool. The shared state is kept alive by the
    /// leases themselves, so a connection returned after the pool is gone is
    /// simply closed rather than writing through a dangling pointer.
    ///
    /// A thread blocked in `acquire()` holds no lease and so is not covered
    /// by that; it keeps the state alive with a reference of its own and is
    /// woken by the pool's destructor.
    class Lease {
    public:
        Lease() = default;
        ~Lease();

        Lease(Lease&&) noexcept;
        Lease& operator=(Lease&&) noexcept;

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]] driver::Connection& operator*() const { return *connection_; }
        [[nodiscard]] driver::Connection* operator->() const { return connection_.get(); }
        [[nodiscard]] driver::Connection& get() const { return *connection_; }

        [[nodiscard]] bool valid() const noexcept { return connection_ != nullptr; }
        explicit operator bool() const noexcept { return valid(); }

        /// Return the connection now rather than at end of scope.
        ///
        /// The session is shed, so the next borrower inherits none of it. A
        /// transaction still open -- by the count, or by the server where the
        /// driver can say -- is rolled back first, and then the session itself
        /// is reset. On MySQL and MariaDB that is the server's own connection
        /// reset: temporary tables, user variables, prepared statements, table
        /// locks, `GET_LOCK()` locks and session variables -- `autocommit`
        /// among them -- all go, and the character set the options asked for
        /// is applied again afterwards.
        ///
        /// On MySQL and MariaDB that costs, per return: a `ROLLBACK` where a
        /// transaction is open (and a `DO 0` first when the last statement
        /// failed, to read the status flags afresh); the reset itself, one
        /// round trip; the character set, one more; and one more for each of
        /// `MySqlOptions::init_statements`. Every server-side prepared
        /// statement goes with the reset, so a statement cache is emptied on
        /// every return -- `PoolOptions::reset_session_on_return` = false keeps
        /// the session, and the cache with it.
        ///
        /// SQLite has no session to reset, and there the promise stops at what
        /// the driver itself set: the busy timeout and `foreign_keys`. A
        /// `PRAGMA` a borrower changed (`journal_mode`, `synchronous`,
        /// `case_sensitive_like`), a database it `ATTACH`ed and a `TEMP` table
        /// it created stay on the handle, because nothing can enumerate them
        /// in order to undo them. Code that changes those on a pooled
        /// connection has to change them back itself.
        ///
        /// A connection whose reset fails is closed rather than returned.
        void release();

        /// Give the connection back closed rather than reusable.
        ///
        /// For when something has gone wrong on it and its state is no
        /// longer known -- a half-consumed result, an abandoned transaction.
        void discard();

    private:
        friend class ConnectionPool;
        struct State;

        Lease(std::shared_ptr<State> state, std::unique_ptr<driver::Connection> connection,
              std::uint64_t generation);

        std::shared_ptr<State>              state_;
        std::unique_ptr<driver::Connection> connection_;
        bool                                poisoned_ = false;

        /// The pool's generation the connection's statement cache had caught
        /// up with when it was handed out.
        std::uint64_t generation_ = 0;

        /// The connection's `schema_changes_seen()` when it was handed out.
        std::uint64_t schema_changes_ = 0;
    };

    /// Take a connection, opening one if the pool is below `max_size`.
    ///
    /// Safe against the pool being destroyed while this call is blocked: it
    /// takes its own reference to the shared state, since a thread waiting
    /// for a connection is the one participant holding no lease.
    ///
    /// \throws connection_error if nothing comes free within
    ///         `checkout_timeout`, naming the limit rather than leaving the
    ///         caller to guess why it blocked, or if the pool was destroyed
    ///         while this call was waiting.
    [[nodiscard]] Lease acquire();

    /// Connections open and not currently leased.
    [[nodiscard]] std::size_t idle() const;

    /// Connections currently leased out.
    [[nodiscard]] std::size_t in_use() const;

    /// Close every idle connection. Leased ones are closed when returned.
    void close_idle();

    /// Empty every connection's statement cache, each at its next checkout --
    /// the ones idle now and the ones leased out alike.
    ///
    /// For DDL the pool cannot see: made from outside it -- another process,
    /// a migration tool, a connection opened without the pool -- or hidden
    /// from `driver::changes_schema()` behind another keyword, inside a
    /// `CALL`, a compound statement, an `EXECUTE` or a later statement of a
    /// multi-statement string. A statement prepared before a column was
    /// renamed keeps its old name; see `driver::Connection::prepare_cached()`.
    /// A lease already held keeps its cache until it is returned and taken
    /// again; call its own `clear_statement_cache()` for that.
    void clear_statement_caches();

private:
    std::shared_ptr<Lease::State> state_;
};

} // namespace schemata
