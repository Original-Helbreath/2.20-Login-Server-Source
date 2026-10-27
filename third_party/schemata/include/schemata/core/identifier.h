#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <schemata/sql/dialect.h>

namespace schemata {

/// Identifier handling.
///
/// SQL has no way to parameterise a table or column name: bound parameters
/// are values only. Identifiers are therefore the one place where text
/// reaches the server as syntax rather than as data, which makes them the
/// real injection surface in any mapping library.
///
/// Schemata closes that surface structurally rather than by filtering. Every
/// identifier is quoted on emit by the functions below. Most originate in the
/// model; these public APIs take one from the caller instead, and each is
/// quoted by the same `quote_identifier()` as the model's names:
///
/// - `insert_rows()` and `upsert_rows()` in `<schemata/query/bulk.h>`, and
///   their `Context` forwarders: a table and its column names.
/// - `select_rows()`, `for_each_row()`, `count_rows()`, `max_of()`,
///   `insert_row()`, `update_rows()` and `delete_rows()` in
///   `<schemata/query/rows.h>`, and their `Context` forwarders: a table, the
///   columns read, written or assigned (`ColumnValues`), the column of
///   `max_of()`, and every column a `RowFilter` from `where::` names or an
///   `OrderBy` sorts by.
/// - `TableTarget::schema` -- of `BulkOptions`, `ReadOptions` and the writes
///   above -- or the name an alias is bound to in
///   `TableTarget::schema_bindings`: a schema name, qualifying the table.
/// - `driver::Connection::savepoint()`, `release_savepoint()` and
///   `rollback_to()`, and `Context::savepoint()`, `release_savepoint()` and
///   `rollback_to()`, which reach them: a savepoint name.
/// - `ensure_database()`: a database name.
/// - `copy_schema()`: the schema copied and the schema made, and
///   `DiffOptions::backup_schema`, the schema `ensure_schema()` copies into
///   before a destructive change: a schema name, in `CREATE DATABASE` and in
///   every table the copy names.
/// - The target schema of `DiffOptions::schema` (`ensure_schema()`),
///   `MigrateOptions::schema` (`migrate()`), and the `schema` argument of
///   `verify_schema()`, `Context::verify_schema()`, `read_schema()`,
///   `recorded_fingerprint()`, `applied_migrations()` and
///   `pending_migrations()`: a schema name, sent as `USE`.
/// - `SchemaBinding::name` in `DiffOptions::schema_bindings`: a schema name,
///   in the `REFERENCES` of a key into it.
/// - The DDL builders of `<schemata/sql/ddl.h>` that take a name as a string
///   -- `drop_table_sql()`, `rename_table_sql()`, `drop_index_sql()`,
///   `drop_column_sql()`, `drop_trigger_sql()`, `drop_foreign_key_sql()`,
///   `add_check_sql()`, `drop_check_sql()` and the table argument of the
///   rest: whatever they are handed.
///
/// The calls that send a statement refuse an empty name, or one holding a
/// NUL, as the caller's mistake before the name is sent: the tables and
/// columns of the bulk writes and of the reads and writes by name, and the
/// savepoints, with `query_build_error`, the schema and database names -- a
/// `TableTarget`'s schema and a copy's among them -- with `schema_error`, as
/// `identifier_problem()` finds them (an empty target schema is not a name:
/// it means the session's own). The DDL builders only produce text, and leave
/// that to `quote_identifier()`.

/// Quote an identifier for the given dialect.
///
/// Escaping is by delimiter doubling, which is the complete and standard
/// escape in both families: a backtick inside a MySQL identifier becomes
/// two backticks, a double quote inside a SQLite identifier becomes two
/// double quotes. The result is therefore safe for any input that is a
/// valid C++ string without an embedded NUL.
///
/// \throws std::invalid_argument if \p name is empty or contains a NUL. From
///         a name the model holds that is a defect in the model; from one a
///         caller handed a DDL builder directly, the caller's. The APIs that
///         send a statement check a caller's name first and raise their own
///         error, so this is not what their callers see.
[[nodiscard]] std::string quote_identifier(std::string_view name, Dialect d);

/// Check an identifier against the dialect's own rules.
///
/// Distinct from quoting: quoting makes an identifier *safe*, this makes it
/// *legal*. A name that passes quoting may still be rejected by the server
/// for being too long or ending in a space, and it is better to catch that
/// while building the model than at migration time.
///
/// What it deliberately does **not** do is demand a tidy name. A table called
/// `ev"il; DROP TABLE item; --` passes, and should: it is a name the servers
/// accept, delimiter doubling quotes it correctly, and the row it protects
/// survives. Refusing it would be filtering, and filtering is the weaker of
/// the two ways to close this surface -- it has to anticipate every hostile
/// spelling, where quoting has to be right once.
///
/// That is why this is more permissive than `collation_problem` just below,
/// which demands a bare word for the opposite reason: a collation name cannot
/// be portably quoted at all, so there is nothing there to be right once.
/// The two rules differ because the two surfaces differ, not by oversight.
///
/// \returns nothing if the identifier is acceptable, otherwise a short
///          description of the problem.
[[nodiscard]] std::optional<std::string> identifier_problem(std::string_view name,
                                                            Dialect          d);

/// Check a collation name against what can safely be emitted.
///
/// A collation is the second thing that reaches the server as syntax rather
/// than as data, and unlike an identifier it cannot be portably quoted: MySQL
/// accepts a bare word or a string literal, SQLite accepts only a bare word.
/// So the surface is closed the same way, structurally rather than by
/// escaping -- a collation name is a bare word of letters, digits and
/// underscores, and anything else is refused before it can be written.
///
/// Every real collation on all three engines is already such a word:
/// `utf8mb4_0900_ai_ci`, `utf8mb4_uca1400_ai_ci`, `NOCASE`, `BINARY`, `RTRIM`.
///
/// Stricter than `identifier_problem` above, and for a reason that does not
/// apply there: an identifier can be quoted, so its rule only has to be about
/// what the server will accept. This one has no quoting to fall back on, so
/// the rule is the whole defence. A name nobody would write is refused here
/// and permitted there, which is the asymmetry working rather than failing.
///
/// \returns nothing if the name is acceptable, otherwise a short description
///          of the problem.
[[nodiscard]] std::optional<std::string> collation_problem(std::string_view name);

/// Longest identifier the dialect accepts, in characters.
///
/// MySQL and MariaDB cap identifiers at 64 characters -- characters, not
/// bytes, so a name of 64 two-byte characters is 128 bytes and still legal.
/// SQLite imposes no limit of its own beyond the statement length limit, so
/// Schemata applies the MySQL cap everywhere by default: a model that stays
/// portable is worth more than a few extra characters in a column name.
[[nodiscard]] constexpr std::size_t max_identifier_length(Dialect) noexcept {
    return 64;
}

/// A name this library made up, shortened where it would not fit.
///
/// Generated names join a prefix, a table and a column or two -- `FK_`, the
/// table, the table it references, the column -- and nothing stops that
/// passing the limit `identifier_problem` holds every name to. A real schema
/// reached 65 characters with one foreign key. The library then refused a name
/// it had invented itself, and the only way past was to name the constraint by
/// hand.
///
/// A name that fits comes back unchanged, so nothing that could ever have been
/// applied is renamed. One that does not keeps as much of its start as fits
/// beside an underscore and eight hex digits of the whole name's hash: two long
/// names that share a start still come out different, and one long name comes
/// out the same every time, which is what a differ comparing names needs.
/// Shortened by characters, never through the middle of one.
[[nodiscard]] std::string generated_identifier(std::string name, Dialect d);

namespace detail {

/// What a message can print of a name the caller handed in: the part before
/// its first NUL, or all of it when it has none. A NUL in the text would end
/// `what()` there, and the reason after the name would go with it.
[[nodiscard]] inline std::string_view before_nul(std::string_view name) noexcept {
    return name.substr(0, name.find('\0'));
}

/// Refuse a name a caller handed the library that `quote_identifier()` cannot
/// quote: an empty one, or one holding a NUL. `what` is the message's subject
/// -- "the savepoint name", say.
///
/// Called before anything is sent, so the mistake is reported as the caller's
/// rather than as the model defect `quote_identifier()` would take it for. A
/// driver of one's own calls it from its savepoint calls, as the two here do.
///
/// \throws query_build_error
void require_quotable(std::string_view name, std::string_view what);

} // namespace detail

} // namespace schemata
