#pragma once

#include <schemata/core/types.h>
#include <schemata/core/value.h>
#include <schemata/driver/driver.h>
#include <schemata/model/model.h>

namespace schemata {

/// Read one result column as the type the model says it is.
///
/// The model's `ColumnType` decides which accessor is called, not the
/// engine's idea of what it stored. That matters on SQLite, where a column
/// has an affinity rather than a type and a value written as an integer can
/// come back declared as anything: asking for what the model declared keeps
/// the two engines returning the same thing for the same schema.
///
/// Conversion to the C++ type happens afterwards, in `ValueConverter`, which
/// is where a boolean stored as an integer or a timestamp stored as text is
/// put back together.
[[nodiscard]] Value read_column(const driver::Statement& row, int index, ColumnType type);

/// Fill one entity from the current row.
///
/// Columns are read positionally in the model's declaration order, which is
/// the order `QueryPlan::select_sql()` names them. Nothing is matched by
/// name at run time, so a column added to the table out of band cannot shift
/// the mapping.
void materialise(const Table& table, const driver::Statement& row, void* entity);

/// Fill one entity from the current row, matching result columns to mapped
/// columns *by name*.
///
/// The counterpart to `materialise` for SQL this library did not write. A
/// hand-written `SELECT` will not name the columns in the model's declaration
/// order, and reading it positionally would put the values in the wrong
/// fields whenever the two happened to be the same width -- silently, and
/// with no type error to stop it where the types line up.
///
/// Matching is exact first, then case-insensitive over ASCII, because the two
/// engine families disagree about column-name case and neither disagrees
/// about anything else. A mapped column the result does not carry is an
/// error naming that column, not a null.
void materialise_by_name(const Table& table, const driver::Statement& row, void* entity);

/// Fill one entity from the current row, starting at result column `first`.
///
/// `materialise` with an offset, for a row that carries more than one entity
/// side by side -- a join's -- where this one's columns begin part-way along.
/// `materialise(table, row, e)` is `materialise_at(table, row, 0, e)`.
void materialise_at(const Table& table, const driver::Statement& row, int first, void* entity);

/// Fill an entity of a known type.
template <typename T>
void materialise_into(const Table& table, const driver::Statement& row, T& entity) {
    materialise(table, row, &entity);
}

} // namespace schemata
