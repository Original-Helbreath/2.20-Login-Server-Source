#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <schemata/model/model.h>
#include <schemata/sql/dialect.h>

namespace schemata {

/// DDL generation.
///
/// Pure string production: nothing here touches a connection, so every
/// statement Schemata can emit is testable against a golden expectation with
/// no database present. That is where the majority of dialect regressions
/// get caught.

/// Render a column's type in this dialect.
///
/// The pivot of the type system. `ColumnType` is engine-neutral; this is the
/// only place that knows what each engine calls it.
///
/// `strict` asks for the spelling a SQLite `STRICT` table accepts, which is
/// one of INTEGER, REAL, TEXT and BLOB: no length, no `DECIMAL(p,s)`, and a
/// decimal stored as the REAL it is bound as. Nothing on the MySQL family.
[[nodiscard]] std::string column_type_sql(const Column& column, Dialect dialect,
                                          bool strict = false);

/// The engine's own collation for each column of one table, by column name.
///
/// Collation aliases are a model concept and resolving one needs the model,
/// so nothing below ever sees an alias: by the time DDL is generated a
/// collation is whatever this engine calls it, or nothing at all. That keeps
/// these functions pure string production, which is what makes every
/// statement Schemata can emit testable without a database.
///
/// Two things put an entry in here, and they are indistinguishable by the
/// time they arrive: a collation the model asked for, and one the database
/// already had that a rewrite must not destroy.
class ColumnCollations {
public:
    void set(std::string_view column, std::string_view collation);

    /// The collation for a column, or empty when it has none.
    [[nodiscard]] std::string_view get(std::string_view column) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    /// The engine's name for the table's own default collation, emitted by
    /// `create_table_sql` on the MySQL family as `COLLATE=` after the
    /// character set. Empty leaves the character set's default.
    void set_table_default(std::string_view collation) { table_default_ = std::string{collation}; }
    [[nodiscard]] std::string_view table_default() const noexcept { return table_default_; }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
    std::string                                      table_default_;
};

/// The type to declare for a column, where the database's own spelling is to
/// be kept rather than the model's, by column name.
///
/// The same argument as `ColumnCollations`, one level along, and the reason it
/// is a second class rather than a second use of that one: a collation entry
/// says what a column is collated as, and a type entry says how to spell a
/// type the model and the database already agree on.
///
/// SQLite keeps a column's declared type as the text it was given, and other
/// readers go by that text: Python's `PARSE_DECLTYPES` turns a `DATE` column
/// into a `date` object and a `TIMESTAMP` into a `datetime`. `NUMERIC(10,2)`,
/// `DATE` and `NVARCHAR(30)` all mean exactly what the model means, so a
/// rebuild that regenerated `DECIMAL(10,2)`, `DECIMAL(10,0)` and `VARCHAR(30)`
/// changed no schema Schemata can see and every schema those readers can.
///
/// Only ever filled in with a spelling that compares equal to the model's own
/// -- see `carried_declared_type` in the differ. A type the model has actually
/// changed is written the model's way.
class ColumnTypes {
public:
    void set(std::string_view column, std::string_view declared_type);

    /// The type for a column, or empty to render the model's own.
    [[nodiscard]] std::string_view get(std::string_view column) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

/// Render one column's definition as it appears inside CREATE TABLE.
///
/// `inline_primary_key` asks for SQLite's special form, where a single
/// integer identity key must be spelled `INTEGER PRIMARY KEY AUTOINCREMENT`
/// on the column itself rather than in a separate clause.
///
/// `collation` is the engine's own name, emitted verbatim as `COLLATE <name>`
/// and refused outright if it is not a bare word -- see `collation_problem`.
///
/// It defaults to nothing so that a caller rendering a column for its own sake
/// -- a test, a tool printing DDL -- need not carry one. **Any caller emitting
/// a statement that will run against a live table must pass it**, including
/// when the model says nothing: a `MODIFY` that omits the collation resets the
/// column to the table default, and a rebuild regenerates the table without
/// it. `collation_for()` in the differ is what works out which value that is.
///
/// `declared_type` overrides the type this column would otherwise render as.
/// Empty -- the usual case -- renders the model's own; see `ColumnTypes` for
/// the one caller that passes anything else and why.
///
/// `strict` is for a column of a SQLite `STRICT` table -- see
/// `column_type_sql`. A `declared_type` such a table would refuse is not
/// used.
[[nodiscard]] std::string column_definition_sql(const Column&    column,
                                                Dialect          dialect,
                                                bool             inline_primary_key,
                                                std::string_view collation     = {},
                                                std::string_view declared_type = {},
                                                bool             strict        = false);

/// `CREATE TABLE`, including primary key and foreign key clauses.
///
/// Unique constraints are *not* emitted as constraints. They are unique
/// indexes, because that is how both engines report them when read back:
/// MySQL lists them in `STATISTICS` and SQLite in `pragma_index_list`.
/// Emitting them the way they are read keeps the differ from seeing drift that
/// is not there.
///
/// By default every index is left to `table_index_statements`. With
/// `with_indexes` on the MySQL family they are declared inside this statement
/// instead -- `UNIQUE KEY` and `KEY` under the same names and columns, which
/// read back identically -- so that a new table and its indexes arrive in one
/// statement where every statement commits on its own. SQLite has no inline
/// index syntax, and `with_indexes` changes nothing there.
///
/// `types` names the columns whose declared type is to be written as the
/// database already spells it rather than as the model would -- see
/// `ColumnTypes`. Only the SQLite rebuild fills it in, and only for a type
/// both sides already agree on.
///
/// A `Table::strict` table ends `STRICT` on SQLite, with its columns spelled
/// the way that requires.
[[nodiscard]] std::string create_table_sql(const Table& table, Dialect dialect,
                                           const ColumnCollations& collations   = {},
                                           const ColumnTypes&      types        = {},
                                           bool                    with_indexes = false);

/// The triggers a table needs that its column definitions could not express.
///
/// Empty on the MySQL family, which says `ON UPDATE CURRENT_TIMESTAMP` in the
/// column itself. SQLite has no such clause, so a column that asks for it
/// gets an `AFTER UPDATE` trigger instead -- one per column, named after it,
/// so that adding or dropping one touches nothing else.
///
/// The trigger finds the row to stamp by the table's primary key, so `table`
/// must carry its key columns as well as the column being stamped -- the
/// reader regenerates this same text to recognise the trigger as the
/// library's, and a statement generated without them does not match one
/// generated with them.
[[nodiscard]] std::vector<std::string> table_trigger_statements(const Table& table,
                                                                Dialect     dialect);

/// The name of the trigger backing `on_update_now` for one column.
[[nodiscard]] std::string on_update_trigger_name(std::string_view table,
                                                 std::string_view column);

/// Drop a trigger. SQLite scopes trigger names per database, not per table,
/// so no table is named here.
[[nodiscard]] std::string drop_trigger_sql(std::string_view name, Dialect dialect);

/// Every index statement for a table, including those implied by a column
/// marked unique.
[[nodiscard]] std::vector<std::string> table_index_statements(const Table& table,
                                                              Dialect      dialect);

/// The name of the unique index implied by a column marked `unique()`.
///
/// Deterministic, so that the differ can match what it reads back against
/// what the model implies.
[[nodiscard]] std::string implicit_unique_index_name(std::string_view table,
                                                     std::string_view column);

/// The name a column's own CHECK is given on the MySQL family:
/// `CK_<table>_<column>`, shortened as every generated name is.
///
/// One name whichever way the CHECK arrives -- in the table's `CREATE TABLE`
/// or added to a table already there -- so a schema built in one run and one
/// grown over several read back alike. Only for naming: the differ matches a
/// CHECK by what it says, never by what it is called.
[[nodiscard]] std::string column_check_name(std::string_view table, std::string_view column);

[[nodiscard]] std::string create_index_sql(std::string_view table, const Index& index,
                                           Dialect dialect);
[[nodiscard]] std::string drop_index_sql(std::string_view table, std::string_view index,
                                         Dialect dialect);

[[nodiscard]] std::string drop_table_sql(std::string_view table, Dialect dialect);

/// `ALTER TABLE ... RENAME TO ...`. The one ALTER all three engines agree on.
[[nodiscard]] std::string rename_table_sql(std::string_view from, std::string_view to,
                                           Dialect dialect);

/// `ALTER TABLE <table> RENAME COLUMN <from> TO <to>`: SQLite 3.25+, MySQL 8.0,
/// MariaDB 10.5.2+. In place on all three, every row keeping its value. See
/// `Column::renamed_from`.
[[nodiscard]] std::string rename_column_sql(std::string_view table, std::string_view from,
                                            std::string_view to, Dialect dialect);

/// `ALTER TABLE ... ADD COLUMN`.
///
/// SQLite restricts this more than MySQL does: the column cannot be a
/// primary key or unique, and cannot be NOT NULL without a default. Callers
/// that need those must rebuild the table instead.
///
/// `strict` says the live table is a SQLite `STRICT` one, which refuses a
/// column declared any other way -- whatever the model says about it.
///
/// `after` places the column, on the MySQL family: `AFTER` the column it
/// names, or `FIRST` where it names none (an empty string). Left out, the
/// column goes last, which is all SQLite can do -- it is ignored there.
[[nodiscard]] std::string add_column_sql(std::string_view table, const Column& column,
                                         Dialect dialect, std::string_view collation = {},
                                         bool                              strict = false,
                                         const std::optional<std::string>& after  = {});

/// One column `reorder_columns_sql` moves: redefined as `column` with
/// `collation`, `AFTER` the column `after` names, or `FIRST` where it is empty.
///
/// `definition`, where it is not empty, is written in place of `column` and
/// `collation`, verbatim: the live column's own clause, as `SHOW CREATE TABLE`
/// prints it, so the move changes its place and nothing else.
struct ColumnMove {
    Column      column;
    std::string collation;
    std::string after;
    std::string definition{};
};

/// `ALTER TABLE ... MODIFY COLUMN ... AFTER ...`, one clause a column, for the
/// MySQL family only; SQLite rebuilds the table instead.
///
/// The clauses run in order, each against the order the ones before it left.
/// `instant` asks for `ALGORITHM=INSTANT`, which the server refuses (1845,
/// 1846) rather than copy the table.
[[nodiscard]] std::string reorder_columns_sql(std::string_view               table,
                                              const std::vector<ColumnMove>& moves,
                                              Dialect dialect, bool instant);

[[nodiscard]] std::string drop_column_sql(std::string_view table, std::string_view column,
                                          Dialect dialect);

/// `ALTER TABLE ... MODIFY COLUMN`, for the MySQL family only.
///
/// SQLite has no equivalent at all, which is why the differ has to be able
/// to express a table rebuild rather than only a list of ALTERs.
[[nodiscard]] std::string modify_column_sql(std::string_view table, const Column& column,
                                            Dialect dialect,
                                            std::string_view collation = {});

[[nodiscard]] std::string add_foreign_key_sql(std::string_view table, const ForeignKey& fk,
                                              Dialect dialect);
[[nodiscard]] std::string drop_foreign_key_sql(std::string_view table, std::string_view name,
                                               Dialect dialect);

/// Give a foreign key another name, MySQL family only, in one statement:
/// `ALTER TABLE t DROP FOREIGN KEY <from>, ADD CONSTRAINT <fk.name> FOREIGN
/// KEY ... <actions>` -- neither server has a RENAME for a key. With
/// `index_from`, `, RENAME INDEX <index_from> TO <fk.name>` as well: the index
/// InnoDB made for the key under its old name, which DROP FOREIGN KEY leaves
/// standing and the new key would go on resting on under that name.
///
/// Meant to run with the session's `foreign_key_checks` at 0, so the rows are
/// not read again: the key given up enforced the same rule on them until the
/// statement ran. Both servers apply the three clauses together or not at
/// all. See `SchemaOperation::suspends_foreign_keys`.
[[nodiscard]] std::string rename_foreign_key_sql(std::string_view  table,
                                                 std::string_view  from,
                                                 const ForeignKey& fk, Dialect dialect,
                                                 std::string_view index_from = {});

/// Add or drop a CHECK constraint on the MySQL family.
///
/// SQLite has no ALTER for either, so both throw there: a CHECK that has to
/// come or go on SQLite is a table rebuild, and the differ knows that before
/// it gets here. The expression is emitted verbatim, as it is everywhere else
/// -- see `Column::check`.
[[nodiscard]] std::string add_check_sql(std::string_view table, std::string_view name,
                                        std::string_view expression, Dialect dialect);
[[nodiscard]] std::string drop_check_sql(std::string_view table, std::string_view name,
                                         Dialect dialect);

/// Replace a CHECK in one statement, MySQL family only: `drop_check_sql()`'s
/// DROP of `from` and `, ADD CONSTRAINT <name> CHECK (<expression>)`. Both
/// servers apply the two together or not at all, so a row the new rule
/// refuses fails the statement (MySQL 3819, MariaDB 4025) with the old CHECK
/// still in place -- where a DROP and an ADD run apart leave the table with
/// none. `name` may be `from`.
[[nodiscard]] std::string replace_check_sql(std::string_view table, std::string_view from,
                                            std::string_view name, std::string_view expression,
                                            Dialect dialect);

/// True when a single integer identity primary key must be declared inline.
///
/// SQLite's `AUTOINCREMENT` is only legal on a column declared exactly
/// `INTEGER PRIMARY KEY`, so such a table cannot use a separate primary key
/// clause.
[[nodiscard]] bool uses_inline_identity_key(const Table& table, Dialect dialect);

} // namespace schemata
