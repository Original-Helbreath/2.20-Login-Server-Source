#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include <schemata/core/error.h>
#include <schemata/core/value.h>
#include <schemata/driver/driver.h>
#include <schemata/model/model.h>
#include <schemata/query/table_target.h>
#include <schemata/schema/differ.h>
#include <schemata/sql/dialect.h>

namespace schemata {

/// How a multi-row write is cut into statements, and what an upsert keys on.
///
/// Multi-row writes are `insert_many()` and `upsert_many()` for entities, and
/// `insert_rows()` and `upsert_rows()` for a table named with its columns and
/// rows of values. Each sends `INSERT ... VALUES (...), (...), ...`, as many
/// rows to a statement as the engine takes:
///
/// - no more than `max_rows_per_statement`, where that is set;
/// - no more than fit the parameters one statement may bind
///   (`Connection::max_parameters()`: 32766 on a current SQLite, 65,535 on the
///   MySQL family);
/// - on the MySQL family, with the connection's statement cache on and
///   `max_rows_per_statement` not set, no more than bind 4,096 parameters
///   between them -- 372 rows of 11 columns, and always at least one row. A
///   statement carrying a whole call is one that only a call of exactly as
///   many rows can reuse, and the server prepares it at a cost by its
///   parameters; cut this way, every call shares its full statements, kept
///   prepared, and prepares at most its last. With the cache on, 4,684 rows
///   of 11 columns in one statement of 51,524 parameters, prepared for the
///   call, took about two thirds longer than the same rows cut and already
///   prepared (measured, MySQL 8.4 on loopback). With the cache off there is
///   nothing to share -- every statement is prepared and closed -- so the
///   call is left in as few statements as the other bounds allow, as on
///   SQLite, which prepares in the process;
/// - on the MySQL family, no more than fit in nine tenths of the server's
///   `max_allowed_packet`, with each row's size estimated from its values: a
///   string or a blob by its length in bytes, a number -- a bool included,
///   which is bound as a BIGINT -- at its full width, and a few bytes of
///   overhead for each. A row that does not fit on its own is refused, naming
///   it, before anything is sent. The packet is read once per session and
///   kept on the connection: the session's value is read only, so `SET
///   SESSION max_allowed_packet` is refused by the server and a `SET GLOBAL`
///   reaches only sessions started after it. It is read again after
///   `Connection::reset_session()` -- a pool's return, by default -- and by a
///   new connection.
///
/// **What is kept prepared.** With the connection's statement cache on
/// (`Connection::prepare_cached()`; it is off by default), on every engine,
/// a statement's SQL is its shape: the table and its schema, the columns, the
/// kind of write and the number of rows. So a call repeated in the same shape
/// -- one row at a time, or a hundred -- is one Execute a statement where it
/// was a Prepare, an Execute and a Close: after its first call where the
/// cache has a place free, after its second where the cache is full.
///
/// - Every full statement has the same SQL whatever the call, and is kept as
///   any statement is. Within one call it is prepared once and run for each
///   full statement, with the cache on or off.
/// - A statement cut short -- the last of a call, the only one of a call that
///   fits in a statement, or one the packet ended early -- has SQL of its own
///   for each row count, and a caller whose count varies would fill the cache
///   with them. So one takes another statement's place only by coming
///   back:
///   - with a place free in the cache, it is kept at once;
///   - with the cache full, one the connection has not sent before is
///     prepared and closed on its own, and remembered; sent a second time it
///     is kept as any statement is, and the cache's least recently used
///     statement goes for it. The connection remembers the last 64 it sent
///     and did not keep, or twice as many as it may keep where that is more;
///   - a connection keeps no more than
///     `StatementCacheOptions::max_partial_bulk_statements` of them at once
///     -- 16 unless set, and none at zero -- and no more than half of what
///     its cache may hold (`StatementCacheOptions::max_statements`): one in
///     a cache of two or three, none in a cache of one;
///   - one that binds more than 4,096 parameters is not kept.
///
/// **A row count that is not kept is sent as chunks**, on MySQL and MariaDB
/// in a statement cache with 24 places past the row counts it may keep: one
/// of 40 statements or more, at the default bound. Where the rule above would
/// have a statement prepared and closed on its own -- the bound reached or
/// set to zero, a first sending in a full cache, more than 4,096 parameters
/// -- its rows go instead as statements of a few fixed sizes, largest first:
/// 16 rows times a power of two, none past 4,096 parameters, then what is
/// left under 16 rows as one statement. Two hundred rows of eleven columns
/// are 128, 64 and 8. Each size has one SQL text whatever the call, kept in
/// the cache as a full statement is and not counted against the bound, and
/// a shape has 20 of them at eleven columns, 24 at the most. So:
///
/// - a caller whose row count repeats is where it was: its statement
///   prepared on its first call and one Execute after, for as many row
///   counts as the bound allows. Past the bound the ones kept are not
///   displaced by a row count that merely comes by, and give up their place
///   when the cache lets go of them (`max_idle`, or the least recently used
///   in a cache that is full) -- or to a row count sent much more often:
///   one that is 16 of the connection's last 64 statements cut short, and
///   twice what the least sent of the kept ones is besides, takes that
///   one's place. So a row count that starts repeating after the bound is
///   full is chunks for its first 16 calls or so and one Execute after,
///   while row counts that take turns -- 21 of them at a bound of 16, or
///   200 -- never displace one another. A row count that repeats but is
///   under a quarter of the connection's statements cut short does not earn
///   a place that way, and stays chunks while the bound is full: raise
///   `max_partial_bulk_statements` to cover the row counts that repeat;
/// - a caller whose row count varies prepares the chunk sizes and nothing
///   after: no Prepare a call, and two or three Executes where a row count
///   kept is one (2.45 a call for 1 to 200 rows, 1.21 for 30 to 50,
///   measured at the default bound);
/// - a call sent as several chunks is a call of several statements, with
///   what that means below: a transaction of its own, or a savepoint in the
///   one open. A row count of 1 to 15, or exactly a chunk size, is one
///   statement and is sent alone;
/// - the rows left over from a call past the cut are composed the same way,
///   inside the transaction or savepoint the call already has;
/// - the rows written, the count returned and what is refused are the same.
///   A statement the server refuses is reported as it was; the rows of the
///   chunks before it are rolled back with the call.
///
/// On SQLite, which prepares in the process, and in a cache without those
/// 24 places, nothing is composed and nothing else is changed: past the
/// bound, one that is kept takes the place of the least recently sent of the
/// others, and a caller whose calls take more row counts than that prepares
/// and closes on most of them.
///
/// **Sizing the cache for it.** The chunk statements of one shape -- a
/// table, its columns and a kind of write -- take up to 24 cache places,
/// about 20 for most. So a connection composes as many shapes at once as
/// `max_statements` has 24 places for beside the row counts it may keep
/// (`max_partial_bulk_statements`, or half the cache where that is fewer):
/// none in a cache under 40, one from 40, two in a cache of 64, four in
/// one of 128, ten in one of 256, at the default bound of 16. A shape
/// past those is left the rule of a cache with no room, above -- prepared
/// and closed, or keeping a place by its row count -- until one of the
/// composed shapes has not been composed for 256 of the connection's
/// statements cut short, when it takes that one's place. So size
/// `max_statements` at 24 for each table written at varying row counts,
/// plus the row counts kept, plus the caller's own queries: the chunk
/// statements are ordinary cache entries, and in a cache already full of
/// the caller's statements they take the least recently used of those.
///
/// So a caller whose row count never repeats takes no statement's place in a
/// full cache but for the chunk sizes; and a shape that repeats costs a full
/// cache one Prepare more than an empty one, then one Execute a call. One
/// the cache has since let go of -- the least recently used, in a cache too
/// small for what runs through it -- earns its place again the same way.
///
/// With the statement cache off every statement is prepared and closed on its
/// own, but for a call's full statement, above, and nothing is composed: a
/// call that fits in one statement is one. What is kept leaves the cache
/// as any statement does: the least recently used when it is full, one idle
/// past `max_idle`, one the engine refused, and all of them wherever
/// `Connection::prepare_cached()` says the cache is emptied.
///
/// **A call is all or nothing, on a table that keeps transactions** -- InnoDB,
/// and every SQLite table. With no transaction open it runs in one of its own,
/// committed at the end and rolled back if any statement fails. Inside an open
/// transaction it joins it, under a savepoint of its own -- `schemata_bulk_`
/// and a number no other call has had, so a savepoint of the caller's is never
/// replaced -- that a failure rolls back to: a caller who catches the error and
/// goes on finds none of the call's rows, and the caller's own work before it
/// untouched. A call that fits in one statement sends it alone, which is
/// atomic by itself -- unless it is sent as chunks, above, which is a call of
/// several. On a MySQL table that keeps none -- MyISAM, MEMORY and
/// the other non-transactional engines -- `BEGIN`, `ROLLBACK` and savepoints
/// undo nothing, and the rows written before a failure stay written, those of
/// the failing statement ahead of the row it failed on included.
///
/// An open transaction is one the connection began (`Connection::begin()`, a
/// `Context` scope) or one the server reports open that it did not: begun as
/// raw SQL through `execute()`, or the standing transaction of a session at
/// `autocommit = 0` once a statement has work pending in it. That one is
/// joined the same way, under the savepoint, and nothing is committed: it is
/// the caller's to end. Two things the server does not report, on the MySQL
/// family, and a call of several statements then begins and commits a
/// transaction of its own:
///
/// - **`LOCK TABLES`.** The call's `START TRANSACTION` releases the session's
///   table locks, as the server does for any -- as would one the caller
///   sent, or `Connection::begin()`. What keeps them: `SET autocommit = 0`
///   before `LOCK TABLES`, which a call of several statements then leaves
///   held (measured, MySQL 8.4). Keeping the call to one statement does too,
///   but with the statement cache on that is no longer told by the row
///   count alone: one short of a full statement may be sent as chunks.
/// - **`autocommit = 0` with nothing pending.** No transaction is open until
///   a statement starts one, so the call's rows are committed by the call,
///   and a `ROLLBACK` after it does not take them back.
///
/// **It bypasses the change tracker**, as `Query::update()` does: nothing is
/// tracked or snapshotted, and an entity a `Context` tracks keeps the values it
/// had, so a later `save_changes()` writes them back over the rows written
/// here. **Generated keys are not read back.** An identity column is left for
/// the engine to fill, and the entities keep what they held: the MySQL family
/// does not promise one statement's rows consecutive ids -- InnoDB's default
/// `innodb_autoinc_lock_mode` = 2 interleaves them with other sessions' -- so
/// there is no way to tell which row got which. Where an entity needs its key,
/// save it through `ChangeTracker` or `Context::save_changes()`.
///
/// The target schema -- `schema` and `schema_bindings` -- is `TableTarget`'s,
/// which a bulk write shares with the reads and writes by name of
/// `<schemata/query/rows.h>`. See `TableTarget` for how it is resolved and checked.
struct BulkOptions : TableTarget {
    /// Rows per statement at most. Zero, the default, sends as many as the
    /// parameter limit and the packet allow -- and, on the MySQL family with
    /// the statement cache on, no more than bind 4,096 parameters, as above.
    /// Set it to lift that: any value is honoured up to the parameter limit,
    /// so one at or past it (65,535, say) sends a call that fits in one
    /// statement as one.
    std::size_t max_rows_per_statement = 0;

    /// An upsert's key: the columns whose collision makes a row an update.
    ///
    /// Empty means the table's primary key, for an entity or a table the model
    /// holds; a table the model does not hold has to name them. Each must be
    /// one of the columns written. On SQLite they must be the columns of the
    /// primary key or of a unique index -- the engine refuses anything else.
    /// On MySQL and MariaDB the same is checked before anything is sent, for a
    /// reason of the engine's own: `ON DUPLICATE KEY UPDATE` fires on a
    /// collision with *any* unique key of the table, and cannot be told which
    /// one to key on. So a row that collides on another unique key -- a unique
    /// email, with the upsert keyed on the id -- updates the row it collided
    /// with there, where SQLite raises `unique_violation`.
    ///
    /// Refused by `insert_many()` and `insert_rows()`, which have no key.
    std::vector<std::string> key_columns;

    /// The columns an upsert overwrites on a row that is already there.
    ///
    /// Unset means every column written that is not a key column. An empty
    /// list leaves a row that is there as it is: `DO NOTHING` on SQLite, and on
    /// the MySQL family `ON DUPLICATE KEY UPDATE k = k` on the first key
    /// column, which changes nothing. Not `INSERT IGNORE`, which would also
    /// swallow every other error of the statement -- a NOT NULL, a CHECK, a
    /// foreign key -- as a warning. A key column cannot be one of them.
    ///
    /// Refused by `insert_many()` and `insert_rows()`, which update nothing.
    std::optional<std::vector<std::string>> update_columns;
};

namespace detail {

/// A multi-row statement in three parts: `head`, then `rows` copies of `row`
/// separated by ", ", then `tail`.
struct BulkSql {
    std::string head;
    std::string row;
    std::string tail;

    [[nodiscard]] std::string text(std::size_t rows) const;
};

/// What an upsert keys on and overwrites, by column name.
struct BulkUpsert {
    std::vector<std::string> keys;
    std::vector<std::string> updates;
};

/// The statement for a multi-row insert, or an upsert where `upsert` is set.
///
/// `version` decides MySQL's spelling: 8.0.19 and later name the new row with
/// an alias (`AS schemata_row ... c = schemata_row.c`), before it `VALUES(c)`,
/// which 8.0.20 deprecates. MariaDB has no row alias and takes `VALUE(c)`.
///
/// `schema`, where it is not empty, qualifies the table: `schema`.`table`.
[[nodiscard]] BulkSql bulk_sql(Dialect dialect, ServerVersion version, std::string_view table,
                               const std::vector<std::string>& columns,
                               const BulkUpsert*               upsert,
                               std::string_view                schema = {});

/// The bytes one value is counted as against `max_allowed_packet`.
///
/// Its payload -- a string's or a blob's length, eight for any number, a bool
/// included, since the MySQL driver binds one as `MYSQL_TYPE_LONGLONG` -- and
/// the most the protocol spends around it: a length prefix, the type, a bit
/// of the null map.
[[nodiscard]] std::size_t bulk_value_bytes(const Value& value) noexcept;

/// The parameters a MySQL-family statement binds at most where
/// `BulkOptions::max_rows_per_statement` is not set and the statement cache
/// is on; and the most a statement cut short may bind and be kept in it.
inline constexpr std::size_t bulk_default_parameters = 4096;

/// The statements cut short a connection keeps in its statement cache at
/// most unless told otherwise: the default of
/// `StatementCacheOptions::max_partial_bulk_statements`, which is what
/// bounds them, with half the cache's `max_statements`.
inline constexpr std::size_t bulk_kept_shorts = 16;

/// How many statements cut short a connection remembers having sent and not
/// kept, its cache being full, so as to keep one that is sent again, at the
/// least: it remembers twice as many as it may keep where that is more. A
/// hash each. Sized past the 47 calls of the save this was written for, so
/// that a shape sent once a save is still remembered at the next however
/// many other shapes the save sends between.
inline constexpr std::size_t bulk_remembered_shorts = 64;

/// The unit of the statements a short statement is composed of where it is
/// not kept under its own row count: one of 1 to 15 rows, and this many times
/// a power of two.
inline constexpr std::size_t bulk_chunk_unit = 16;

/// The cache places the chunk statements of one shape may come to: 15, and
/// 16 to 4,096 rows of one column, which is nine more. Eleven columns have
/// 20.
inline constexpr std::size_t bulk_composed_shape_places = 24;

/// How many shapes a connection composes at once: as many as the cache has
/// `bulk_composed_shape_places` for beside the `kept` statements it may keep
/// under their own row count -- what
/// `StatementCacheOptions::max_partial_bulk_statements` and half the cache
/// allow. None where it has not that room for one, which leaves such a cache
/// the rule it had: at the default bound of 16, none under a cache of 40,
/// one from 40, two at 64, four at 128, ten at 256.
[[nodiscard]] constexpr std::size_t bulk_composed_shapes(
    std::size_t capacity, std::size_t kept) noexcept {
    return capacity < kept ? 0 : (capacity - kept) / bulk_composed_shape_places;
}

/// How long a shape keeps its place among the ones composed without being
/// composed: this many statements cut short on the connection, after which
/// another shape that asks takes it.
inline constexpr std::uint64_t bulk_composed_shape_idle = 256;

/// How many of the statements cut short a connection sent last it remembers
/// the row counts of, where it composes the ones not kept; and how many of
/// those a row count not kept must be to take a kept one's place -- this
/// many, and twice what the kept one sent least has besides.
inline constexpr std::size_t bulk_recent_shorts   = 64;
inline constexpr std::size_t bulk_earned_sendings = 16;

/// The statements a short statement of `rows` rows is sent as where it is
/// composed, largest first: powers of two times `bulk_chunk_unit`, then what
/// is left under sixteen rows as one statement.
///
/// No part binds more than `bulk_default_parameters`, so the sizes for a
/// shape are few and fixed whatever the call: 200 rows of eleven columns are
/// 128, 64 and 8. Rows too wide for sixteen to a statement are sent as many
/// as fit at a time. A part is never larger than the statement it stands
/// for, so what cut that one -- `max_rows_per_statement`, the parameter
/// limit, the packet -- holds for each.
///
/// \returns empty where one row binds more than `bulk_default_parameters`,
///          or has no column: such a statement is not composed.
[[nodiscard]] std::vector<std::size_t> compose_bulk_statement(
    std::size_t rows, std::size_t columns);

/// What a multi-row write is cut to fit.
struct BulkChunkLimits {
    /// Values per row.
    std::size_t columns = 0;

    /// `BulkOptions::max_rows_per_statement`; zero for no bound of its own.
    std::size_t max_rows = 0;

    /// `Connection::max_parameters()`.
    std::size_t max_parameters = 0;

    /// The parameters a statement binds where `max_rows` is zero:
    /// `bulk_default_parameters` on the MySQL family with the statement
    /// cache on; zero for no bound but `max_parameters`, as on SQLite and
    /// with the cache off.
    std::size_t default_parameters = 0;

    /// The bytes a statement's SQL and parameters may come to; zero where no
    /// packet bounds them, as on SQLite.
    std::size_t packet_budget = 0;

    /// The SQL outside the rows: `BulkSql::head` and `tail`.
    std::size_t fixed_bytes = 0;

    /// One row's SQL and the separator after it.
    std::size_t row_text_bytes = 0;

    /// For the message when a row fits nowhere.
    std::string_view table;
};

/// Rows per full statement: `max_rows`, bounded by the parameters a statement
/// may bind; with no `max_rows`, as many as bind `default_parameters`, one at
/// the least.
///
/// \throws query_build_error when one row has more columns than a statement
///         may bind parameters.
[[nodiscard]] std::size_t bulk_rows_per_statement(const BulkChunkLimits& limits);

/// How many rows each statement carries, in order.
///
/// Each takes rows until it is full or, with a `packet_budget`, the next row
/// would take it past the budget.
///
/// \throws query_build_error naming the row that does not fit in the budget on
///         its own.
[[nodiscard]] std::vector<std::size_t> plan_bulk_chunks(
    const std::vector<std::vector<Value>>& rows, const BulkChunkLimits& limits);

/// The entity forms, once the entities are type-erased.
[[nodiscard]] std::int64_t bulk_entities(driver::Connection& connection, const Model& model,
                                         std::type_index                 entity,
                                         const std::vector<const void*>& entities,
                                         const BulkOptions& options, bool upsert);

template <typename R>
concept EntityRange =
    std::ranges::forward_range<const R> &&
    std::is_lvalue_reference_v<std::ranges::range_reference_t<const R>> &&
    std::is_class_v<std::ranges::range_value_t<R>> &&
    !std::is_same_v<std::ranges::range_value_t<R>, std::vector<Value>>;

template <EntityRange R>
[[nodiscard]] std::int64_t bulk_range(driver::Connection& connection, const Model& model,
                                      const R& entities, const BulkOptions& options,
                                      bool upsert) {
    using T = std::remove_cvref_t<std::ranges::range_value_t<R>>;
    std::vector<const void*> pointers;
    if constexpr (std::ranges::sized_range<const R>) {
        pointers.reserve(static_cast<std::size_t>(std::ranges::size(entities)));
    }
    for (const auto& entity : entities) {
        pointers.push_back(std::addressof(entity));
    }
    return bulk_entities(connection, model, std::type_index(typeid(T)), pointers, options,
                         upsert);
}

} // namespace detail

/// Insert every entity of `entities` -- a vector, an array, a span -- with
/// multi-row `INSERT` statements.
///
/// Writes what a tracked insert writes: every mapped column but a generated
/// key, through the same converters, each value refused as a save refuses it.
/// See `BulkOptions` for how the rows are cut into statements, the transaction
/// a call runs in, and why the generated keys are not read back.
///
/// Written to the table in `BulkOptions::schema`, where that is set.
///
/// \returns the rows inserted, as the engines count them.
/// \throws  query_build_error for an entity the model does not hold, or when
///          `options` names key or update columns; schema_error for a
///          `BulkOptions::schema` refused as that says; whatever the driver
///          raises otherwise, with nothing of the call's left written on a
///          table that keeps transactions.
template <detail::EntityRange R>
std::int64_t insert_many(driver::Connection& connection, const Model& model, const R& entities,
                         const BulkOptions& options = {}) {
    return detail::bulk_range(connection, model, entities, options, false);
}

/// Insert the entities, or overwrite the rows already there under their key.
///
/// The key is `BulkOptions::key_columns`, the primary key by default. The
/// columns written are every mapped column but a generated key, which is
/// written too where it is one of the key columns. An entity already saved
/// sends the key it holds, and finds its row by it. A new one -- holding the
/// zero of an entity never saved -- sends NULL there, so the row is inserted
/// with a key the engine generates, on every engine and whatever the MySQL
/// family's `sql_mode` (`NO_AUTO_VALUE_ON_ZERO` included); as on
/// `insert_many()`, that key is not read back into the entity. So a stored row
/// whose generated key is 0 cannot be reached by this call: an entity holding
/// 0 is new, and is inserted under a key of its own. Where the key is another
/// unique key, a generated one is left out: a row inserted is given one, a row
/// overwritten keeps its own.
///
/// SQL per engine, with `k` the key and `c` a column overwritten:
///
/// - SQLite: `INSERT ... VALUES ... ON CONFLICT (k) DO UPDATE SET c =
///   excluded.c`, or `DO NOTHING` for an empty `update_columns`;
/// - MySQL 8.0.19 and later: `INSERT ... VALUES ... AS schemata_row ON
///   DUPLICATE KEY UPDATE c = schemata_row.c`, and `c = VALUES(c)` before it;
/// - MariaDB: `... ON DUPLICATE KEY UPDATE c = VALUE(c)`.
///
/// On the MySQL family the key must be the table's primary key or one of its
/// unique indexes, as the model has it -- see `BulkOptions::key_columns` for
/// why -- and anything else is refused with nothing sent.
///
/// \returns the engines' own count, which differs. SQLite counts one for each
///          row inserted or overwritten, and none for one `DO NOTHING` left.
///          MySQL and MariaDB count one for each row inserted, two for each
///          overwritten, and none for one already holding the values -- or one
///          for that, on a connection opened with `MySqlOptions::found_rows`.
template <detail::EntityRange R>
std::int64_t upsert_many(driver::Connection& connection, const Model& model, const R& entities,
                         const BulkOptions& options = {}) {
    return detail::bulk_range(connection, model, entities, options, true);
}

/// Insert rows of values into `table`, one value per column of `columns` in
/// each row.
///
/// For a table no entity maps, or rows that are not entities. These and
/// `upsert_rows()` (and their `Context` forwarders) take the table and column
/// names from the caller rather than the model -- one of the few APIs that do,
/// listed in `<schemata/core/identifier.h>`: each is quoted by the same
/// `quote_identifier()` as the model's, and an empty one, or one holding a
/// NUL, is refused with `query_build_error` before anything is sent.
/// Every value is a bound parameter. A row of the wrong width is refused
/// naming it, and so is a column named twice -- in any case, since no engine
/// tells column names apart by case. No rows returns 0 with nothing sent.
///
/// `table` is a bare name, of a table in `BulkOptions::schema` -- the
/// connection's default schema, unless that is set. A schema-qualified `db.t`
/// is not supported: it is quoted as one name, dot and all, and names a table
/// called that. Set `BulkOptions::schema` to write into another schema.
///
/// The overload taking a `Model` checks a table it holds: a column it does not
/// map is refused, and each value is held to its column as a save holds it.
/// Rows are written as given all the same, generated key and all.
///
/// \returns the rows inserted, as the engines count them.
std::int64_t insert_rows(driver::Connection& connection, std::string_view table,
                         const std::vector<std::string>&        columns,
                         const std::vector<std::vector<Value>>& rows,
                         const BulkOptions&                     options = {});

std::int64_t insert_rows(driver::Connection& connection, const Model& model,
                         std::string_view table, const std::vector<std::string>& columns,
                         const std::vector<std::vector<Value>>& rows,
                         const BulkOptions&                     options = {});

/// Insert rows of values into `table`, or overwrite the rows already there
/// under their key.
///
/// As `upsert_many()`, and as `insert_rows()` for the rows. The key has to be
/// named for a table the model does not hold; on the MySQL family it is then
/// checked against the table's unique indexes as the server reports them
/// (`SHOW INDEX`, which sees a `TEMPORARY` table and takes the name exactly),
/// a prefix or functional index not counting. A table the server does not
/// find there -- in `BulkOptions::schema`, or the session's default schema --
/// is refused with `query_build_error`, naming it, nothing sent; so is one
/// where the session has no default schema and none is named. A view has no
/// indexes of its own, so an upsert into one is refused as keyed on nothing,
/// as is one into a table with no unique key; the refusal says to upsert into
/// the base table in case it is a view.
///
/// What the key check costs, on the MySQL family: for a table the model
/// holds, nothing -- it is checked against the model. For one it does not,
/// one `SHOW INDEX` the first time a connection upserts into it, and none
/// after: the keys the server reported are kept on the connection, by target
/// schema and table, until its statement cache is next emptied -- a `USE` or
/// DDL sent through it, `reset_session()` (a pool's return, by default), a
/// pool's catch-up after DDL another lease sent, or
/// `Connection::clear_statement_cache()`. That holds whether the statement
/// cache is on or not. So a pooled caller at the default
/// `PoolOptions::reset_session_on_return` = true pays one `SHOW INDEX` per
/// lease for such a table, and no lookup of `BulkOptions::schema` after the
/// connection's first write into it: the reset keeps that. A unique key added
/// or dropped by another connection or process is not seen until then, as a
/// column renamed there is not seen by the statement cache: follow such DDL
/// with `clear_statement_cache()`, or `ConnectionPool::clear_statement_caches()`.
/// Until then an upsert keyed on a unique index since dropped is let through,
/// and `ON DUPLICATE KEY UPDATE` matches rows on whatever unique keys the table
/// has left; one keyed on an index since added is refused.
std::int64_t upsert_rows(driver::Connection& connection, std::string_view table,
                         const std::vector<std::string>&        columns,
                         const std::vector<std::vector<Value>>& rows,
                         const BulkOptions&                     options = {});

std::int64_t upsert_rows(driver::Connection& connection, const Model& model,
                         std::string_view table, const std::vector<std::string>& columns,
                         const std::vector<std::vector<Value>>& rows,
                         const BulkOptions&                     options = {});

} // namespace schemata
