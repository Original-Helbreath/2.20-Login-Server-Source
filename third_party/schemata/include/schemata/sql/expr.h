#pragma once

#include <chrono>
#include <cmath>
#include <concepts>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <schemata/core/converter.h>
#include <schemata/core/error.h>
#include <schemata/core/value.h>
#include <schemata/model/model.h>
#include <schemata/sql/dialect.h>

namespace schemata {

/// Predicate expressions and the SQL they lower to.
///
/// Per D1 the typed layer is exactly one template deep: `col(&User::email)`
/// is typed, so comparing it against the wrong kind of value is a compile
/// error, and the comparison operators immediately produce an untyped
/// runtime node. Everything past that point -- `&&`, `||`, the emitter, the
/// query builder -- works on plain runtime values, so the templates never
/// nest and the diagnostics stay short.
///
/// Values never reach the SQL text. Every comparand becomes a bound
/// parameter, which makes sanitisation a property of the design rather than
/// a rule to remember.

enum class BinaryOp {
    eq,
    ne,
    lt,
    le,
    gt,
    ge,
    like,
    and_,
    or_,
};

enum class UnaryOp {
    not_,
    is_null,
    is_not_null,
};

/// What an aggregate does to the rows of a group.
///
/// `none` is not an aggregate at all -- it is a bare column reference. It is
/// a member of this enumeration rather than a separate case because a select
/// list mixes the two freely (`region, SUM(amount)`), and one representation
/// for both is what lets the grouping rule below be checked in one pass.
enum class AggregateOp {
    none,
    count,
    sum,
    avg,
    min,
    max,
};

[[nodiscard]] std::string_view to_string(BinaryOp op) noexcept;
[[nodiscard]] std::string_view to_string(UnaryOp op) noexcept;
[[nodiscard]] std::string_view to_string(AggregateOp op) noexcept;

struct ExprNode;

/// Shared because predicates are copied freely -- combined, stored on a
/// query, reused across runs -- and the nodes are immutable once built.
using ExprPtr = std::shared_ptr<const ExprNode>;

/// A reference to a mapped member.
///
/// Carries the `MemberKey` rather than a column name, because `col()` has no
/// model to consult. Resolution to a name happens at emit time, which is
/// also what lets one predicate be emitted against more than one dialect.
struct ColumnRef {
    MemberKey member;
};

/// A bound parameter. There is deliberately no literal node.
struct Literal {
    Value value;
};

struct Binary {
    BinaryOp op = BinaryOp::eq;
    ExprPtr  lhs;
    ExprPtr  rhs;
};

struct Unary {
    UnaryOp op = UnaryOp::not_;
    ExprPtr operand;
};

/// `IN` and `NOT IN` over a fixed list of values.
///
/// An empty list is legal here and emits a constant instead of `IN ()`,
/// which is a syntax error on both engines.
struct InList {
    ExprPtr            operand;
    std::vector<Value> values;
    bool               negated = false;
};

/// One item of a select list, or one aggregate inside a `HAVING` clause.
///
/// `op == AggregateOp::none` means the bare column, which is why this doubles
/// as the select-list item type: `SELECT region, SUM(amount)` is two of
/// these, and `GROUP BY` validation only has to walk one list.
///
/// `member` is absent only for `COUNT(*)`, the one aggregate that does not
/// name a column.
struct Aggregate {
    AggregateOp              op = AggregateOp::none;
    std::optional<MemberKey> member;
    bool                     distinct = false;

    [[nodiscard]] bool is_aggregate() const noexcept { return op != AggregateOp::none; }
};

/// `EXISTS (SELECT 1 FROM <target> WHERE <correlation> AND <predicate>)`.
///
/// Built from a navigation the model already declares, so the correlation is
/// resolved rather than spelled out.
///
/// Everything here arrives already rendered, because all of it needs the
/// model: the two correlated column names, and the inner predicate, whose
/// members belong to the *target* entity and so resolve against a different
/// table than everything around it. Keeping the rendering out here is what
/// lets the emitter stay free of the model layer -- the property that makes
/// every operator and quoting rule testable with a two-line resolver.
struct Exists {
    /// The inner table, quoted.
    std::string target_table;

    /// The correlated equality, both sides quoted and table-qualified.
    std::string inner_column;
    std::string outer_column;

    /// The caller's narrowing, already lowered against the target table.
    /// Empty means "any related row at all".
    std::string        predicate_sql;
    std::vector<Value> predicate_params;

    bool negated = false;
};

struct ExprNode {
    std::variant<ColumnRef, Literal, Binary, Unary, InList, Aggregate, Exists> node;
};

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

/// SQL text plus the parameters to bind, in order.
struct SqlFragment {
    std::string        text;
    std::vector<Value> params;
};

/// How a member becomes a column reference in the emitted SQL.
///
/// Taking a callback rather than a `Model` keeps the emitter free of the
/// model layer, so every operator, precedence rule and quoting decision is
/// testable with a two-line resolver: no database, no entity types, no
/// `describe()`.
using ColumnResolver = std::function<std::string(const MemberKey&)>;

/// Lower an expression tree to SQL.
///
/// Parentheses are added only where precedence requires them, so the output
/// is readable in a test expectation and in a log.
[[nodiscard]] SqlFragment emit_expr(const ExprNode&       expr,
                                    Dialect               dialect,
                                    const ColumnResolver& resolve);

// ---------------------------------------------------------------------------
// The typed layer
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
struct query_operand {
    using type = T;
};

template <typename T>
struct query_operand<std::optional<T>> {
    using type = T;
};

} // namespace detail

/// What a column compares against: `std::optional<T>` compares against `T`.
template <typename T>
using query_operand_t = typename detail::query_operand<T>::type;

namespace detail {

/// A character type. Integral, but not a number anyone means to compare a
/// count against: excluding them stops `view_count == 'x'` binding 120.
///
/// The list is the types a character literal can actually have. `signed char`
/// and `unsigned char` are deliberately absent even though they are narrow
/// integers, because on MSVC they *are* `std::int8_t` and `std::uint8_t`:
/// excluding them mapped such a column happily and then refused to compare it
/// against anything at all, including a literal of its own type. Nobody
/// writes `signed char` meaning a letter; they write `char`.
template <typename T>
inline constexpr bool is_character_v =
    std::same_as<T, char> || std::same_as<T, char8_t> || std::same_as<T, char16_t> ||
    std::same_as<T, char32_t> || std::same_as<T, wchar_t>;

/// Any `std::chrono::sys_time`, whatever its precision.
template <typename T>
struct is_sys_time : std::false_type {};

template <typename D>
struct is_sys_time<std::chrono::sys_time<D>> : std::true_type {};

template <typename T>
inline constexpr bool is_sys_time_v = is_sys_time<T>::value;

/// An ordinary number: integral or floating, but not bool, not a character.
template <typename T>
concept PlainNumber = (std::integral<T> || std::floating_point<T>) &&
                      !std::same_as<T, bool> && !is_character_v<T>;

} // namespace detail

/// Satisfied when `Given` may be compared against a column holding `Wanted`.
///
/// Deliberately narrower than implicit conversion, because the conversions
/// C++ performs silently are exactly the ones that produce a query nobody
/// meant to write: `active == 5` would otherwise compile and compare a
/// boolean column against `true`, a pointer would convert to `bool` just as
/// quietly, and `view_count == 'x'` would compare against 120.
///
/// Conversion between plain numbers is allowed, since an integer literal
/// against a `BIGINT` or a `DOUBLE` column is ordinary. What that admits --
/// a value the column's own type cannot hold, whether an integer too large
/// for it or a floating value out of its range -- is caught by
/// `operand_value` when the predicate is built, because the types alone
/// cannot tell `view_count == 5LL` from `view_count == 3000000000LL`.
///
/// A `sys_time` of any precision compares against a timestamp column, so
/// `system_clock::now()` works whatever that clock's resolution happens to
/// be: 100ns on MSVC, 1ns on libstdc++, microseconds on libc++. Requiring an
/// exact match would make identical source compile on one platform and fail
/// on the other two.
template <typename Given, typename Wanted>
concept Comparand =
    std::same_as<std::remove_cvref_t<Given>, Wanted> ||
    (std::same_as<Wanted, std::string> && std::convertible_to<const Given&, std::string_view>) ||
    (detail::PlainNumber<Wanted> && detail::PlainNumber<std::remove_cvref_t<Given>>) ||
    (std::same_as<Wanted, timestamp> && detail::is_sys_time_v<std::remove_cvref_t<Given>>);

namespace detail {

[[nodiscard]] inline ExprPtr make(ExprNode node) {
    return std::make_shared<const ExprNode>(std::move(node));
}

/// Two raised to `power`, as a floating type.
///
/// Exact for every power a column type can ask for: a power of two survives
/// into float and double alike until the exponent runs out, which 64 does not
/// come close to doing. `std::ldexp` would say it better but is not constexpr
/// before C++23.
template <typename F>
[[nodiscard]] constexpr F two_to_the(int power) noexcept {
    F result = F{1};
    for (int i = 0; i < power; ++i) {
        result *= F{2};
    }
    return result;
}

/// Refuse a value the column's type cannot hold.
///
/// `static_cast` would wrap it silently, and the result is not merely wrong
/// but inverted: on an int32 column, `view_count > 3000000000` becomes
/// `view_count > -1294967296`, so a filter written to match nothing matches
/// every row -- and that reaches `remove()` as readily as `to_vector()`.
///
/// A floating value against an integral column is the same hazard with worse
/// footing: out of range there is undefined behaviour rather than wrapping,
/// and NaN fits nothing at all. A fractional one is the same hazard again,
/// quieter still -- the cast truncates toward zero, so `count < 10.5` becomes
/// `count < 10` and stops matching a row the written predicate includes, and
/// `count > -3.5` becomes `count > -3` and stops matching -3. Nothing is out
/// of range and nothing is undefined; the query simply means something else.
///
/// So an integral column takes whole numbers only, whatever type they arrive
/// as. A floating column narrows rather than truncates, which is quieter
/// still: a `float` column given 1e-300 gets zero and the predicate silently
/// becomes one about zeroes. All of it arrives here because `Comparand`
/// admits any plain number against any other, which is what makes `id == 5`
/// ordinary.
template <typename Wanted, typename Given>
void require_representable(const Given& value) {
    constexpr bool integral_column = std::integral<Wanted> && !std::same_as<Wanted, bool>;

    if constexpr (integral_column && std::integral<Given> && !std::same_as<Given, bool>) {
        if (!std::in_range<Wanted>(value)) {
            throw query_build_error(
                "schemata: the value compared against this column does not fit its type, "
                "and narrowing it would change the query's meaning. Widen the column or "
                "range-check the value first.");
        }
    } else if constexpr (integral_column && std::floating_point<Given>) {
        // Ordered so each case gets the complaint that fits it. NaN is not
        // fractional and not out of range, it is simply not a number; and
        // -2147483648.5 is refused for being fractional, which is the thing
        // the caller can act on, rather than for a range it truncates into.
        if (!std::isfinite(value)) {
            throw query_build_error(
                "schemata: the value compared against this column is not a finite number, "
                "so it has no defined conversion to the column's type and the comparison "
                "cannot be built.");
        }
        if (std::trunc(value) != value) {
            throw query_build_error(
                "schemata: this column holds whole numbers, and the value compared against "
                "it is not one. Converting it would truncate toward zero and change which "
                "rows match -- 'x < 10.5' would ask for 'x < 10', which excludes 10. Round "
                "the value the way the comparison means it and pass a whole number.");
        }

        // The bound is the column's maximum plus one rather than the maximum
        // itself: that is a power of two and survives the trip into Given
        // exactly, where the maximum may round up and quietly admit the one
        // value past the end.
        constexpr Given limit  = two_to_the<Given>(std::numeric_limits<Wanted>::digits);
        constexpr Given bottom = std::is_signed_v<Wanted> ? -limit : Given{0};

        if (value < bottom || value >= limit) {
            throw query_build_error(
                "schemata: the value compared against this column does not fit its type, "
                "and narrowing it would change the query's meaning. Widen the column or "
                "range-check the value first.");
        }
    } else if constexpr (std::floating_point<Wanted> && std::floating_point<Given> &&
                         (sizeof(Given) > sizeof(Wanted))) {
        // A REAL column is the quiet member of the family. The comparand is
        // narrowed to the column's own type before it is bound, so a value
        // the column cannot hold does not fail -- it becomes a different
        // value, and the predicate goes on to mean something the caller did
        // not write.
        //
        // Two of those changes are categorical rather than a matter of
        // precision, and they are the ones worth refusing. `price == 1e-300`
        // becomes `price = 0` and matches every zero row; `price < 1e308`
        // becomes `price < inf` and matches everything. Ordinary precision
        // loss -- 0.1 not being exactly 0.1 in a float -- is inherent to the
        // column and is left alone, or a REAL column could not be queried at
        // all.
        //
        // NaN is not refused here. It narrows to NaN and means the same
        // thing before and after, so narrowing is not what is wrong with it;
        // `require_comparable` refuses it for what is.
        if (!std::isnan(value)) {
            const Wanted narrowed = static_cast<Wanted>(value);

            if (std::isfinite(value) && !std::isfinite(narrowed)) {
                throw query_build_error(
                    "schemata: the value compared against this column is larger than the "
                    "column's type can hold, and narrowing it turns it into infinity -- so "
                    "the comparison would ask about every row rather than the ones "
                    "intended. Widen the column, or compare against a value it can "
                    "represent.");
            }
            if (value != Given{0} && narrowed == Wanted{0}) {
                throw query_build_error(
                    "schemata: the value compared against this column is smaller than the "
                    "column's type can hold, and narrowing it turns it into zero -- so the "
                    "comparison would ask about rows holding zero rather than the ones "
                    "intended. Widen the column, or compare against a value it can "
                    "represent.");
            }
        }
    }
}

/// Refuse NaN as something to compare a column against.
///
/// Not a question of range, which is why it is not part of the check above:
/// NaN survives every conversion unchanged. What it does not survive is the
/// comparison. It is unequal to everything, itself included, and the engines
/// do not even agree about that much -- SQLite has no representation for it
/// and binds NULL instead, so `x != NaN`, which the written predicate says is
/// every row, matches none there.
///
/// Infinity is deliberately let through. `x < inf` is an ordinary "every
/// finite value" bound that all three engines answer alike, and refusing it
/// on one of them is exactly what made the same query throw on SQLite and run
/// on the MySQL family. Whether a value may be *stored* is a different rule,
/// and `require_storable` below is where it lives.
template <typename Given>
void require_comparable(const Given& value) {
    if constexpr (std::floating_point<Given>) {
        if (std::isnan(value)) {
            throw query_build_error(
                "schemata: NaN cannot be compared against a column. It is unequal to "
                "everything, itself included, and the engines disagree even about that: "
                "SQLite binds it as NULL, so every comparison with it matches no row there "
                "whatever the operator says. Compare against the condition the NaN stands "
                "for instead -- is_null(), or a range.");
        }
    }
}

/// Refuse a value on its way into a column that no engine stores alike.
///
/// The writing half of the rule `require_comparable` states for reading, and
/// stricter, because here infinity is refused too. MySQL and MariaDB reject
/// all three non-finite values with error 1264; SQLite keeps an infinity and
/// writes NaN as NULL without a word, which in a nullable column is a value
/// nobody can tell from one never written. Asked here, on the paths that
/// write, the caller gets the same answer on every engine -- rather than the
/// library's message on one and a server error on the other two.
///
/// `subject` says what was being written, for the message: the column, where
/// the caller knows it.
inline void require_storable(double value, std::string_view subject) {
    if (std::isfinite(value)) {
        return;
    }
    const bool nan = std::isnan(value);
    throw query_build_error(std::format(
        "schemata: {} would store {}, and no engine stores it the same way: MySQL and "
        "MariaDB refuse it, and SQLite would {}. Store a sentinel the column can hold, or "
        "leave the column null deliberately.",
        subject, nan ? "NaN" : (value > 0 ? "positive infinity" : "negative infinity"),
        nan ? "write NULL in its place without a word" : "keep it"));
}

/// The same, for a value already converted. Only a real can be non-finite.
inline void require_storable(const Value& value, std::string_view subject) {
    if (std::holds_alternative<double>(value.storage())) {
        require_storable(value.as_real(), subject);
    }
}

/// Whether a date or timestamp is text the parsers read back, year in four
/// digits. True for anything that is not text, which is not this rule's to
/// judge. Out of line because it parses.
[[nodiscard]] bool reads_back_as_calendar(const Value& value, ColumnType type);

/// Refuse a date or timestamp that no engine can read back.
///
/// The stored form is fixed -- `YYYY-MM-DD`, and a time after it -- so a value
/// it cannot say is not stored differently, it is stored wrongly. A
/// `schemata::date{}` renders as 0000-00-00, a day its month does not have as
/// itself, and a year past 9999 in five digits. MySQL refused those, MariaDB
/// kept the zero date and SQLite kept every one -- and from then on each read
/// of the row failed, a whole-table read included. Refused before a statement
/// exists, as a non-finite double is, so every engine gives the same answer.
///
/// Judged on the rendered text rather than on the C++ value: the write paths
/// only ever see a `Value`, and the rule that matters is the round trip
/// itself, that what is written is what `parse_date` and `parse_timestamp`
/// read back.
///
/// `subject` opens the message and says what was about to happen to the
/// value: "'orders.due' would store", "a comparison would use".
[[noreturn]] void refuse_unreadable_calendar(const Value&     value,
                                             ColumnType       type,
                                             std::string_view subject);

/// The same, naming the column. Formats nothing unless it refuses, because
/// the write paths call it for every column of every row.
///
/// A date or timestamp column is held to the calendar rule as well.
inline void require_storable(const Value& value, const Table& table, const Column& column) {
    if (std::holds_alternative<double>(value.storage()) && !std::isfinite(value.as_real())) {
        require_storable(value.as_real(), std::format("'{}.{}'", table.name, column.name));
    }
    if ((column.type == ColumnType::date || column.type == ColumnType::timestamp) &&
        !reads_back_as_calendar(value, column.type)) {
        refuse_unreadable_calendar(value, column.type,
                                   std::format("'{}.{}' would store", table.name, column.name));
    }
}

/// The same check, for a column whose type is only known at run time.
///
/// `where(col(&User::id) == 1.5)` knows the member's C++ type and refuses at
/// the call. `find<User>(1.5)` does not: a key is written as a literal, so
/// `find<User>(7)` has to work whether the column is int32 or int64, and the
/// conversion goes by shape rather than by the exact declared type. That left
/// the two paths disagreeing about the same mistake -- one raised, the other
/// quietly matched nothing, which looks exactly like a row that is not there.
///
/// Dispatching on the declared ColumnType puts both on the same rules and the
/// same messages.
template <typename Given>
void require_key_representable(ColumnType type, const Given& value) {
    switch (type) {
        case ColumnType::int8:   require_representable<std::int8_t>(value); return;
        case ColumnType::int16:  require_representable<std::int16_t>(value); return;
        case ColumnType::int32:  require_representable<std::int32_t>(value); return;
        case ColumnType::int64:  require_representable<std::int64_t>(value); return;
        case ColumnType::uint8:  require_representable<std::uint8_t>(value); return;
        case ColumnType::uint16: require_representable<std::uint16_t>(value); return;
        case ColumnType::uint32: require_representable<std::uint32_t>(value); return;
        case ColumnType::uint64: require_representable<std::uint64_t>(value); return;
        case ColumnType::real32: require_representable<float>(value); return;
        case ColumnType::real:   require_representable<double>(value); return;

        // Nothing to say. A key of one of these arrives as text or as a
        // converter's own Value, and the numeric narrowing rules do not
        // describe it.
        case ColumnType::boolean:
        case ColumnType::decimal:
        case ColumnType::text:
        case ColumnType::blob:
        case ColumnType::timestamp:
        case ColumnType::date:
            return;
    }
}

/// Check a key against the column it will be compared with.
///
/// Only the cases the compile-time path would also have stopped: a number
/// that does not survive the trip into the column's type, and text where a
/// number belongs. A key of the wrong shape is not merely a query that
/// matches nothing -- on the `where()` side it does not compile at all.
inline void require_key_matches(const Value& key, ColumnType type) {
    const bool numeric_column =
        type == ColumnType::int8 || type == ColumnType::int16 ||
        type == ColumnType::int32 || type == ColumnType::int64 ||
        type == ColumnType::uint8 || type == ColumnType::uint16 ||
        type == ColumnType::uint32 || type == ColumnType::uint64 ||
        type == ColumnType::real32 || type == ColumnType::real;

    if (key.is_null()) {
        throw query_build_error(
            "schemata: a null key matches no row, because SQL comparison against null is "
            "never true. Use where() with a null test if that is what the query means.");
    }

    if (std::holds_alternative<std::int64_t>(key.storage())) {
        require_key_representable(type, key.as_int64());
    } else if (std::holds_alternative<std::uint64_t>(key.storage())) {
        require_key_representable(type, key.as_uint64());
    } else if (std::holds_alternative<double>(key.storage())) {
        require_key_representable(type, key.as_real());
        require_comparable(key.as_real());
    } else if (numeric_column && std::holds_alternative<std::string>(key.storage())) {
        throw query_build_error(
            "schemata: this column holds numbers and the key is text, so the comparison "
            "would match no row whatever is in the table. Pass a number, or look the row "
            "up by a column that holds text.");
    }

    if ((type == ColumnType::date || type == ColumnType::timestamp) &&
        !reads_back_as_calendar(key, type)) {
        refuse_unreadable_calendar(key, type, "find() would look for");
    }
}

/// Convert a comparand to the column's own type before storing it.
///
/// The conversion is what makes `email == "bob"` bind a string rather than a
/// pointer, and `id == 5` bind a 64-bit integer rather than an int.
///
/// A date or timestamp no engine could read back is refused here too,
/// because a comparison against one means something different on each of
/// them: SQLite compares the text, where "10000-01-01" sorts before
/// "2024-01-01". `subject` opens that refusal and says what the value was
/// for.
template <typename Wanted, typename Given>
[[nodiscard]] Value operand_value(const Given&     value,
                                  std::string_view subject = "a comparison would use") {
    Value converted = [&] {
        if constexpr (std::same_as<Wanted, timestamp> && is_sys_time_v<Given> &&
                      !std::same_as<Given, timestamp>) {
            // A coarsening time_point conversion is not a static_cast, and the
            // clock's own precision varies by standard library.
            return ValueConverter<timestamp>::to_value(
                std::chrono::time_point_cast<std::chrono::microseconds>(value));
        } else {
            // Range first, so a NaN against an integral column is told it has
            // no conversion -- the more specific complaint -- before being
            // told it cannot be compared.
            require_representable<Wanted>(value);
            require_comparable(value);
            return ValueConverter<Wanted>::to_value(static_cast<Wanted>(value));
        }
    }();

    const ColumnType type = ValueConverter<Wanted>::column_type();
    if ((type == ColumnType::date || type == ColumnType::timestamp) &&
        !reads_back_as_calendar(converted, type)) {
        refuse_unreadable_calendar(converted, type, subject);
    }
    return converted;
}

} // namespace detail

/// A runtime predicate. Untyped on purpose: see the note at the top.
///
/// Combining predicates is a plain non-template operation, so an expression
/// of any size instantiates nothing.
class Predicate {
public:
    Predicate() = default;
    explicit Predicate(ExprPtr node) : node_(std::move(node)) {}

    [[nodiscard]] bool           empty() const noexcept { return node_ == nullptr; }
    [[nodiscard]] const ExprPtr& node() const noexcept { return node_; }

private:
    ExprPtr node_;
};

/// Combine two predicates.
///
/// An empty predicate is the identity, so a caller building a filter up
/// conditionally does not need a special case for "nothing selected yet".
[[nodiscard]] Predicate operator&&(const Predicate& lhs, const Predicate& rhs);
[[nodiscard]] Predicate operator||(const Predicate& lhs, const Predicate& rhs);
[[nodiscard]] Predicate operator!(const Predicate& operand);

/// A reference to one mapped member, carrying its type so that comparisons
/// against the wrong kind of value do not compile.
template <typename C, typename M>
class ColumnExpr {
public:
    using entity_type  = C;
    using member_type  = M;
    using operand_type = query_operand_t<M>;

    explicit ColumnExpr(M C::*ptr) : key_(member_key(ptr)) {}

    [[nodiscard]] const MemberKey& key() const noexcept { return key_; }
    [[nodiscard]] ExprPtr node() const { return detail::make(ExprNode{ColumnRef{key_}}); }

private:
    MemberKey key_;
};

/// Name a mapped member in a predicate.
template <typename C, typename M>
[[nodiscard]] ColumnExpr<C, M> col(M C::*ptr) {
    return ColumnExpr<C, M>(ptr);
}

// Each comparison gets two overloads: column against value, and column
// against column. Both collapse to the same runtime node.
#define SCHEMATA_COMPARISON(symbol, op)                                                       \
    template <typename C, typename M, typename V>                                             \
        requires Storable<M> && Comparand<V, query_operand_t<M>>                              \
    [[nodiscard]] Predicate operator symbol(ColumnExpr<C, M> column, const V& value) {        \
        return Predicate(detail::make(ExprNode{Binary{                                        \
            op, column.node(),                                                                \
            detail::make(ExprNode{                                                            \
                Literal{detail::operand_value<query_operand_t<M>>(value)}})}}));              \
    }                                                                                         \
                                                                                              \
    template <typename C, typename M, typename D, typename N>                                 \
        requires std::same_as<query_operand_t<M>, query_operand_t<N>>                         \
    [[nodiscard]] Predicate operator symbol(ColumnExpr<C, M> lhs, ColumnExpr<D, N> rhs) {     \
        return Predicate(detail::make(ExprNode{Binary{op, lhs.node(), rhs.node()}}));         \
    }

SCHEMATA_COMPARISON(==, BinaryOp::eq)
SCHEMATA_COMPARISON(!=, BinaryOp::ne)
SCHEMATA_COMPARISON(<, BinaryOp::lt)
SCHEMATA_COMPARISON(<=, BinaryOp::le)
SCHEMATA_COMPARISON(>, BinaryOp::gt)
SCHEMATA_COMPARISON(>=, BinaryOp::ge)

#undef SCHEMATA_COMPARISON

/// `LIKE`, with the pattern bound rather than interpolated.
///
/// Wildcards are the caller's to write. Schemata does not escape `%` or `_`
/// in the pattern, because a pattern with no wildcards is a literal match
/// and would be better spelled `==`.
template <typename C, typename M>
    requires std::same_as<query_operand_t<M>, std::string>
[[nodiscard]] Predicate like(ColumnExpr<C, M> column, std::string_view pattern) {
    return Predicate(detail::make(
        ExprNode{Binary{BinaryOp::like, column.node(),
                        detail::make(ExprNode{Literal{Value{std::string(pattern)}}})}}));
}

/// Null tests are allowed on any column, not only optional ones: an outer
/// join makes a non-nullable column null on the unmatched side.
template <typename C, typename M>
[[nodiscard]] Predicate is_null(ColumnExpr<C, M> column) {
    return Predicate(detail::make(ExprNode{Unary{UnaryOp::is_null, column.node()}}));
}

template <typename C, typename M>
[[nodiscard]] Predicate is_not_null(ColumnExpr<C, M> column) {
    return Predicate(detail::make(ExprNode{Unary{UnaryOp::is_not_null, column.node()}}));
}

namespace detail {

template <typename C, typename M, typename Range>
[[nodiscard]] Predicate in_range(ColumnExpr<C, M> column, const Range& values, bool negated) {
    InList list;
    list.operand = column.node();
    list.negated = negated;
    for (const auto& value : values) {
        list.values.push_back(operand_value<query_operand_t<M>>(value));
    }
    return Predicate(make(ExprNode{std::move(list)}));
}

} // namespace detail

/// `IN` over a list of values.
///
/// An empty list is not an error: it emits a constant false, which is what
/// the predicate means. Refusing it would push the special case onto every
/// caller filtering by a set that happens to be empty.
template <typename C, typename M, typename V>
    requires Comparand<V, query_operand_t<M>>
[[nodiscard]] Predicate in(ColumnExpr<C, M> column, std::initializer_list<V> values) {
    return detail::in_range(column, values, false);
}

template <typename C, typename M, typename V>
    requires Comparand<V, query_operand_t<M>>
[[nodiscard]] Predicate in(ColumnExpr<C, M> column, const std::vector<V>& values) {
    return detail::in_range(column, values, false);
}

template <typename C, typename M, typename V>
    requires Comparand<V, query_operand_t<M>>
[[nodiscard]] Predicate not_in(ColumnExpr<C, M> column, std::initializer_list<V> values) {
    return detail::in_range(column, values, true);
}

template <typename C, typename M, typename V>
    requires Comparand<V, query_operand_t<M>>
[[nodiscard]] Predicate not_in(ColumnExpr<C, M> column, const std::vector<V>& values) {
    return detail::in_range(column, values, true);
}

// ---------------------------------------------------------------------------
// Aggregates
// ---------------------------------------------------------------------------

/// An aggregate, carrying the C++ type its result comes back as.
///
/// One template deep like `ColumnExpr`, and for the same reason: `R` is what
/// makes `select(sum(&Sale::amount))` produce a `double` rather than a
/// `Value`, and what makes `having(sum(&Sale::amount) > "x")` a compile
/// error. Past construction everything is the runtime `Aggregate` node.
///
/// `R` is the *final* result type, optional included. Every aggregate but
/// `COUNT` returns null over a group with no non-null values -- `SUM` of
/// nothing is not zero -- so those are spelled `std::optional<...>` here
/// rather than quietly turning a null into a default-constructed value.
template <typename R>
class Agg {
public:
    using result_type = R;

    explicit Agg(Aggregate node) : node_(std::move(node)) {}

    [[nodiscard]] const Aggregate& term() const noexcept { return node_; }
    [[nodiscard]] ExprPtr          node() const { return detail::make(ExprNode{node_}); }

private:
    Aggregate node_;
};

namespace detail {

/// What `SUM` of a column comes back as.
///
/// An integer column sums to an integer, which keeps an exact total exact --
/// going through `double` would start losing whole units past 2^53. Anything
/// else sums to `double`.
///
/// The one case this cannot carry is a total that overflows 64 bits, and
/// every engine refuses it with a `query_error` rather than answer wrongly.
/// SQLite raises "integer overflow". MySQL and MariaDB return `DECIMAL` for an
/// integer `SUM` and hold the total, which the driver then refuses to read
/// into an int64, naming what the server sent. A wider return type would
/// carry it, at a cost to every ordinary total that nobody wants to pay.
template <typename M>
using sum_result_t =
    std::conditional_t<std::integral<query_operand_t<M>>, std::int64_t, double>;

template <typename C, typename M>
[[nodiscard]] Aggregate agg_node(AggregateOp op, M C::*ptr, bool distinct = false) {
    return Aggregate{op, member_key(ptr), distinct};
}

} // namespace detail

/// `COUNT(*)` -- how many rows are in the group.
[[nodiscard]] inline Agg<std::int64_t> count() {
    return Agg<std::int64_t>(Aggregate{AggregateOp::count, std::nullopt, false});
}

/// `COUNT(column)` -- how many rows have a non-null value there.
///
/// Deliberately different from `count()` on a nullable column, which is the
/// whole reason to spell it this way.
template <typename C, typename M>
    requires Storable<query_operand_t<M>>
[[nodiscard]] Agg<std::int64_t> count(M C::*ptr) {
    return Agg<std::int64_t>(detail::agg_node(AggregateOp::count, ptr));
}

/// `COUNT(DISTINCT column)`.
template <typename C, typename M>
    requires Storable<query_operand_t<M>>
[[nodiscard]] Agg<std::int64_t> count_distinct(M C::*ptr) {
    return Agg<std::int64_t>(detail::agg_node(AggregateOp::count, ptr, true));
}

/// `SUM(column)`. Null over a group with no non-null values, which is not
/// the same as zero and is not reported as zero.
template <typename C, typename M>
    requires detail::PlainNumber<query_operand_t<M>>
[[nodiscard]] Agg<std::optional<detail::sum_result_t<M>>> sum(M C::*ptr) {
    return Agg<std::optional<detail::sum_result_t<M>>>(
        detail::agg_node(AggregateOp::sum, ptr));
}

/// `AVG(column)`. Always fractional, whatever the column holds, and a double
/// on every engine. The MySQL family would average whole numbers as `DECIMAL`,
/// rounded to the four places `div_precision_increment` allows, so there the
/// average is taken over the column as DOUBLE. The engines then agree to
/// within a double's precision.
template <typename C, typename M>
    requires detail::PlainNumber<query_operand_t<M>>
[[nodiscard]] Agg<std::optional<double>> avg(M C::*ptr) {
    return Agg<std::optional<double>>(detail::agg_node(AggregateOp::avg, ptr));
}

/// `MIN(column)` and `MAX(column)`, in the column's own type.
///
/// Spelled out rather than `min` and `max` because `<windows.h>` defines
/// those as function-like macros, and a function-like macro expands wherever
/// the name is followed by `(` -- including `schemata::min(...)` and
/// `query.min(...)`. A consumer who includes `<windows.h>` without
/// `NOMINMAX` is ordinary, and this library's headers are tested against
/// exactly that.
template <typename C, typename M>
    requires Storable<query_operand_t<M>>
[[nodiscard]] Agg<std::optional<query_operand_t<M>>> minimum(M C::*ptr) {
    return Agg<std::optional<query_operand_t<M>>>(detail::agg_node(AggregateOp::min, ptr));
}

template <typename C, typename M>
    requires Storable<query_operand_t<M>>
[[nodiscard]] Agg<std::optional<query_operand_t<M>>> maximum(M C::*ptr) {
    return Agg<std::optional<query_operand_t<M>>>(detail::agg_node(AggregateOp::max, ptr));
}

// An aggregate compares against a value the same way a column does, so that
// `having(sum(&Sale::amount) > 1000)` type-checks against what SUM returns
// rather than against what the column holds.
#define SCHEMATA_AGG_COMPARISON(symbol, op)                                                   \
    template <typename R, typename V>                                                         \
        requires Comparand<V, query_operand_t<R>>                                             \
    [[nodiscard]] Predicate operator symbol(const Agg<R>& agg, const V& value) {              \
        return Predicate(detail::make(ExprNode{Binary{                                        \
            op, agg.node(),                                                                   \
            detail::make(ExprNode{                                                            \
                Literal{detail::operand_value<query_operand_t<R>>(value)}})}}));              \
    }

SCHEMATA_AGG_COMPARISON(==, BinaryOp::eq)
SCHEMATA_AGG_COMPARISON(!=, BinaryOp::ne)
SCHEMATA_AGG_COMPARISON(<, BinaryOp::lt)
SCHEMATA_AGG_COMPARISON(<=, BinaryOp::le)
SCHEMATA_AGG_COMPARISON(>, BinaryOp::gt)
SCHEMATA_AGG_COMPARISON(>=, BinaryOp::ge)

#undef SCHEMATA_AGG_COMPARISON

// ---------------------------------------------------------------------------
// Assignments, for a set-based UPDATE
// ---------------------------------------------------------------------------

/// One `SET column = ?` clause.
///
/// The value is a bound parameter like every other, so a bulk update is
/// sanitised by the same property as everything else here: values never
/// reach the SQL text.
struct Assignment {
    MemberKey member;
    Value     value;
};

/// Assign a value to a mapped member, for `Query::update()`.
///
/// Typed through the same `Comparand` rule the comparison operators use, so
/// `set(&User::active, 5)` does not compile for the same reason
/// `col(&User::active) == 5` does not.
///
/// The value rule is the stricter, storing one: infinity compares but does not
/// store, so `set(&Reading::d, inf)` is refused where `col(&Reading::d) < inf`
/// is not. Checked first, so a NaN is told it cannot be stored rather than
/// that it cannot be compared. A date no engine could read back is likewise
/// told it cannot be stored.
template <typename C, typename M, typename V>
    requires Storable<query_operand_t<M>> && Comparand<V, query_operand_t<M>>
[[nodiscard]] Assignment set(M C::*ptr, const V& value) {
    if constexpr (std::floating_point<V>) {
        detail::require_storable(static_cast<double>(value), "this assignment");
    }
    return Assignment{member_key(ptr), detail::operand_value<query_operand_t<M>>(
                                           value, "this assignment would store")};
}

/// Assign null. Separate from the typed overload because `std::nullopt` has
/// no column type to convert against, and because writing a null into a
/// column the model says is not nullable should be caught where the plan can
/// see the column -- which `Query::update()` does.
template <typename C, typename M>
[[nodiscard]] Assignment set_null(M C::*ptr) {
    return Assignment{member_key(ptr), Value{}};
}

} // namespace schemata
