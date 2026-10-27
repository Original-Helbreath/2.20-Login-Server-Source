#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <schemata/core/error.h>
#include <schemata/driver/driver.h>
#include <schemata/model/model.h>
#include <schemata/query/materialise.h>
#include <schemata/query/plan.h>
#include <schemata/sql/expr.h>

namespace schemata {

namespace detail {

/// What one `select()` item contributes: its C++ result type, and the
/// runtime term the plan stores.
///
/// Two specialisations rather than an overload set, because the result type
/// has to be available to build the tuple before anything is called.
template <typename Item>
struct select_result;

template <typename C, typename M>
struct select_result<M C::*> {
    using type = M;
    [[nodiscard]] static Aggregate term(M C::*ptr) {
        return Aggregate{AggregateOp::none, member_key(ptr), false};
    }
};

template <typename R>
struct select_result<Agg<R>> {
    using type = R;
    [[nodiscard]] static Aggregate term(const Agg<R>& aggregate) { return aggregate.term(); }
};

/// Read one projected row into its tuple.
///
/// Positional, like the entity materialiser and for the same reason: the
/// select list was emitted from this same list of items, in this order.
template <typename Row, std::size_t... I>
[[nodiscard]] Row read_row(const driver::Statement& row, const std::vector<ColumnType>& types,
                           std::index_sequence<I...>) {
    Row out;
    (ValueConverter<std::tuple_element_t<I, Row>>::from_value(
         read_column(row, static_cast<int>(I), types[I]), std::get<I>(out)),
     ...);
    return out;
}

/// Refuse a statement that would end or restart the caller's transaction.
///
/// Called by every raw-SQL entry point before it prepares anything. Does
/// nothing when no transaction opened through Schemata is open: outside one,
/// transaction control is the caller's own business.
///
/// \throws query_build_error, and nothing is sent.
void require_no_transaction_control(driver::Connection& connection, std::string_view sql);

/// Bind one value to parameter `index`, as `bind_params()` binds each of its
/// own. For a caller whose values are not held in one vector.
void bind_value(driver::Statement& stmt, int index, const Value& value);

} // namespace detail

/// Bind a fragment's parameters to a prepared statement, in order.
void bind_params(driver::Statement& stmt, const std::vector<Value>& params);

/// Refuse an assignment whose value no engine stores alike.
///
/// `set()` already refuses one, but an `Assignment` is a plain aggregate, and
/// one built by hand reaches `Query::update()` without ever passing through
/// it. Declared here rather than left to the plan so the check sits on the
/// path that executes, beside the binding.
void require_storable_assignments(const Table&                   table,
                                  const std::vector<Assignment>& assignments);

/// Fill the navigations named by `includes` on already-materialised entities.
///
/// Not a template: it works entirely through the type-erased accessors the
/// model records, so the second query is compiled once for the program
/// rather than once per entity type.
void load_included(driver::Connection& connection, const Model& model, const Table& table,
                   const std::vector<MemberKey>& includes,
                   const std::vector<void*>&     entities);

/// A query over one entity type.
///
/// Deliberately thin: every method forwards to `QueryPlan`, which is not a
/// template, so the SQL building is compiled once for the whole program
/// rather than once per entity. What this layer adds is the entity type --
/// enough to materialise rows and to make `col(&User::email) == 5` a compile
/// error.
///
/// Execution is eager (D3). `to_vector()` runs the statement and returns
/// rows; there is no deferred iterator, no identity map and no lazy loading,
/// so the SQL that runs is predictable from reading the call site.
template <typename T>
class Query {
public:
    Query(driver::Connection& connection, const Model& model)
        : connection_(&connection), plan_(make_plan(connection, model)) {}

    /// The plan keeps a pointer to the model, so a temporary one would
    /// dangle from the first call onwards -- `Query<T> q(db, make_model())`
    /// compiled and crashed on first use. Refused at compile time, as
    /// `Context` and `ChangeTracker` refuse it.
    Query(driver::Connection&, const Model&&) = delete;

    /// Narrow the result. Repeated calls are combined with AND.
    Query& where(const Predicate& predicate) {
        plan_.filter(predicate);
        return *this;
    }

    /// Narrow the groups, for a grouped query. Repeated calls are combined
    /// with AND.
    ///
    /// `HAVING` rather than `WHERE`, so the predicate may name aggregates:
    /// `having(sum(&Sale::amount) > 1000)`.
    Query& having(const Predicate& predicate) {
        plan_.having(predicate);
        return *this;
    }

    /// Group the result. Every selected column that is not aggregated must
    /// appear here, or the query is refused -- see `QueryPlan`.
    template <typename... Ms>
    Query& group_by(Ms T::*... ptrs) {
        (plan_.group(member_key(ptrs)), ...);
        return *this;
    }

    /// `SELECT DISTINCT`. Applies to whatever the select list is, which for
    /// an unprojected query is every mapped column.
    Query& distinct(bool on = true) {
        plan_.distinct(on);
        return *this;
    }

    Query& order_by(Ordering term) {
        plan_.order(std::move(term));
        return *this;
    }

    /// Order ascending by a member, for the common case.
    template <typename C, typename M>
    Query& order_by(M C::*ptr) {
        plan_.order(Ordering{Aggregate{AggregateOp::none, member_key(ptr), false}, false});
        return *this;
    }

    /// Order ascending by an aggregate, for a grouped result.
    template <typename R>
    Query& order_by(const Agg<R>& aggregate) {
        plan_.order(Ordering{aggregate.term(), false});
        return *this;
    }

    /// Also load a related entity, in a second statement.
    ///
    /// Two queries rather than a join (D3). A join would repeat every parent
    /// once per child, leaving the materialiser to collapse the duplicates
    /// and the caller to pay for the wide columns again on every row; with
    /// two children navigations it would multiply them together. Instead the
    /// parents are read, their keys collected, and the related rows fetched
    /// with a single `IN` -- two statements whatever the fan-out, and both
    /// of them plain enough to recognise in a slow query log.
    ///
    /// Including the same navigation twice replaces what it held rather than
    /// appending to it.
    template <typename M>
    Query& include(M T::*ptr) {
        const MemberKey key = member_key(ptr);
        if (plan_.table().find_navigation(key) == nullptr) {
            // Checked here rather than when the query runs, so the message
            // arrives at the include() that is wrong.
            throw query_build_error(std::format(
                "schemata: include() names a member of '{}' that describe() does not "
                "declare as a navigation; add has_many() or has_one() for it",
                plan_.table().name));
        }
        includes_.push_back(key);
        return *this;
    }

    /// Keep only rows that have at least one related row satisfying
    /// `inner`.
    ///
    ///     ctx.query<User>()
    ///        .where_any(&User::posts, col(&Post::created_at) >= march);
    ///
    /// `inner` names members of the *related* entity; naming a member of the
    /// queried one is refused, because inside the subquery it is not in
    /// scope. Omitting it entirely means "has any related row at all".
    ///
    /// Emitted as a correlated `EXISTS`, not a join, so the result is still
    /// one row per matching entity -- paging, `count()` and the materialiser
    /// all keep meaning what they meant. Reading the related rows is what
    /// `include()` is for, and the two combine freely.
    template <typename M>
    Query& where_any(M T::*ptr, const Predicate& inner = Predicate{}) {
        plan_.filter(plan_.related(member_key(ptr), inner, false));
        return *this;
    }

    /// The negation: keep only rows with no related row satisfying `inner`.
    ///
    /// With no `inner`, "has no related rows at all" -- the orphan query,
    /// which has no other spelling here.
    ///
    /// Note that `where_none(nav, p)` is not `!where_any(nav, p)` applied per
    /// related row: it asks that *no* related row satisfies `p`, which a row
    /// with no related rows also satisfies.
    template <typename M>
    Query& where_none(M T::*ptr, const Predicate& inner = Predicate{}) {
        plan_.filter(plan_.related(member_key(ptr), inner, true));
        return *this;
    }

    /// At most `rows` rows.
    Query& take(std::int64_t rows) {
        plan_.limit(rows);
        return *this;
    }

    /// Skip `rows` rows. Paging without an order is not deterministic on
    /// either engine, so pair it with `order_by`.
    Query& skip(std::int64_t rows) {
        plan_.offset(rows);
        return *this;
    }

    [[nodiscard]] std::vector<T> to_vector() const {
        const auto fragment = plan_.select_sql();

        std::vector<T> rows;
        {
            // Scoped so the statement is closed before any include() runs
            // its own. Both drivers buffer a result set, so overlapping
            // statements would work -- but not having to rely on that keeps
            // the two engines doing the same thing.
            auto stmt = connection_->prepare_cached(fragment.text);
            bind_params(*stmt, fragment.params);

            while (stmt->step()) {
                T entity{};
                materialise(plan_.table(), *stmt, &entity);
                rows.push_back(std::move(entity));
            }
        }

        if (!includes_.empty() && !rows.empty()) {
            std::vector<void*> entities;
            entities.reserve(rows.size());
            for (T& row : rows) {
                entities.push_back(&row);
            }
            load_included(*connection_, plan_.model(), plan_.table(), includes_, entities);
        }
        return rows;
    }

    /// Read a subset of columns, or aggregates over them, instead of whole
    /// entities.
    ///
    /// Each item is either a mapped member or an aggregate:
    ///
    ///     q.select(&User::id, &User::email);          // vector<tuple<...>>
    ///     q.select(&User::email);                     // vector<string>
    ///     q.group_by(&Sale::region)
    ///      .select(&Sale::region, sum(&Sale::amount));
    ///
    /// A single item comes back as a plain vector of that type rather than a
    /// vector of one-element tuples, because a one-element tuple is a
    /// nuisance at every call site and there is nothing it disambiguates.
    ///
    /// Aggregates other than `count()` come back as `std::optional`: over a
    /// group with no non-null values they are null, and `SUM` of nothing is
    /// not zero.
    ///
    /// The query itself is not modified, so one filter can be read several
    /// ways.
    template <typename... Items>
        requires(sizeof...(Items) > 0)
    [[nodiscard]] auto select(Items... items) const {
        using Row = std::tuple<typename detail::select_result<Items>::type...>;

        if (!includes_.empty()) {
            // Silently dropping them would be worse: include() says "also
            // load the related rows", and a projection has nowhere to put
            // them.
            throw query_build_error(
                "schemata: select() projects columns and include() loads related entities; "
                "the two do not combine. Drop the include(), or read whole entities with "
                "to_vector()");
        }

        Query copy = *this;
        (copy.plan_.project(detail::select_result<Items>::term(items)), ...);

        const auto fragment = copy.plan_.select_sql();
        const auto types    = copy.plan_.projection_types();

        auto stmt = connection_->prepare_cached(fragment.text);
        bind_params(*stmt, fragment.params);

        std::vector<Row> rows;
        while (stmt->step()) {
            rows.push_back(
                detail::read_row<Row>(*stmt, types, std::index_sequence_for<Items...>{}));
        }

        if constexpr (sizeof...(Items) == 1) {
            std::vector<std::tuple_element_t<0, Row>> flat;
            flat.reserve(rows.size());
            for (Row& row : rows) {
                flat.push_back(std::move(std::get<0>(row)));
            }
            return flat;
        } else {
            return rows;
        }
    }

    /// The first matching row, or nothing.
    ///
    /// Spelled as an optional rather than as a throwing accessor because a
    /// row that is not there is an ordinary outcome, not a fault (D11).
    [[nodiscard]] std::optional<T> first() const {
        Query copy = *this;
        copy.take(1);
        auto rows = copy.to_vector();
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::move(rows.front());
    }

    /// How many rows match, ignoring take() and skip().
    [[nodiscard]] std::int64_t count() const {
        const auto fragment = plan_.count_sql();
        auto       stmt     = connection_->prepare_cached(fragment.text);
        bind_params(*stmt, fragment.params);
        if (!stmt->step()) {
            return 0;
        }
        return stmt->get_int64(0);
    }

    /// One aggregate over the whole filtered set.
    ///
    /// The scalar shorthand for `select(aggregate).front()`, for the common
    /// case of wanting one number rather than a grouped result.
    ///
    ///     const auto total = q.total(sum(&Sale::amount)).value_or(0.0);
    ///
    /// Named `total` rather than `aggregate` because `aggregate` reads as a
    /// verb at the call site and this returns a value.
    template <typename R>
    [[nodiscard]] R total(const Agg<R>& aggregate) const {
        auto rows = select(aggregate);
        if (rows.empty()) {
            // An ungrouped aggregate always returns exactly one row, so this
            // is unreachable through the query builder. It is here so that a
            // grouped query misused this way yields the empty answer rather
            // than reading past the end.
            return R{};
        }
        return std::move(rows.front());
    }

    /// `SUM`, `AVG`, `MIN`, `MAX` and `COUNT(column)` over the filtered set.
    ///
    /// `minimum` and `maximum` rather than `min` and `max` because
    /// `<windows.h>` defines those as function-like macros, which expand
    /// after a `.` just as readily as anywhere else.
    ///
    /// All but `count` return `std::optional`: over no rows, or over rows
    /// that are all null there, the answer is null and not zero.
    template <typename M>
    [[nodiscard]] auto sum(M T::*ptr) const {
        return total(schemata::sum(ptr));
    }

    template <typename M>
    [[nodiscard]] std::optional<double> avg(M T::*ptr) const {
        return total(schemata::avg(ptr));
    }

    template <typename M>
    [[nodiscard]] auto minimum(M T::*ptr) const {
        return total(schemata::minimum(ptr));
    }

    template <typename M>
    [[nodiscard]] auto maximum(M T::*ptr) const {
        return total(schemata::maximum(ptr));
    }

    /// How many rows have a non-null value in this column, which on a
    /// nullable column is deliberately not the same as `count()`.
    template <typename M>
    [[nodiscard]] std::int64_t count(M T::*ptr) const {
        return total(schemata::count(ptr));
    }

    template <typename M>
    [[nodiscard]] std::int64_t count_distinct(M T::*ptr) const {
        return total(schemata::count_distinct(ptr));
    }

    /// Whether anything matches.
    ///
    /// Drops any `include()` first: the answer is a bool, and loading the
    /// related rows of the one entity it looks at would be a statement per
    /// navigation spent on something nobody is going to read.
    [[nodiscard]] bool any() const {
        Query copy = *this;
        copy.includes_.clear();
        return copy.first().has_value();
    }

    /// Delete every matching row, returning how many went.
    ///
    /// A query with no filter deletes the whole table, which is what the
    /// call says, so it is not refused -- but it is worth reading twice.
    ///
    /// `take()` and `skip()` are refused rather than ignored: SQLite cannot
    /// express a limited DELETE, so honouring them would mean the same code
    /// deleting ten rows on one engine and all of them on the other. To
    /// delete a bounded set, select it first and delete by key.
    ///
    /// `order_by()` is accepted and does nothing. Without a bound it changes
    /// nothing about which rows match, so there is nothing to refuse -- and
    /// refusing it would break the ordinary shape of building one query and
    /// both reading from it and deleting through it.
    std::int64_t remove() {
        const auto fragment = plan_.delete_sql();
        auto       stmt     = connection_->prepare_cached(fragment.text);
        bind_params(*stmt, fragment.params);
        (void)stmt->step();
        return connection_->affected_rows();
    }

    /// Update every matching row in one statement, returning how many the
    /// engine reported changed.
    ///
    ///     ctx.query<User>()
    ///        .where(col(&User::last_seen) < cutoff)
    ///        .update(set(&User::active, false));
    ///
    /// **This bypasses the change tracker.** Nothing here is staged, nothing
    /// is snapshotted, and an entity already tracked in a `Context` keeps the
    /// values it had -- so a later `save_changes()` on that entity will write
    /// them back over this. Bulk writes and tracked writes are two ways of
    /// owning the same row; pick one per row. `remove()` has the same
    /// property.
    ///
    /// A query with no filter updates the whole table, which is what the call
    /// says, so it is not refused -- but it is worth reading twice.
    ///
    /// Assigning to a key column is refused: a set-based key rewrite cannot
    /// be tracked, and where foreign keys are not enforced it silently
    /// orphans related rows.
    ///
    /// The return value is the engine's own count, and the two engine
    /// families count differently: SQLite reports rows *matched*, the MySQL
    /// family reports rows *changed*, so a row already holding the new values
    /// is counted by one and not the other. Do not use it as a test of
    /// whether the rows were there -- unless the MySQL-family connection was
    /// opened with `MySqlOptions::found_rows`, which makes it count matched
    /// rows too.
    template <typename... As>
        requires(sizeof...(As) > 0 && (std::same_as<std::decay_t<As>, Assignment> && ...))
    std::int64_t update(As&&... assignments) {
        return update(std::vector<Assignment>{std::forward<As>(assignments)...});
    }

    /// The same, for a list built programmatically.
    std::int64_t update(const std::vector<Assignment>& assignments) {
        require_storable_assignments(plan_.table(), assignments);
        const auto fragment = plan_.update_sql(assignments);
        auto       stmt     = connection_->prepare_cached(fragment.text);
        bind_params(*stmt, fragment.params);
        (void)stmt->step();
        return connection_->affected_rows();
    }

    /// The SQL this query would run, without running it.
    ///
    /// The seam the emitter tests use, and the one to reach for when a query
    /// does something unexpected.
    [[nodiscard]] SqlFragment to_sql() const { return plan_.select_sql(); }

    /// The SQL a `select()` with these items would run, without running it.
    template <typename... Items>
        requires(sizeof...(Items) > 0)
    [[nodiscard]] SqlFragment to_sql(Items... items) const {
        Query copy = *this;
        (copy.plan_.project(detail::select_result<Items>::term(items)), ...);
        return copy.plan_.select_sql();
    }

    /// The SQL an `update()` with these assignments would run.
    [[nodiscard]] SqlFragment update_sql(const std::vector<Assignment>& assignments) const {
        return plan_.update_sql(assignments);
    }

private:
    static QueryPlan make_plan(driver::Connection& connection, const Model& model) {
        const Table* table = model.find<T>();
        if (table == nullptr) {
            throw query_build_error(
                "schemata: this entity is not registered in the model; call add<T>() first");
        }
        return QueryPlan(model, *table, connection.dialect());
    }

    driver::Connection*    connection_;
    QueryPlan              plan_;
    std::vector<MemberKey> includes_;
};

/// Start a query.
///
/// A free function alongside `Context::query<T>()`, which is the same thing
/// with the connection and the model already in hand.
///
/// **Lifetime.** The returned `Query` keeps a pointer to both arguments and
/// copies neither, so it needs a model and a connection that outlive it.
/// A temporary model is refused at compile time: chaining straight through to
/// `to_vector()` in one expression would survive it, but `auto q =
/// from<T>(db, make_model())` compiles to exactly the same call and crashes
/// on first use, and no overload can tell the two apart. `Context` is the way
/// not to have to think about any of it.
template <typename T>
[[nodiscard]] Query<T> from(driver::Connection& connection, const Model& model) {
    return Query<T>(connection, model);
}

template <typename T>
Query<T> from(driver::Connection&, const Model&&) = delete;

// ---------------------------------------------------------------------------
// Raw SQL, typed
// ---------------------------------------------------------------------------

/// Run a statement that returns no rows, with bound parameters.
///
/// `Connection::execute` takes no parameters, so without this the only
/// parameterised write available is through an entity. Returns the engine's
/// affected-row count, with the caveat `Query::update()` documents: SQLite
/// counts rows matched and the MySQL family counts rows changed.
///
/// The SQL is the caller's and is read no further than its first keyword.
/// Parameters are bound, so the values in them are closed the same way every
/// other value here is; what the statement *does* is the caller's to get
/// right.
///
/// With one exception. While a transaction opened through Schemata is open,
/// transaction control is refused unsent with `query_build_error`: `COMMIT`,
/// `ROLLBACK`, `BEGIN`, `START TRANSACTION`, `END`, `XA`, and a `SET` of this
/// session's `autocommit`. Each of those ends or restarts the transaction
/// where the connection's count, the change tracker and the RAII guard cannot
/// follow, and the next `commit()` would then report as saved work the server
/// had already committed or thrown away. Use `Context::transaction()`, or
/// `begin()`, `commit()` and `rollback()`. `ROLLBACK TO SAVEPOINT` is not
/// transaction control and is allowed; `Context::savepoint()` is the version
/// that keeps the tracker in step.
///
/// What no amount of reading the statement can cover is a `CALL`. A procedure
/// that commits, rolls back or starts a transaction of its own does it out of
/// the client's sight: the server's transaction session-tracking reports only
/// which state the transaction is now in, so a procedure that committed and
/// wrote again is indistinguishable from one that did nothing. A procedure
/// like that is outside what Schemata can see -- where it ends the transaction
/// and a later statement notices, the outcome is reported as unknown for the
/// caller to resolve, and where it commits and opens another, nothing here
/// will notice at all.
std::int64_t execute_sql(driver::Connection& connection, std::string_view sql,
                         const std::vector<Value>& params = {});

/// Read entities out of hand-written SQL.
///
/// The escape hatch that keeps the types: anything the query builder cannot
/// express -- a window function, a `UNION`, a recursive CTE, an engine's own
/// extension -- can still come back as mapped entities rather than as columns
/// read one at a time.
///
///     auto users = schemata::from_sql<User>(
///         conn, model,
///         "SELECT u.* FROM users u JOIN orders o ON o.user_id = u.id "
///         "GROUP BY u.id HAVING COUNT(*) > ?",
///         {schemata::Value{std::int64_t{5}}});
///
/// Columns are matched **by name**, so the `SELECT` must name every mapped
/// column, aliased to the column's own name where it is computed. A missing
/// one is an error naming it rather than a silent null.
///
/// Nothing here is tracked: these are plain entities, like everything
/// `to_vector()` returns. Pass one to `Context::track()` to make it writable.
///
/// A `CALL` is read here like any other statement, with one limit: a
/// procedure sends a result set for every `SELECT` it runs, and this reads the
/// **first**. The rest are drained so the connection stays usable, and their
/// rows are not returned.
///
/// Transaction control is refused here as it is in `execute_sql()`, and for
/// the same reason; see that function for what is refused and what a `CALL`
/// can still do unseen.
template <typename T>
[[nodiscard]] std::vector<T> from_sql(driver::Connection& connection, const Model& model,
                                      std::string_view          sql,
                                      const std::vector<Value>& params = {}) {
    const Table* table = model.find<T>();
    if (table == nullptr) {
        throw query_build_error(
            "schemata: this entity is not registered in the model; call add<T>() first");
    }

    detail::require_no_transaction_control(connection, sql);

    auto stmt = connection.prepare_cached(sql);
    bind_params(*stmt, params);

    std::vector<T> rows;
    while (stmt->step()) {
        T entity{};
        materialise_by_name(*table, *stmt, &entity);
        rows.push_back(std::move(entity));
    }
    return rows;
}

/// Read arbitrary columns out of hand-written SQL, as the types named here.
///
///     auto totals = schemata::rows_from_sql<std::string, double>(
///         conn, "SELECT region, SUM(amount) FROM sales GROUP BY region");
///
/// Positional, not by name: there is no model to match against, and the
/// caller has just written the select list. The requested C++ type decides
/// how each column is read, so a column asked for as `std::string` is read as
/// text whatever the engine thinks it stored.
///
/// One type comes back as a plain vector rather than a vector of
/// one-element tuples, matching `Query::select()`.
///
/// A `CALL` is read here too, and as in `from_sql()` it is the procedure's
/// **first** result set that comes back; the rest are drained. The column
/// count of a procedure is only known once it has run, so a mismatch is
/// reported after the call rather than before it.
///
/// Transaction control is refused here as it is in `execute_sql()`, and for
/// the same reason; see that function for what is refused and what a `CALL`
/// can still do unseen.
template <typename... Ts>
    requires(sizeof...(Ts) > 0)
[[nodiscard]] auto rows_from_sql(driver::Connection& connection, std::string_view sql,
                                 const std::vector<Value>& params = {}) {
    using Row = std::tuple<Ts...>;

    const std::vector<ColumnType> types{ValueConverter<Ts>::column_type()...};

    detail::require_no_transaction_control(connection, sql);

    auto stmt = connection.prepare_cached(sql);
    bind_params(*stmt, params);

    // A CALL reports no columns until the procedure has run, so for one the
    // count is checked after the first step rather than before it. Asking too
    // early is what answered "the statement returns 0" for every procedure,
    // however many rows it was about to send.
    const bool runs_first = driver::statement_kind(sql) == driver::StatementKind::routine;

    const auto require_columns = [&] {
        if (stmt->column_count() >= static_cast<int>(sizeof...(Ts))) {
            return;
        }
        if (runs_first && stmt->column_count() == 0) {
            throw query_build_error(std::format(
                "schemata: this CALL returned no result set, so the {} column(s) asked for "
                "cannot be read from it. A procedure that only writes belongs in "
                "execute_sql(); one whose rows are wanted here has to SELECT them.",
                sizeof...(Ts)));
        }
        throw query_build_error(std::format(
            "schemata: {} column(s) were asked for but the statement returns {}",
            sizeof...(Ts), stmt->column_count()));
    };

    if (!runs_first) {
        require_columns();
    }

    std::vector<Row> rows;
    bool             checked = !runs_first;
    while (stmt->step()) {
        if (!checked) {
            require_columns();
            checked = true;
        }
        rows.push_back(detail::read_row<Row>(*stmt, types, std::index_sequence_for<Ts...>{}));
    }
    if (!checked) {
        require_columns();
    }

    if constexpr (sizeof...(Ts) == 1) {
        std::vector<std::tuple_element_t<0, Row>> flat;
        flat.reserve(rows.size());
        for (Row& row : rows) {
            flat.push_back(std::move(std::get<0>(row)));
        }
        return flat;
    } else {
        return rows;
    }
}

} // namespace schemata
