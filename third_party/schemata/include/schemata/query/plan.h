#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <schemata/model/model.h>
#include <schemata/sql/dialect.h>
#include <schemata/sql/expr.h>

namespace schemata {

/// One ORDER BY term: a mapped member, or an aggregate over one.
struct Ordering {
    Aggregate term;
    bool      descending = false;
};

/// Order ascending by a mapped member.
template <typename C, typename M>
[[nodiscard]] Ordering asc(M C::*ptr) {
    return Ordering{Aggregate{AggregateOp::none, member_key(ptr), false}, false};
}

template <typename C, typename M>
[[nodiscard]] Ordering desc(M C::*ptr) {
    return Ordering{Aggregate{AggregateOp::none, member_key(ptr), false}, true};
}

/// Order by an aggregate, for a grouped result.
template <typename R>
[[nodiscard]] Ordering asc(const Agg<R>& aggregate) {
    return Ordering{aggregate.term(), false};
}

template <typename R>
[[nodiscard]] Ordering desc(const Agg<R>& aggregate) {
    return Ordering{aggregate.term(), true};
}

/// Everything about a query that does not depend on the entity type.
///
/// `Query<T>` is a thin typed shell over this, so the SQL building, the
/// member resolution and the dialect handling are compiled once rather than
/// once per entity -- and are testable without naming an entity type at all.
class QueryPlan {
public:
    QueryPlan(const Model& model, const Table& table, Dialect dialect);

    /// Keeps a pointer to the model, so a temporary one is refused.
    QueryPlan(const Model&&, const Table&, Dialect) = delete;

    /// Narrow the result. Repeated calls are combined with AND, so a filter
    /// can be built up in a loop.
    void filter(const Predicate& predicate);

    void order(Ordering term);
    void limit(std::int64_t rows);
    void offset(std::int64_t rows);

    /// Name one select-list item. Calling this at all replaces the default
    /// "every mapped column" list, so the first call decides the shape of
    /// the result and later ones extend it.
    void project(Aggregate item);

    /// Add a `GROUP BY` term.
    void group(MemberKey member);

    /// Narrow the groups. Repeated calls are combined with AND, like
    /// `filter`.
    void having(const Predicate& predicate);

    /// `SELECT DISTINCT`.
    void distinct(bool on) noexcept { distinct_ = on; }

    /// A predicate over a declared relationship: "has a related row, and
    /// that row satisfies `inner`".
    ///
    /// A correlated `EXISTS` rather than a join. D3 already decided against
    /// joining for `include()` -- a join repeats every parent once per child
    /// and multiplies with two navigations -- and the same reasoning holds
    /// for filtering, where a join would additionally need the duplicates
    /// collapsed before the caller ever saw them. `EXISTS` keeps the `FROM`
    /// clause one table, so the materialiser, the paging and the row count
    /// all keep meaning what they meant.
    ///
    /// `inner` names members of the *target* entity, and is lowered here
    /// against the target's table. An empty `inner` means "any related row".
    ///
    /// \throws query_build_error if the member is not a declared navigation,
    ///         or if the correlation cannot be resolved to a single column
    ///         on each side.
    [[nodiscard]] Predicate related(const MemberKey& navigation, const Predicate& inner,
                                    bool negated) const;

    [[nodiscard]] bool has_projection() const noexcept { return !projection_.empty(); }

    /// `SELECT <columns> FROM <table> ...`
    ///
    /// With no projection this names every mapped column explicitly, in the
    /// model's declaration order, so the materialiser reads them positionally
    /// without a second lookup and a column added to the table out of band
    /// cannot shift the mapping.
    ///
    /// \throws query_build_error if the select list, the grouping and the
    ///         ordering do not agree -- see `require_groupable`.
    [[nodiscard]] SqlFragment select_sql() const;

    /// `SELECT COUNT(*) ...` over the same filter, ignoring order and paging.
    ///
    /// Wrapped in a derived table when `DISTINCT`, a grouping or an
    /// aggregate means the number of result rows is not the number of
    /// matching rows.
    [[nodiscard]] SqlFragment count_sql() const;

    /// `DELETE FROM <table> ...` over the same filter.
    [[nodiscard]] SqlFragment delete_sql() const;

    /// `UPDATE <table> SET ... ` over the same filter.
    [[nodiscard]] SqlFragment update_sql(const std::vector<Assignment>& assignments) const;

    /// The column types of the select list, in order, for reading a
    /// projected row back.
    [[nodiscard]] std::vector<ColumnType> projection_types() const;

    [[nodiscard]] const Table& table() const noexcept { return *table_; }
    [[nodiscard]] const Model& model() const noexcept { return *model_; }
    [[nodiscard]] Dialect      dialect() const noexcept { return dialect_; }

private:
    [[nodiscard]] std::string        qualified(const MemberKey& member) const;
    [[nodiscard]] const Column&      column_for(const MemberKey& member) const;

    /// The same lookup scoped to another table, for the inside of an
    /// `EXISTS`. The outer rule -- "only columns of the queried entity" --
    /// is exactly what must not apply there.
    [[nodiscard]] const Column&  column_in(const Table& scope, const MemberKey& member) const;
    [[nodiscard]] std::string    qualified_in(const Table& scope, const MemberKey& member) const;
    [[nodiscard]] ColumnResolver resolver_for(const Table& scope) const;
    [[nodiscard]] ColumnResolver     resolver() const;
    void                             append_select_list(SqlFragment& out) const;
    void                             append_where(SqlFragment& out) const;
    void                             append_group(SqlFragment& out) const;
    void                             append_having(SqlFragment& out) const;
    void                             append_order(SqlFragment& out) const;
    void                             append_paging(SqlFragment& out) const;
    void                             append_term(SqlFragment& out, const Aggregate& term) const;

    /// Refuse a select list the two engine families would answer
    /// differently.
    ///
    /// The MySQL family runs with `ONLY_FULL_GROUP_BY` and refuses a bare
    /// column that is neither grouped nor aggregated; SQLite accepts it and
    /// returns a value from an arbitrary row of the group. Letting that
    /// through is how a query passes on a developer's SQLite and reports
    /// different numbers in production, so it is refused here -- on every
    /// engine equally, before any SQL is emitted.
    void require_groupable() const;

    [[nodiscard]] bool has_aggregate() const;

    const Model*                model_;
    const Table*                table_;
    Dialect                     dialect_;
    Predicate                   predicate_;
    Predicate                   having_;
    std::vector<Aggregate>      projection_;
    std::vector<MemberKey>      groups_;
    std::vector<Ordering>       orderings_;
    std::optional<std::int64_t> limit_;
    std::optional<std::int64_t> offset_;
    bool                        distinct_ = false;
};

} // namespace schemata
