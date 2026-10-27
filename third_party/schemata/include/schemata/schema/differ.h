#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <schemata/model/model.h>
#include <schemata/schema/snapshot.h>
#include <schemata/sql/ddl.h>

namespace schemata {

enum class OperationKind {
    create_table,
    drop_table,
    add_column,
    drop_column,
    alter_column,
    create_index,
    drop_index,
    add_foreign_key,
    drop_foreign_key,
    add_check,
    create_trigger,
    drop_trigger,
    rebuild_table,

    /// Move a table's primary key to other columns: `DROP PRIMARY KEY, ADD
    /// PRIMARY KEY` in one statement, MySQL family only. SQLite rebuilds the
    /// table instead.
    alter_primary_key,

    /// Drop a CHECK the model retired, MySQL family only. SQLite rebuilds the
    /// table instead. See `EntityBuilder::retire_check`.
    drop_check,

    /// Set a table's default collation to the model's, MySQL family only.
    /// See `Model::default_collation`.
    alter_table_collation,

    /// Move columns already in the table into the model's order: one `ALTER
    /// TABLE ... MODIFY COLUMN ... AFTER`, MySQL family only. SQLite rebuilds
    /// the table instead. Never destructive: the columns keep their rows.
    ///
    /// Proposed on its own only where MariaDB can make it without copying the
    /// table, asked for at `ALGORITHM=INSTANT` (see
    /// `SchemaOperation::notice_if_refused`), and otherwise only when
    /// `DiffOptions::reorder_columns` asks for it. See that option.
    reorder_columns,

    /// Rename a column in place, as the model declares it was
    /// (`Column::renamed_from`): `ALTER TABLE ... RENAME COLUMN` on every
    /// engine, with whatever CHECK, index or trigger has to follow it. Never
    /// destructive: every row keeps its value. Runs before anything else on
    /// an existing table, so the rest of the diff is made against the new
    /// name. Last, so every other kind keeps its value.
    rename_column,
};

[[nodiscard]] std::string_view to_string(OperationKind kind) noexcept;

/// How much an operation can cost you if it is wrong.
///
/// The safety rail from D4 rests on this classification: `ensure_schema()`
/// applies additive and widening operations, and refuses destructive ones
/// unless they are explicitly opted into for that run.
enum class Severity {
    /// Creates something that was not there. Nothing can be lost.
    additive,

    /// Relaxes a constraint -- making a column nullable, lengthening a
    /// string. Existing rows stay valid.
    widening,

    /// Can lose data or structure: dropping a column or table, narrowing a
    /// type, adding a constraint existing rows might violate.
    destructive,
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

/// A read-only question about the rows already in a table, which decides
/// whether an operation can cost anything. See `SchemaOperation::probes`.
struct RowProbe {
    /// `SELECT COUNT(*) ...`, counting the rows the operation would break.
    std::string count_sql;

    /// `SELECT ... LIMIT 1`, one such row's key values as text, to name in the
    /// description. Asked only when the count is not zero; may be empty.
    std::string example_sql;

    /// What those rows break, completing "N row(s) ...": "break the CHECK",
    /// "reference no row of 'parent'", and so on.
    std::string breaks;

    /// How the example's values are introduced, as "the first keyed (id)".
    std::string example_is;
};

/// A new CHECK or foreign key that names a column the same run adds, which
/// the rows already there would break at that column's default. See
/// `SchemaOperation::arriving_checks`.
struct ArrivingConstraint {
    /// The constraint's name -- a column's own CHECK by `column_check_name()`.
    std::string name;

    /// What to ask the table about this constraint alone: a subset of the
    /// operation's `probes`. Empty where it cannot be asked beforehand.
    std::vector<RowProbe> probes{};
};

struct SchemaOperation {
    OperationKind kind     = OperationKind::create_table;
    Severity      severity = Severity::additive;

    std::string table;

    /// The column, index or constraint involved. Empty for table-level work.
    std::string object;

    /// Statements to run, in order.
    ///
    /// Usually one. A table rebuild is many, and creating a table carries its
    /// indexes alongside it so the two can never be applied apart.
    std::vector<std::string> statements;

    /// Why this operation is proposed, in a form fit for a drift report.
    std::string description;

    /// True when foreign keys are not to be enforced while this runs.
    ///
    /// On SQLite: a table rebuild drops and recreates a table, and a table
    /// caught in a reference cycle with others being dropped cannot go first
    /// under live enforcement whichever one is chosen. Neither can be fixed by
    /// ordering, and `PRAGMA foreign_keys` is a no-op inside a transaction, so
    /// whoever applies the diff has to turn enforcement off before it begins.
    ///
    /// On the MySQL family: a foreign key renamed in place -- `DROP FOREIGN
    /// KEY old, ADD CONSTRAINT new ...` in one statement -- runs with the
    /// session's `foreign_key_checks` at 0 for that statement alone, and put
    /// back to what it was afterwards, whether the statement succeeded or
    /// not. The rows are not read again: the key given up enforced the same
    /// rule on them until the statement ran.
    ///
    /// Said here rather than inferred from the operation's kind, so that the
    /// applier does not have to know which kinds need it.
    bool suspends_foreign_keys = false;

    /// True when the operation can only go through on a table with no rows:
    /// it adds a NOT NULL column with no default, and there is nothing to
    /// fill the rows already there with.
    ///
    /// `ensure_schema()` asks the table before anything runs. Empty, the
    /// column goes in -- by a rebuild on SQLite, which cannot add one any
    /// other way. With rows it is refused, naming the column and the two ways
    /// through: a DEFAULT, or a `before_schema` migration that adds it
    /// nullable, backfills and tightens it. On MySQL and MariaDB
    /// `allow_destructive` lets it through instead, and the server fills each
    /// row with the type's zero value.
    ///
    /// `severity` is what it costs on a table with rows -- destructive on
    /// every engine -- and `severity_when_empty` what it costs on one without,
    /// which is what `ensure_schema()` applies it at there. There is an
    /// unavoidable window on the MySQL family: a row inserted between the
    /// question and the `ALTER` is filled with the zero value.
    bool     needs_empty_table   = false;
    Severity severity_when_empty = Severity::additive;

    /// Questions to put to the table when the operation is destructive only
    /// because rows already there might break it: a new CHECK, a new foreign
    /// key, a collation move under a unique index or a CHECK.
    ///
    /// The same mechanism as `needs_empty_table`, asking about the rows that
    /// matter rather than about any row. `ensure_schema()` and
    /// `verify_schema()` ask each before anything runs, read-only. When every
    /// count is zero the operation is applied, and reported, at
    /// `severity_when_clean`; otherwise it keeps `severity`, and its
    /// description says how many rows break it and names the first. A probe
    /// the server will not answer -- one past MySQL's `max_execution_time` or
    /// MariaDB's `max_statement_time` (1969), or on a table the user may not
    /// SELECT from -- leaves the operation as it was, its description ending
    /// `(the table could not be asked: <why>)`.
    ///
    /// `diff_schema()` alone asks nothing, so an operation carrying probes is
    /// reported at `severity` there: what it can cost before the rows are
    /// known.
    ///
    /// Each is a scan of the table, or an anti-join to the referenced one for
    /// a foreign key, so it costs about what the `ALTER` would spend checking
    /// the same rows; they are attached only where the answer can change the
    /// severity, and skipped where the table is known empty. Between the probe
    /// and the `ALTER` a row can arrive that breaks it: MySQL and MariaDB both
    /// check the rows again as the `ALTER` runs -- `ADD CONSTRAINT ... CHECK`
    /// (on MariaDB while `check_constraint_checks` is on, its default) and
    /// `ADD FOREIGN KEY` under `foreign_key_checks` alike -- and refuse it
    /// with the table unchanged. SQLite probes and applies inside one
    /// transaction.
    std::vector<RowProbe> probes{};
    Severity              severity_when_clean = Severity::additive;

    /// True when a server that refuses the statement leaves the operation
    /// undone rather than failing the run: a column reorder asked of MariaDB
    /// at `ALGORITHM=INSTANT`, which the server refuses (1845, 1846) where it
    /// could only do it by copying the table. `ensure_schema()` then moves the
    /// operation into `SchemaDiff::notices`, with the server's reason, and
    /// goes on: the order of the columns is never worth a failed deploy.
    bool notice_if_refused = false;

    /// What a `reorder_columns` operation moves, in order: each column and the
    /// one it goes after. Empty for every other kind.
    ///
    /// `diff_schema()` writes each column's definition as the model has it.
    /// `ensure_schema()` writes it again from the live table before it runs --
    /// as `SHOW CREATE TABLE` prints the column, so a default, a COMMENT or
    /// INVISIBLE the model does not say survives the move -- for every column
    /// nothing else in the run redefines. A column whose live clause cannot be
    /// read is left where it is, with a notice saying so.
    std::vector<ColumnMove> column_moves{};

    /// The foreign keys this operation adds that the live table did not have
    /// under any name: an `add_foreign_key` that is neither a rename nor a
    /// recreate, or a `rebuild_table` one of whose reasons is such a key.
    /// Empty otherwise. `ensure_schema()` reads it to defer a key into a table
    /// the same run creates (see `EnsureResult::deferred_foreign_keys`).
    std::vector<std::string> new_foreign_keys{};

    /// The CHECKs this operation adds that name a column the same run adds,
    /// and that are destructive as the diff stands: an `add_check`, or a
    /// `rebuild_table` one of whose reasons is such a CHECK -- each with the
    /// probes of its own reason, so a rebuild destructive for some other
    /// reason does not make it look so. Never one that takes the place of a
    /// live CHECK under its name. Empty otherwise. `ensure_schema()` reads it,
    /// under `DiffOptions::hold_back_for_backfill`, to hold back a CHECK the
    /// rows would break at the new column's default until an `after_columns`
    /// migration has backfilled them (see `EnsureResult::deferred_checks`).
    std::vector<ArrivingConstraint> arriving_checks{};

    /// The same for foreign keys: the new keys this operation adds on a
    /// column the same run adds, which every row already there would hold the
    /// column's default in, not NULL -- an `add_foreign_key`, or a
    /// `rebuild_table` one of whose reasons is such a key. Empty otherwise.
    /// See `DeferredForeignKey::new_columns`.
    std::vector<ArrivingConstraint> arriving_foreign_keys{};
};

struct SchemaDiff {
    std::vector<SchemaOperation> operations;

    /// What differs and is not drift: columns in another order than the
    /// model declares them, which this run does not change. Informational --
    /// `empty()`, `has_destructive()` and `verify_schema()` ignore them, and
    /// `report()` lists them after the operations. See
    /// `DiffOptions::reorder_columns`.
    std::vector<std::string> notices;

    /// No operation. A diff holding only notices is empty: the schema matches.
    [[nodiscard]] bool empty() const noexcept { return operations.empty(); }

    [[nodiscard]] bool has_destructive() const noexcept;

    /// Operations that would be refused without an explicit opt-in.
    [[nodiscard]] std::vector<const SchemaOperation*> destructive() const;

    /// Every statement, in application order.
    [[nodiscard]] std::vector<std::string> statements() const;

    /// A human-readable account of the drift, for `verify_schema()`, with the
    /// notices after it.
    [[nodiscard]] std::string report() const;
};

/// Where a schema alias a model refers to lives on this server.
///
/// Implicit from a name, so the common case reads as a pair:
///
///     options.schema_bindings = {{"world", "hb_dev_world"},
///                                {"player", "hb_dev_player"}};
struct SchemaBinding {
    SchemaBinding(std::string name_, const Model* model_ = nullptr)
        : name(std::move(name_)), model(model_) {}
    SchemaBinding(const char* name_, const Model* model_ = nullptr)
        : name(name_), model(model_) {}

    /// The schema's name on the server.
    ///
    /// Quoted into the `REFERENCES` of a key into the schema, so a name that
    /// cannot be an identifier -- one holding a NUL, say -- is refused with
    /// `schema_error` by `ensure_schema()` and `verify_schema()` before the
    /// schema is read.
    std::string name;

    /// The model that schema is ensured from, when the caller has it.
    ///
    /// Optional. Given, a key into the schema is checked against it before
    /// the server is asked anything: that the table and the columns exist,
    /// that the columns are its primary key or a unique index, that each
    /// integer column has the width and signedness of the one referencing it,
    /// and that each text column is collated as the one referencing it -- each
    /// as its own model collates it, its own collation or its model's default.
    /// The live table is checked either way, once the schema is read.
    /// Borrowed for the call and not kept.
    const Model* model = nullptr;
};

/// Schema alias to where it lives, for every alias a model's keys name.
using SchemaBindings = std::map<std::string, SchemaBinding, std::less<>>;

struct DiffOptions {
    /// Permit operations classified destructive.
    ///
    /// Off by default, and deliberately per-run rather than per-context:
    /// dropping a column is the sort of thing that should be decided at the
    /// moment it happens, not configured once and forgotten.
    bool allow_destructive = false;

    /// Propose dropping tables that exist in the database but not in the
    /// model.
    ///
    /// Off by default, because a database routinely holds tables that are
    /// none of Schemata's business -- other applications, other tools,
    /// migration leftovers.
    ///
    /// Two kinds are never proposed even when this is on, because no model
    /// can declare them and so every model reads as having dropped them: a
    /// SQLite virtual table, and the ordinary tables a virtual table's module
    /// keeps its data in (`notes_fts_data` and the rest). Dropping one of
    /// those shadow tables leaves the virtual table that owns it corrupt.
    bool drop_extra_tables = false;

    /// Read the live schema even when the recorded fingerprint matches.
    ///
    /// The fingerprint records which *model* the database was last aligned
    /// to. It cannot know that something else has changed the database since,
    /// so the fast path will happily skip a run that would have healed real
    /// drift. Set this when healing out-of-band changes is the point --
    /// `verify_schema()` always reads and is unaffected.
    bool ignore_fingerprint = false;

    /// Move columns already in a table into the order the model declares
    /// them, where that takes rebuilding the table.
    ///
    /// The model's declaration order is the order wanted, and a new column is
    /// always added at its place in it (`ADD COLUMN ... AFTER`, or `FIRST`) on
    /// MySQL and MariaDB, which both do that without copying the table. SQLite
    /// cannot place one, and adds it last.
    ///
    /// Columns already there in another order -- added last by a release
    /// before rc.18, or by SQLite, or moved in the model -- are a notice
    /// (`SchemaDiff::notices`), never drift: `verify_schema()` passes, and
    /// nothing counts as destructive. Where they are moved:
    ///
    ///   - MariaDB moves them in place (`ALGORITHM=INSTANT`) without being
    ///     asked, on the first run that reads the schema -- one whose model
    ///     changed, or with `ignore_fingerprint` -- not at every start. Where
    ///     it would have to copy the table it refuses, and the move stays a
    ///     notice.
    ///   - MySQL rebuilds the table to move a column, and SQLite always does;
    ///     both only with this set.
    ///
    /// A move changes the column's place and nothing else: on the MySQL family
    /// the column is redefined from the live table's own clause, and a SQLite
    /// rebuild made for the move alone keeps each column's live default. See
    /// `SchemaOperation::column_moves`. The rebuild is never destructive for the
    ///     move itself -- the rows are copied whole -- though a SQLite rebuild
    ///     is for whatever else it would lose (see `diff_schema()`).
    ///
    /// Off by default: on a large table a rebuild is minutes of work, and a
    /// column's place is not worth that by surprise. Set, the run reads the
    /// live schema even when the fingerprint matches, as `ignore_fingerprint`
    /// does -- the columns were out of order under that fingerprint too -- so
    /// set it for the run that should move them, not for every start.
    bool reorder_columns = false;

    /// How long to wait for another process that is already changing this
    /// schema, on the engines that have an advisory lock.
    ///
    /// Schema changes run one at a time: two replicas starting together would
    /// otherwise both compute the same work and both do it. The loser waits
    /// this long and then says so, rather than failing with whatever the
    /// engine happened to raise first.
    ///
    /// Zero does not wait at all, which is what a deploy that would rather
    /// retry wants. A negative value is read as zero: passed through, the two
    /// servers take it in opposite directions -- MySQL waits for ever, MariaDB
    /// fails at once -- so a computed timeout that slipped below zero would
    /// behave differently on each.
    ///
    /// The lock is the one `migrate()` takes, and its name is given there, for
    /// an operator asking the server who holds it.
    int lock_timeout_seconds = 30;

    /// Where each schema alias the model's foreign keys name lives, for this
    /// run. See `EntityBuilder::references`.
    ///
    /// Supplied per run rather than declared in the model, because the name is
    /// the environment's: the same model is ensured into `hb_dev_player`
    /// against `hb_dev_world` in one place and into `hbv_17_player` against
    /// `hbv_17_world` in the next, and fingerprints the same in both. A key
    /// whose alias is missing here is refused, naming the alias, and never
    /// read as a key into this schema.
    ///
    /// The schema a key refers into has to be there before the key can be: on
    /// a first build, ensure the schemas in the order their keys run -- the one
    /// referenced first -- and drop them in the reverse.
    ///
    /// `ensure_schema()` records the bindings its model's keys used in
    /// `__schemata_meta`, as their own row beside the fingerprint -- which
    /// they never reach -- and trusts the fingerprint only while both match:
    /// a schema aligned with `world` bound to one name is not aligned with it
    /// bound to another. Where the server folds names
    /// (`lower_case_table_names` non-zero) the names are recorded lower-cased,
    /// as the server compares them, so `World_X` on one run and `world_x` on
    /// the next is one binding; where it compares names as written, they are
    /// two.
    SchemaBindings schema_bindings{};

    /// The schema to work on, when it is not the session's own: a physical
    /// name, or an alias in `schema_bindings`. Empty is the session's.
    ///
    /// For one connection serving several schemas. The call reads, locks,
    /// records and changes that schema as it would the session's own, and the
    /// session's default schema is what it was afterwards -- `SELECT
    /// DATABASE()` answers the same before and after. A schema the server
    /// does not have is refused by name. MySQL and MariaDB only, and the
    /// connection needs a default schema of its own to go back to -- MySQL
    /// cannot put a session back to having none. Any schema will do: a
    /// process that creates every schema it uses may connect with
    /// `information_schema` as its default.
    std::string schema{};

    /// Copy the schema into a new one before a run that destroys something,
    /// under this name. See `backup`, which names the copy itself.
    ///
    /// MySQL and MariaDB only. The copy is made only when the diff the run is
    /// about to apply -- settled, after the rows were asked (see
    /// `SchemaOperation::probes`) -- holds a destructive operation, which it
    /// can only once `allow_destructive` lets it through: inside the schema
    /// lock `ensure_schema()` holds for the run, after everything was read and
    /// before its first statement. A run with nothing destructive copies
    /// nothing. `EnsureResult::backup_schema` names the copy made.
    ///
    /// What the copy holds, and how it is taken, is `copy_schema()`'s: every
    /// base table, each created `LIKE` its original and filled from one
    /// consistent snapshot of the schema, without a lock on the live tables;
    /// no foreign key, no trigger. It is the caller's to keep or drop.
    ///
    /// **What it misses.** The snapshot is InnoDB's: a MyISAM, Aria, MEMORY or
    /// ARCHIVE table is copied as it is when read. The copy is taken inside
    /// the run's lock, but the lock keeps out other runs, not writers: a row
    /// written after the snapshot opened and before the destructive statement
    /// is in neither the copy nor -- where that statement drops what it was
    /// written into -- the altered table. The copy is one transaction, so a
    /// large one can pass `max_binlog_cache_size` and reaches a replica as
    /// one. A connection lost mid-copy leaves the part it had made, since the
    /// DROP of it cannot run, and a later run naming that copy is refused as
    /// the server already having the schema: drop it by hand.
    ///
    /// Refused with `schema_error` before anything is read: on SQLite, and a
    /// name that cannot be a schema's (`identifier_problem()`, 64 characters
    /// at most). One the server already has is refused as the copy is about to
    /// be made -- still before the run's first statement, and so before
    /// anything is changed. A copy that fails stops the run there, with
    /// nothing applied.
    std::string backup_schema{};

    /// Copy the schema before a run that destroys something, as
    /// `backup_schema` does, into one named `<schema>__bk_<yyyymmdd>_<hhmmss>`
    /// after the schema the run changes and the UTC time the call began:
    /// `hb_world__bk_20260930_141503`. Letters, digits and underscores, so
    /// valid unquoted, and they sort by time. A schema name longer than 43
    /// characters makes one longer than the 64 a schema may have, and is
    /// refused. `backup_schema`, where it is set, is the name instead.
    bool backup = false;

    /// Design rules to hold the model to besides the ones every run applies
    /// (`Model::validate()`): off by default. A model breaking one is refused
    /// with `schema_error` before anything is read, as an invalid model is.
    /// Give `verify_schema()` the same, so the gate refuses what the deploy
    /// would.
    ValidateOptions validation{};

    /// Hold back, rather than refuse, a new CHECK or foreign key on a column
    /// this run adds to a table with rows, where the rows would break it at
    /// the column's default (SCH-0044). The column goes in, the constraint is
    /// reported in `EnsureResult::deferred_checks` or
    /// `EnsureResult::deferred_foreign_keys`, the fingerprint is not recorded,
    /// and the next `ensure_schema()` -- after an `after_columns` migration
    /// has backfilled the rows -- adds it, or refuses it if a row still
    /// breaks it.
    ///
    /// Off by default, and off a run refuses such a constraint on its first
    /// run with nothing applied, as before. `migrate_and_ensure()` turns it on
    /// for its first `ensure_schema()` when an `after_columns` migration is
    /// pending; set it here to run the passes by hand. A foreign key into a
    /// table the same run creates waits for the seed whatever this says
    /// (`EnsureResult::deferred_foreign_keys`).
    bool hold_back_for_backfill = false;
};

/// Compare the model against a live schema.
///
/// The one engine behind `ensure_schema()` and `verify_schema()`.
///
/// Column defaults are deliberately **not** compared. Engines rewrite them on
/// the way in -- quoting them, reformatting them, resolving expressions --
/// so a literal comparison reports drift on a database that matches, which is
/// the failure that makes a differ untrustworthy. Defaults are still read
/// into the snapshot and still emitted on CREATE; they are simply not a
/// reason to propose an alteration.
///
/// They are emitted on every rewrite too, which has a consequence. Every
/// MySQL-family `MODIFY` writes the model's default back with the column, and
/// every SQLite rebuild regenerates the whole table from the model. So a
/// default changed in the model lands as a side effect of unrelated work -- on
/// MySQL and MariaDB when that column is next altered for another reason, on
/// SQLite when anything about the table next needs a rebuild -- and until then
/// the database keeps the old one and nothing reports it. Change a default with
/// a migration, alongside the model.
///
/// Which columns make each table's primary key *is* compared. A change is
/// destructive: `alter_primary_key` on the MySQL family, a rebuild on SQLite.
/// A foreign key standing on a column whose type, length or collation changes
/// is dropped before the column work and added back after it, at the severity
/// of that change, because neither server alters a column under a key.
///
/// An index no model could have declared -- over an expression, over some of
/// the rows, over a column prefix, or over a generated column -- is left out
/// altogether. Reported as an ordinary index it read as one the model had
/// dropped, which is destructive, so an application that indexed its own data
/// found every later change to that table refused along with it. It is neither
/// proposed for dropping nor counted as drift. What is not silent is a SQLite
/// rebuild, which regenerates the table from the model and would lose such an
/// index along with anything else the model cannot say -- a CHECK of the
/// application's own, a UNIQUE constraint, a generated column, DEFERRABLE,
/// WITHOUT ROWID, STRICT. A rebuild that would lose any of them is
/// `destructive` whatever its reasons were, and its description names each
/// one.
///
/// A CHECK is compared as the model writes it and the snapshot holds it, each
/// reduced to a canonical form, and replaced where one under the model's name
/// says another rule. `ensure_schema()` and `verify_schema()` also ask the
/// server how it prints a rule that reads as redefined, and compare that
/// (`SnapshotTable::canonical_checks`). A caller going through the public
/// `read_schema()` and then this gets no such print: a spelling the
/// normaliser does not know the server's print of -- MariaDB's `LENGTH`,
/// MySQL's `CAST` -- reads as a redefined CHECK and is proposed for
/// replacement.
[[nodiscard]] SchemaDiff diff_schema(const Model&          desired,
                                     const SchemaSnapshot& actual,
                                     Dialect               dialect,
                                     const DiffOptions&    options = {});

/// Order tables so that every foreign key's target is created first.
///
/// Returns names in a safe creation order. A cycle cannot be ordered, and the
/// names involved are returned through `cyclic` so the caller can fall back
/// to creating those tables without their foreign keys and adding them
/// afterwards.
[[nodiscard]] std::vector<const Table*> creation_order(const Model&               model,
                                                       std::vector<std::string>* cyclic);

} // namespace schemata
