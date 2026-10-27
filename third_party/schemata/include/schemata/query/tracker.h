#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <typeindex>
#include <vector>

#include <schemata/core/error.h>
#include <schemata/core/value.h>
#include <schemata/driver/driver.h>
#include <schemata/model/model.h>

namespace schemata {

/// What a tracked entity is waiting to become.
enum class EntityState {
    /// Not in the database yet.
    added,

    /// In the database. Whether it needs an UPDATE is decided at save time
    /// by comparing against the snapshot, not by the caller saying so.
    unchanged,

    /// In the database, and to be deleted.
    removed,
};

/// Pending row changes, applied together by `save_changes()`.
///
/// The tracker holds pointers to entities the caller owns; it never copies
/// them and never takes ownership. That follows from D3 -- there is no
/// identity map and no proxying, so an entity is a plain struct that happens
/// to be known to the tracker. The consequence is the one rule here: a
/// tracked entity must outlive the tracker, or be released with `forget()`.
///
/// Worth stating plainly, because breaking it is quiet. Nothing detects a
/// tracked entity going out of scope -- there is no identity map to notice
/// and no proxy to intercept -- so the next `save_changes()` reads whatever
/// now occupies that memory and sends an UPDATE built from it. The statement
/// succeeds. The shape that does it is an ordinary-looking loop that declares
/// its entity inside the body and tracks it, and the fix is to keep the
/// entities alive for as long as the tracker refers to them: a vector that
/// outlives the loop, or `forget()` before each one goes.
///
/// What it does hold is a snapshot of each tracked entity's column values,
/// taken when it was tracked. That is what makes the UPDATE minimal: at save
/// time the current values are compared against the snapshot and only the
/// columns that actually differ are sent.
class ChangeTracker {
public:
    explicit ChangeTracker(const Model& model) : model_(&model) {}

    /// A tracker outlives the expression that made it, so a temporary model
    /// would dangle from the first call onwards. Refused at compile time
    /// rather than left to surface as a crash somewhere else.
    explicit ChangeTracker(const Model&&) = delete;

    /// Register a new entity to be inserted.
    template <typename T>
    void add(T& entity) {
        enter(&entity, std::type_index(typeid(T)), EntityState::added);
    }

    /// Register an entity that already exists, snapshotting it.
    ///
    /// Call this after materialising a row you intend to modify. Anything
    /// changed on it afterwards becomes the UPDATE.
    ///
    /// Calling it again on an entity already tracked takes a fresh snapshot,
    /// so reloading a row into the same object and tracking it again starts
    /// from what the row holds now -- provided the object still holds the same
    /// row. If its key differs from the one it was tracked under, that is
    /// refused: see `remove()`.
    template <typename T>
    void track(T& entity) {
        enter(&entity, std::type_index(typeid(T)), EntityState::unchanged);
    }

    /// Register an existing entity to be deleted.
    ///
    /// The DELETE is addressed by the key the entity held when it was
    /// tracked. If that key has changed since -- whether the key was edited
    /// on a tracked entity or the object now holds a different row, which
    /// cannot be told apart from here -- this refuses rather than guessing
    /// which row was meant: one guess deletes the row the object used to
    /// hold, the other deletes a row nobody asked about. `forget()` the
    /// entity first if it now stands for a different row.
    template <typename T>
    void remove(T& entity) {
        enter(&entity, std::type_index(typeid(T)), EntityState::removed);
    }

    /// Stop tracking an entity, discarding any pending change to it.
    ///
    /// The way to release an entity that is about to go out of scope. Work
    /// staged for it inside a transaction is let go as well, so a rollback
    /// afterwards neither writes a key back into the object nor puts it back
    /// as pending -- it may be gone by then.
    template <typename T>
    void forget(T& entity) {
        release(&entity);
    }

    /// How many entities are tracked, whatever their state.
    [[nodiscard]] std::size_t tracked() const noexcept { return entries_.size(); }

    /// Forget everything without applying it.
    ///
    /// Everything means the staging and the withdrawn keys too: nothing is
    /// left that a rollback could write into, and nothing is left that could
    /// refuse a later save over a batch that has been abandoned.
    void clear() noexcept;

    /// Apply every pending change, in one transaction.
    ///
    /// Order follows the foreign keys: inserts run parent-first so a row
    /// never references one that does not exist yet, and deletes run in
    /// reverse so a row is never removed while something still points at it.
    /// Within one table, the order entities were registered is preserved.
    ///
    /// A generated key is read back into the entity after its insert, so the
    /// entity holds the row's real identity when this returns.
    ///
    /// What that does *not* do is fill in a child's foreign key. Navigations
    /// are value containers with no link back to the parent object (D3), so
    /// nothing here can know that `post.author_id` was meant to follow
    /// `user.id`. Adding both in one call would send whatever `author_id`
    /// happened to hold -- a zero -- and the server would reject it. Insert
    /// the parent, read its key off the entity, assign it, then save the
    /// child:
    ///
    ///     tracker.add(user);
    ///     tracker.save_changes(db);      // user.id is now the real key
    ///     post.author_id = user.id;
    ///     tracker.add(post);
    ///     tracker.save_changes(db);
    ///
    /// Deletes have no such gap: the keys already exist, so parents and
    /// children can go in one call and the ordering here handles them.
    ///
    /// A row removed and another added under the same key in one call is a
    /// replacement, and the old row is deleted before anything is inserted --
    /// taking with it, children first, the removed rows that point at it. The
    /// one order that cannot be had is refused before anything is sent: an
    /// update still pointing at a row that has to go early, since updates run
    /// after inserts. Save that update first.
    ///
    /// After a successful save every added and unchanged entity is tracked
    /// as unchanged with a fresh snapshot, and removed entities are
    /// forgotten -- so calling twice does not repeat the work.
    ///
    /// An update or delete addresses its row by the model's key, and it is
    /// the database that makes a key unique. Where the table's primary key is
    /// not the model's, one entity's statement can reach several rows; that
    /// raises `schema_error` naming the table, and nothing is saved. MySQL and
    /// MariaDB count only the rows an UPDATE changed, so a second row already
    /// holding the new values is not noticed there. A statement that reaches
    /// no row at all is not an error.
    ///
    /// \returns the number of statements executed.
    /// \throws  anything the driver raises. A statement that fails is rolled
    ///          back first, so a failed save leaves the database as it was.
    ///
    /// The exception is a commit whose outcome is unknown, which on MySQL and
    /// MariaDB is what a connection lost under the `COMMIT` leaves: the server
    /// may have committed it and died before the reply. Nothing is withdrawn
    /// then -- the rows may be there, with the keys these entities were given
    /// -- and the work is staged as `save_within()` stages it instead.
    /// `has_staged_work()` reports that, every later save is refused, and
    /// `commit_within()` or `rollback_within()` says which way it went once
    /// the data has been checked.
    std::size_t save_changes(driver::Connection& connection);

    /// Apply every pending change without opening a transaction.
    ///
    /// For a caller that already has one open. Neither engine nests
    /// transactions, and they fail differently: SQLite refuses a second
    /// BEGIN, while MySQL's START TRANSACTION silently commits whatever was
    /// in flight -- so a nested save would quietly end the caller's
    /// transaction and leave half its work committed.
    ///
    /// Each call is atomic inside the caller's transaction. It runs under a
    /// savepoint of its own, so a failure partway takes back the statements
    /// before it and leaves the entries pending exactly as they were -- a
    /// caller who catches the error and carries on, skipping the row the
    /// database refused, does not find the rows before it applied twice. The
    /// savepoint is named `schemata_save_within`; do not take one of that
    /// name yourself.
    ///
    /// Refuses a transaction the server has already ended -- a deadlock, a
    /// trigger's `RAISE(ROLLBACK)`, DDL's implicit commit -- rather than
    /// running each statement on its own, committed at once, as if it were
    /// still part of one.
    ///
    /// Success is not the end of it either. The caller's transaction may
    /// still roll back after this returns, and then the rows are gone --
    /// so what was applied is staged rather than baselined, and the caller
    /// has to say which way it went with `commit_within()` or
    /// `rollback_within()`. `Context::Scope` does that for you; a caller
    /// driving a `Transaction` by hand has to do it itself.
    std::size_t save_within(driver::Connection& connection);

    /// How much work is staged right now.
    ///
    /// The counterpart to a savepoint. A savepoint marks a place in the
    /// transaction the caller can return to; this marks the matching place
    /// in the staging, so returning to one can return to the other.
    ///
    /// Opaque on purpose: the numbers mean nothing outside this class, and a
    /// mark taken from one tracker must not be given to another.
    struct StageMark {
        std::size_t entries = 0;
        std::size_t keys    = 0;
    };

    [[nodiscard]] StageMark stage_mark() const noexcept;

    /// Undo the staging back to `mark`, as a `ROLLBACK TO SAVEPOINT` undoes
    /// the rows.
    ///
    /// Without this, a rollback to a savepoint takes the rows away and
    /// leaves the tracker believing they were saved -- so the commit that
    /// follows baselines them, `save_changes()` reports nothing to do, and
    /// the work is gone with nothing raised to say so.
    ///
    /// Restores the same way `rollback_within()` does, counting from the
    /// mark instead of from the start of the transaction.
    void rollback_within_to(StageMark mark) noexcept;

    /// Whether a `save_within()` is still waiting to be resolved.
    ///
    /// True between a `save_within()` and the `commit_within()` or
    /// `rollback_within()` that says how it went.
    [[nodiscard]] bool has_staged_work() const noexcept { return staged_.has_value(); }

    /// The transaction a `save_within()` joined has committed.
    ///
    /// What was applied is now permanent, so the staged undo is dropped and
    /// the entries stay baselined.
    void commit_within() noexcept;

    /// The transaction a `save_within()` joined has rolled back.
    ///
    /// Puts each entity back as the row stood when the transaction began,
    /// with the caller's latest word on what should become of it. An insert
    /// becomes pending again, an update goes back to the old snapshot, and
    /// generated keys written into entities are undone -- those rows no
    /// longer exist, and an entity still holding one would hand it to a child
    /// on the retry. An entity removed by a later save in the same
    /// transaction comes back removed, and one inserted and then removed
    /// leaves nothing to do at all.
    ///
    /// An entity the caller has registered again since the last save keeps
    /// what it was registered as, measured against the row as it now is. One
    /// it has forgotten is left alone.
    void rollback_within() noexcept;

private:
    struct Entry {
        void*              entity = nullptr;
        const Table*       table  = nullptr;
        EntityState        state  = EntityState::added;
        std::vector<Value> snapshot;
    };

    /// A generated key written into an entity, and what it held before.
    ///
    /// Kept so it can be put back if the rows it came from never survive:
    /// an entity still holding the id of a row that was rolled back would
    /// hand it to a child on the retry and produce a foreign key violation
    /// against a row that was never there.
    struct WrittenKey {
        /// Null once the caller has forgotten the entity, which may since
        /// have been destroyed. Nothing is written through it then.
        void*         entity = nullptr;
        const Column* column = nullptr;
        Value         previous;

        /// The key that was written, as the entity reads it back.
        ///
        /// Held here rather than read off the entity at undo time, so the
        /// withdrawn key can still be remembered for an entity that is gone.
        Value         written;

        /// Whose key it is, so a child pointing at it can be recognised.
        std::string   table;
    };

    /// A generated key that was handed out and then taken back.
    ///
    /// Putting the key back in the parent is only half the repair. A caller
    /// who copied it into a child -- `book.author_id = author.id`, the
    /// ordinary way to relate two new rows -- still holds it, and nothing
    /// here can reach into that copy: it is a plain assignment in their code,
    /// not something the tracker was told about.
    ///
    /// So the value is remembered instead, and the next save refuses to
    /// write a foreign key still pointing at it. The alternative is what used
    /// to happen: on SQLite the id gets reused by somebody else and the child
    /// silently attaches to the wrong parent, and on the MySQL family every
    /// subsequent save fails with a foreign key violation against a row that
    /// was never there.
    struct StaleKey {
        std::string table;
        Value       value;
    };

    /// Saves that have been applied but whose transaction has not resolved.
    ///
    /// `entries` holds one record per entity per save: each entity a save
    /// had work for, as it stood just before that save. Every save is
    /// recorded, not only the first to touch an entity, because a savepoint
    /// mark is a position in this list -- an entity saved on both sides of a
    /// savepoint needs a record past the mark -- and because the rollback
    /// needs the last thing a transaction did to an entity as well as the
    /// first. A record whose entity has since been forgotten keeps its place,
    /// with the entity cleared, so marks taken before stay true.
    struct Staged {
        std::vector<Entry>      entries;
        std::vector<WrittenKey> keys;

        /// Which transaction this was staged inside.
        ///
        /// Without it, `save_within()` cannot tell a second save in the same
        /// transaction from the first save in a new one, and the two want
        /// opposite treatment: the first merges into the staging, the second
        /// means the staging belongs to a transaction nobody resolved.
        std::uint64_t generation = 0;
    };

    /// True when anything is actually pending: an insert, a delete, or a
    /// tracked entity whose columns differ from its snapshot.
    [[nodiscard]] bool has_work() const;
    [[nodiscard]] static bool has_work(const Entry& entry);

    /// Run every pending statement, in order. No transaction of its own.
    ///
    /// Generated keys written into entities are appended to `written_keys`,
    /// including when this throws: whether they are put back depends on
    /// whether the caller's transaction survives, which only the caller
    /// knows. `early` is what `deletes_before_inserts()` returned.
    std::size_t apply_all(driver::Connection&      connection,
                          std::vector<WrittenKey>& written_keys,
                          const std::vector<bool>& early);

    /// Put generated keys back, and remember what was taken away.
    ///
    /// Not static: the keys it withdraws have to be recorded, so a child
    /// still holding a copy of one can be caught before it is written.
    void undo_written_keys(const std::vector<WrittenKey>& keys) noexcept;

    /// The updates and deletes, once the inserts have landed.
    std::size_t apply_rest(driver::Connection&             connection,
                           const std::vector<std::size_t>& order,
                           std::size_t                     applied,
                           const std::vector<bool>&        early);

    /// Which removed entries have to be deleted before anything is inserted.
    ///
    /// Indexed like `entries_`. A row removed while another is added under
    /// the same key goes first, or the insert meets its key; so does every
    /// removed row pointing at one that goes first, or that delete meets its
    /// foreign key. Called before anything is sent.
    ///
    /// \throws query_build_error when an update still points at a row that
    ///         has to go first, which no order can serve: updates run after
    ///         inserts.
    [[nodiscard]] std::vector<bool> deletes_before_inserts() const;

    /// Mark what was applied as the new baseline and drop what was removed.
    void rebaseline();

    /// Record applied work whose transaction has not resolved yet.
    ///
    /// Merged into whatever is staged already, which is what a second
    /// `save_within()` inside the same transaction wants. `generation` is the
    /// transaction the work was applied in, so staging left behind by one that
    /// has since ended can be told from staging that is still live.
    void stage(std::uint64_t generation, std::vector<Entry> entries,
               std::vector<WrittenKey> keys);

    /// Put back everything staged from these positions onward.
    ///
    /// The one restore both `rollback_within()` and `rollback_within_to()`
    /// use, so the two cannot disagree about what a rollback means.
    void restore_since(std::size_t entries, std::size_t keys) noexcept;

    void enter(void* entity, std::type_index type, EntityState state);
    void release(const void* entity) noexcept;

    /// Entry indices grouped by table, in foreign-key dependency order.
    [[nodiscard]] std::vector<std::size_t> ordered_indices() const;

    const Model*       model_;
    std::vector<Entry> entries_;

    /// Refuse to carry staging across a transaction boundary.
    ///
    /// Staged work is only meaningful while the transaction that produced it
    /// is still open. Once it has ended without either resolve being called,
    /// nothing here knows whether those rows survived -- and both guesses are
    /// wrong in a way that shows up as data: keeping the staging re-inserts
    /// them if the transaction committed, discarding it loses them if it did
    /// not.
    void require_live_staging(const driver::Connection& connection) const;

    /// Refuse to write a foreign key pointing at a row that was rolled back.
    void require_no_stale_foreign_keys() const;

    /// Keys handed out and then taken back, until a save gets past them.
    std::vector<StaleKey> stale_keys_;

    /// Set between a `save_within()` and the commit or rollback that decides
    /// whether it counted.
    std::optional<Staged> staged_;
};

} // namespace schemata
