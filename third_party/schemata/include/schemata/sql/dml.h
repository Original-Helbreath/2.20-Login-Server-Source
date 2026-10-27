#pragma once

#include <optional>
#include <vector>

#include <schemata/core/value.h>
#include <schemata/model/model.h>
#include <schemata/sql/dialect.h>
#include <schemata/sql/expr.h>

namespace schemata {

/// Statement generation for rows, as `ddl.h` is for schema.
///
/// Everything here takes an entity as `const void*` alongside its `Table`,
/// reaching the fields through the type-erased accessors the model holds. So
/// none of it is a template, all of it compiles once, and all of it is
/// testable without a database: a statement and its parameter list are just
/// a value to compare against.
///
/// No value is ever interpolated. Every column value, and every key value in
/// a WHERE clause, is a bound parameter.

/// Every mapped column's current value, in declaration order.
///
/// This is what the change tracker snapshots so it can tell later which
/// columns actually changed.
[[nodiscard]] std::vector<Value> capture(const Table& table, const void* entity);

/// `INSERT INTO ... VALUES (...)`.
///
/// An identity column is omitted so the engine generates it. Its value is
/// read back afterwards and written into the entity, which is why it must
/// not be sent: a zero would otherwise be taken as the key the caller wanted.
[[nodiscard]] SqlFragment insert_statement(const Table& table, const void* entity,
                                           Dialect dialect);

/// `UPDATE ... SET ... WHERE <key>`, naming only the columns that changed.
///
/// \returns nothing when no column differs from `before`, so an unchanged
///          entity costs no round trip at all.
[[nodiscard]] std::optional<SqlFragment> update_statement(const Table&              table,
                                                          const void*               entity,
                                                          const std::vector<Value>& before,
                                                          Dialect                   dialect);

/// `DELETE FROM ... WHERE <key>`, addressing the row by the entity's
/// current key.
///
/// For a one-off delete of something that was never tracked.
[[nodiscard]] SqlFragment delete_statement(const Table& table, const void* entity,
                                           Dialect dialect);

/// `DELETE FROM ... WHERE <key>`, addressing the row by its snapshotted key.
///
/// The form to use for a tracked entity, and the counterpart to
/// `update_statement`: the key in the WHERE has to be the row's key as the
/// database has it. Taking it from the entity would address whatever the
/// caller last assigned, which may be an entirely different row.
[[nodiscard]] SqlFragment delete_statement(const Table&              table,
                                           const std::vector<Value>& before,
                                           Dialect                   dialect);

/// The single identity column, if this table has one.
///
/// Used to decide whether a generated key needs reading back after an
/// insert. Null when the key is supplied by the caller.
[[nodiscard]] const Column* identity_column(const Table& table);

} // namespace schemata
