#pragma once

#include <string>

#include <schemata/schema/differ.h>

namespace schemata {

/// The schema a call that names its table reaches it in.
///
/// Shared by every call that takes a table by name from the caller: the
/// multi-row writes of `<schemata/query/bulk.h>` (`BulkOptions` is one), and
/// the reads and writes of `<schemata/query/rows.h>` (`ReadOptions` is one;
/// the writes take it as it is).
struct TableTarget {
    /// The schema the table is in, when it is not the session's own: a
    /// physical name, or an alias in `schema_bindings`. Empty, the default,
    /// is the connection's default schema.
    ///
    /// For one connection reaching several schemas -- a session defaulted to
    /// `information_schema`, say, that names every table's schema. No `USE`
    /// is sent: every statement qualifies the table with the schema -- the
    /// statement itself, and whatever the call asks the server about the
    /// table first (`SHOW INDEX` for an upsert's key, `SHOW COLUMNS` for
    /// `insert_row()`'s generated key). So the session's default schema is
    /// what it was afterwards, and the statement cache keeps what it holds. A
    /// statement is cached under its text, which names the schema, so two
    /// schemas never share one.
    ///
    /// Resolved as `DiffOptions::schema` is: a name `schema_bindings` binds,
    /// looked up by its exact spelling, is the physical name it is bound to;
    /// any other is taken as physical. Refused with `schema_error` before
    /// anything is sent: on SQLite, where a connection is one database --
    /// `main` included -- and a name that cannot be an identifier. A schema
    /// the server does not have is refused by name with `schema_error`, before
    /// anything is read or written: asked once per connection, and remembered
    /// until a `USE` or DDL through it, a pool's catch-up, or
    /// `Connection::clear_statement_cache()` -- not across `reset_session()`,
    /// which keeps it, so a pooled connection asks once and not once per
    /// lease. A remembered schema dropped since is refused the same way, and
    /// forgotten, when a call finds it gone.
    /// Unlike the schema calls' target, this needs no default schema on the
    /// connection: nothing is switched, so nothing is switched back.
    std::string schema{};

    /// Where the aliases `schema` may name live. The same map as
    /// `DiffOptions::schema_bindings`, and it can be the same object; only
    /// `SchemaBinding::name` is read here.
    SchemaBindings schema_bindings{};
};

} // namespace schemata
