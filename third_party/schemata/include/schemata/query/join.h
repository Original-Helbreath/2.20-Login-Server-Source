#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <schemata/core/error.h>
#include <schemata/driver/driver.h>
#include <schemata/model/model.h>
#include <schemata/query/join_kind.h>
#include <schemata/query/plan.h>
#include <schemata/query/query.h>
#include <schemata/sql/expr.h>

namespace schemata {

/// Everything about a two-table join that does not depend on the entity
/// types: the SQL building, the member resolution and the dialect.
///
/// The join counterpart of `QueryPlan`, and not a template for the same
/// reason: `JoinQuery<A, B>` is a thin typed shell over it.
///
///     SELECT "a"."c1" AS "l0", ..., "b"."c1" AS "r0", ...
///       FROM "a" [LEFT] JOIN "b" ON <on>
///      [WHERE <filter>] [ORDER BY ...] [LIMIT ? [OFFSET ?]]
///
/// Every select-list column is aliased by side and position (`l0`, `l1`,
/// ..., `r0`, ...), so two tables that both map `id` -- or `name`, or any
/// other column -- never hand back two result columns of the same name. The
/// rows are read positionally, as `Query<T>` reads its own.
///
/// Tables are named as `Query<T>` names them: unqualified, in the
/// connection's default schema.
/// For tables in other schemas, or a model with no C++ entity types, join by
/// table and column name instead: `JoinSpec` in `<schemata/query/rows.h>`.
class JoinPlan {
public:
    JoinPlan(const Model& model, const Table& left, const Table& right, Dialect dialect,
             JoinKind kind);

    /// Keeps a pointer to the model, so a temporary one is refused.
    JoinPlan(const Model&&, const Table&, const Table&, Dialect, JoinKind) = delete;

    /// Add to the `ON` condition. Repeated calls are combined with AND, which
    /// is how a multi-column key is joined on.
    void on(const Predicate& predicate);

    /// Narrow the result. May name members of either side; repeated calls
    /// are combined with AND.
    void filter(const Predicate& predicate);

    /// Order by a member of either side. Aggregates are refused: a join is
    /// not grouped.
    void order(Ordering term);
    void limit(std::int64_t rows);
    void offset(std::int64_t rows);

    /// `SELECT <both sides' columns> FROM <left> [LEFT] JOIN <right> ...`
    ///
    /// \throws query_build_error if no `ON` condition was given, or if any
    ///         predicate or ordering names a member of neither side.
    [[nodiscard]] SqlFragment select_sql() const;

    /// `SELECT COUNT(*)` over the same join and filter, ignoring order and
    /// paging.
    [[nodiscard]] SqlFragment count_sql() const;

    /// The result column the right-hand entity starts at.
    [[nodiscard]] int right_offset() const noexcept;

    /// Whether the current row carries a right-hand row at all.
    ///
    /// Always true for an inner join. For a left join, false when a key
    /// column of the right-hand table is NULL: a key column is never NULL in
    /// a stored row, so a NULL there can only be the join's own.
    [[nodiscard]] bool right_present(const driver::Statement& row) const;

    [[nodiscard]] const Table& left() const noexcept { return *left_; }
    [[nodiscard]] const Table& right() const noexcept { return *right_; }
    [[nodiscard]] const Model& model() const noexcept { return *model_; }
    [[nodiscard]] Dialect      dialect() const noexcept { return dialect_; }
    [[nodiscard]] JoinKind     kind() const noexcept { return kind_; }

private:
    [[nodiscard]] std::string    qualified(const MemberKey& member) const;
    [[nodiscard]] ColumnResolver resolver() const;
    void                         append_from(SqlFragment& out) const;
    void                         append_where(SqlFragment& out) const;

    const Model*                model_;
    const Table*                left_;
    const Table*                right_;
    Dialect                     dialect_;
    JoinKind                    kind_;
    Predicate                   on_;
    Predicate                   predicate_;
    std::vector<Ordering>       orderings_;
    std::optional<std::int64_t> limit_;
    std::optional<std::int64_t> offset_;
};

/// A query over two mapped entities joined on a condition, returning both.
///
///     auto friends = ctx.join<CharacterFriend, Character>(
///                           col(&CharacterFriend::other_character_id) == col(&Character::id))
///                        .where(col(&CharacterFriend::character_id) == me)
///                        .order_by(&Character::name)
///                        .take(50)
///                        .to_vector();   // vector<pair<CharacterFriend, Character>>
///
/// One statement, whatever the row count. An inner join comes back as
/// `std::pair<A, B>`; a left join (`left_join()`) as
/// `std::pair<A, std::optional<B>>`, empty where no right-hand row matched.
///
/// Filters, the `ON` condition and orderings may name members of either
/// entity; naming a member of any other is refused. The two entities must be
/// different types: a member pointer names its entity by type, so a self-join
/// could not say which side a member means.
///
/// Nothing is tracked and nothing is included, as with `Query<T>`.
template <typename A, typename B, JoinKind Kind = JoinKind::inner>
class JoinQuery {
    static_assert(!std::is_same_v<A, B>,
                  "schemata: a join needs two different entity types; a self-join cannot "
                  "tell its two sides apart by member pointer");

public:
    using right_type = std::conditional_t<Kind == JoinKind::left, std::optional<B>, B>;
    using row_type   = std::pair<A, right_type>;

    JoinQuery(driver::Connection& connection, const Model& model, const Predicate& on)
        : connection_(&connection), plan_(make_plan(connection, model)) {
        plan_.on(on);
    }

    JoinQuery(driver::Connection&, const Model&&, const Predicate&) = delete;

    /// Add to the `ON` condition, combined with AND.
    JoinQuery& on(const Predicate& predicate) {
        plan_.on(predicate);
        return *this;
    }

    /// Narrow the result. Repeated calls are combined with AND.
    JoinQuery& where(const Predicate& predicate) {
        plan_.filter(predicate);
        return *this;
    }

    JoinQuery& order_by(Ordering term) {
        plan_.order(std::move(term));
        return *this;
    }

    /// Order ascending by a member of either side.
    template <typename C, typename M>
    JoinQuery& order_by(M C::*ptr) {
        static_assert(std::is_same_v<C, A> || std::is_same_v<C, B>,
                      "schemata: order_by() names a member of neither side of the join");
        plan_.order(Ordering{Aggregate{AggregateOp::none, member_key(ptr), false}, false});
        return *this;
    }

    JoinQuery& take(std::int64_t rows) {
        plan_.limit(rows);
        return *this;
    }

    /// Skip `rows` rows; pair it with `order_by`.
    JoinQuery& skip(std::int64_t rows) {
        plan_.offset(rows);
        return *this;
    }

    [[nodiscard]] std::vector<row_type> to_vector() const {
        const auto fragment = plan_.select_sql();
        auto       stmt     = connection_->prepare_cached(fragment.text);
        bind_params(*stmt, fragment.params);

        const int right_at = plan_.right_offset();

        std::vector<row_type> rows;
        while (stmt->step()) {
            row_type row{};
            materialise_at(plan_.left(), *stmt, 0, &row.first);
            if constexpr (Kind == JoinKind::left) {
                if (plan_.right_present(*stmt)) {
                    B right{};
                    materialise_at(plan_.right(), *stmt, right_at, &right);
                    row.second = std::move(right);
                }
            } else {
                materialise_at(plan_.right(), *stmt, right_at, &row.second);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    /// The first row, or nothing.
    [[nodiscard]] std::optional<row_type> first() const {
        JoinQuery copy = *this;
        copy.take(1);
        auto rows = copy.to_vector();
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::move(rows.front());
    }

    /// How many rows the join yields, ignoring take() and skip().
    [[nodiscard]] std::int64_t count() const {
        const auto fragment = plan_.count_sql();
        auto       stmt     = connection_->prepare_cached(fragment.text);
        bind_params(*stmt, fragment.params);
        if (!stmt->step()) {
            return 0;
        }
        return stmt->get_int64(0);
    }

    /// The SQL this join would run, without running it.
    [[nodiscard]] SqlFragment to_sql() const { return plan_.select_sql(); }

private:
    static JoinPlan make_plan(driver::Connection& connection, const Model& model) {
        const Table* left  = model.find<A>();
        const Table* right = model.find<B>();
        if (left == nullptr || right == nullptr) {
            throw query_build_error(
                "schemata: a joined entity is not registered in the model; call add<T>() "
                "for both sides first");
        }
        return JoinPlan(model, *left, *right, connection.dialect(), Kind);
    }

    driver::Connection* connection_;
    JoinPlan            plan_;
};

/// Start an inner join of `A` and `B` on `on`. The returned query keeps a
/// pointer to both arguments, as `from<T>()` does.
template <typename A, typename B>
[[nodiscard]] JoinQuery<A, B> join(driver::Connection& connection, const Model& model,
                                   const Predicate& on) {
    return JoinQuery<A, B>(connection, model, on);
}

template <typename A, typename B>
JoinQuery<A, B> join(driver::Connection&, const Model&&, const Predicate&) = delete;

/// Start a left join: every `A`, with `B` absent where nothing matched.
///
/// A filter on a member of `B` is applied after the join, as SQL applies it,
/// so it drops the rows where `B` is absent; put such a condition in `on()`
/// to keep them.
template <typename A, typename B>
[[nodiscard]] JoinQuery<A, B, JoinKind::left> left_join(driver::Connection& connection,
                                                        const Model&        model,
                                                        const Predicate&    on) {
    return JoinQuery<A, B, JoinKind::left>(connection, model, on);
}

template <typename A, typename B>
JoinQuery<A, B, JoinKind::left> left_join(driver::Connection&, const Model&&,
                                          const Predicate&) = delete;

} // namespace schemata
