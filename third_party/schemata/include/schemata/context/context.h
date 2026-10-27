#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <schemata/context/pool.h>
#include <schemata/core/converter.h>
#include <schemata/core/error.h>
#include <schemata/core/identifier.h>
#include <schemata/driver/driver.h>
#include <schemata/model/model.h>
#include <schemata/query/bulk.h>
#include <schemata/query/join.h>
#include <schemata/query/query.h>
#include <schemata/query/rows.h>
#include <schemata/query/tracker.h>
#include <schemata/schema/differ.h>
#include <schemata/schema/manage.h>

namespace schemata {

/// A transaction that rolls back unless it is committed.
///
/// The RAII shape matters more here than anywhere else in the library: an
/// early return or a thrown exception between BEGIN and COMMIT would
/// otherwise leave the connection inside an open transaction, holding locks,
/// with nothing to close it. Forgetting to commit loses work; forgetting to
/// roll back corrupts the next thing that connection does.
///
/// Not nestable, because neither engine nests: SQLite refuses a second
/// BEGIN, and MySQL's START TRANSACTION silently commits whatever was in
/// flight. For partial rollback take `Context::savepoint()` inside a
/// `Context::begin()` Scope: the connection's raw savepoint does not mark the
/// tracker's staging, so returning to it loses tracked work silently.
class Transaction {
public:
    explicit Transaction(driver::Connection& connection) : connection_(&connection) {
        connection_->begin();
    }

    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;

    Transaction(Transaction&& other) noexcept
        : connection_(std::exchange(other.connection_, nullptr)),
          finished_(std::exchange(other.finished_, true)) {}

    Transaction& operator=(Transaction&& other) noexcept {
        if (this != &other) {
            rollback();
            connection_ = std::exchange(other.connection_, nullptr);
            finished_   = std::exchange(other.finished_, true);
        }
        return *this;
    }

    ~Transaction() { rollback(); }

    void commit() {
        if (finished_ || connection_ == nullptr) {
            return;
        }
        finished_ = true;
        connection_->commit();
    }

    /// Roll back now. Called automatically on destruction, and it never
    /// throws: a failure here would replace whatever exception is already
    /// unwinding with a less useful one.
    void rollback() noexcept {
        if (finished_ || connection_ == nullptr) {
            return;
        }
        finished_ = true;
        try {
            connection_->rollback();
        } catch (...) {
        }
    }

    /// True once committed or rolled back.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

private:
    driver::Connection* connection_ = nullptr;
    bool                finished_   = false;
};

/// One unit of work: a connection, a model, and the changes pending on it.
///
/// The front door. Everything below it -- `Query`, `ChangeTracker`,
/// `ensure_schema` -- also works standalone, so nothing here is load-bearing
/// for correctness; it exists so a caller does not have to carry a
/// connection and a model to every call.
///
/// **Single-thread-affine, exactly like EF Core's DbContext** (D9). Change
/// tracking is inherently per-context state, and any other rule would mean
/// locking the hot path. Give each thread its own Context over its own
/// lease; that is what `ConnectionPool` is for.
///
/// A Context is cheap to construct: it holds a lease, not a connection of
/// its own, so making one per unit of work is the intended usage.
class Context {
public:
    /// Borrow a connection the caller owns. The connection must outlive the
    /// Context.
    Context(driver::Connection& connection, const Model& model)
        : connection_(&connection), model_(&model), tracker_(model) {}

    /// Take ownership of a pool lease, returned when the Context is
    /// destroyed.
    Context(ConnectionPool::Lease lease, const Model& model)
        : lease_(std::move(lease)),
          connection_(&lease_->get()),
          model_(&model),
          tracker_(model) {}

    /// A Context outlives the expression that made it, so a temporary model
    /// would dangle from the first call onwards -- `ensure_schema()` would
    /// read a destroyed Model, find no tables, create nothing and report
    /// success. `ChangeTracker` has refused this from the start; the front
    /// door had not, and `Context ctx(db, make_app_model())` is exactly what
    /// the scaffolder's generated factory invites.
    Context(driver::Connection&, const Model&&)  = delete;
    Context(ConnectionPool::Lease, const Model&&) = delete;

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    // -- access ------------------------------------------------------------

    [[nodiscard]] driver::Connection& connection() const noexcept { return *connection_; }
    [[nodiscard]] const Model&        model() const noexcept { return *model_; }
    [[nodiscard]] Dialect             dialect() const { return connection_->dialect(); }
    [[nodiscard]] ChangeTracker&      tracker() noexcept { return tracker_; }

    // -- reading -----------------------------------------------------------

    /// Start a query over one entity.
    template <typename T>
    [[nodiscard]] Query<T> query() const {
        return Query<T>(*connection_, *model_);
    }

    /// Start an inner join of two entities on `on`, read in one statement as
    /// `std::pair<A, B>` rows. See `JoinQuery`.
    template <typename A, typename B>
    [[nodiscard]] JoinQuery<A, B> join(const Predicate& on) const {
        return JoinQuery<A, B>(*connection_, *model_, on);
    }

    /// The same as a left join: `std::pair<A, std::optional<B>>` rows.
    template <typename A, typename B>
    [[nodiscard]] JoinQuery<A, B, JoinKind::left> left_join(const Predicate& on) const {
        return JoinQuery<A, B, JoinKind::left>(*connection_, *model_, on);
    }

    /// Load a row by primary key, as an untracked copy.
    ///
    /// Returns `std::optional` rather than throwing, because a row that is
    /// not there is an ordinary outcome (D11). The entity comes back by value
    /// and nothing tracks it, since the object the caller keeps does not
    /// exist until this has returned: change it and `save_changes()` saves
    /// nothing. To save changes to it, pass that object to `track()` before
    /// changing it. Tracking holds on to *that* object, so it must outlive
    /// the Context or be released with `forget()`.
    template <typename T, typename V>
    [[nodiscard]] std::optional<T> find(V&& key) const {
        const Table* table = model_->find<T>();
        if (table == nullptr) {
            throw query_build_error(
                "schemata: this entity is not registered in the model; call add<T>() first");
        }
        const auto keys = table->key_columns();
        if (keys.size() != 1) {
            throw query_build_error(
                "schemata: find() takes a single-column primary key; query by hand for a "
                "composite one");
        }
        return query<T>().where(key_predicate(*keys.front(), std::forward<V>(key))).first();
    }

    // -- raw SQL -------------------------------------------------------------

    /// Read entities out of hand-written SQL, on this context's connection
    /// and inside whatever transaction it is in.
    ///
    /// The point of the forwarder: reaching for `connection()` to run raw SQL
    /// is how a statement ends up outside the transaction the surrounding
    /// code thinks it is in.
    ///
    /// Columns are matched by name; nothing is tracked. Pass a result to
    /// `track()` to make it writable.
    template <typename T>
    [[nodiscard]] std::vector<T> from_sql(std::string_view          sql,
                                          const std::vector<Value>& params = {}) const {
        return schemata::from_sql<T>(*connection_, *model_, sql, params);
    }

    /// Read arbitrary columns out of hand-written SQL, as the types named.
    template <typename... Ts>
        requires(sizeof...(Ts) > 0)
    [[nodiscard]] auto rows_from_sql(std::string_view          sql,
                                     const std::vector<Value>& params = {}) const {
        return schemata::rows_from_sql<Ts...>(*connection_, sql, params);
    }

    /// Run a parameterised statement that returns no rows.
    ///
    /// Bypasses the change tracker exactly as `Query::update()` and
    /// `Query::remove()` do, and with the same consequence: a tracked entity
    /// whose row this writes keeps the values it had.
    ///
    /// Inside a transaction, transaction control written out by hand is
    /// refused before anything is sent: `COMMIT`, `ROLLBACK`, `BEGIN`, `START
    /// TRANSACTION`, `XA` and a `SET` of this session's `autocommit` raise
    /// `query_build_error`. Use `transaction()`, or `begin()`, `commit()` and
    /// `rollback()`, and `savepoint()` for a partial rollback.
    ///
    /// What still gets through is a statement that ends the transaction
    /// without being about transactions at all: DDL on the MySQL family,
    /// which commits implicitly, and a `CALL` whose procedure commits or
    /// rolls back inside itself. Those raise `query_error` once the engine has
    /// run them, and every statement after is refused unsent, as is the
    /// commit. What they ended cannot be taken back, so the error is there to
    /// stop the rest of the transaction running outside it. See
    /// `schemata::execute_sql()` for what a procedure can still do unseen.
    std::int64_t execute_sql(std::string_view          sql,
                             const std::vector<Value>& params = {}) const {
        return schemata::execute_sql(*connection_, sql, params);
    }

    // -- writing -----------------------------------------------------------

    template <typename T>
    void add(T& entity) {
        tracker_.add(entity);
    }

    template <typename T>
    void track(T& entity) {
        tracker_.track(entity);
    }

    template <typename T>
    void remove(T& entity) {
        tracker_.remove(entity);
    }

    template <typename T>
    void forget(T& entity) {
        tracker_.forget(entity);
    }

    /// Insert many entities with multi-row statements, on this context's
    /// connection and inside whatever transaction it is in.
    ///
    /// Bypasses the change tracker, and reads no generated key back: see
    /// `BulkOptions` for both, for how the rows are cut into statements, and
    /// for the transaction a call runs in -- one of its own, or under a
    /// savepoint in a `Scope` already open.
    template <detail::EntityRange R>
    std::int64_t insert_many(const R& entities, const BulkOptions& options = {}) {
        return schemata::insert_many(*connection_, *model_, entities, options);
    }

    /// Insert many entities, or overwrite the rows already there under their
    /// key. See `schemata::upsert_many()`.
    template <detail::EntityRange R>
    std::int64_t upsert_many(const R& entities, const BulkOptions& options = {}) {
        return schemata::upsert_many(*connection_, *model_, entities, options);
    }

    /// Insert rows of values into a table, the model checking one it holds.
    /// See `schemata::insert_rows()`.
    std::int64_t insert_rows(std::string_view table, const std::vector<std::string>& columns,
                             const std::vector<std::vector<Value>>& rows,
                             const BulkOptions&                     options = {}) {
        return schemata::insert_rows(*connection_, *model_, table, columns, rows, options);
    }

    /// Insert rows of values, or overwrite the rows already there under their
    /// key. See `schemata::upsert_rows()`.
    std::int64_t upsert_rows(std::string_view table, const std::vector<std::string>& columns,
                             const std::vector<std::vector<Value>>& rows,
                             const BulkOptions&                     options = {}) {
        return schemata::upsert_rows(*connection_, *model_, table, columns, rows, options);
    }

    // -- by table and column name ------------------------------------------
    //
    // The calls of <schemata/query/rows.h>, on this context's connection and
    // with its model, which checks a table it holds and types what is read
    // from it. None of them touches the change tracker.

    /// Read rows of a table by column name. See `schemata::select_rows()`;
    /// empty `columns` reads every column the model maps on a table it holds.
    [[nodiscard]] RowSet select_rows(std::string_view                table,
                                     const std::vector<std::string>& columns = {},
                                     const RowFilter&                filter  = {},
                                     const ReadOptions&              options = {}) const {
        return schemata::select_rows(*connection_, *model_, table, columns, filter, options);
    }

    /// Read rows one at a time into `fn`. See `schemata::for_each_row()`.
    template <typename F>
        requires std::invocable<F&, const RowView&>
    std::int64_t for_each_row(std::string_view table, const std::vector<std::string>& columns,
                              const RowFilter& filter, const ReadOptions& options,
                              F&& fn) const {
        return schemata::for_each_row(*connection_, *model_, table, columns, filter, options,
                                      std::forward<F>(fn));
    }

    /// Count the rows a filter matches. See `schemata::count_rows()`.
    [[nodiscard]] std::int64_t count_rows(std::string_view table, const RowFilter& filter = {},
                                          const TableTarget& target = {}) const {
        return schemata::count_rows(*connection_, *model_, table, filter, target);
    }

    /// The largest value of a column. See `schemata::max_of()`.
    [[nodiscard]] Value max_of(std::string_view table, std::string_view column,
                               const RowFilter& filter = {},
                               const TableTarget& target = {}) const {
        return schemata::max_of(*connection_, *model_, table, column, filter, target);
    }

    /// Insert one row by column name, and read its generated key back. See
    /// `schemata::insert_row()`.
    std::optional<std::int64_t> insert_row(std::string_view                table,
                                           const std::vector<std::string>& columns,
                                           const std::vector<Value>&       values,
                                           const TableTarget&              target = {}) {
        return schemata::insert_row(*connection_, *model_, table, columns, values, target);
    }

    /// Assign columns of the rows a filter matches; the rows matched. See
    /// `schemata::update_rows()`.
    std::int64_t update_rows(std::string_view table, const ColumnValues& set,
                             const RowFilter& filter, const TableTarget& target = {}) {
        return schemata::update_rows(*connection_, *model_, table, set, filter, target);
    }

    /// Delete the rows a filter matches. See `schemata::delete_rows()`.
    std::int64_t delete_rows(std::string_view table, const RowFilter& filter,
                             const TableTarget& target = {}) {
        return schemata::delete_rows(*connection_, *model_, table, filter, target);
    }

    /// Delete at most `limit.limit` of the rows a filter matches, in
    /// `limit.order_by`'s order. See `schemata::delete_rows()`.
    std::int64_t delete_rows(std::string_view table, const RowFilter& filter,
                             const DeleteLimit& limit, const TableTarget& target = {}) {
        return schemata::delete_rows(*connection_, *model_, table, filter, limit, target);
    }

    /// Apply every pending change.
    ///
    /// Opens a transaction of its own unless one is already open on this
    /// Context, in which case the changes join it and the caller decides
    /// when to commit -- and until they do, what was applied stays pending
    /// in the tracker, so a rollback leaves it retryable rather than
    /// silently dropping it.
    ///
    /// Refuses, with `query_build_error` and nothing written, to join a
    /// transaction the server has already ended -- rolled back as a deadlock
    /// victim, or committed by DDL -- which the Context would otherwise still
    /// count as open. Joining would write every change committed at once,
    /// with the caller still believing it could roll them back.
    ///
    /// **There is no optimistic concurrency, and none is planned here.** An
    /// UPDATE or DELETE that matches no row is not an error: the statement
    /// ran, nothing matched, and `save_changes()` returns normally. Entity
    /// Framework Core raises `DbUpdateConcurrencyException` in the same
    /// situation, so this is worth stating plainly rather than leaving to be
    /// discovered -- Schemata models no concurrency token, reads no row
    /// version, and makes no claim that the row you loaded is the row you
    /// wrote to.
    ///
    /// It is not an oversight that could be closed cheaply. The obvious
    /// check -- treat zero affected rows as a lost update -- is not portable:
    /// the MySQL family reports rows *changed* rather than rows *matched*, so
    /// a row that is present and already holds the new values reports zero
    /// there and one on SQLite. `MySqlOptions::found_rows` is the connection
    /// flag that makes the MySQL family count matched rows as SQLite does,
    /// but it changes what `affected_rows()` means for every caller, so it is
    /// the application's to set, and nothing here depends on it.
    ///
    /// Until there is one, express it in the model you already have: give the
    /// row a version or updated-at column and make the write conditional on
    /// it with `where()`, then check the affected count yourself.
    std::size_t save_changes() {
        return connection_->in_transaction() ? tracker_.save_within(*connection_)
                                             : tracker_.save_changes(*connection_);
    }

    /// Take a savepoint, marking the staged work alongside it.
    ///
    /// The reason to go through the Context rather than the connection: a
    /// `ROLLBACK TO SAVEPOINT` takes the rows away, and the tracker has no
    /// way to notice. Left to itself it goes on believing that work was
    /// saved, the commit baselines it, and `save_changes()` afterwards
    /// reports nothing to do -- the rows are gone and nothing was raised.
    ///
    /// These three keep the two in step. `connection().savepoint()` still
    /// exists and still works; it simply does not know about the tracker.
    ///
    /// An empty name, or one holding a NUL, is refused by all three with
    /// `query_build_error` before anything is sent.
    void savepoint(std::string name) {
        detail::require_quotable(name, "the savepoint name");
        require_transaction("savepoint()");
        connection_->savepoint(name);
        // Re-taking a name moves it, exactly as the engines do.
        erase_savepoint(name);
        savepoints_.emplace_back(std::move(name), tracker_.stage_mark());
    }

    /// Return to a savepoint, putting the work staged since it back to
    /// pending.
    void rollback_to(const std::string& name) {
        detail::require_quotable(name, "the savepoint name");
        require_transaction("rollback_to()");
        const auto mark = find_savepoint(name);
        connection_->rollback_to(name);
        tracker_.rollback_within_to(mark);

        // Savepoints taken after this one are gone with it, as they are on
        // the server. The name itself survives and can be rolled back to
        // again, which is what both engines do.
        forget_savepoints_after(name);
    }

    /// Release a savepoint. What it held stays part of the transaction.
    void release_savepoint(const std::string& name) {
        detail::require_quotable(name, "the savepoint name");
        require_transaction("release_savepoint()");
        connection_->release_savepoint(name);
        forget_savepoints_after(name);
        erase_savepoint(name);
    }

    /// A transaction that also tells its Context it is open.
    ///
    /// While one is alive, `save_changes()` joins it instead of opening a
    /// transaction of its own, and this is what tells the tracker afterwards
    /// whether that save counted -- a save inside a transaction is not
    /// settled until the transaction is.
    ///
    /// Once finished, a Scope is done with the Context: committing or rolling
    /// it back again does nothing at all. begin() is legal again the moment
    /// one commits, and by then the Context's staging and savepoints belong
    /// to whichever Scope came next.
    class Scope {
    public:
        Scope(Context& context, driver::Connection& connection)
            : context_(&context), transaction_(connection) {}

        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;

        ~Scope() {
            if (context_ == nullptr || transaction_.finished()) {
                // Finished already, so whatever is staged on the Context and
                // whatever savepoints it holds are some later Scope's.
                return;
            }

            // Unfinished means this transaction is being abandoned, taking
            // with it whatever a save_changes() inside this scope applied.
            // Those entries have to go back to being pending, or the work is
            // lost with nothing raised to say so.
            transaction_.rollback();
            resolve_without_commit();
            context_->savepoints_.clear();
        }

        void commit() {
            if (transaction_.finished()) {
                return;
            }
            try {
                transaction_.commit();
            } catch (...) {
                // Refused: rolled back, or already ended some other way.
                resolve_without_commit();
                context_->savepoints_.clear();
                throw;
            }
            context_->tracker_.commit_within();
            context_->savepoints_.clear();
        }

        void rollback() noexcept {
            if (transaction_.finished()) {
                return;
            }
            transaction_.rollback();
            resolve_without_commit();
            context_->savepoints_.clear();
        }

        [[nodiscard]] bool finished() const noexcept { return transaction_.finished(); }

    private:
        /// Settle the staging for a transaction that did not commit here.
        ///
        /// Called after the connection's own rollback, so what the connection
        /// learned while ending it is known first. Usually the work is gone
        /// and goes back to pending. But a transaction ended by a statement
        /// inside it -- DDL's implicit commit, a stored procedure's own COMMIT
        /// -- or one whose commit was sent and whose reply was lost with the
        /// connection may just as well have committed, and putting that work
        /// back as pending is exactly how a retry writes it twice. Nothing
        /// here can tell which, so the staging is left unresolved: the tracker
        /// refuses the next save until the caller says, with commit_within()
        /// or rollback_within().
        void resolve_without_commit() noexcept {
            if (context_->connection_->transaction_loss() ==
                driver::Connection::TransactionLoss::outcome_unknown) {
                return;
            }
            context_->tracker_.rollback_within();
        }

        // The nesting refusal lives on the connection rather than here, so
        // it also catches a second Context over the same connection and a
        // bare Transaction alongside this one -- both of which walked past a
        // check that only knew about one Context, and on the MySQL family
        // the nested BEGIN is what commits the outer transaction's work.
        Context*    context_;
        Transaction transaction_;  // destroyed first: rolls back if uncommitted
    };

    /// Begin a transaction spanning several operations.
    ///
    /// Rolls back if it goes out of scope without a commit. Prefer
    /// `transaction()` where the work fits in a lambda: it cannot forget the
    /// commit.
    [[nodiscard]] Scope begin() { return Scope(*this, *connection_); }

    /// Run `body` inside a transaction, committing if it returns normally.
    ///
    /// The shape to reach for: it cannot forget the commit, cannot leak the
    /// transaction on an exception, and keeps the Context's own bookkeeping
    /// straight.
    /// A body that never returns -- one ending in a throw, which is a
    /// perfectly ordinary way to abandon a transaction -- makes the commit
    /// below provably unreachable at that instantiation, and MSVC says so
    /// (C4702). It only sees it with optimisation on, so a caller building
    /// with /WX finds it in Release and not in Debug. The commit is right for
    /// every body that does return, and the Scope rolls back for the ones
    /// that do not.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
    template <typename F>
    decltype(auto) transaction(F&& body) {
        Scope scope(*this, *connection_);
        if constexpr (std::is_void_v<decltype(body())>) {
            body();
            scope.commit();
        } else {
            auto result = body();
            scope.commit();
            return result;
        }
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    // -- schema ------------------------------------------------------------

    EnsureResult ensure_schema(const DiffOptions& options = {}) {
        return schemata::ensure_schema(*connection_, *model_, options);
    }

    /// `schema` and `validation` as for the free `verify_schema()`: an alias
    /// bound in `bindings`, or a physical name.
    [[nodiscard]] SchemaDiff verify_schema(const SchemaBindings&  bindings   = {},
                                           std::string_view       schema     = {},
                                           const ValidateOptions& validation = {}) const {
        return schemata::verify_schema(*connection_, *model_, bindings, schema, validation);
    }

private:
    /// `column == key`, built without naming the entity's member type.
    template <typename V>
    [[nodiscard]] static Predicate key_predicate(const Column& column, V&& key) {
        Value bound = to_key_value(std::forward<V>(key));

        // The same guard `where()` applies, on the same values, with the same
        // messages. Without it the two ways of asking for one row disagreed
        // about identical mistakes: `where(col(&Parent::id) == 1.5)` raised
        // and `find<Parent>(1.5)` returned nothing, which is indistinguishable
        // from a row that simply is not there.
        detail::require_key_matches(bound, column.type);

        return Predicate(detail::make(ExprNode{
            Binary{BinaryOp::eq, detail::make(ExprNode{ColumnRef{column.member}}),
                   detail::make(ExprNode{Literal{std::move(bound)}})}}));
    }

    /// A key as a bound value.
    ///
    /// Converted by shape rather than by exact type, because a key is
    /// normally written as a literal and `find<User>(7)` must work whether
    /// the column is int32 or int64, just as `find<User>("bob")` must work
    /// for a char array. Going through `ValueConverter<V>` alone would
    /// require the caller to spell the exact C++ type the model declared.
    ///
    /// By shape, but never through a type that loses the value: the guard in
    /// `key_predicate` can only judge what it is handed.
    template <typename V>
    [[nodiscard]] static Value to_key_value(V&& key) {
        using Bare = std::remove_cvref_t<V>;
        if constexpr (std::is_same_v<Bare, Value>) {
            return std::forward<V>(key);
        } else if constexpr (std::is_same_v<Bare, bool>) {
            return Value{key};
        } else if constexpr (std::is_integral_v<Bare> && std::is_unsigned_v<Bare>) {
            // Carried as what it is. Cast through int64, the top half of a
            // uint64 wrapped to a negative number before the guard saw it: a
            // key above 2^63 that a BIGINT UNSIGNED column really holds was
            // refused as not fitting its own column, and the largest uint64
            // against a signed key arrived as -1 and quietly matched nothing
            // -- where where() refused it.
            return Value{static_cast<std::uint64_t>(key)};
        } else if constexpr (std::is_integral_v<Bare>) {
            return Value{static_cast<std::int64_t>(key)};
        } else if constexpr (std::is_floating_point_v<Bare>) {
            return Value{static_cast<double>(key)};
        } else if constexpr (std::is_convertible_v<const Bare&, std::string_view>) {
            return Value{std::string(std::string_view(key))};
        } else {
            return ValueConverter<Bare>::to_value(key);
        }
    }

    void require_transaction(std::string_view what) const {
        if (!connection_->in_transaction()) {
            throw query_build_error(std::format(
                "schemata: {} needs an open transaction. SQLite's SAVEPOINT would quietly "
                "start one and the MySQL family's would not, so the same code would roll "
                "back on one engine and commit on the other. Open a transaction with "
                "begin() or transaction() first.",
                what));
        }

        // Counted as open is not the same as open. After the engine has rolled
        // the transaction back, SQLite's SAVEPOINT quietly starts a fresh one
        // -- and the commit that follows then succeeds, baselining work whose
        // rows went with the rollback.
        connection_->require_live_transaction(what);
    }

    [[nodiscard]] ChangeTracker::StageMark find_savepoint(const std::string& name) const {
        for (const auto& [held, mark] : savepoints_) {
            if (held == name) {
                return mark;
            }
        }
        throw query_build_error(std::format(
            "schemata: no savepoint named '{}' was taken on this Context. A savepoint "
            "taken directly on the connection is not one of these -- it does not mark "
            "the staged work, which is the whole reason to take it here.",
            name));
    }

    void erase_savepoint(const std::string& name) {
        savepoints_.erase(std::remove_if(savepoints_.begin(), savepoints_.end(),
                                         [&](const auto& held) {
                                             return held.first == name;
                                         }),
                          savepoints_.end());
    }

    /// Drop everything taken after `name`, which the engines discard too.
    void forget_savepoints_after(const std::string& name) {
        for (std::size_t i = 0; i < savepoints_.size(); ++i) {
            if (savepoints_[i].first == name) {
                savepoints_.resize(i + 1);
                return;
            }
        }
    }

    std::optional<ConnectionPool::Lease> lease_;
    driver::Connection*                  connection_ = nullptr;
    const Model*                         model_      = nullptr;
    ChangeTracker                        tracker_;

    /// Savepoints taken through this Context, innermost last, each with
    /// where the staging stood when it was taken.
    std::vector<std::pair<std::string, ChangeTracker::StageMark>> savepoints_;
};

} // namespace schemata
