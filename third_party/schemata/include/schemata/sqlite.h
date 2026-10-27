#pragma once

// SQLite backend.
//
// Include this header and link sch_sqlite. There is no preprocessor switch
// to set: the backend you get is the archive you link, and including this
// header without linking sch_sqlite is a link error naming the missing
// factory rather than a silent misconfiguration.

#include <chrono>
#include <memory>
#include <string>

#include <schemata/driver/driver.h>

namespace schemata {

struct SqliteOptions {
    /// Database file path, or ":memory:" for a private in-memory database.
    std::string path;

    /// Enable write-ahead logging on connect.
    ///
    /// On by default because the rollback journal permits only one reader
    /// or one writer at a time, which makes any concurrent use fail with
    /// SQLITE_BUSY. WAL allows concurrent readers alongside a single
    /// writer, which is what a connection pool needs to be useful at all.
    ///
    /// A request, not a requirement. Setting the journal mode is itself a
    /// write, so a read-only file cannot take one; rather than refuse to open
    /// it, the connection keeps whatever mode the file already has. A
    /// read-only database that was created in WAL mode stays in it and loses
    /// nothing.
    bool wal = true;

    /// Open the database without the ability to change it.
    ///
    /// For a tool that only looks. The scaffold reads a schema and writes C++,
    /// and opening read-write meant a mistyped path created an empty database
    /// rather than saying the file was not there -- and pointing it at a real
    /// one left a -wal and a -shm file beside it, and switched the journal
    /// mode of a database whose application had deliberately not.
    ///
    /// Nothing about the connection can write: SQLite refuses the file's
    /// creation, every statement that would change a row, and every pragma
    /// that would change the file. `wal` is therefore ignored when this is
    /// set, because switching the journal mode is itself a write.
    bool read_only = false;

    /// How long a blocked statement waits for a competing writer before
    /// giving up. SQLite permits exactly one writer at a time; without a
    /// busy timeout, contention surfaces as an immediate error rather than
    /// as a short wait.
    ///
    /// A wait, not a queue. SQLite's busy handler sleeps and tries again, and
    /// a writer that keeps taking the lock -- another process, most often --
    /// can win every one of those tries until this runs out. What surfaces
    /// then is a `lock_timeout`, which the caller has to be ready to retry.
    std::chrono::milliseconds busy_timeout{5000};

    /// Enforce foreign key constraints.
    ///
    /// SQLite leaves these off by default for backwards compatibility,
    /// which silently turns declared relationships into documentation.
    bool foreign_keys = true;
};

[[nodiscard]] std::unique_ptr<driver::Connection> sqlite(SqliteOptions options);
[[nodiscard]] std::unique_ptr<driver::Connection> sqlite(std::string path);

} // namespace schemata
