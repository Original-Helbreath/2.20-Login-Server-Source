#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <schemata/core/converter.h>
#include <schemata/core/error.h>
#include <schemata/core/value.h>
#include <schemata/driver/driver.h>
#include <schemata/model/model.h>
#include <schemata/query/join_kind.h>
#include <schemata/query/table_target.h>
#include <schemata/sql/dialect.h>
#include <schemata/sql/expr.h>

namespace schemata {

/// Reads and writes by table and column name.
///
/// For a table no entity maps, or a caller that works in rows rather than
/// entities: `select_rows()`, `for_each_row()`, `count_rows()` and `max_of()`
/// read, and `insert_row()`, `update_rows()` and `delete_rows()` write, a
/// table named as a string, its columns named as strings, its rows narrowed by
/// a `RowFilter` built from `where::`. Every table and column name is quoted by
/// `quote_identifier()` -- they are among the names a caller hands the library,
/// listed in `<schemata/core/identifier.h>` -- and an empty one, or one holding
/// a NUL, is refused with `query_build_error` before anything is sent. Every
/// value is a bound parameter. Every statement goes through the connection's
/// statement cache (`Connection::prepare_cached()`).
///
/// `table` is a bare name, of a table in the target schema (`TableTarget`): the
/// connection's default schema, or the one `TableTarget::schema` names, which
/// qualifies the table in the statement itself, so no `USE` is sent. A
/// schema-qualified `db.t` is quoted as one name, dot and all.
///
/// Each call has an overload taking a `Model`. Where the model holds the table,
/// a column it does not map is refused, values read back as the model types
/// the column (as an entity's are), a value written is held to its column as a
/// save holds it, and a read defaults its columns and its order from the
/// model. Where it does not, the overload is the plain call.
///
/// None of them touches the change tracker: an entity a `Context` tracks keeps
/// the values it had.

class RowFilter;

namespace detail {

/// What one node of a `RowFilter` does.
enum class RowFilterOp : unsigned char {
    all,          ///< every row: `where::all()`
    none,         ///< no row: `!where::all()`
    eq,
    ne,
    lt,
    le,
    gt,
    ge,
    in,           ///< `column IN (values)`; no values is no row
    is_null,
    is_not_null,
    starts_with,  ///< `column LIKE ? ESCAPE '!'`, the prefix escaped
    and_,
    or_,
    not_,
};

struct RowFilterNode;
using RowFilterPtr = std::shared_ptr<const RowFilterNode>;

/// Where an `and_`, `or_` or `not_` node keeps its operands; see
/// `RowFilterNode::operands`. Defined with the filters.
struct RowFilterOperands;

/// Immutable once built, and shared between the filters combined from it.
struct RowFilterNode {
    RowFilterOp op = RowFilterOp::all;

    /// The column a comparison, an `in`, a null test or a prefix is about.
    std::string column;

    /// One for a comparison, the list for an `in`, the prefix -- as text,
    /// unescaped -- for `starts_with`.
    std::vector<Value> values;

    /// The operands of `and_` and `or_`, two or more, and of `not_`, one: the
    /// first `operand_count` of a list this node may share with the nodes it
    /// was built from and into.
    ///
    /// N-ary, so a filter built up in a loop -- `f = f || where::eq(...)` -- is
    /// one node of N operands rather than a tree N deep: combining two filters
    /// of one operator merges their operands, and appending to a node that
    /// nothing has appended to since shares its list rather than copying it.
    /// Nothing that walks a filter, frees one or writes one as SQL recurses
    /// deeper than its operators alternate.
    std::shared_ptr<RowFilterOperands> operands;
    std::size_t                        operand_count = 0;
};

[[nodiscard]] RowFilter compare_filter(RowFilterOp op, std::string_view column, Value value);
[[nodiscard]] RowFilter in_filter(std::string_view column, std::vector<Value> values);

/// A C++ value as the `Value` a filter binds: a `Value` as it is, text as
/// text, a whole number as a signed 64-bit one (an unsigned 64-bit one as
/// itself), a floating one as a double, and anything else a `ValueConverter`
/// takes through it. A character is refused at compile time, as the entity
/// builder refuses one: `where::eq("n", 'x')` would compare against 120.
template <typename T>
[[nodiscard]] Value filter_value(const T& value) {
    if constexpr (std::same_as<T, Value>) {
        return value;
    } else if constexpr (std::convertible_to<const T&, std::string_view>) {
        return Value{std::string(std::string_view(value))};
    } else if constexpr (std::same_as<T, bool>) {
        return Value{value};
    } else if constexpr (std::integral<T>) {
        static_assert(!detail::is_character_v<T>,
                      "a character is not a number to compare a column against; pass a "
                      "string or an integer");
        if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::uint64_t)) {
            return Value{static_cast<std::uint64_t>(value)};
        } else {
            return Value{static_cast<std::int64_t>(value)};
        }
    } else if constexpr (std::floating_point<T>) {
        return Value{static_cast<double>(value)};
    } else {
        static_assert(Storable<T>, "no ValueConverter takes this type to a Value");
        return ValueConverter<T>::to_value(value);
    }
}

} // namespace detail

/// A condition on a table's rows, by column name.
///
/// Built with the functions of `where::` and combined with `&&`, `||` and `!`.
/// Separate from the entity builder's `Predicate`, which names members rather
/// than columns.
///
/// A default-constructed filter is empty: it names no condition. A read takes
/// that as every row. **A write refuses it** -- `update_rows()` and
/// `delete_rows()` raise `query_build_error` -- so a filter built up in a loop
/// that ends up holding nothing does not reach every row by accident; pass
/// `where::all()` to mean every row. Empty is the identity for `&&` and `||`,
/// and `!` leaves it empty.
class RowFilter {
public:
    RowFilter() = default;
    explicit RowFilter(detail::RowFilterPtr node) : node_(std::move(node)) {}

    [[nodiscard]] bool empty() const noexcept { return node_ == nullptr; }
    [[nodiscard]] const detail::RowFilterPtr& node() const noexcept { return node_; }

private:
    detail::RowFilterPtr node_;
};

/// Both conditions. Empty is the identity, and `where::all()` too.
[[nodiscard]] RowFilter operator&&(const RowFilter& lhs, const RowFilter& rhs);

/// Either condition. Empty is the identity; with `where::all()` it is all.
[[nodiscard]] RowFilter operator||(const RowFilter& lhs, const RowFilter& rhs);

/// Not the condition, as `NOT (...)`. SQL's three values hold: `!where::eq("a",
/// 1)` matches no row where `a` is NULL. Empty stays empty.
[[nodiscard]] RowFilter operator!(const RowFilter& operand);

/// The conditions a `RowFilter` is made of.
///
/// A comparison binds its value; a NULL or a NaN is refused with
/// `query_build_error` when the filter is built, since a comparison with
/// either matches no row whatever the operator says -- `is_null()` asks for a
/// NULL. The column is checked when the filter is used, against the table.
namespace where {

/// Every row: the one way to let `update_rows()` or `delete_rows()` reach
/// every row of the table.
[[nodiscard]] RowFilter all();

// `column = value`, `<>`, `<`, `<=`, `>`, `>=`.
template <typename T>
[[nodiscard]] RowFilter eq(std::string_view column, const T& value) {
    return detail::compare_filter(detail::RowFilterOp::eq, column,
                                  detail::filter_value(value));
}
template <typename T>
[[nodiscard]] RowFilter ne(std::string_view column, const T& value) {
    return detail::compare_filter(detail::RowFilterOp::ne, column,
                                  detail::filter_value(value));
}
template <typename T>
[[nodiscard]] RowFilter lt(std::string_view column, const T& value) {
    return detail::compare_filter(detail::RowFilterOp::lt, column,
                                  detail::filter_value(value));
}
template <typename T>
[[nodiscard]] RowFilter le(std::string_view column, const T& value) {
    return detail::compare_filter(detail::RowFilterOp::le, column,
                                  detail::filter_value(value));
}
template <typename T>
[[nodiscard]] RowFilter gt(std::string_view column, const T& value) {
    return detail::compare_filter(detail::RowFilterOp::gt, column,
                                  detail::filter_value(value));
}
template <typename T>
[[nodiscard]] RowFilter ge(std::string_view column, const T& value) {
    return detail::compare_filter(detail::RowFilterOp::ge, column,
                                  detail::filter_value(value));
}

/// `column IN (values)`, each bound. No values matches no row -- `1 = 0`,
/// since `IN ()` is a syntax error on every engine -- and a NULL among them is
/// refused, as it is in a comparison: `NOT IN` a list holding one matches no
/// row at all.
///
/// Each value is a parameter of its own. A statement binding more than the
/// connection's `max_parameters()` -- 32766 on SQLite from 3.32, 65,535 on the
/// MySQL family, counting every other value the statement binds -- is refused
/// with `query_build_error` naming both numbers, before anything is sent. And
/// each length of list is a statement text of its own, `IN (?, ?)` and `IN (?,
/// ?, ?)` alike, so a caller whose lists vary in length fills the statement
/// cache with one statement per length: pad a list to a few fixed lengths (a
/// value repeated matches nothing more), or split it into lists of one length.
[[nodiscard]] RowFilter in(std::string_view column, std::vector<Value> values);

template <std::ranges::input_range R>
    requires(!std::same_as<std::remove_cvref_t<R>, std::vector<Value>>)
[[nodiscard]] RowFilter in(std::string_view column, const R& values) {
    std::vector<Value> converted;
    for (const auto& value : values) {
        converted.push_back(detail::filter_value(value));
    }
    return detail::in_filter(column, std::move(converted));
}

template <typename T>
[[nodiscard]] RowFilter in(std::string_view column, std::initializer_list<T> values) {
    std::vector<Value> converted;
    converted.reserve(values.size());
    for (const auto& value : values) {
        converted.push_back(detail::filter_value(value));
    }
    return detail::in_filter(column, std::move(converted));
}

[[nodiscard]] RowFilter is_null(std::string_view column);
[[nodiscard]] RowFilter is_not_null(std::string_view column);

/// Rows whose `column` begins with `prefix`: `column LIKE ? ESCAPE '!'`, bound
/// to the prefix with `!`, `%` and `_` escaped by a `!` and a `%` after it. So
/// the prefix is taken literally, wildcards and all, on every engine and
/// whatever the MySQL family's `sql_mode` -- `NO_BACKSLASH_ESCAPES` included,
/// since the escape is not a backslash and a backslash in the prefix is an
/// ordinary character. An empty prefix matches every row whose column is not
/// NULL.
///
/// On the MySQL family a prefix is what lets `LIKE` use an index on the
/// column: the optimiser reads the bound pattern as a range over it, as it
/// would a literal one.
///
/// Compared as each engine compares `LIKE`, which is not the same:
///
/// - MySQL and MariaDB: under the column's collation. A `_ci` collation
///   matches without regard to case, an `_ai` one without regard to accents
///   too, so `starts_with("name", "e")` finds "Eve" and, under
///   `utf8mb4_0900_ai_ci`, "Eloise" with an accented E; a `_bin` or `_cs` one
///   matches exactly. Unlike `=`, trailing spaces count.
/// - SQLite: without regard to case for the 26 ASCII letters, and exactly for
///   every other character, whatever the column's collation -- unless
///   `PRAGMA case_sensitive_like` is on, which makes it exact.
///
/// The prefix is escaped byte by byte, which is exact in UTF-8 -- SQLite's
/// text, and the MySQL family's `utf8mb4`, the connection character set unless
/// `MySqlOptions::charset` names another -- where no byte of a multi-byte
/// character is `!`, `%` or `_`, and in any single-byte set. A connection set to
/// sjis, cp932, gbk or big5 is another matter: there a character's second byte
/// can be 0x5F, `_`, and the escape put before it splits the character, so a
/// prefix holding such a character is not one to pass here on that connection.
[[nodiscard]] RowFilter starts_with(std::string_view column, std::string_view prefix);

} // namespace where

/// One column to sort by, and which way.
struct OrderBy {
    std::string column;
    bool        descending = false;

    [[nodiscard]] static OrderBy asc(std::string column_name) {
        OrderBy out;
        out.column = std::move(column_name);
        return out;
    }
    [[nodiscard]] static OrderBy desc(std::string column_name) {
        OrderBy out;
        out.column     = std::move(column_name);
        out.descending = true;
        return out;
    }
};

/// How `select_rows()` and `for_each_row()` read: the target schema, the
/// order, and how many rows.
struct ReadOptions : TableTarget {
    /// The order the rows come in, first column first.
    ///
    /// Empty, the default, is the primary key ascending where the call takes a
    /// `Model` that holds the table -- so reading it in pages with `limit` and
    /// `offset` sees each row once, while nothing writes -- and otherwise no
    /// `ORDER BY` at all: the rows come in whatever order the engine reads
    /// them, which can change from one run to the next. Name the columns for
    /// an order that matters.
    std::vector<OrderBy> order_by;

    /// The most rows to read: `LIMIT ?`, bound. Unset reads every row.
    std::optional<std::size_t> limit;

    /// Rows to skip first: `OFFSET ?`, bound. Without a `limit` it is sent
    /// with the engine's "no limit" (`LIMIT -1` on SQLite, the largest
    /// unsigned 64-bit number on the MySQL family). Meaningful only with an
    /// order.
    std::size_t offset = 0;
};

/// How many rows a limited `delete_rows()` deletes, and which go first.
///
/// Spelled `DeleteLimit{1000}` or `DeleteLimit{1000, {OrderBy::asc("at")}}`:
/// the constructor is explicit and there is no default one, so a bare `{}`
/// passed for the old overloads' `target` still means a `TableTarget`.
struct DeleteLimit {
    explicit DeleteLimit(std::size_t most, std::vector<OrderBy> order = {})
        : limit(most), order_by(std::move(order)) {}

    /// The most rows one call deletes. 0 is refused.
    std::size_t limit;

    /// Which rows go first, first column first: `{OrderBy::asc("sent_at")}`
    /// deletes the oldest. Empty, the default, is the primary key ascending
    /// where the call takes a `Model` that holds the table, and otherwise no
    /// `ORDER BY` at all: whichever rows the engine finds first.
    ///
    /// **Without a model, name an order, and a unique one.** With no order, or
    /// one that ties, which rows go is up to the engine and can differ from
    /// run to run. Under MySQL/MariaDB statement-based replication, it can
    /// differ between the primary and a replica: the server flags such a
    /// `DELETE ... LIMIT` as unsafe (warning 1592) and replicas can drift.
    /// End the order on the primary key, or on another unique column.
    std::vector<OrderBy> order_by;
};

/// What `select_rows()` read: the columns, in the order read, and a row of
/// values for each row, parallel to them.
struct RowSet {
    std::vector<std::string>        columns;
    std::vector<std::vector<Value>> rows;

    /// Where `name` is in `columns`, compared without regard to ASCII case;
    /// nothing where it is not there.
    [[nodiscard]] std::optional<std::size_t> column_index(
        std::string_view name) const noexcept;
};

/// One row, as `for_each_row()` hands it to its callback: the statement's
/// current row, read in place.
///
/// Positions are those of the columns read, fixed for the statement -- find
/// one by name with `column_index()` once, before the rows, rather than per
/// row. Nothing is copied until asked for: `get_text()` and `get_blob()` are
/// views of the row as the driver holds it (`Statement::get_text_view()`),
/// **valid only until the callback returns**; `get_value()` builds a `Value`,
/// typed as `select_rows()` types it. A position past the last column throws
/// `std::out_of_range`.
class RowView {
public:
    [[nodiscard]] std::size_t                     size() const noexcept;
    [[nodiscard]] const std::vector<std::string>& columns() const noexcept {
        return *columns_;
    }

    /// Where `name` is among the columns, compared without regard to ASCII
    /// case; nothing where it is not there.
    [[nodiscard]] std::optional<std::size_t> column_index(
        std::string_view name) const noexcept;

    [[nodiscard]] bool is_null(std::size_t index) const;

    /// As the driver reads them: zero for NULL, as for a number column.
    [[nodiscard]] std::int64_t get_int64(std::size_t index) const;
    [[nodiscard]] double       get_double(std::size_t index) const;

    /// Views, empty for NULL, valid until the callback returns.
    [[nodiscard]] std::string_view           get_text(std::size_t index) const;
    [[nodiscard]] std::span<const std::byte> get_blob(std::size_t index) const;

    /// The value, as `select_rows()` would have put it in its `RowSet`.
    [[nodiscard]] Value get_value(std::size_t index) const;

    /// For the library, which lends one to each callback: the statement on
    /// its row, the columns read, the model's column for each where a model
    /// holds the table (null, or none at all, where not), and the table as a
    /// message names it. Each must outlive the view.
    RowView(const driver::Statement& statement, const std::vector<std::string>& columns,
            const std::vector<const Column*>& mapped, std::string_view table = {}) noexcept
        : statement_(&statement), columns_(&columns), mapped_(&mapped), table_(table) {}

private:
    [[nodiscard]] int at(std::size_t index) const;

    const driver::Statement*          statement_;
    const std::vector<std::string>*   columns_;
    const std::vector<const Column*>* mapped_;
    std::string_view                  table_;
};

/// Ordered column-value pairs: what `update_rows()` assigns.
using ColumnValues = std::vector<std::pair<std::string, Value>>;

namespace detail {

/// The escape character `where::starts_with()` puts in `ESCAPE '!'`: not a
/// backslash, which the MySQL family reads inside a string literal unless
/// `NO_BACKSLASH_ESCAPES` is set, and which is no escape at all on SQLite.
inline constexpr char like_escape = '!';

/// A value as the engine hands it back, for a column no model types
/// (`Statement::cell_kind()`): what `select_rows()` reads without a model.
[[nodiscard]] Value natural_value(const driver::Statement& row, int index);

/// `prefix` as a `LIKE` pattern matching what begins with it: each `!`, `%`
/// and `_` escaped with `!`, and a `%` after.
[[nodiscard]] std::string like_prefix(std::string_view prefix);

/// A filter as SQL, without `WHERE`: empty text for an empty filter and for
/// `where::all()`. Column names are refused as `require_quotable()` refuses
/// them, `subject` naming the filter for the message.
[[nodiscard]] SqlFragment filter_sql(const RowFilter& filter, Dialect dialect,
                                     std::string_view subject = "a filter");

// The statements, qualified with `schema` where it is not empty. Each takes
// names already checked; `filter_sql()` is the one that checks its own.

[[nodiscard]] SqlFragment select_rows_sql(Dialect dialect, std::string_view schema,
                                          std::string_view                table,
                                          const std::vector<std::string>& columns,
                                          const RowFilter&                filter,
                                          const std::vector<OrderBy>&     order_by,
                                          std::optional<std::size_t>      limit,
                                          std::size_t                     offset);

[[nodiscard]] SqlFragment count_rows_sql(Dialect dialect, std::string_view schema,
                                         std::string_view table, const RowFilter& filter);

[[nodiscard]] SqlFragment max_of_sql(Dialect dialect, std::string_view schema,
                                     std::string_view table, std::string_view column,
                                     const RowFilter& filter);

[[nodiscard]] SqlFragment update_rows_sql(Dialect dialect, std::string_view schema,
                                          std::string_view table, const ColumnValues& set,
                                          const RowFilter& filter);

[[nodiscard]] SqlFragment delete_rows_sql(Dialect dialect, std::string_view schema,
                                          std::string_view table, const RowFilter& filter);

/// A delete of at most `limit` rows, taken in `order_by`'s order.
///
/// - MySQL family: `DELETE FROM t WHERE ... ORDER BY ... LIMIT ?`.
/// - SQLite, where `DELETE ... LIMIT` needs a compile option builds rarely
///   carry: `DELETE FROM t WHERE k IN (SELECT k FROM t WHERE ... ORDER BY ...
///   LIMIT ?)`, `k` being `row_key` -- one name, the rowid (or an alias of
///   it), or a WITHOUT ROWID table's primary-key columns, compared as a row
///   value where there are several. `row_key` is not read on the MySQL family.
///
/// `limit` is bound as given; `delete_rows()` refuses 0.
[[nodiscard]] SqlFragment delete_rows_sql(Dialect dialect, std::string_view schema,
                                          std::string_view table, const RowFilter& filter,
                                          const std::vector<OrderBy>&     order_by,
                                          std::int64_t                    limit,
                                          const std::vector<std::string>& row_key);

/// `INSERT INTO t (a, b) VALUES (?, ?)`; with no columns, a row of defaults:
/// `DEFAULT VALUES` on SQLite, `() VALUES ()` on the MySQL family.
[[nodiscard]] std::string insert_row_sql(Dialect dialect, std::string_view schema,
                                         std::string_view                table,
                                         const std::vector<std::string>& columns);

/// A callback taking a row, without the cost of `std::function`: a pointer to
/// the caller's callable and a function that calls it.
struct RowVisitor {
    void* callable = nullptr;
    bool (*call)(void*, const RowView&) = nullptr;
};

[[nodiscard]] std::int64_t for_each_row(driver::Connection& connection, const Model* model,
                                        std::string_view                table,
                                        const std::vector<std::string>& columns,
                                        const RowFilter& filter, const ReadOptions& options,
                                        RowVisitor visit);

/// `F` is the callable's type, not a reference; it may be const. Not a
/// function type: `for_each_rows()` decays one to a pointer first.
template <typename F>
[[nodiscard]] RowVisitor visitor(F& fn) {
    static_assert(!std::is_function_v<F>, "decay a function to a pointer first");
    RowVisitor out;
    out.callable = const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
    out.call     = [](void* callable, const RowView& row) -> bool {
        auto& f = *static_cast<F*>(callable);
        if constexpr (std::is_void_v<std::invoke_result_t<F&, const RowView&>>) {
            f(row);
            return true;
        } else {
            return static_cast<bool>(f(row));
        }
    };
    return out;
}

/// `for_each_row()` with any callable: a function named as itself -- whose
/// type is a function type, which has no object to point at -- through a
/// pointer to it that lives for the call.
template <typename F>
std::int64_t for_each_rows(driver::Connection& connection, const Model* model,
                           std::string_view table, const std::vector<std::string>& columns,
                           const RowFilter& filter, const ReadOptions& options, F& fn) {
    if constexpr (std::is_function_v<F>) {
        F* pointer = &fn;
        return for_each_row(connection, model, table, columns, filter, options,
                            visitor<F*>(pointer));
    } else {
        return for_each_row(connection, model, table, columns, filter, options,
                            visitor<F>(fn));
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

/// Read `columns` of the rows of `table` that `filter` matches.
///
/// `SELECT a, b FROM t WHERE ... ORDER BY ... LIMIT ? OFFSET ?`, through the
/// statement cache. An empty `filter` reads every row.
///
/// `columns` empty means every column the model maps on the table, in the
/// model's order, in the overload taking a model that holds it. Without one it
/// is refused with `query_build_error`: there is no list to read, and `SELECT
/// *` would read whatever the table has today, in its order, into positions
/// the caller wrote against yesterday's. Name them. A column named twice is
/// refused, in any case.
///
/// Each value comes back as the column's type:
///
/// - where a model holds the table, as the model types it -- the `Value` an
///   entity's field is read from (`read_column()`): a DECIMAL as a double, a
///   boolean as an integer, a date or timestamp as its text;
/// - otherwise as the engine hands it back (`Statement::cell_kind()`): an
///   integer as a signed 64-bit one -- an unsigned one above that range as
///   `uint64_t` -- a FLOAT or DOUBLE as a double, text as text, a binary
///   string or BLOB as a blob, NULL as a null `Value`. On the MySQL family a
///   DECIMAL, a date, a time, a JSON document, an ENUM or a SET is its text;
///   on SQLite each value is its own storage class, whatever the column's
///   affinity.
///
/// The order is `ReadOptions::order_by`, or where that is empty the primary
/// key ascending for a table a model passed here holds, and otherwise none.
///
/// \throws query_build_error for a name refused, as the header says, or a
///         column the model does not map; schema_error for a target schema
///         refused (`TableTarget`); whatever the driver raises otherwise.
[[nodiscard]] RowSet select_rows(driver::Connection& connection, std::string_view table,
                                 const std::vector<std::string>& columns,
                                 const RowFilter&                filter  = {},
                                 const ReadOptions&              options = {});

[[nodiscard]] RowSet select_rows(driver::Connection& connection, const Model& model,
                                 std::string_view                table,
                                 const std::vector<std::string>& columns,
                                 const RowFilter&                filter  = {},
                                 const ReadOptions&              options = {});

/// Read as `select_rows()` does, one row at a time, into `fn`.
///
/// `fn` takes a `const RowView&` and returns `bool` -- false to stop, and no
/// row after that one is read -- or nothing, to read every row. Nothing is
/// copied from a row but what `fn` asks for, and no `Value` is built unless it
/// calls `RowView::get_value()`: a caller reading a few thousand cells as
/// numbers and views pays for none of the strings `select_rows()` would make.
///
/// The row is the statement's current row, so a view taken from it lasts until
/// `fn` returns. `fn` may run other statements on the connection -- the MySQL
/// family has already read the whole result into the client, SQLite steps its
/// own -- but must not use this call's statement, which it cannot reach. An
/// exception from `fn` goes on out, and the statement is closed rather than
/// kept.
///
/// \returns the rows handed to `fn`, the one it stopped at included.
template <typename F>
    requires std::invocable<F&, const RowView&>
std::int64_t for_each_row(driver::Connection& connection, std::string_view table,
                          const std::vector<std::string>& columns, const RowFilter& filter,
                          const ReadOptions& options, F&& fn) {
    return detail::for_each_rows(connection, nullptr, table, columns, filter, options, fn);
}

template <typename F>
    requires std::invocable<F&, const RowView&>
std::int64_t for_each_row(driver::Connection& connection, const Model& model,
                          std::string_view table, const std::vector<std::string>& columns,
                          const RowFilter& filter, const ReadOptions& options, F&& fn) {
    return detail::for_each_rows(connection, &model, table, columns, filter, options, fn);
}

/// How many rows of `table` `filter` matches: `SELECT COUNT(*)`. An empty
/// filter counts every row.
[[nodiscard]] std::int64_t count_rows(driver::Connection& connection, std::string_view table,
                                      const RowFilter& filter = {},
                                      const TableTarget& target = {});

[[nodiscard]] std::int64_t count_rows(driver::Connection& connection, const Model& model,
                                      std::string_view table, const RowFilter& filter = {},
                                      const TableTarget& target = {});

/// The largest value of `column` among the rows `filter` matches:
/// `SELECT MAX(c)`, typed as `select_rows()` types a value.
///
/// \returns a null `Value` where no row matches, or every one matched holds
///          NULL -- `MAX` does not tell the two apart, and neither does this.
[[nodiscard]] Value max_of(driver::Connection& connection, std::string_view table,
                           std::string_view column, const RowFilter& filter = {},
                           const TableTarget& target = {});

[[nodiscard]] Value max_of(driver::Connection& connection, const Model& model,
                           std::string_view table, std::string_view column,
                           const RowFilter& filter = {}, const TableTarget& target = {});

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

/// Insert one row: `values[i]` into `columns[i]`.
///
/// `INSERT INTO t (a, b) VALUES (?, ?)`, through the statement cache; with no
/// columns, a row of the table's defaults. A value no engine stores alike -- a
/// NaN or an infinity, and where a model holds the table a date no engine
/// reads back -- is refused before anything is sent, as a save refuses it.
///
/// \returns the generated key, where the table has a column the engine fills
///          with one, and otherwise nothing:
///
/// - on MySQL and MariaDB, its AUTO_INCREMENT column: `mysql_insert_id()`,
///   the value the engine gave the row -- or the one `values` gave the
///   column, where the caller wrote it explicitly, since that is the value
///   the row has. Written as NULL, or as 0 unless `sql_mode` has
///   `NO_AUTO_VALUE_ON_ZERO`, the engine generates one.
/// - on SQLite, an INTEGER PRIMARY KEY, which is the table's rowid:
///   `sqlite3_last_insert_rowid()`, generated or written. A table keyed on
///   anything else has a rowid too, and it is not a key: nothing.
///
/// Which column that is comes from the model where it holds the table (its
/// identity key), and otherwise from the table as the engine describes it --
/// `SHOW COLUMNS` on the MySQL family, `PRAGMA table_info` and `index_list`
/// on SQLite -- asked once per connection, table and target schema, and kept
/// with the unique keys `upsert_rows()` reads, forgotten when they are: a
/// `USE` or DDL through the connection, `reset_session()`, a pool's catch-up,
/// `Connection::clear_statement_cache()`.
///
/// \throws query_build_error for a name refused, a count of values that is
///         not the count of columns, a column named twice, or a value refused;
///         schema_error for a target schema refused; whatever the driver
///         raises otherwise -- a constraint violation among them.
std::optional<std::int64_t> insert_row(driver::Connection& connection, std::string_view table,
                                       const std::vector<std::string>& columns,
                                       const std::vector<Value>&       values,
                                       const TableTarget&              target = {});

std::optional<std::int64_t> insert_row(driver::Connection& connection, const Model& model,
                                       std::string_view table,
                                       const std::vector<std::string>& columns,
                                       const std::vector<Value>&       values,
                                       const TableTarget&              target = {});

/// Assign `set` to every row of `table` that `filter` matches.
///
/// `UPDATE t SET a = ?, b = ? WHERE ...`, one statement, through the statement
/// cache. An empty `filter` is refused with `query_build_error`; `where::all()`
/// reaches every row.
///
/// \returns the rows the filter **matched**, whether or not the assignment
///          changed them: a row already holding the values counts, on every
///          engine and whether or not the connection was opened with
///          `MySqlOptions::found_rows` (`Connection::matched_rows()`). SQLite
///          counts matched rows itself; the MySQL family counts changed ones,
///          so the server's info line for the statement is read instead.
std::int64_t update_rows(driver::Connection& connection, std::string_view table,
                         const ColumnValues& set, const RowFilter& filter,
                         const TableTarget& target = {});

std::int64_t update_rows(driver::Connection& connection, const Model& model,
                         std::string_view table, const ColumnValues& set,
                         const RowFilter& filter, const TableTarget& target = {});

/// Delete every row of `table` that `filter` matches.
///
/// `DELETE FROM t WHERE ...`, one statement, through the statement cache. An
/// empty `filter` is refused with `query_build_error`; `where::all()` reaches
/// every row.
///
/// \returns the rows deleted from `table`. Rows a foreign key's `ON DELETE
///          CASCADE` deletes from other tables are not counted, on any engine:
///          the MySQL family counts the statement's own table, and SQLite's
///          `sqlite3_changes()` leaves out what a foreign key action or a
///          trigger does.
std::int64_t delete_rows(driver::Connection& connection, std::string_view table,
                         const RowFilter& filter, const TableTarget& target = {});

std::int64_t delete_rows(driver::Connection& connection, const Model& model,
                         std::string_view table, const RowFilter& filter,
                         const TableTarget& target = {});

/// Delete at most `limit.limit` of the rows `filter` matches, the first ones
/// in `limit.order_by`'s order: a batched purge, one bounded statement at a
/// time, so no one statement holds its locks for long.
///
///     const DeleteLimit batch{1000, {OrderBy::asc("sent_at"), OrderBy::asc("id")}};
///     while (schemata::delete_rows(conn, "chat_messages",
///                                  where::lt("sent_at", cutoff), batch) > 0) {
///     }
///
/// See `DeleteLimit::order_by` on choosing a unique order without a model,
/// for statement-based replication.
///
/// - MySQL family: `DELETE FROM t WHERE ... ORDER BY ... LIMIT ?`.
/// - SQLite: `DELETE FROM t WHERE rowid IN (SELECT rowid FROM t WHERE ...
///   ORDER BY ... LIMIT ?)`, as `DELETE ... LIMIT` needs
///   SQLITE_ENABLE_UPDATE_DELETE_LIMIT. A WITHOUT ROWID table is keyed on its
///   primary key instead. Which applies is asked of the table on each call
///   (`PRAGMA table_info` and a probe prepared, not run), so this costs two
///   more prepares than the plain delete on SQLite.
///
/// On SQLite an `order_by` column the table does not have is refused with
/// `query_build_error`, model or not, since SQLite would read the quoted name
/// as a string and order by nothing; the servers raise their own error.
///
/// A `limit` of 0 is refused with `query_build_error`, as is an empty filter:
/// `where::all()` reaches every row, `limit` at a time.
///
/// \returns the rows deleted from `table`, at most `limit`; 0 once nothing
///          matches. Cascades are not counted, as for the plain delete.
std::int64_t delete_rows(driver::Connection& connection, std::string_view table,
                         const RowFilter& filter, const DeleteLimit& limit,
                         const TableTarget& target = {});

std::int64_t delete_rows(driver::Connection& connection, const Model& model,
                         std::string_view table, const RowFilter& filter,
                         const DeleteLimit& limit, const TableTarget& target = {});

// ---------------------------------------------------------------------------
// Joins by name
// ---------------------------------------------------------------------------

/// One side of a `JoinSpec`: a table, the schema it is in, the columns read
/// from it, and the name filters and orders call it by.
struct JoinTable {
    /// The schema the table is in, as for every call of this header: empty
    /// for the connection's default schema. Each side has its own, so the two
    /// tables can be in two schemas of one connection. Refused on SQLite with
    /// `schema_error`, as everywhere (`TableTarget`).
    TableTarget target{};

    /// The table: a bare name, quoted as one name.
    std::string table{};

    /// The columns read from this side, in order. Empty means every column
    /// the model maps on the table, in the overload taking a model that holds
    /// it, and is refused otherwise -- as `select_rows()` refuses it. A column
    /// named twice is refused.
    std::vector<std::string> columns{};

    /// What a filter, an order or a result column calls this side:
    /// `"<alias>.<column>"`. Empty, the default, is `table`. Give one side an
    /// alias where both sides have the same table name -- one table in two
    /// schemas -- which is refused otherwise. Compared without regard to ASCII
    /// case, and may not hold a dot. Never sent: the statement calls the two
    /// sides `l` and `r` whatever their aliases, so one shape of join is one
    /// statement text.
    std::string alias{};
};

/// A column of the left side equal to a column of the right side: one term of
/// a join's `ON`. Each is a bare column of its own side, not qualified.
struct JoinOn {
    std::string left{};
    std::string right{};
};

/// A two-table join by table and column name: what `select_rows()` and
/// `for_each_row()` take in place of a table.
///
///     schemata::TableTarget player;
///     player.schema = "hb_player";
///     schemata::JoinSpec friends;
///     friends.left  = {player, "character_friends", {"id", "other_character_id"}};
///     friends.right = {player, "characters", {"id", "name"}};
///     friends.on    = {{"other_character_id", "id"}};
///     auto rows = schemata::select_rows(*db, friends,
///                                       where::eq("character_friends.character_id", me));
///     // rows.columns: "character_friends.id", "character_friends.other_character_id",
///     //               "characters.id", "characters.name"
///
/// One statement:
///
///     SELECT l.c1 AS l0, ..., r.c1 AS r0, ...
///       FROM [schema.]left AS l {INNER|LEFT} JOIN [schema.]right AS r
///         ON l.a = r.b [AND ...] [AND <on_filter>]
///      [WHERE <filter>] [ORDER BY ...] [LIMIT ? [OFFSET ?]]
///
/// **Naming a column** in a filter, an order or `on_filter`:
/// `"<alias>.<column>"`, the alias being the side's (`JoinTable::alias`, or its
/// table). That is unambiguous whatever the tables are called and whatever
/// columns they share: both sides may have an `id`, and both may be one table
/// name in two schemas as long as one side has an alias. A bare `"column"` is
/// taken only where the overload takes a model that holds both tables and
/// exactly one of them maps the column; anywhere else it is refused as
/// ambiguous, with `query_build_error`. A qualifier naming neither side is
/// refused too.
///
/// **Left join:** every left row is kept; where no right row matched, every
/// right-side cell reads as NULL (`RowView::is_null()`, or a null `Value`).
/// Test a right column that is never NULL in a stored row -- its key -- to tell
/// the two apart. A `filter` on a right column is a WHERE, not an ON: it runs
/// after the join, so it drops the rows with no right match. To keep them, put
/// the condition in `on_filter`, which narrows which right row matches.
struct JoinSpec {
    JoinTable left{};
    JoinTable right{};
    JoinKind  kind = JoinKind::inner;

    /// The column equalities, combined with AND: one for a single-column key,
    /// several for a composite one. Empty is refused: a join with no `ON` is a
    /// cross join, never what a join meant.
    std::vector<JoinOn> on{};

    /// More of the `ON` condition, ANDed after `on`, naming columns as a
    /// filter does: `where::ge("characters.level", 50)`. For a left join, the
    /// place for a condition on the right side that should not drop left rows.
    RowFilter on_filter{};
};

/// How a join is read: the order and how many rows. As `ReadOptions`, less the
/// target, which each side of the `JoinSpec` carries.
struct JoinReadOptions {
    /// The order, first column first, each named as a filter names one
    /// (`"characters.name"`). Empty is the key of each side the model holds,
    /// left first, ascending, in the overload taking a model; otherwise no
    /// `ORDER BY` at all. With neither side modelled and no `order_by`,
    /// `LIMIT`/`OFFSET` pages therefore have no fixed order.
    std::vector<OrderBy> order_by{};

    /// `LIMIT ?`, bound; unset reads every row.
    std::optional<std::size_t> limit{};

    /// `OFFSET ?`, bound, as `ReadOptions::offset`.
    std::size_t offset = 0;
};

/// Which side of a join.
enum class JoinSide : std::uint8_t { left, right };

/// What a join `select_rows()` read: a `RowSet` whose columns are the left
/// side's then the right side's, each `"<alias>.<column>"`, so
/// `column_index("characters.name")` finds one. Where each side starts is
/// kept too.
struct JoinRowSet : RowSet {
    /// The position of the first right-side column: the count of left ones.
    std::size_t right_begin = 0;

    /// The two sides' aliases, as the column names carry them.
    std::string left_alias{};
    std::string right_alias{};

    using RowSet::column_index;

    /// Where `column` of `side` is among `columns`, compared without regard
    /// to ASCII case; nothing where that side does not have it.
    [[nodiscard]] std::optional<std::size_t> column_index(JoinSide         side,
                                                          std::string_view column) const;
};

namespace detail {

[[nodiscard]] std::int64_t for_each_joined_row(driver::Connection& connection,
                                               const Model* model, const JoinSpec& join,
                                               const RowFilter&       filter,
                                               const JoinReadOptions& options,
                                               RowVisitor             visit);

template <typename F>
std::int64_t for_each_joined_rows(driver::Connection& connection, const Model* model,
                                  const JoinSpec& join, const RowFilter& filter,
                                  const JoinReadOptions& options, F& fn) {
    if constexpr (std::is_function_v<F>) {
        F* pointer = &fn;
        return for_each_joined_row(connection, model, join, filter, options,
                                   visitor<F*>(pointer));
    } else {
        return for_each_joined_row(connection, model, join, filter, options, visitor<F>(fn));
    }
}

} // namespace detail

/// Read the two tables of `join`, joined, the rows `filter` matches: one
/// statement, through the statement cache. The same shape of join -- tables,
/// schemas, columns, `ON`, the filter's shape, the order, whether there is a
/// limit -- is the same statement text, so a call repeated with other values
/// is lent the statement the last one left in the cache.
///
/// Values are typed as `select_rows()` types them: by the model where it holds
/// that side's table, and otherwise as the engine hands them back. Every name
/// is checked as `select_rows()` checks it, against the model where it holds
/// the side's table.
///
/// \throws query_build_error for a name refused (see `JoinSpec`), an empty
///         `on`, two sides of one alias, or a column the model does not map;
///         schema_error for a target schema refused; whatever the driver
///         raises otherwise -- a table the schema does not have among it.
[[nodiscard]] JoinRowSet select_rows(driver::Connection& connection, const JoinSpec& join,
                                     const RowFilter&       filter  = {},
                                     const JoinReadOptions& options = {});

[[nodiscard]] JoinRowSet select_rows(driver::Connection& connection, const Model& model,
                                     const JoinSpec& join, const RowFilter& filter = {},
                                     const JoinReadOptions& options = {});

/// Read a join a row at a time into `fn`, as the table form of
/// `for_each_row()` does: `RowView::get_text()` is a view of the row, valid
/// until `fn` returns, and `RowView::columns()` are `"<alias>.<column>"`, left
/// side first, so `column_index("characters.name")` finds one.
template <typename F>
    requires std::invocable<F&, const RowView&>
std::int64_t for_each_row(driver::Connection& connection, const JoinSpec& join,
                          const RowFilter& filter, const JoinReadOptions& options, F&& fn) {
    return detail::for_each_joined_rows(connection, nullptr, join, filter, options, fn);
}

template <typename F>
    requires std::invocable<F&, const RowView&>
std::int64_t for_each_row(driver::Connection& connection, const Model& model,
                          const JoinSpec& join, const RowFilter& filter,
                          const JoinReadOptions& options, F&& fn) {
    return detail::for_each_joined_rows(connection, &model, join, filter, options, fn);
}

} // namespace schemata
